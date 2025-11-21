#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"
#include "ldmsd_jobmgr_query.h"
#include "ldmsd_tenant.h"

#define JOB_ID_NAME_LEN 512

#define DEFAULT_SCHEMA_NAME "test_per_job_metrics"
#define DEFAULT_MAX_NUM_PIDS 10

#define DEFAULT_KEY_NAME "job_id"

/*
 * The sampler plugin creates a set per job
 */

typedef struct pid_src_s {
	FILE *mf;
	TAILQ_ENTRY(pid_src_s) ent;
} *pid_src_t;

typedef struct job_set_s {
	ldms_set_t set;
	int is_exited; /* 1 means job has been exited. */
	TAILQ_HEAD(pid_src_list, pid_src_s) pid_src_list;
	TAILQ_ENTRY(job_set_s) set_ent;
} *job_set_t;

TAILQ_HEAD(job_set_list, job_set_s);

typedef struct job_metrics_s {
	ovis_log_t log;
	const char *producer;

	/* Reference to tenant definition (for event subscription)*/
	ldmsd_tenant_def_t tdef;
	ldmsd_tenant_event_consumer_t event_handle;
	uint8_t owns_tenant;

	/* Schema for per-job sets */
	ldms_schema_t schema;
	int *mids; /* Metric ID of metrics in the schema  TODO: check if this's accurate */
	ldms_record_t recdef; /* record definition including jobmgr's record entries */
	int recdef_mid; /* Metric ID of the record definition */
	int mlist_mid; /* Metric ID of the list of records */
	int *rec_ent_mids; /* Metric IDS of all record entries */

	/* Cache record entry index (from tenant record) */
	int job_id_midx; /* TODO: assign this value */
	int key_midx; /* TODO: assign this value */
	int task_pid_midx; /* TODO: Assign this value */

	/* Per-job set management */
	pthread_mutex_t jset_list_lock;
	struct job_set_list jset_list; /* List of per-job sets */
} *job_metrics_t;

struct ldmsd_str_list *default_job_av;

static const char *usage(ldmsd_plug_handle_t handle)
{
	return "config name=<inst_name> tenant=<tenant_def_name> producer=<producer_name>\n"
	       "    tenant       - (optional) Name of tenant definition to use\n"
	       "    producer     - Producer name for the sets\n";
}

static int constructor(ldmsd_plug_handle_t handle)
{
	int rc;
	job_metrics_t inst = calloc(1, sizeof(*inst));
	if (!inst)
		return ENOMEM;
	ldmsd_plug_ctxt_set(handle, inst);
	inst->log = ldmsd_plug_log_get(handle);

	/* Create default attribute list */
	default_job_av = calloc(1, sizeof(*default_job_av));
	if (!default_job_av) {
		rc = ENOMEM;
		goto err;
	}
	TAILQ_INIT(default_job_av);

	/* Add default attributes */
	const char *default_attrs[] = {"job_id", "step_id", "task_id", "task_pid"};
	for (int i = 0; i < 4; i++) {
		struct ldmsd_str_ent *ent = calloc(1, sizeof(*ent));
		if (!ent) {
			rc = ENOMEM;
			goto err;
		}
		ent->str = strdup(default_attrs[i]);
		if (!ent->str) {
			free(ent);
			rc = ENOMEM;
			goto err;
		}
		TAILQ_INSERT_TAIL(default_job_av, ent, entry);
	}

	pthread_mutex_init(&inst->jset_list_lock, NULL);
	TAILQ_INIT(&inst->jset_list);
	return 0;
err:
	if (default_job_av) {
		struct ldmsd_str_ent *ent;
		while ((ent = TAILQ_FIRST(default_job_av))) {
			TAILQ_REMOVE(default_job_av, ent, entry);
			free((char*)ent->str);
			free(ent);
		}
		free(default_job_av);
	}
	free(inst);
	return rc;
}

/* The caller must hold the job set list lock */
static job_set_t __jset_find(job_metrics_t jm, const char *job_id)
{
	job_set_t jset;
	const char *jid;
	ldms_mval_t mv;

	TAILQ_FOREACH(jset, &jm->jset_list, set_ent) {
		mv = ldms_metric_get(jset->set, jm->mids[0]);
		jid = mv->a_char;
		if (0 == strcmp(jid, job_id)) {
			return jset;
		}
	}
	return NULL;
}

