/* per_job_sampler_base.c - Base library implementation for per-job samplers
 *
 * This library handles all the complex aspects that plugin authors
 * shouldn't have to worry about.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>

#include "per_job_sampler_base.h"
#include "ldmsd_jobmgr_query.h"
#include "coll/rbt.h"

#define BINDING_KEY_MAX_LEN 256

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

/* PID entry in per-job RBT */
struct pid_entry_s {
	pid_t pid;
	ldms_mval_t pid_rec;  /* PID record in the list */
	struct rbn rbn;       /* RBT node (keyed by pid) */
};

/* Per-job base instance - one per active job */
struct per_job_base_s {
	char job_id[LDMSD_JOBMGR_JOB_ID_LEN];
	char binding_key[BINDING_KEY_MAX_LEN];

	/* LDMS set for this job */
	ldms_set_t set;
	char set_name[256];

	/* Cached metric indices (set-level, copied from sampler at creation) */
	int job_id_midx;
	int binding_key_midx;
	int pid_list_midx;
	int pid_recdef_midx;

	/* Plugin-defined per-job metric indices (copied from sampler) */
	int *per_job_metric_midx;
	int  num_per_job_metrics;

	/* PID tracking for this job */
	struct rbt pid_rbt;  /* pid_t → pid_entry_s */
	int num_pids;

	/* Plugin's per-job context */
	void *job_ctxt;

	/* Back-reference to parent sampler */
	struct per_job_sampler_s *sampler;

	/* RBT linkage (in sampler's job_rbt) */
	struct rbn rbn;
};

/* Per-job sampler - master controller */
struct per_job_sampler_s {
	ldmsd_plug_handle_t pi;
	ovis_log_t log;

	/* Schema (shared across all jobs) */
	ldms_schema_t schema;
	char *schema_name;
	char *producer;
	char *instance;  /* For self-contained mode */

	/* Cached schema-level metric indices (copied into each job base) */
	int job_id_midx;
	int binding_key_midx;
	int pid_recdef_midx;
	int pid_list_midx;

	/* Plugin-defined per-job metric indices (copied into each job base) */
	int *per_job_metric_midx;
	int  num_per_job_metrics;

	/* PID record definition */
	ldms_record_t pid_rec_def;
	int max_pids_per_job;
	size_t pid_list_heap_sz;

	/* Plugin callbacks and context */
	struct per_job_plugin_callbacks_s callbacks;
	void *plugin_ctxt;

	/* Tenant integration */
	ldmsd_tenant_def_t tenant_def;
	ldmsd_tenant_event_consumer_t event_consumer;
	int owns_tenant;  /* True if we created internal tenant */

	/* Job tracking */
	struct rbt job_rbt;  /* job_id → per_job_base_t */
	pthread_mutex_t job_mutex;

	/* Cached field indices from tenant/jobmgr */
	int job_id_field_idx;
	int binding_key_field_idx;
	int task_pid_field_idx;
};

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

static int job_id_cmp(void *a, const void *b)
{
	return strcmp((char *)a, (char *)b);
}

static int pid_cmp(void *a, const void *b)
{
	pid_t l = *(pid_t *)a;
	pid_t r = *(pid_t *)b;
	if (l < r) return -1;
	if (l > r) return 1;
	return 0;
}

/* Find job base by job_id */
static per_job_base_t find_job_base(per_job_sampler_t sampler, const char *job_id)
{
	struct rbn *rbn = rbt_find(&sampler->job_rbt, job_id);
	if (!rbn)
		return NULL;
	return container_of(rbn, struct per_job_base_s, rbn);
}

