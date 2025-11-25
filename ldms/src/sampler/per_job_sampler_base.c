/* per_job_sampler_base.c */

#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/queue.h>

#include "per_job_sampler_base.h"

#define JOB_ID_LEN 512
#define DEFAULT_MAX_NUM_PIDS 1024
#define DEFAULT_KEY_NAME "job_id"

/* Forward declarations */
static int __create_internal_tenant(per_job_base_t base,
				    struct attr_value_list *avl,
				    const char *key_field);
static int per_job_event_cb(ldmsd_tenant_def_t tdef,
			    ldmsd_jobmgr_query_event_type_t etype,
			    ldms_mval_t qrec, void *arg);

/* Job set structure */
typedef struct job_set_s
{
	ldms_set_t set;
	int is_exited;
	TAILQ_ENTRY(job_set_s)
	entry;
} *job_set_t;

TAILQ_HEAD(job_set_list, job_set_s);

/* Base structure */
struct per_job_base_s
{
	/* Configuration */
	ovis_log_t log;
	const char *plugin_name;
	const char *producer;
	const char *schema_name;

	/* Tenant integration */
	ldmsd_tenant_def_t tdef;
	ldmsd_tenant_event_consumer_t event_handle;
	uint8_t owns_tenant;

	/* Field index cache */
	int job_id_midx;
	int key_midx;
	int task_pid_midx;

	/* Schema and metrics */
	ldms_schema_t schema;
	int *mids;	      /* Metric IDs for standard fields */
	ldms_record_t recdef; /* PID record definition */
	int recdef_mid;	      /* Metric ID of record definition */
	int mlist_mid;	      /* Metric ID of PID record list */
	int *rec_ent_mids;    /* Metric IDs within record */
	int rec_ent_count;    /* Number of entries in record */

	/* Per-job set management */
	pthread_mutex_t jset_list_lock;
	struct job_set_list jset_list;

	/* Plugin callback */
	per_job_base_sample_pid_fn_t sample_pid_fn;
	void *plugin_ctxt;
};

/* Default job attributes for self-contained mode */
static struct ldmsd_str_list *default_job_av = NULL;

static void __init_default_job_av()
{
	if (default_job_av)
		return;

	default_job_av = calloc(1, sizeof(*default_job_av));
	if (!default_job_av)
		return;

	TAILQ_INIT(default_job_av);

	const char *attrs[] = {"job_id", "step_id", "task_id", "task_pid"};
	for (int i = 0; i < 4; i++)
	{
		struct ldmsd_str_ent *ent = calloc(1, sizeof(*ent));
		if (!ent)
			return;
		ent->str = strdup(attrs[i]);
		if (!ent->str)
		{
			free(ent);
			return;
		}
		TAILQ_INSERT_TAIL(default_job_av, ent, entry);
	}
}

/* Standard per-job metrics */
static struct ldms_metric_template_s per_job_std_metrics[] = {
    {"job_id", 0, LDMS_V_CHAR_ARRAY, "", JOB_ID_LEN},
    {"binding_key", 0, LDMS_V_CHAR_ARRAY, "", JOB_ID_LEN},
    {0},
};

