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

typedef struct tenant_job_scheduler_s {
	const char *schema; /* Job schema */
	ldmsd_tenant_col_map_t col_maps; /* Array of metric ID or record ID in job sets. */
} *tenant_job_scheduler_t;

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

static ldms_metric_template_t __find_common_metric(const char *s)
{
	ldms_metric_template_t m;
	m = __find_job_metric(s);
	if (!m)
		m = __find_job_task_metric(s);
	return m;
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
	ldms_metric_template_t job_met;

	job_met = __find_common_metric(value);
	if (!job_met) {
		goto plugin_metric;
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
		free(tmet->mtempl.name);
		return ENOMEM;
	}
	return 0;

 plugin_metric:
	/*
	 * TODO: See if \c value a jobmgr plugin-specific metric or not
	 */
	return ENOENT;
}

static int init_source_ctxt(struct ldmsd_tenant_data_s *tdata)
{
	int i;
	struct ldmsd_tenant_metric_s *tmet;
	struct tenant_job_scheduler_s *ctxt;

	ctxt = malloc(sizeof(*ctxt));
	if (!ctxt) {
		goto enomem;
	}
	/*
	 * TODO: It would be great if we can access all available jobmgr schema here so we can initialize cols_cfg for each schema
	 */
	ctxt->schema = "jobmgr_slurm";
	ctxt->col_maps = malloc(sizeof(*ctxt->col_maps) * tdata->mcount);
	if (!ctxt->col_maps) {
		free(ctxt);
		goto enomem;
	}

	for (i = 0, tmet = TAILQ_FIRST(&tdata->mlist); (i < tdata->mcount) && tmet;
		i++, tmet = TAILQ_NEXT(tmet, ent)) {

		}




	return 0;
 enomem:
	ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
	return ENOMEM;
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

static int __resolve_column_src(struct ldmsd_tenant_data_s *tsrc, ldms_set_t set)
{
	int i;
	struct tenant_job_scheduler_s *ctxt;
	struct ldmsd_tenant_metric_list *mlist = &tsrc->mlist;
	struct ldmsd_tenant_metric_s *tmet;
	ldmsd_tenant_col_map_t col;
	ldms_mval_t rec;

	if (tsrc->src_ctxt) {
		/* The column maps have been resolved. Nothing to do. */
		return 0;
	}

	ctxt = malloc(sizeof(*ctxt));
	if (!ctxt) {
		goto enomem;
	}
	ctxt->schema = strdup(ldms_set_schema_name_get(set));
	if (!ctxt->schema) {
		free(ctxt);
		goto enomem;
	}

	ctxt->col_maps = malloc(sizeof(*ctxt->col_maps) * tsrc->mcount);
	if (!ctxt->col_maps) {
		free(ctxt->schema);
		free(ctxt);
		goto enomem;
	}

	for (i = 0, tmet = TAILQ_FIRST(&tsrc->mlist); (i < tsrc->mcount) && tmet;
				i < tsrc->mcount, tmet = TAILQ_NEXT(tmet, ent)) {
		col = &ctxt->col_maps[i];
		col->rec_mid = -1;
		col->mid = ldms_metric_by_name(set, tmet->mtempl.name);
		if (col->mid < 0) {
			col->mid = ldms_metric_by_name(set, common_jobset_metrics[LDMSD_JOBSET_MID_TASK_LIST].name);
			assert(col->mid >= 0); /* the list of tasks must exist in any jobmgr sets. */
			rec = ldms_metric_get(set, col->mid);
			col->rec_mid = ldms_record_metric_find(rec, tmet->mtempl.name);
			if (col->rec_mid < 0) {
				/* Can't find the metric. Mark as missing. */
				col->mid = -1;
				continue;
			}
			col->type = LDMS_V_LIST;
			col->ele_type = tmet->mtempl.type;
		} else {
			col->type = tmet->mtempl.type;
		}
	}
	tsrc->src_ctxt = ctxt;
	return 0;
 enomem:
	ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
	return ENOMEM;
}

static int __init_col_iter(ldmsd_tenant_col_iter_t iter, ldmsd_tenant_col_map_t col_map, ldms_set_t set)
{
	ldms_mval_t mval;

	iter->map = col_map;
	iter->set = set;
	iter->exhausted = 0;

	if (col_map->mid < 0) {
		iter->type = LDMSD_TENANT_ITER_T_MISSING;
		return 0;
	}

	mval = ldms_metric_get(set, col_map->mid);

	switch (col_map->type) {
	case LDMS_V_LIST:
		iter->type = LDMSD_TENANT_ITER_T_LIST;
		iter->state.list.curr = ldms_list_first(set, mval, &iter->state.list.type, &iter->state.list.len);
		if (!iter->state.list.curr) {
			iter->exhausted = 1;
		}
		break;
	case LDMS_V_CHAR_ARRAY:
		/* TODO: handle this */
		break;
	case LDMS_V_RECORD_ARRAY:
		iter->type = LDMSD_TENANT_ITER_T_REC_ARRAY;
		iter->state.rec_array.array = mval;
		iter->state.rec_array.max_len = ldms_record_array_len(mval);
		iter->state.rec_array.curr_idx = 0;

		if (iter->state.rec_array.max_len > 0) {
			iter->state.rec_array.curr_rec = ldms_record_array_get_inst(mval, 0);
		} else {
			iter->exhausted = 1;
		}
	default:
		if (ldms_type_is_array(col_map->type)) {

		}
	}

}

static int __jobset_rows(ldms_set_t set, struct ldmsd_tenant_data_s *tdata,
					 struct tenant_job_scheduler_s *ctxt,
					 struct ldmsd_tenant_row_list_s *rlist)
{
	int i;
	struct ldmsd_tenant_col_iter_s iter[tdata->mcount];
	for (i = 0; i < tdata->mcount; i++) {

	}

	return 0;
}

/* TODO: Update this when receive the updated job_scehduler APIs from Narate */
static int job_scheduler_get_tenant_values(struct ldmsd_tenant_data_s *tdata,
					   struct ldmsd_tenant_row_list_s *rlist)
{
	int i, j, k, rc;
	int num_rows;
	ldms_mval_t v;
	struct ldmsd_tenant_metric_s *tmet;
	struct tenant_job_scheduler_s *ctxt = tdata->src_ctxt;
	ldms_set_t job_set = ldmsd_jobset_first();

	if (!job_set) {
		/* No job set fill all column as NA. */
		/* TODO: Complete this. */
		return 0;
	}

	assert(ctxt); /* ctxt was initialized at the init time. */
	if (0 == strcmp(ctxt->schema, ldms_set_schema_name_get(job_set))) {
		assert(0 == ENOSYS); /* TODO: Extend this to support multiple jobmgr existence */
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