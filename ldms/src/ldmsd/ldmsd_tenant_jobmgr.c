#include "ldms.h"
#include "ldmsd_jobmgr.h"
#include "ldmsd_tenant.h"

typedef struct tenant_jobmgr_ctxt_s {
	ldmsd_jobmgr_query_t query_handle;
} *tenant_jobmgr_ctxt_t;

ovis_log_t tn_jobmgr_log = NULL; /* TODO: register the log handle */

#define TENANT_JOBMGR_INIT_NUM_ROWS 4

static int jobmgr_can_provide(const char *value)
{
	ldms_metric_template_t m;

	m = (ldms_metric_template_t)ldmsd_jobmgr_metric_lookup(value);
	if (m) {
		return 1;
	}
	return 0;
}

static int jobmgr_init_metric(const char *name, ldmsd_tenant_metric_t tmet)
{
	ldms_metric_template_t m =
		(ldms_metric_template_t)ldmsd_jobmgr_metric_lookup(name);
	if (!m)
		memcpy(&tmet->mtempl, m, sizeof(tmet->mtempl));
	tmet->mtempl.name = strdup(m->name);
	if (!tmet->mtempl.name) {
		ovis_log(tn_jobmgr_log, OVIS_LCRIT,
			 "Memory allocation failure.\n");
		return ENOMEM;
	}
	tmet->mtempl.unit = strdup((m->unit ? m->unit : ""));
	if (!tmet->mtempl.unit) {
		ovis_log(tn_jobmgr_log, OVIS_LCRIT,
			 "Memory allocation failure.\n");
		free((char *)tmet->mtempl.name);
		return ENOMEM;
	}
	return 0;
}

static void jobmgr_cleanup_metric(ldmsd_tenant_metric_t tmet)
{
	free((char *)tmet->mtempl.name);
	free((char *)tmet->mtempl.unit);
}

static int jobmgr_init_ctxt(ldmsd_tenant_data_t tdata)
{
	int i, rc;
	ldmsd_tenant_metric_t tmet;
	const char *metrics[tdata->mcount];
	tenant_jobmgr_ctxt_t ctxt;

	tdata->init_num_rows = TENANT_JOBMGR_INIT_NUM_ROWS;

	ctxt = malloc(sizeof(*ctxt));
	if (!ctxt) {
		ovis_log(tn_jobmgr_log, OVIS_LCRIT,
			 "Memory allocation failure.\n");
		return ENOMEM;
	}

	for (i = 0, tmet = TAILQ_FIRST(&tdata->mlist);
	     i < tdata->mcount && tmet; i++, tmet = TAILQ_NEXT(tmet, ent)) {
		metrics[i] = tmet->mtempl.name;
	}

	ctxt->query_handle = ldmsd_jobmgr_query_new(tdata->mcount, metrics);
	if (!ctxt->query_handle) {
		rc = errno;
		ovis_log(tn_jobmgr_log, OVIS_LWARN,
			 "Cannot query the job manager metrics. Error: %s\n",
			 strerror(rc));
		free(ctxt);
		return rc;
	}

	assert(ctxt->query_handle->qres_size == tdata->rlist_meta.row_size);

	tdata->src_ctxt = (void *)ctxt;
	return 0;
}

static void jobmgr_cleanup_ctxt(void *_ctxt)
{
	struct tenant_jobmgr_ctxt_s *ctxt;
	ctxt = (struct tenant_jobmgr_ctxt_s *)_ctxt;
	ldmsd_jobmgr_query_free(ctxt->query_handle);
	free(ctxt);
}

static int __filled_row(ldmsd_tenant_data_t tdata,
			ldmsd_tenant_row_list_t rlist)
{
	int i;
	ldmsd_tenant_metric_t tmet;
	ldms_mval_t dst;
	ldmsd_tenant_row_t row = TAILQ_FIRST(&rlist->rows);

	for (i = 0, tmet = TAILQ_FIRST(&tdata->mlist); tmet;
	     i++, tmet = TAILQ_NEXT(tmet, ent)) {
		dst = LDMSD_TENANT_ROW_CELL_PTR_AT_OFFSET(
			row, tdata->rlist_meta.col_offsets[i]);
		ldmsd_tenant_mval_missing_val(dst, tmet->mtempl.type,
					      tmet->mtempl.len);
	}
	rlist->active_rows++;
	return 0;
}

static int jobmgr_get_values(ldmsd_tenant_data_t tdata,
			     ldmsd_tenant_row_list_t rlist, int *is_missing)
{
	struct tenant_jobmgr_ctxt_s *ctxt;
	ldmsd_jobmgr_qres_list_t qres_list;
	ldmsd_jobmgr_qres_t qres;
	ldmsd_tenant_row_t row;

	*is_missing = 0;
	rlist->active_rows = 0;
	ctxt = (struct tenant_jobmgr_ctxt_s *)tdata->src_ctxt;
	qres_list = ldmsd_jobmgr_query_ls(ctxt->query_handle);
	if (!qres_list) {
		__filled_row(tdata, rlist);
		*is_missing = 1;
		return 0;
	}

	if (rlist->allocated_rows < qres_list->len) {
		rlist = ldmsd_tenant_row_list_resize(rlist, qres_list->len);
		if (!rlist)
			return ENOMEM;
	}

	qres = TAILQ_FIRST(&qres_list->tailq);
	row = TAILQ_FIRST(&rlist->rows);
	while (qres && row) {
		memcpy(row->data, qres->data, tdata->rlist_meta.row_size);
		qres = TAILQ_NEXT(qres, entry);
		row = TAILQ_NEXT(row, ent);
		rlist->active_rows++;
	}

	ldmsd_jobmgr_qres_list_free(qres_list);
	return 0;
}

struct ldmsd_tenant_source_s tenant_jobmgr_source = {
	.type = LDMSD_TENANT_SRC_JOBMGR,
	.name = "tenant_src_jobmgr",
	.can_provide = jobmgr_can_provide,
	.init_tenant_metric = jobmgr_init_metric,
	.cleanup_tenant_metric = jobmgr_cleanup_metric,
	.init_source_ctxt = jobmgr_init_ctxt,
	.cleanup_source_ctxt = jobmgr_cleanup_ctxt,
	.get_tenant_values = jobmgr_get_values,
};
