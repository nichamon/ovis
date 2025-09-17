#include "ldms.h"
#include "ldmsd_jobmgr.h"
#include "ldmsd_tenant.h"

/*
 * TODO:
 * Things to think
 * - how can we know what are the available metrics?
 *   - There are common metrics in ldmsd_jobmgr that we can query,
 *     but there is no reliable way to get jobmgr plugin-specific metrics.
 *     We can't wait until job sets are created.
 *
 * Things to decide
 * - How will we handle and retreive job data? I want to avoid caching job data to avoid
 *   doubling caching job data with job sets.
 *
 *   Assumptions:
 *    - There may be multiple tenant definitions
 *    - Most system sets use the same tenant definition
 *    - A few sets use different tenant definitions
 *    - A job metric could be included in multiple tenant definitions
 *    - The interval between two job events could be sub second
 *    - There could be mulitple jobs at a point in time
 *    - At every second, N sets sample() at the same time --> bottleneck if many sets race for a lock to get access to the up-to-date job metric
 *
 *   - Option 1: Query job sets for required metrics at sample time()
 *     - pros: most straight forware approach
 *     - cons: the querying happens once for each set at sample(). That is at every second, we go throug the job set list and create the value tables as many time as the number of sets
 *             high CPU footprint
 *
 *   - Option 2: Subscribe on job event
 *      - For each tenant definition, value table (tdata->vtbl) gets update when a job event is delivered. Use write-lock.
          - At job event delivery time, the code needs to determine how to update the value tables.
 *      - For each sample(), take read-lock of the tenant data part, return the value table without querying job sets
 *
 *   - Option 3: Mix of subscribe and iterate through job list at sample() by updating GN at job event delivery and updating value tables at sample()?
 *      - At sample():
 *        - Check if gn cached in tenant_data == latest gn (changed at job event delivery),
 *          - Iterate through job sets to get update data
 */



struct tenant_job_scheduler_s {
	const char *schema; /* Job schema */
	struct ldmsd_tenant_col_cfg_s *cols_cfg; /* Array of col_cfg_s */
};

struct tenant_job_scheduler_metric_s {
	int src_mid;
	int src_rec_mid;
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
}

static ldms_metric_template_t __find_job_task_metric(const char *s)
{
	ldms_metric_template_t job_metric;
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
	else if (__find_job_task_metric(value))
		return 1;
	else
		return 0;
}

static int job_scheduler_init_tenant_metric(const char *value,
					    struct ldmsd_tenant_metric_s *tmet)
{
	/* TODO: implement this */
	ldms_metric_template_t job_met;
	struct tenant_job_scheduler_s *ctxt;
	int is_job_task = 0; /* 1 if it is a job task metric. */

	job_met = __find_job_metric(value);
	if (!job_met) {
		job_met = __find_job_task_metric(value);
		if (!job_met)
			return ENOENT;
		is_job_task = 1;
	}

	ctxt = malloc(sizeof(*ctxt));
	if (!ctxt) {
		ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
		return ENOMEM;
	}
	ctxt->is_task = is_job_task;

	memcpy(&tmet->mtempl, job_met, sizeof(tmet->mtempl));
	tmet->mtempl.name = strdup(job_met->name);
	if (!tmet->mtempl.name) {
		ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
		free(ctxt);
		return ENOMEM;
	}

	tmet->mtempl.unit = strdup((job_met->unit?job_met->unit:""));
	if (!tmet->mtempl.unit) {
		ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
		free(tmet->mtempl.name);
		free(ctxt);
		return ENOMEM;
	}

	tmet->src_data = ctxt;
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

	ldms_set_t jset;
	int num_rows = 0;
	struct ldmsd_tenant_metric_s *tmet;
	struct tenant_job_scheduler_s *ctxt;

	TAILQ_FOREACH(tmet, &tsrc->mlist, ent) {

	}

	return 2;
}

// /* TODO: Update this when receive the updated job_scehduler APIs from Narate */
// static int job_scheduler_get_tenant_values(struct ldmsd_tenant_data_s *tdata,
// 					   struct ldmsd_tenant_row_table_s *vtbl)
// {
// 	int i, j, k, rc;
// 	int num_rows;
// 	ldms_mval_t v;
// 	struct ldmsd_tenant_metric_s *tmet;

// 	/* TODO: delme for testing only*/
// 	static int idx = -1;
// 	idx++;

// 	/* Determine the number of combinations */
// 	// num_rows = __num_rows_get(tdata);



// 	if (num_rows > vtbl->allocated_rows) {
// 		rc = ldmsd_tenant_row_table_resize(vtbl, num_rows);
// 		if (rc)
// 			return rc;
// 	}

// 	for (i = 0; i < num_rows; i++) {
// 		j = 0;
// 		TAILQ_FOREACH(tmet, &tdata->mlist, ent) {
// 			v = LDMSD_TENANT_ROWTBL_CELL_PTR(vtbl, i, j);
// 			if (tmet->mtempl.type == LDMS_V_CHAR_ARRAY) {
// 				for (k = 0; k < 2; k++) {
// 					v->a_char[k] = 'a' + idx + i;
// 				}
// 			} else if (tmet->mtempl.type == LDMS_V_U32) {
// 				v->v_u32 = (uint32_t)(idx + i);
// 			} else {
// 				assert("Unexpected metric value type");
// 			}
// 			j++;
// 		}
// 	}
// 	vtbl->active_rows = num_rows;
// 	return 0;
// }

static int __resolve_column_src(struct ldmsd_tenant_data_s *tsrc, ldms_set_t set, struct ldmsd_tenant_col_cfg_s *cols_cfg, int num_cols)
{
	int i;
	struct ldmsd_tenant_col_cfg_s *col_cfg;
	struct ldmsd_tenant_metric_list *mlist = &tsrc->mlist;

	for (i = 0; i < num_cols; i++) {
		col_cfg = &cols_cfg[i];
		col_cfg->mid =
	}

	return 0;
}

static void __resolve_col_src_idx(ldms_set_t job_set, struct ldmsd_tenant_metric_list_s *mlist)
{

}

/* TODO: Update this when receive the updated job_scehduler APIs from Narate */
static int job_scheduler_get_tenant_values(struct ldmsd_tenant_data_s *tdata,
					   struct ldmsd_tenant_row_list_s *rlist)
{
	int i, j, k, rc;
	int num_rows;
	ldms_mval_t v;
	struct ldmsd_tenant_metric_s *tmet;

	ldms_set_t job_set;

	for (job_set = ldmsd_jobset_first(); job_set; job_set = ldmsd_jobset_next(job_set)) {

	}

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