per_job_base_t per_job_base_config(struct attr_value_list *avl,
				   const char *plugin_name,
				   const char *def_schema,
				   ovis_log_t mylog,
				   char **error)
{
	int rc;
	per_job_base_t base;
	char *producer, *tenant_name, *key_field, *schema_name;

	__init_default_job_av();

	base = calloc(1, sizeof(*base));
	if (!base)
	{
		if (error)
			*error = strdup("Memory allocation failure");
		errno = ENOMEM;
		return NULL;
	}

	base->log = mylog;
	base->plugin_name = strdup(plugin_name);

	pthread_mutex_init(&base->jset_list_lock, NULL);
	TAILQ_INIT(&base->jset_list);

	/* Parse producer (required) */
	producer = av_value(avl, "producer");
	if (!producer)
	{
		if (error)
			*error = strdup("'producer' is required");
		rc = EINVAL;
		goto err;
	}
	base->producer = strdup(producer);

	/* Parse schema name */
	schema_name = av_value(avl, "schema");
	if (!schema_name || schema_name[0] == '\0')
		base->schema_name = strdup(def_schema);
	else
		base->schema_name = strdup(schema_name);

	/* Parse key_field (optional) */
	key_field = av_value(avl, "key_field");

	/* Parse tenant (optional) */
	tenant_name = av_value(avl, "tenant");

	if (tenant_name)
	{
		/* Shared tenant mode */
		base->tdef = ldmsd_tenant_def_find(tenant_name);
		if (!base->tdef)
		{
			if (error)
				asprintf(error, "Tenant definition '%s' not found", tenant_name);
			rc = ENOENT;
			goto err;
		}
		base->owns_tenant = 0;
	}
	else
	{
		/* Self-contained mode */
		rc = __create_internal_tenant(base, avl, key_field);
		if (rc)
		{
			if (error)
				*error = strdup("Failed to create internal tenant");
			goto err;
		}
		base->owns_tenant = 1;
	}

	/* Get field indices from tenant */
	const char *key_name;
	base->job_id_midx = ldmsd_tenant_def_get_key_name(base->tdef, &key_name);
	if (base->job_id_midx < 0)
	{
		if (error)
			*error = strdup("Tenant has no key field");
		rc = EINVAL;
		goto err;
	}

	/* If key_field specified, find it; otherwise use job_id */
	if (key_field)
	{
		/* TODO: Need API to get specific field index by name */
		base->key_midx = base->job_id_midx; /* Temporary */
	}
	else
	{
		base->key_midx = base->job_id_midx;
	}

	/* Get task_pid index - assume it's at index 3 for now */
	base->task_pid_midx = 3;

	return base;

err:
	if (base)
	{
		free((char *)base->plugin_name);
		free((char *)base->producer);
		free((char *)base->schema_name);
		free(base);
	}
	errno = rc;
	return NULL;
}

static int __create_internal_tenant(per_job_base_t base,
				    struct attr_value_list *avl,
				    const char *key_field)
{
	char tenant_name[256];
	const char *key = key_field ? key_field : DEFAULT_KEY_NAME;

	snprintf(tenant_name, sizeof(tenant_name), "__per_job_%s_internal",
		 base->plugin_name);

	base->tdef = ldmsd_tenant_def_new(tenant_name,
					  base->producer,
					  default_job_av,
					  key,
					  getuid(), getgid(), 0600);
	if (!base->tdef)
	{
		ovis_log(base->log, OVIS_LERROR,
			 "Failed to create internal tenant definition\n");
		return errno;
	}

	return 0;
}

ldms_schema_t per_job_base_schema_create(per_job_base_t base,
				         struct ldms_metric_template_s *pid_metrics,
				         int max_num_pids)
{
	size_t heap_sz;

	if (!base || !pid_metrics) { /* TODO: consider making pid_metrics optional. Another thing we should pair up pid_metrics with the pid_sample_fn */
		errno = EINVAL;
		return NULL;
	}

	if (max_num_pids <= 0)
		max_num_pids = DEFAULT_MAX_NUM_PIDS;

	/* Count PID metrics */
	base->rec_ent_count = 0;
	while (pid_metrics[base->rec_ent_count].name != NULL)
		base->rec_ent_count++;

	/* Create record definition */
	base->rec_ent_mids = calloc(base->rec_ent_count, sizeof(int));
	if (!base->rec_ent_mids) {
		ovis_log(base->log, OVIS_LCRIT, "Memory allocation failure.\n");
		errno = ENOMEM;
		goto err;
	}

	base->recdef = ldms_record_from_template("pid_record",
						 pid_metrics,
						 base->rec_ent_mids);
	if (!base->recdef) {
		ovis_log(base->log, OVIS_LERROR,
			 "Failed to create record definition\n");
		goto err;
	}

	/* Create schema */
	base->mids = calloc(3, sizeof(int)); /* job_id, binding_key, terminator */
	if (!base->mids) {
		ovis_log(base->log, OVIS_LCRIT, "Memory allocation failure.\n");
		errno = ENOMEM;
		goto err;
	}

	base->schema = ldms_schema_from_template(base->schema_name,
						 per_job_std_metrics,
						 base->mids);
	if (!base->schema) {
		ovis_log(base->log, OVIS_LERROR, "Failed to create schema\n");
		goto err;
	}

	/* Add record definition to schema */
	base->recdef_mid = ldms_schema_record_add(base->schema, base->recdef);
	if (base->recdef_mid < 0) {
		ovis_log(base->log, OVIS_LERROR,
			 "Failed to add record to schema\n");
		errno = -base->recdef_mid;
		goto err;
	}

	/* Add PID list to schema */
	heap_sz = ldms_record_heap_size_get(base->recdef) * max_num_pids;
	base->mlist_mid = ldms_schema_metric_list_add(base->schema,
						      "pid_list", "", heap_sz);
	if (base->mlist_mid < 0) {
		ovis_log(base->log, OVIS_LERROR, "Failed to add list to schema\n");
		errno = -base->mlist_mid;
		goto err;
	}

	/* Register for tenant events */
	base->event_handle = ldmsd_tenant_event_register(base->tdef,
							 per_job_event_cb,
							 base);
	if (!base->event_handle) {
		ovis_log(base->log, OVIS_LERROR,
			 "Failed to register for tenant events\n");
		goto err;
	}

	return base->schema;

 err:
	free(base->rec_ent_mids);
	ldms_record_delete(base->recdef);
	free(base->mids);
	ldms_schema_delete(base->schema);
	return NULL;
}