/* Create internal tenant for self-contained/custom-key modes */
static ldmsd_tenant_def_t create_internal_tenant(per_job_sampler_t sampler,
						  const char *key_field)
{
	struct ldmsd_str_list attr_list = TAILQ_HEAD_INITIALIZER(attr_list);
	struct ldmsd_str_ent *attr;
	ldmsd_tenant_def_t tdef;

	/* Build attribute list: job_id, task_pid, [key_field if different] */
	const char *attrs[] = {"job_id", "task_pid", NULL};
	for (int i = 0; attrs[i]; i++) {
		attr = malloc(sizeof(*attr));
		if (!attr) {
			/* Clean up */
			while ((attr = TAILQ_FIRST(&attr_list))) {
				TAILQ_REMOVE(&attr_list, attr, entry);
				free((char *)attr->str);
				free(attr);
			}
			return NULL;
		}
		attr->str = strdup(attrs[i]);
		TAILQ_INSERT_TAIL(&attr_list, attr, entry);
	}

	/* Add key field if it's not job_id */
	if (key_field && strcmp(key_field, "job_id") != 0) {
		attr = malloc(sizeof(*attr));
		if (!attr)
			goto err;
		attr->str = strdup(key_field);
		TAILQ_INSERT_TAIL(&attr_list, attr, entry);
	}

	/* Create tenant definition */
	tdef = ldmsd_tenant_def_new(sampler->instance ? sampler->instance : sampler->schema_name,
				     sampler->producer,
				     &attr_list,
				     key_field ? key_field : "job_id",
				     getuid(), getgid(), 0660);

	/* Clean up attribute list */
	while ((attr = TAILQ_FIRST(&attr_list))) {
		TAILQ_REMOVE(&attr_list, attr, entry);
		free((char *)attr->str);
		free(attr);
	}

	return tdef;

err:
	while ((attr = TAILQ_FIRST(&attr_list))) {
		TAILQ_REMOVE(&attr_list, attr, entry);
		free((char *)attr->str);
		free(attr);
	}
	return NULL;
}

/* Create schema with per-job metrics and PID list */
static int create_schema(per_job_sampler_t sampler)
{
	int rc;
	struct per_job_metric_desc_s *per_job_metrics = NULL;
	int num_per_job_metrics = 0;

	sampler->schema = ldms_schema_new(sampler->schema_name);
	if (!sampler->schema)
		return ENOMEM;

	/* Add standard per-job metrics */
	sampler->job_id_midx = ldms_schema_metric_array_add(sampler->schema,
							     "job_id",
							     LDMS_V_CHAR_ARRAY,
							     LDMSD_JOBMGR_JOB_ID_LEN);
	if (sampler->job_id_midx < 0)
		return -sampler->job_id_midx;

	sampler->binding_key_midx = ldms_schema_metric_array_add(sampler->schema,
								  "binding_key",
								  LDMS_V_CHAR_ARRAY,
								  BINDING_KEY_MAX_LEN);
	if (sampler->binding_key_midx < 0)
		return -sampler->binding_key_midx;

	/* Ask plugin for per-job metrics */
	if (sampler->callbacks.define_per_job_metrics) {
		rc = sampler->callbacks.define_per_job_metrics(&per_job_metrics,
							       &num_per_job_metrics,
							       sampler->plugin_ctxt);
		if (rc)
			return rc;

		/* Add plugin's per-job metrics */
		if (num_per_job_metrics > 0) {
			sampler->per_job_metric_midx = calloc(num_per_job_metrics, sizeof(int));
			if (!sampler->per_job_metric_midx)
				return ENOMEM;

			for (int i = 0; i < num_per_job_metrics; i++) {
				if (ldms_type_is_array(per_job_metrics[i].type)) {
					sampler->per_job_metric_midx[i] =
						ldms_schema_metric_array_add(sampler->schema,
									      per_job_metrics[i].name,
									      per_job_metrics[i].type,
									      per_job_metrics[i].count);
				} else {
					sampler->per_job_metric_midx[i] =
						ldms_schema_metric_add_with_unit(sampler->schema,
										 per_job_metrics[i].name,
										 per_job_metrics[i].unit,
										 per_job_metrics[i].type);
				}
				if (sampler->per_job_metric_midx[i] < 0)
					return -sampler->per_job_metric_midx[i];
			}
			sampler->num_per_job_metrics = num_per_job_metrics;
		}
		free(per_job_metrics);
	}

	/* Add PID record definition and list only if plugin requested it */
	if (sampler->pid_rec_def) {
		sampler->pid_recdef_midx = ldms_schema_record_add(sampler->schema,
								   sampler->pid_rec_def);
		if (sampler->pid_recdef_midx < 0)
			return -sampler->pid_recdef_midx;

		sampler->pid_list_heap_sz = sampler->max_pids_per_job *
					    ldms_record_heap_size_get(sampler->pid_rec_def);

		sampler->pid_list_midx = ldms_schema_metric_list_add(sampler->schema,
								      "pid_list",
								      "",
								      sampler->pid_list_heap_sz);
		if (sampler->pid_list_midx < 0)
			return -sampler->pid_list_midx;
	} else {
		sampler->pid_recdef_midx = -1;
		sampler->pid_list_midx   = -1;
	}

	return 0;
}