static int __record_metrics_set(job_metrics_t jm, ldms_mval_t rec)
{
	int i = 1; /* Skip pid */
	int card = ldms_record_card(rec);
	uint64_t pid = ldms_record_get_u64(rec, 0);

	for (i = 1; i < card; i++) {
		ldms_record_set_u64(rec, i, pid);
	}
	return 0;
}

static void __populate_pid_record(job_metrics_t jm, job_set_t jset,
					 ldms_mval_t pid_val)
{
	ldms_mval_t rec, lh;

	ldms_transaction_begin(jset->set);
	rec = ldms_record_alloc(jset->set, jm->recdef_mid);
	if (!rec) {
		ldms_transaction_end(jset->set);
		ovis_log(jm->log, OVIS_LERROR, "Failed to allocate record for PID\n");
		return;
	}

	ldms_record_set_u64(rec, 0, pid_val->v_u64); /* Set pid value */
	__record_metrics_set(jm, rec);
	lh = ldms_metric_get(jset->set, jm->mlist_mid);
	ldms_list_append_record(jset->set, lh, rec);

	ldms_transaction_end(jset->set);
}

static int __task_start_handle(job_metrics_t jm, ldmsd_jobmgr_query_event_t e)
{
	int rc = 0;
	job_set_t jset;
	ldms_mval_t job_id, pid;
	ldms_mval_t qrec = e->start_end.qrec;

	job_id = ldms_record_metric_get(qrec, jm->job_id_midx);
	pid = ldms_record_metric_get(qrec, jm->task_pid_midx);

	pthread_mutex_lock(&jm->jset_list_lock);
	jset = __jset_find(jm, job_id->a_char);
	pthread_mutex_unlock(&jm->jset_list_lock);

	if (!jset) {
		/*
		 * This can happen if ldmsd starts after the job has started.
		 */
		ovis_log(jm->log, OVIS_LINFO,
			 "job set for job_id '%s' doesn't exist.\n", job_id->a_char);
		return 0;
	}

	__populate_pid_record(jm, jset, pid);

	return rc;
}

static int __job_end_handle(job_metrics_t jm, ldmsd_jobmgr_query_event_t e)
{
	job_set_t jset;
	ldms_mval_t qrec = e->start_end.qrec;

	ldms_mval_t job_id = ldms_record_metric_get(qrec, jm->job_id_midx);

	pthread_mutex_lock(&jm->jset_list_lock);
	jset = __jset_find(jm, job_id->a_char);
	if (!jset) {
		pthread_mutex_unlock(&jm->jset_list_lock);
		ovis_log(jm->log, OVIS_LINFO,
			 "job set for job_id '%s' not found on job_end.\n",
			 job_id->a_char);
		return 0;
	}
	jset->is_exited = 1;
	pthread_mutex_unlock(&jm->jset_list_lock);
	return 0;
}

static ldms_set_t __create_set(job_metrics_t jm, const char *job_id, const char *job_key)
{
	ldms_set_t set;
	ldms_mval_t mv;
	char set_name[512];

	snprintf(set_name, sizeof(set_name), "%s/%s", jm->producer, job_id);

	set = ldms_set_new(set_name, jm->schema);
	if (!set) {
		ovis_log(jm->log, OVIS_LERROR, "Failed to create set '%s'\n", set_name);
		return NULL;
	}

	/* Initialize job_id and binding_key */
	ldms_transaction_begin(set);
	mv = ldms_metric_get(set, jm->mids[0]);
	snprintf(mv->a_char, JOB_ID_NAME_LEN, "%s", job_id);

	mv = ldms_metric_get(set, jm->mids[1]);
	snprintf(mv->a_char, JOB_ID_NAME_LEN, "%s", job_key);
	ldms_transaction_end(set);

	return set;
}

static int __job_start_handle(job_metrics_t jm, ldmsd_jobmgr_query_event_t e)
{
	job_set_t jset;
	ldms_mval_t qrec = e->start_end.qrec;
	ldms_mval_t job_id, job_key;

	jset = calloc(1, sizeof(*jset));
	if (!jset) {
		ovis_log(jm->log, OVIS_LCRIT, "Memory allocation failure.\n");
		return ENOMEM;
	}

	TAILQ_INIT(&jset->pid_src_list);

	job_id = ldms_record_metric_get(qrec, jm->job_id_midx);
	job_key = ldms_record_metric_get(qrec, jm->key_midx);

	jset->set = __create_set(jm, job_id->a_char, job_key->a_char);
	if (!jset->set) {
		free(jset);
		return ENOMEM;
	}

	pthread_mutex_lock(&jm->jset_list_lock);
	TAILQ_INSERT_TAIL(&jm->jset_list, jset, set_ent);
	pthread_mutex_unlock(&jm->jset_list_lock);

	ldms_set_publish(jset->set);
	return 0;
}