int per_job_base_set_pid_sampler(per_job_base_t base,
				 per_job_base_sample_pid_fn_t sample_fn,
				 void *plugin_ctxt)
{
	if (!base || !sample_fn)
		return EINVAL;

	base->sample_pid_fn = sample_fn;
	base->plugin_ctxt = plugin_ctxt;

	return 0;
}

/* Helper: Find job set by job_id */
static job_set_t __jset_find(per_job_base_t base, const char *job_id)
{
	job_set_t jset;
	ldms_mval_t mv;
	const char *jid;

	TAILQ_FOREACH(jset, &base->jset_list, entry)
	{
		mv = ldms_metric_get(jset->set, base->mids[0]);
		jid = mv->a_char;
		if (0 == strcmp(jid, job_id))
			return jset;
	}
	return NULL;
}

/* Helper: Create per-job set */
static ldms_set_t __create_job_set(per_job_base_t base,
				   const char *job_id,
				   const char *binding_key)
{
	ldms_set_t set;
	ldms_mval_t mv;
	char set_name[512];

	snprintf(set_name, sizeof(set_name), "%s/%s", base->producer, job_id);

	set = ldms_set_new(set_name, base->schema);
	if (!set)
	{
		ovis_log(base->log, OVIS_LERROR, "Failed to create set '%s'\n",
			 set_name);
		return NULL;
	}

	/* Initialize job_id and binding_key */
	ldms_transaction_begin(set);
	mv = ldms_metric_get(set, base->mids[0]);
	snprintf(mv->a_char, JOB_ID_LEN, "%s", job_id);

	mv = ldms_metric_get(set, base->mids[1]);
	snprintf(mv->a_char, JOB_ID_LEN, "%s", binding_key);
	ldms_transaction_end(set);

	ldms_set_publish(set);
	ldmsd_set_register(set, base->plugin_name);

	return set;
}

/* Event handlers */
static int __job_start_handle(per_job_base_t base, ldms_mval_t qrec)
{
	job_set_t jset;
	ldms_mval_t job_id, binding_key;

	jset = calloc(1, sizeof(*jset));
	if (!jset) {
		ovis_log(base->log, OVIS_LCRIT, "Memory allocation failure\n");
		return ENOMEM;
	}

	job_id = ldms_record_metric_get(qrec, base->job_id_midx);
	binding_key = ldms_record_metric_get(qrec, base->key_midx);

	jset->set = __create_job_set(base, job_id->a_char, binding_key->a_char);
	if (!jset->set) {
		free(jset);
		return ENOMEM;
	}

	pthread_mutex_lock(&base->jset_list_lock);
	TAILQ_INSERT_TAIL(&base->jset_list, jset, entry);
	pthread_mutex_unlock(&base->jset_list_lock);

	return 0;
}

static int __task_start_handle(per_job_base_t base, ldms_mval_t qrec)
{
	job_set_t jset;
	ldms_mval_t job_id, pid, lh, rec;

	job_id = ldms_record_metric_get(qrec, base->job_id_midx);
	pid = ldms_record_metric_get(qrec, base->task_pid_midx);

	pthread_mutex_lock(&base->jset_list_lock);
	jset = __jset_find(base, job_id->a_char);
	pthread_mutex_unlock(&base->jset_list_lock);

	if (!jset) {
		ovis_log(base->log, OVIS_LINFO,
			 "Job set for job_id '%s' not found\n", job_id->a_char);
		return 0;
	}

	/* Allocate PID record */
	ldms_transaction_begin(jset->set);
	rec = ldms_record_alloc(jset->set, base->recdef_mid);
	if (!rec) {
		ldms_transaction_end(jset->set);
		ovis_log(base->log, OVIS_LERROR, "Failed to allocate PID record\n");
		return ENOMEM;
	}

	/* Set PID value in first field */
	ldms_record_set_u64(rec, 0, pid->v_u64);

	/* Add to list */
	lh = ldms_metric_get(jset->set, base->mlist_mid);
	ldms_list_append_record(jset->set, lh, rec);
	ldms_transaction_end(jset->set);

	return 0;
}