/* Create a per-job base instance */
static per_job_base_t create_job_base(per_job_sampler_t sampler,
				       const char *job_id,
				       const char *binding_key)
{
	per_job_base_t job_base;
	int rc;

	job_base = calloc(1, sizeof(*job_base));
	if (!job_base)
		return NULL;

	snprintf(job_base->job_id, sizeof(job_base->job_id), "%s", job_id);
	snprintf(job_base->binding_key, sizeof(job_base->binding_key), "%s", binding_key);
	snprintf(job_base->set_name, sizeof(job_base->set_name), "%s/%s",
		 sampler->producer, job_id);

	job_base->sampler = sampler;
	rbt_init(&job_base->pid_rbt, pid_cmp);

	/* Copy cached indices from sampler */
	job_base->job_id_midx = sampler->job_id_midx;
	job_base->binding_key_midx = sampler->binding_key_midx;
	job_base->pid_list_midx = sampler->pid_list_midx;
	job_base->pid_recdef_midx = sampler->pid_recdef_midx;
	job_base->num_per_job_metrics = sampler->num_per_job_metrics;

	if (job_base->num_per_job_metrics > 0) {
		job_base->per_job_metric_midx = malloc(job_base->num_per_job_metrics *
						       sizeof(int));
		if (!job_base->per_job_metric_midx) {
			free(job_base);
			return NULL;
		}
		memcpy(job_base->per_job_metric_midx,
		       sampler->per_job_metric_midx,
		       job_base->num_per_job_metrics * sizeof(int));
	}

	/* Create LDMS set */
	job_base->set = ldms_set_new(job_base->set_name, sampler->schema);
	if (!job_base->set) {
		free(job_base->per_job_metric_midx);
		free(job_base);
		return NULL;
	}

	/* Initialize set metrics */
	ldms_metric_array_set_str(job_base->set, job_base->job_id_midx, job_id);
	ldms_metric_array_set_str(job_base->set, job_base->binding_key_midx, binding_key);
	ldms_set_producer_name_set(job_base->set, sampler->producer);

	/* Publish and register */
	ldms_set_publish(job_base->set);
	ldmsd_set_register(job_base->set, ldmsd_plug_cfg_name_get(sampler->pi));

	/* Call plugin's job_init if provided */
	if (sampler->callbacks.job_init) {
		rc = sampler->callbacks.job_init(job_base, &job_base->job_ctxt);
		if (rc) {
			ldms_set_delete(job_base->set);
			free(job_base->per_job_metric_midx);
			free(job_base);
			return NULL;
		}
	}

	return job_base;
}

/* Destroy a per-job base instance */
static void destroy_job_base(per_job_base_t job_base)
{
	struct rbn *rbn;
	struct pid_entry_s *pent;

	/* Call plugin's cleanup */
	if (job_base->sampler->callbacks.job_cleanup) {
		job_base->sampler->callbacks.job_cleanup(job_base, job_base->job_ctxt);
	}

	/* Clean up PIDs */
	while ((rbn = rbt_min(&job_base->pid_rbt))) {
		rbt_del(&job_base->pid_rbt, rbn);
		pent = container_of(rbn, struct pid_entry_s, rbn);
		free(pent);
	}

	/* Clean up set */
	if (job_base->set) {
		ldmsd_set_deregister(ldms_set_instance_name_get(job_base->set),
				     ldmsd_plug_cfg_name_get(job_base->sampler->pi));
		ldms_set_unpublish(job_base->set);
		ldms_set_delete(job_base->set);
	}

	free(job_base->per_job_metric_midx);
	free(job_base);
}