static int per_job_event_cb(ldmsd_tenant_def_t tdef,
			    ldmsd_jobmgr_query_event_type_t etype,
			    ldms_mval_t qrec, void *arg)
{
	job_metrics_t jm = (job_metrics_t)arg;
	struct ldmsd_jobmgr_query_event_s ev;

	/* Build event structure */
	ev.event_type = etype;
	ev.start_end.qrec = qrec;

	switch (etype) {
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_START:
		return __job_start_handle(jm, &ev);
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_END:
		return __job_end_handle(jm, &ev);
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_START:
		/* Nothing to do */
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_END:
		/* Nothing to do */
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_START:
		return __task_start_handle(jm, &ev);
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_END:
		/* Could mark PID as exited here if needed */
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE:
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_CLEANUP:
	case LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE:
		/* Nothing to do */
		break;
	}
	return 0;
}

static struct ldms_metric_template_s per_job_common_metrics[] = {
	{ "job_id",      0, LDMS_V_CHAR_ARRAY, "", JOB_ID_NAME_LEN },
	{ "binding_key", 0, LDMS_V_CHAR_ARRAY, "", JOB_ID_NAME_LEN },
	{0},
};

static struct ldms_metric_template_s test_recdef[] = {
	{ "pid",	0,	LDMS_V_U64,		"",		1},
	{ "mem",	0,	LDMS_V_U64,		"bytes",	1 },
	{ "cpu",	0,	LDMS_V_U64,		"cycles",	1 },
	{0}
};

static int __create_internal_tenant(job_metrics_t jm, struct attr_value_list *avl)
{
	char *producer = av_value(avl, "producer");

	if (!producer) {
		ovis_log(jm->log, OVIS_LERROR, "'producer' is required\n");
		return EINVAL;
	}

	/* Create internal tenant with default attributes */
	jm->tdef = ldmsd_tenant_def_new("__per_job_internal",
					producer,
					default_job_av,
					DEFAULT_KEY_NAME,
					getuid(), getgid(), 0600);
	if (!jm->tdef) {
		ovis_log(jm->log, OVIS_LERROR,
			 "Failed to create internal tenant definition\n");
		return errno;
	}

	return 0;
}

static int config(ldmsd_plug_handle_t handle,
		  struct attr_value_list *kwl, struct attr_value_list *avl)
{
	int rc;
	size_t heap_sz;
	int max_num_pids = DEFAULT_MAX_NUM_PIDS;
	char *tenant_def_name = NULL;
	const char *key_name = NULL;
	char *producer;
	job_metrics_t jm = ldmsd_plug_ctxt_get(handle);

	producer = av_value(avl, "producer");
	if (!producer) {
		ovis_log(jm->log, OVIS_LERROR, "'producer' is required\n");
		return EINVAL;
	}
	jm->producer = strdup(producer);
	if (!jm->producer)
		return ENOMEM;

	tenant_def_name = av_value(avl, "tenant");
	if (tenant_def_name) {
		jm->tdef = ldmsd_tenant_def_find(tenant_def_name);
		if (!jm->tdef) {
			ovis_log(jm->log, OVIS_LERROR,
				 "Tenant definition '%s' not found\n",
				 tenant_def_name);
			return EINVAL;
		}
		jm->owns_tenant = 0;
	} else {
		/* Create internal tenant */
		rc = __create_internal_tenant(jm, avl);
		if (rc) {
			ovis_log(jm->log, OVIS_LERROR,
				 "Failed to create internal tenant definition.\n");
			return rc;
		}
		jm->owns_tenant = 1;
	}

	/* Get required field indices from tenant */
	jm->job_id_midx = ldmsd_tenant_def_get_key_name(jm->tdef, &key_name);
	if (jm->job_id_midx < 0) {
		ovis_log(jm->log, OVIS_LERROR,
			"Tenant definition has no key field.\n");
		return EINVAL;
	}

	/* For now, assume job_id is the binding key */
	jm->key_midx = jm->job_id_midx;