static int __job_end_handle(per_job_base_t base, ldms_mval_t qrec)
{
	job_set_t jset;
	ldms_mval_t job_id;

	job_id = ldms_record_metric_get(qrec, base->job_id_midx);

	pthread_mutex_lock(&base->jset_list_lock);
	jset = __jset_find(base, job_id->a_char);
	if (jset)
		jset->is_exited = 1;
	/* TODO: Handle when to delete a set of exited jobs */
	pthread_mutex_unlock(&base->jset_list_lock);

	return 0;
}

static int per_job_event_cb(ldmsd_tenant_def_t tdef,
			    ldmsd_jobmgr_query_event_type_t etype,
			    ldms_mval_t qrec, void *arg)
{
	per_job_base_t base = (per_job_base_t)arg;

	switch (etype)
	{
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_START:
		return __job_start_handle(base, qrec);
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_START:
		return __task_start_handle(base, qrec);
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_END:
		return __job_end_handle(base, qrec);
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_START:
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_END:
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_END:
	case LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE:
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_CLEANUP:
	case LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE:
		break;
	}

	return 0;
}

int per_job_base_sample(per_job_base_t base)
{
	job_set_t jset;
	ldms_mval_t lh, rec, pid_val;
	enum ldms_value_type vtype;
	size_t cnt;
	uint64_t pid;
	int rc;

	if (!base || !base->sample_pid_fn)
		return EINVAL;

	pthread_mutex_lock(&base->jset_list_lock);

	TAILQ_FOREACH(jset, &base->jset_list, entry)
	{
		if (jset->is_exited)
			continue;

		ldms_transaction_begin(jset->set);

		lh = ldms_metric_get(jset->set, base->mlist_mid);
		rec = ldms_list_first(jset->set, lh, &vtype, &cnt);

		while (rec)
		{
			if (vtype == LDMS_V_RECORD_INST)
			{
				/* Get PID from first field */
				pid_val = ldms_record_metric_get(rec, 0);
				pid = pid_val->v_u64;

				/* Call plugin's sampling function */
				rc = base->sample_pid_fn(rec, pid, base->plugin_ctxt);
				if (rc)
				{
					ovis_log(base->log, OVIS_LDEBUG,
						 "Failed to sample PID %lu: %d\n", pid, rc);
				}
			}

			rec = ldms_list_next(jset->set, rec, &vtype, &cnt);
		}

		ldms_transaction_end(jset->set);
	}

	pthread_mutex_unlock(&base->jset_list_lock);

	return 0;
}

void per_job_base_del(per_job_base_t base)
{
	job_set_t jset;

	if (!base)
		return;

	/* Unregister from events */
	if (base->event_handle)
	{
		ldmsd_tenant_event_unregister(base->event_handle);
	}

	/* Delete all job sets */
	while ((jset = TAILQ_FIRST(&base->jset_list)))
	{
		TAILQ_REMOVE(&base->jset_list, jset, entry);
		if (jset->set)
		{
			ldmsd_set_deregister(ldms_set_instance_name_get(jset->set),
					     base->plugin_name);
			ldms_set_unpublish(jset->set);
			ldms_set_delete(jset->set);
		}
		free(jset);
	}

	/* Delete tenant if we own it */
	if (base->owns_tenant && base->tdef)
	{
		ldmsd_tenant_def_del(base->tdef);
	}
	else if (base->tdef)
	{
		ldmsd_tenant_def_find_put(base->tdef);
	}

	/* Cleanup schema and other resources */
	if (base->schema)
		ldms_schema_delete(base->schema);
	if (base->recdef)
		ldms_record_delete(base->recdef);

	free((char *)base->plugin_name);
	free((char *)base->producer);
	free((char *)base->schema_name);
	free(base->mids);
	free(base->rec_ent_mids);

	pthread_mutex_destroy(&base->jset_list_lock);

	free(base);
}

const char *per_job_base_producer_get(per_job_base_t base)
{
	return base ? base->producer : NULL;
}