/* Add a PID to a job */
static int add_pid_to_job(per_job_base_t job_base, pid_t pid)
{
	struct pid_entry_s *pent;
	ldms_mval_t list_head, pid_rec;
	int rc;

	/* Check if PID already exists */
	if (rbt_find(&job_base->pid_rbt, &pid))
		return 0;  /* Already exists */

	/* Allocate PID entry */
	pent = calloc(1, sizeof(*pent));
	if (!pent)
		return ENOMEM;

	pent->pid = pid;
	rbn_init(&pent->rbn, &pent->pid);

	/* Allocate PID record in the list */
	list_head = ldms_metric_get(job_base->set, job_base->pid_list_midx);
	if (!list_head) {
		free(pent);
		return EINVAL;
	}

	ldms_transaction_begin(job_base->set);

	pid_rec = ldms_record_alloc(job_base->set, job_base->pid_recdef_midx);
	if (!pid_rec) {
		ldms_transaction_end(job_base->set);
		free(pent);
		return ENOMEM;
	}

	rc = ldms_list_append_record(job_base->set, list_head, pid_rec);
	if (rc) {
		ldms_transaction_end(job_base->set);
		free(pent);
		return rc;
	}

	pent->pid_rec = pid_rec;
	rbt_ins(&job_base->pid_rbt, &pent->rbn);
	job_base->num_pids++;

	/* Call plugin's pid_added callback */
	if (job_base->sampler->callbacks.pid_added) {
		job_base->sampler->callbacks.pid_added(job_base, pid, pid_rec,
						       job_base->job_ctxt);
	}

	ldms_transaction_end(job_base->set);

	return 0;
}

/* Remove a PID from a job */
static void remove_pid_from_job(per_job_base_t job_base, pid_t pid)
{
	struct rbn *rbn;
	struct pid_entry_s *pent;
	ldms_mval_t list_head;

	rbn = rbt_find(&job_base->pid_rbt, &pid);
	if (!rbn)
		return;  /* Not found */

	pent = container_of(rbn, struct pid_entry_s, rbn);

	/* Call plugin's pid_removed callback */
	if (job_base->sampler->callbacks.pid_removed) {
		job_base->sampler->callbacks.pid_removed(job_base, pid,
							 job_base->job_ctxt);
	}

	/* Remove from list and RBT */
	ldms_transaction_begin(job_base->set);
	list_head = ldms_metric_get(job_base->set, job_base->pid_list_midx);
	if (list_head) {
		ldms_list_remove_item(job_base->set, list_head, pent->pid_rec);
	}
	ldms_transaction_end(job_base->set);

	rbt_del(&job_base->pid_rbt, rbn);
	free(pent);
	job_base->num_pids--;
}

/* Extract fields from jobmgr query record */
static void extract_fields(per_job_sampler_t sampler, ldms_mval_t qrec,
			   char *job_id, char *binding_key, pid_t *pid)
{
	ldms_mval_t mv;

	if (job_id) {
		if (sampler->job_id_field_idx >= 0) {
			mv = ldms_record_metric_get(qrec, sampler->job_id_field_idx);
			snprintf(job_id, LDMSD_JOBMGR_JOB_ID_LEN, "%s", mv->a_char);
		} else {
			job_id[0] = '\0';
		}
	}

	if (binding_key) {
		if (sampler->binding_key_field_idx >= 0) {
			mv = ldms_record_metric_get(qrec, sampler->binding_key_field_idx);
			snprintf(binding_key, BINDING_KEY_MAX_LEN, "%s", mv->a_char);
		} else if (job_id) {
			snprintf(binding_key, BINDING_KEY_MAX_LEN, "%s", job_id);
		} else {
			binding_key[0] = '\0';
		}
	}

	if (pid) {
		if (sampler->task_pid_field_idx >= 0) {
			mv = ldms_record_metric_get(qrec, sampler->task_pid_field_idx);
			*pid = mv->v_u64;
		} else {
			*pid = 0;
		}
	}
}

