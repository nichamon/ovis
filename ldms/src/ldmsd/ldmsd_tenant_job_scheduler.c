#include "ldms.h"
#include "ldmsd_jobmgr.h"
#include "ldmsd_tenant.h"

struct job_scheduler_ctxt {
	int mid; /* Metric id in the job set */
	int rec_id; /* Record id in the job set when the metric is in a record */
};

static ldms_metric_template_t __find_job_metric(const char *s)
{
	ldms_metric_template_t job_metric;

	job_metric = common_jobset_metrics;
	while (job_metric->name) {
		if (0 == strcmp(s, job_metric->name)) {
			return job_metric;
		}
		job_metric++;
	}

	job_metric = common_task_rec_metrics;
	while (job_metric->name) {
		if (0 == strcmp(s, job_metric->name)) {
			return job_metric;
		}
		job_metric++;
	}
	return NULL;
}

static int job_scheduler_can_provide(const char *value)
{
	/* TODO: implement this */
	if (__find_job_metric(value))
		return 1;
	else
		return 0;
}

static int job_scheduler_init_tenant_metric(const char *value,
					    struct ldmsd_tenant_metric_s *tmet)
{
	/* TODO: implement this */
	ldms_metric_template_t job_met;

	job_met = __find_job_metric(value);
	if (!job_met) {
		return ENOENT;
	}

	memcpy(&tmet->mtempl, job_met, sizeof(tmet->mtempl));
	tmet->mtempl.name = strdup(job_met->name);
	if (!tmet->mtempl.name) {
		ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
		return ENOMEM;
	}

	tmet->mtempl.unit = strdup((job_met->unit?job_met->unit:""));
	if (!tmet->mtempl.unit) {
		ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
		return ENOMEM;
	}

	tmet->src_data = NULL; /* TODO: Handle this later */
	return 0;
}

static void job_scheduler_cleanup(void *src_data)
{
	/* TODO: implement this */
	assert(0 == ENOSYS);
}

static int __num_rows_get(struct ldmsd_tenant_data_s *tsrc)
{
	/* Calculate the number of records */
	/* TODO: complete this */
	return 2;
}

/* TODO: Update this when receive the updated job_scehduler APIs from Narate */
static int job_scheduler_get_tenant_values(struct ldmsd_tenant_data_s *tdata,
					   struct ldmsd_tenant_row_table_s *vtbl)
{
	int i, j, k, rc;
	int num_rows;
	ldms_mval_t v;
	struct ldmsd_tenant_metric_s *tmet;

	/* TODO: delme for testing only*/
	static int idx = 0;


	/* Determine the number of combinations */
	num_rows = __num_rows_get(tdata);

	if (num_rows > vtbl->allocated_rows) {
		rc = ldmsd_tenant_row_table_resize(vtbl, num_rows);
		if (rc)
			return rc;
	}

	for (i = 0; i < num_rows; i++) {
		j = 0;
		TAILQ_FOREACH(tmet, &tdata->mlist, ent) {
			v = LDMSD_TENANT_ROWTBL_CELL_PTR(vtbl, i, j);
			if (tmet->mtempl.type == LDMS_V_CHAR_ARRAY) {
				for (k = 0; k < tmet->mtempl.len; k++) {
					v->a_char[k] = 'a' + idx + i;
				}
			} else if (tmet->mtempl.type == LDMS_V_U32) {
				v->v_u32 = (uint32_t)(idx + i);
			} else {
				assert("Unexpected metric value type");
			}
			j++;
		}
	}
	vtbl->active_rows = num_rows;
	return 0;
}

struct ldmsd_tenant_source_s tenant_job_scheduler_source = {
	.type = LDMSD_TENANT_SRC_JOB_SCHEDULER,
	.name = "tenant_src_job_scheduler",
	.can_provide = job_scheduler_can_provide,
	.init_tenant_metric = job_scheduler_init_tenant_metric,
	.cleanup = job_scheduler_cleanup,
	.get_tenant_values = job_scheduler_get_tenant_values,
};