	/* Get task_pid index - this would need proper implementation */
	/* jm->task_pid_midx = ... get from tenant */
	jm->task_pid_midx = 3; /* Temporary: assume it's at index 3 */

	/* Create record definition */
	jm->rec_ent_mids = calloc(4, sizeof(int));
	if (!jm->rec_ent_mids)
		return ENOMEM;

	jm->recdef = ldms_record_from_template("pid_recdef", test_recdef, jm->rec_ent_mids);
	if (!jm->recdef) {
		ovis_log(jm->log, OVIS_LERROR, "Failed to create record definition\n");
		return errno;
	}

	/* Create schema */
	jm->mids = calloc(3, sizeof(int));
	if (!jm->mids)
		return ENOMEM;

	jm->schema = ldms_schema_from_template("per_job_schema", per_job_common_metrics, jm->mids);
	if (!jm->schema) {
		ovis_log(jm->log, OVIS_LERROR, "Failed to create schema\n");
		return errno;
	}

	jm->recdef_mid = ldms_schema_record_add(jm->schema, jm->recdef);
	if (jm->recdef_mid < 0) {
		ovis_log(jm->log, OVIS_LERROR, "Failed to add record to schema\n");
		return -jm->recdef_mid;
	}

	heap_sz = ldms_record_heap_size_get(jm->recdef) * max_num_pids;
	jm->mlist_mid = ldms_schema_metric_list_add(jm->schema, "pid_rec_list", "", heap_sz);
	if (jm->mlist_mid < 0) {
		ovis_log(jm->log, OVIS_LERROR, "Failed to add list to schema\n");
		return -jm->mlist_mid;
	}

	/* Register for tenant events */
	jm->event_handle = ldmsd_tenant_event_register(jm->tdef, per_job_event_cb, jm);
	if (!jm->event_handle) {
		ovis_log(jm->log, OVIS_LERROR, "Failed to register for tenant events\n");
		return errno;
	}

	return 0;
}

static int sample(ldmsd_plug_handle_t handle)
{
	job_metrics_t jm = ldmsd_plug_ctxt_get(handle);
	job_set_t jset;
	ldms_mval_t lh, rec;
	size_t cnt;
	enum ldms_value_type vtype;

	pthread_mutex_lock(&jm->jset_list_lock);
	TAILQ_FOREACH(jset, &jm->jset_list, set_ent) {
		if (jset->is_exited)
			continue;
		ldms_transaction_begin(jset->set);
		lh = ldms_metric_get(jset->set, jm->mlist_mid);

		rec = ldms_list_first(jset->set, lh, &vtype, &cnt);
		while (rec) {
			if (vtype == LDMS_V_RECORD_INST) {
				__record_metrics_set(jm, rec);
			}
			rec = ldms_list_next(jset->set, rec, &vtype, &cnt);
		}
		ldms_transaction_end(jset->set);
	}
	pthread_mutex_unlock(&jm->jset_list_lock);

	return 0;
}

static void destructor(ldmsd_plug_handle_t handle)
{
	job_metrics_t jm = ldmsd_plug_ctxt_get(handle);
	job_set_t jset;

	if (!jm)
		return;

	/* Cleanup job sets */
	while ((jset = TAILQ_FIRST(&jm->jset_list))) {
		TAILQ_REMOVE(&jm->jset_list, jset, set_ent);
		if (jset->set) {
			ldms_set_unpublish(jset->set);
			ldms_set_delete(jset->set);
		}
		free(jset);
	}

	/* Unregister from tenant events */
	if (jm->event_handle) {
		ldmsd_tenant_event_unregister(jm->event_handle);
	}

	/* Cleanup tenant if we own it */
	if (jm->owns_tenant && jm->tdef) {
		ldmsd_tenant_def_del(jm->tdef);
	}

	/* Cleanup schema and other resources */
	if (jm->schema)
		ldms_schema_delete(jm->schema);
	if (jm->recdef)
		ldms_record_delete(jm->recdef);

	free((char*)jm->producer);
	free(jm->mids);
	free(jm->rec_ent_mids);

	pthread_mutex_destroy(&jm->jset_list_lock);
	free(jm);
}

struct ldmsd_sampler ldmsd_plugin_interface = {
	.base.type = LDMSD_PLUGIN_SAMPLER,
	.base.flags = LDMSD_PLUGIN_MULTI_INSTANCE,
	.base.config = config,
	.base.usage = usage,
	.base.constructor = constructor,
	.base.destructor = destructor,

	.sample = sample,
};