/* Event handler - called by tenant infrastructure */
static int per_job_sampler_event_handler(ldmsd_tenant_def_t tdef,
					  ldmsd_jobmgr_query_event_type_t event_type,
					  ldms_mval_t qrec,
					  void *cb_arg)
{
	per_job_sampler_t sampler = cb_arg;
	char job_id[LDMSD_JOBMGR_JOB_ID_LEN];
	char binding_key[BINDING_KEY_MAX_LEN];
	pid_t pid;
	per_job_base_t job_base;

	switch (event_type) {
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_START:
		extract_fields(sampler, qrec, job_id, binding_key, &pid);

		pthread_mutex_lock(&sampler->job_mutex);

		/* Find or create job base */
		job_base = find_job_base(sampler, job_id);
		if (!job_base) {
			job_base = create_job_base(sampler, job_id, binding_key);
			if (!job_base) {
				pthread_mutex_unlock(&sampler->job_mutex);
				ovis_log(sampler->log, OVIS_LERROR,
					 "Failed to create job base for %s\n", job_id);
				return ENOMEM;
			}
			rbn_init(&job_base->rbn, job_base->job_id);
			rbt_ins(&sampler->job_rbt, &job_base->rbn);
		}

		pthread_mutex_unlock(&sampler->job_mutex);

		/* Add PID to job */
		add_pid_to_job(job_base, pid);
		break;

	case LDMSD_JOBMGR_QUERY_EVENT_TASK_END:
		extract_fields(sampler, qrec, job_id, NULL, &pid);

		pthread_mutex_lock(&sampler->job_mutex);
		job_base = find_job_base(sampler, job_id);
		pthread_mutex_unlock(&sampler->job_mutex);

		if (job_base) {
			remove_pid_from_job(job_base, pid);
		}
		break;

	case LDMSD_JOBMGR_QUERY_EVENT_JOB_END:
		extract_fields(sampler, qrec, job_id, NULL, NULL);

		pthread_mutex_lock(&sampler->job_mutex);
		job_base = find_job_base(sampler, job_id);
		if (job_base) {
			rbt_del(&sampler->job_rbt, &job_base->rbn);
		}
		pthread_mutex_unlock(&sampler->job_mutex);

		if (job_base) {
			destroy_job_base(job_base);
		}
		break;

	default:
		/* Ignore other events */
		break;
	}

	return 0;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

per_job_sampler_t per_job_sampler_create(
	ldmsd_plug_handle_t pi,
	const char *producer,
	const char *schema_name,
	const char *instance,
	const char *tenant_name,
	const char *key_field,
	ldms_record_t pid_rec_def,
	int max_pids,
	struct per_job_plugin_callbacks_s *callbacks,
	void *plugin_ctxt)
{
	per_job_sampler_t sampler;
	int rc;
	ldmsd_jobmgr_query_t jquery;

	sampler = calloc(1, sizeof(*sampler));
	if (!sampler)
		return NULL;

	sampler->pi = pi;
	sampler->log = ldmsd_plug_log_get(pi);
	sampler->callbacks = *callbacks;
	sampler->plugin_ctxt = plugin_ctxt;
	sampler->producer = strdup(producer);
	sampler->schema_name = strdup(schema_name);
	if (instance)
		sampler->instance = strdup(instance);
	sampler->pid_rec_def = pid_rec_def;
	sampler->max_pids_per_job = max_pids;

	pthread_mutex_init(&sampler->job_mutex, NULL);
	rbt_init(&sampler->job_rbt, job_id_cmp);

	/* Determine mode and get/create tenant */
	if (tenant_name) {
		/* Shared tenant mode */
		sampler->tenant_def = ldmsd_tenant_def_find(tenant_name);
		if (!sampler->tenant_def) {
			ovis_log(sampler->log, OVIS_LERROR,
				 "Tenant '%s' not found\n", tenant_name);
			goto err;
		}
		sampler->owns_tenant = 0;
	} else {
		/* Self-contained or custom key mode */
		sampler->tenant_def = create_internal_tenant(sampler, key_field);
		if (!sampler->tenant_def) {
			ovis_log(sampler->log, OVIS_LERROR,
				 "Failed to create internal tenant\n");
			goto err;
		}
		sampler->owns_tenant = 1;
	}

	/* Get jobmgr query from tenant */
	jquery = ldmsd_tenant_def_get_jquery(sampler->tenant_def);
	if (!jquery) {
		ovis_log(sampler->log, OVIS_LERROR,
			 "Failed to get jobmgr query from tenant\n");
		goto err;
	}

	/* Cache field indices */
	sampler->job_id_field_idx = ldmsd_jobmgr_query_field_index(jquery, "job_id");
	sampler->task_pid_field_idx = ldmsd_jobmgr_query_field_index(jquery, "task_pid");

	/* Get binding key field index */
	if (key_field && strcmp(key_field, "job_id") != 0) {
		sampler->binding_key_field_idx = ldmsd_jobmgr_query_field_index(jquery, key_field);
		if (sampler->binding_key_field_idx < 0) {
			ovis_log(sampler->log, OVIS_LERROR,
				 "Binding key field '%s' not found\n", key_field);
			goto err;
		}
	} else {
		sampler->binding_key_field_idx = sampler->job_id_field_idx;
	}

	/* Create schema */
	rc = create_schema(sampler);
	if (rc) {
		ovis_log(sampler->log, OVIS_LERROR,
			 "Failed to create schema: %d\n", rc);
		goto err;
	}

	/* Register for tenant events */
	sampler->event_consumer = ldmsd_tenant_event_register(sampler->tenant_def,
							       per_job_sampler_event_handler,
							       sampler);
	if (!sampler->event_consumer) {
		ovis_log(sampler->log, OVIS_LERROR,
			 "Failed to register for tenant events\n");
		goto err;
	}

	return sampler;

err:
	per_job_sampler_destroy(sampler);
	return NULL;
}

void per_job_sampler_destroy(per_job_sampler_t sampler)
{
	struct rbn *rbn;
	per_job_base_t job_base;

	if (!sampler)
		return;

	/* Unregister from tenant events */
	if (sampler->event_consumer) {
		ldmsd_tenant_event_unregister(sampler->event_consumer);
	}

	/* Clean up all jobs */
	while ((rbn = rbt_min(&sampler->job_rbt))) {
		rbt_del(&sampler->job_rbt, rbn);
		job_base = container_of(rbn, struct per_job_base_s, rbn);
		destroy_job_base(job_base);
	}

	/* Clean up tenant if we own it */
	if (sampler->owns_tenant && sampler->tenant_def) {
		ldmsd_tenant_def_del(sampler->tenant_def);
	}

	/* Clean up schema */
	if (sampler->schema) {
		ldms_schema_delete(sampler->schema);
	}

	free(sampler->per_job_metric_midx);
	free(sampler->producer);
	free(sampler->schema_name);
	free(sampler->instance);
	pthread_mutex_destroy(&sampler->job_mutex);
	free(sampler);
}

int per_job_sampler_sample(per_job_sampler_t sampler)
{
	struct rbn *rbn;
	per_job_base_t job_base;
	int rc = 0;

	pthread_mutex_lock(&sampler->job_mutex);

	RBT_FOREACH(rbn, &sampler->job_rbt) {
		job_base = container_of(rbn, struct per_job_base_s, rbn);

		/* Begin transaction */
		ldms_transaction_begin(job_base->set);

		/* Call plugin's sample_job callback */
		if (sampler->callbacks.sample_job) {
			rc = sampler->callbacks.sample_job(job_base, job_base->job_ctxt);
		}

		/* End transaction */
		ldms_transaction_end(job_base->set);

		if (rc) {
			ovis_log(sampler->log, OVIS_LERROR,
				 "Failed to sample job %s: %d\n", job_base->job_id, rc);
		}
	}

	pthread_mutex_unlock(&sampler->job_mutex);

	return 0;
}

/* Per-job base APIs */

void *per_job_base_get_plugin_ctxt(per_job_base_t job_base)
{
	return job_base->sampler->plugin_ctxt;
}

ldms_set_t per_job_base_get_set(per_job_base_t job_base)
{
	return job_base->set;
}

ldms_schema_t per_job_base_get_schema(per_job_base_t job_base)
{
	return job_base->sampler->schema;
}

const char *per_job_base_get_job_id(per_job_base_t job_base)
{
	return job_base->job_id;
}

const char *per_job_base_get_binding_key(per_job_base_t job_base)
{
	return job_base->binding_key;
}

int per_job_base_get_job_metric_index(per_job_base_t job_base, const char *metric_name)
{
	return ldms_metric_by_name(job_base->set, metric_name);
}

int per_job_base_get_pid_metric_index(per_job_base_t job_base, const char *metric_name)
{
	return ldms_record_index_get(job_base->sampler->pid_rec_def, metric_name);
}

void per_job_base_set_u64(per_job_base_t job_base, int metric_idx, uint64_t value)
{
	ldms_metric_set_u64(job_base->set, metric_idx, value);
}

void per_job_base_set_u32(per_job_base_t job_base, int metric_idx, uint32_t value)
{
	ldms_metric_set_u32(job_base->set, metric_idx, value);
}

void per_job_base_set_s64(per_job_base_t job_base, int metric_idx, int64_t value)
{
	ldms_metric_set_s64(job_base->set, metric_idx, value);
}

void per_job_base_set_d64(per_job_base_t job_base, int metric_idx, double value)
{
	ldms_metric_set_double(job_base->set, metric_idx, value);
}

ldms_mval_t per_job_base_get_metric(per_job_base_t job_base, int metric_idx)
{
	return ldms_metric_get(job_base->set, metric_idx);
}

int per_job_base_foreach_pid(per_job_base_t job_base,
			      per_job_pid_iterator_fn_t callback,
			      void *arg)
{
	struct rbn *rbn;
	struct pid_entry_s *pent;
	int rc = 0;

	RBT_FOREACH(rbn, &job_base->pid_rbt) {
		pent = container_of(rbn, struct pid_entry_s, rbn);
		rc = callback(job_base, pent->pid, pent->pid_rec, arg);
		if (rc)
			break;
	}

	return rc;
}

ldms_mval_t per_job_base_get_pid_record(per_job_base_t job_base, pid_t pid)
{
	struct rbn *rbn = rbt_find(&job_base->pid_rbt, &pid);
	if (!rbn)
		return NULL;

	struct pid_entry_s *pent = container_of(rbn, struct pid_entry_s, rbn);
	return pent->pid_rec;
}

int per_job_base_get_pid_count(per_job_base_t job_base)
{
	return job_base->num_pids;
}

int per_job_sampler_add_list(per_job_sampler_t sampler,
			      const char *list_name,
			      ldms_record_t rec_def,
			      int max_entries)
{
	int rec_mid, list_mid;
	size_t heap_sz;

	if (!sampler->schema) {
		ovis_log(sampler->log, OVIS_LERROR,
			 "Schema not created yet\n");
		return -EINVAL;
	}

	/* Add record definition to schema */
	rec_mid = ldms_schema_record_add(sampler->schema, rec_def);
	if (rec_mid < 0)
		return rec_mid;

	/* Add list metric */
	heap_sz = max_entries * ldms_record_heap_size_get(rec_def);
	list_mid = ldms_schema_metric_list_add(sampler->schema, list_name, "", heap_sz);
	if (list_mid < 0)
		return list_mid;

	return list_mid;
}
