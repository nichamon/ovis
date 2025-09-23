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
	return NULL;
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
		free((char*)tmet->mtempl.name);
		return ENOMEM;
	}
	return 0;

 plugin_metric:
	/*
	 * TODO: See if \c value a jobmgr plugin-specific metric or not
	 */
	return ENOENT;
}

static void job_scheduler_cleanup(void *src_data)
{
	/* TODO: implement this */
	assert(0 == ENOSYS);
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

static int __resolve_column_src(struct ldmsd_tenant_data_s *tdata, ldms_set_t set)
{
	int i;
	struct tenant_job_scheduler_s *ctxt;
	struct ldmsd_tenant_metric_s *tmet;
	ldmsd_tenant_col_map_t col;
	ldms_mval_t mval, rec;

	if (tdata->src_ctxt) {
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

	ctxt->col_maps = malloc(sizeof(*ctxt->col_maps) * tdata->mcount);
	if (!ctxt->col_maps) {
		free((char *)ctxt->schema);
		free(ctxt);
		goto enomem;
	}

	for (i = 0, tmet = TAILQ_FIRST(&tdata->mlist); (i < tdata->mcount) && tmet;
				i++, tmet = TAILQ_NEXT(tmet, ent)) {
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
			mval = ldms_metric_get(set, col->mid);
			col->type = tmet->mtempl.type;
			if (LDMS_V_LIST == col->type) {
				col->len = ldms_list_len(set, mval);
			} else if (ldms_type_is_array(col->type)) {
				col->len = ldms_metric_array_get_len(set, col->mid);
			}
		}
	}
	tdata->src_ctxt = ctxt;
	return 0;
 enomem:
	ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
	return ENOMEM;
}

static int __init_col_iters(ldms_set_t set, ldmsd_tenant_col_map_t cols,
			  ldmsd_tenant_col_iter_t iters, int num_cols)
{
	int i;
	ldmsd_tenant_col_map_t col;
	ldmsd_tenant_col_iter_t iter;
	enum ldms_value_type type;
	ldms_mval_t mval;

	for (i = 0; i < num_cols; i++) {
		col = &cols[i];
		iter = &iters[i];

		if (col->mid < 0) {
			iter->type = LDMSD_TENANT_ITER_T_MISSING;
			iter->card = 1;
		}

		type = ldms_metric_type_get(set, col->mid);
		mval = ldms_metric_get(set, col->mid);
		switch (type) {
		case LDMS_V_LIST:
			iter->state.list.curr = ldms_list_first(set, mval, &iter->state.list.type, &iter->state.list.len);
			if (LDMS_V_RECORD_INST != iter->state.list.type) {
				ovis_log(NULL, OVIS_LERROR,
					"List '%s' contains non-record elements of type '%s'\n",
					ldms_metric_name_get(set, col->mid),
					ldms_metric_type_to_str(iter->state.list.type));
				/* TODO: cleanup necessary things and return error */
				return ENOTSUP;
			}
			iter->state.list.curr_idx = 0;
			iter->card = ldms_list_len(set, mval);
			if (0 == iter->card) {
				iter->type = LDMSD_TENANT_ITER_T_MISSING;
				iter->card = 1;
				break;
			}
			break;
		case LDMS_V_CHAR:
		case LDMS_V_S8:
		case LDMS_V_U8:
		case LDMS_V_S16:
		case LDMS_V_U16:
		case LDMS_V_S32:
		case LDMS_V_U32:
		case LDMS_V_S64:
		case LDMS_V_U64:
			iter->type = LDMSD_TENANT_ITER_T_SCALAR;
			iter->card = 1;
			iter->state.scalar.mval = mval;
			break;
		case LDMS_V_CHAR_ARRAY:
			iter->type = LDMSD_TENANT_ITER_T_STRING;
			iter->card = 1;
			iter->state.string.len = ldms_metric_array_get_len(set, col->mid);
			iter->state.string.mval = mval;
			break;
		case LDMS_V_S8_ARRAY:
		case LDMS_V_U8_ARRAY:
		case LDMS_V_S16_ARRAY:
		case LDMS_V_U16_ARRAY:
		case LDMS_V_S32_ARRAY:
		case LDMS_V_U32_ARRAY:
		case LDMS_V_S64_ARRAY:
		case LDMS_V_U64_ARRAY:
			iter->type = LDMSD_TENANT_ITER_T_ARRAY;
			iter->card = ldms_metric_array_get_len(set, col->mid);
			iter->state.array.curr_idx = 0;
		default:
			break;
		}
	}
	return 0;
}

static int __get_value_at_index(ldms_set_t set, ldmsd_tenant_col_map_t col_map,
				ldmsd_tenant_col_iter_t iter, int index,
				ldmsd_tenant_row_t row, size_t col_offset)
{
	int i;
	ldms_mval_t output_mval = LDMSD_TENANT_ROW_PTR_BY_OFFSET(row, col_offset);

	switch (iter->type) {
	case LDMSD_TENANT_ITER_T_SCALAR:
		memcpy(output_mval, iter->state.scalar.mval,
			ldms_metric_value_size_get(col_map->type, col_map->len));
		break;
	case LDMSD_TENANT_ITER_T_STRING:
		memcpy(output_mval, iter->state.string.mval,
			ldms_metric_value_size_get(LDMS_V_CHAR, iter->state.string.len));
		break;
	case LDMSD_TENANT_ITER_T_LIST:
		ldms_mval_t le = ldms_metric_get(set, col_map->mid);
		enum ldms_value_type le_type;
		size_t le_len;
		if (index > iter->state.list.curr_idx) {
			for (i = iter->state.list.curr_idx; i <= index; i++) {
				iter->state.list.curr = ldms_list_next(set, le, &le_type, &le_len);
			}
			iter->state.list.curr_idx = i;
		} else if (index < iter->state.list.curr_idx) {
			iter->state.list.curr = ldms_list_first(set, le, &le_type, &le_len);
			i = 0;
			while (i < index) {
				i++;
				iter->state.list.curr = ldms_list_next(set, le, &le_type, &le_len);
			}
		}
		iter->state.list.type = le_type;
		iter->state.list.len = le_len;
		memcpy(output_mval, iter->state.list.curr, iter->state.list.len);
		break;
	case LDMSD_TENANT_ITER_T_ARRAY:
		assert(ENOTSUP);
		break;
	default:
		assert(ENOTSUP);
		break;
	}
	return 0;
}

static int __jobset_rows(ldms_set_t set, struct ldmsd_tenant_data_s *tdata,
					 struct tenant_job_scheduler_s *ctxt,
					 struct ldmsd_tenant_row_list_s *rlist)
{
	int i, j;
	int total_rows = 1;
	int row_idx;
	struct ldmsd_tenant_col_iter_s iters[tdata->mcount];
	struct ldmsd_tenant_row_s *row;
	int temp, idx;

	/* Get cardinality of each column */
	__init_col_iters(set, ctxt->col_maps, iters, tdata->mcount);

	for (i = 0; i < tdata->mcount; i++) {
		total_rows *= iters[i].card;
	}

	assert(total_rows); /* There must be at least 1 row. */

	/* Generate all Cartesian products */
	row_idx = 0;
	row = TAILQ_FIRST(&rlist->rows);
	while (row_idx < total_rows) {
		if (!row) {
			/* Allocate one more row */
			row = calloc(1, rlist->row_size);
			if (!row) {
				goto enomem;
			}
			row->data = malloc(rlist->row_size);
			if (!row->data) {
				free(row);
				goto enomem;
			}
			TAILQ_INSERT_TAIL(&rlist->rows, row, ent);
			rlist->allocated_rows++;
		}

		temp = row_idx;
		for (j = tdata->mcount - 1; j >= 0; j--) {
			idx = temp % iters[j].card;
			temp = temp / iters[j].card;
			/* Extract value at this index */
			__get_value_at_index(set, &ctxt->col_maps[j], &iters[j],
					     idx, row,
					     tdata->row_list.col_offsets[j]);
		}
		rlist->active_rows++;
		row = TAILQ_NEXT(row, ent);
		row_idx++;
	}
	return 0;
 enomem:
	ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
	return ENOMEM;
}

/* TODO: Update this when receive the updated job_scehduler APIs from Narate */
static int job_scheduler_get_tenant_values(struct ldmsd_tenant_data_s *tdata,
					   struct ldmsd_tenant_row_list_s *rlist)
{
	struct tenant_job_scheduler_s *ctxt = tdata->src_ctxt;
	ldms_set_t job_set = ldmsd_jobset_first();

	if (!job_set) {
		/* No job set fill all column as NA. */
		/* TODO: Complete this. */
		return 0;
	}

	if (!ctxt) {
		__resolve_column_src(tdata, job_set);
		ctxt = tdata->src_ctxt;
	}

	if (0 == strcmp(ctxt->schema, ldms_set_schema_name_get(job_set))) {
		assert(0 == ENOSYS); /* TODO: Extend this to support multiple jobmgr existence */
	}

	rlist->active_rows = 0; /* Reset the number of valid rows */

	while (job_set) {
		__jobset_rows(job_set, tdata, ctxt, &tdata->row_list);
		job_set = ldmsd_jobset_next(job_set);
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