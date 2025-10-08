#define _GNU_SOURCE
#include "ldmsd_jobmgr.h"
#include "ldmsd_jobmgr_query.h"

/* in ldmsd_jobmgr.c */
extern ovis_scheduler_t jobmgr_sched;

/* see __init__() function */
struct ldmsd_str_list default_mlist = TAILQ_HEAD_INITIALIZER(default_mlist);
struct ldmsd_str_ent default_marray[] = {
	{"job_id"},
	{"step_id"},
	{"task_id"},
	{"job_start"},
	{"job_end"},
	{"step_start"},
	{"step_end"},
	{"task_start"},
	{"task_end"},
	{0}
};

static TAILQ_HEAD(, ldmsd_jobmgr_query_s) query_tq = TAILQ_HEAD_INITIALIZER(query_tq);
pthread_mutex_t query_tq_mutex = PTHREAD_MUTEX_INITIALIZER;
#define QUERY_TQ_LOCK() pthread_mutex_lock(&query_tq_mutex)
#define QUERY_TQ_UNLOCK() pthread_mutex_unlock(&query_tq_mutex)

static int __post_close(ldmsd_jobmgr_query_t q);

void ldmsd_jobmgr_query_free_cb(void *arg)
{
	ldmsd_jobmgr_query_t q = arg;
	assert(NULL == q->set);
	if (q->recdef)
		ldms_record_delete(q->recdef);
	free(q);
}

ldmsd_jobmgr_query_t
ldmsd_jobmgr_query_new(struct ldmsd_str_list *mlist,
			ldmsd_jobmgr_query_cb_fn_t cb_fn,
			void *cb_arg,
			char **error)
{
	ldmsd_jobmgr_query_t q;
	ldms_record_t recdef;
	struct ldmsd_str_ent *sent;
	const struct ldmsd_jobmgr_query_mdesc_s *mdesc;
	int mid, rc;
	int has_job_id = 0;
	int has_step_id = 0;
	int has_task_id = 0;
	int level = 0;

	if (!mlist)
		mlist = &default_mlist;

	q = calloc(1, sizeof(*q));
	if (!q)
		goto out;

	q->cb_fn = cb_fn;
	q->cb_arg = cb_arg;

	pthread_mutex_init(&q->mutex, NULL);
	ref_init(&q->ref, "init", ldmsd_jobmgr_query_free_cb, q);
	TAILQ_INIT(&q->mgr_tq);

	recdef = ldms_record_create("jobmgr_query_rec");
	if (!recdef)
		goto err_0;
	q->recdef = recdef;

	/* first probe for required IDs */
	TAILQ_FOREACH(sent, mlist, entry) {
		mdesc = ldmsd_jobmgr_metric_lookup(sent->str);
		if (!mdesc) {
			errno = ENOENT;
			if (error) {
				asprintf(error, "Unsupported metric: %s", sent->str);
			}
			goto err_1;
		}
		if (0 == strcmp(mdesc->name, "job_id")) {
			has_job_id = 1;
		} else if (0 == strcmp(mdesc->name, "step_id")) {
			has_step_id = 1;
		} else if (0 == strcmp(mdesc->name, "task_id")) {
			has_task_id = 1;
		}
		level = (level < mdesc->level)?(mdesc->level):(level);
	}

	if (!has_job_id) {
		/* job_id is always required */
		mdesc = ldmsd_jobmgr_metric_lookup("job_id");
		assert(mdesc);
		mid = ldms_record_metric_add(recdef, mdesc->name, mdesc->unit,
					     mdesc->type, mdesc->len);
		if (mid < 0) {
			errno = -mid;
			if (error)
				asprintf(error, "metric %s, add failed: %d", sent->str, -mid);
			goto err_1;
		}
		q->job_id_midx = mid;
	}

	if (level >= 1 && !has_step_id) {
		mdesc = ldmsd_jobmgr_metric_lookup("step_id");
		assert(mdesc);
		mid = ldms_record_metric_add(recdef, mdesc->name, mdesc->unit,
					     mdesc->type, mdesc->len);
		if (mid < 0) {
			errno = -mid;
			if (error)
				asprintf(error, "metric %s, add failed: %d", sent->str, -mid);
			goto err_1;
		}
	}

	if (level >= 2 && !has_task_id) {
		mdesc = ldmsd_jobmgr_metric_lookup("task_id");
		assert(mdesc);
		mid = ldms_record_metric_add(recdef, mdesc->name, mdesc->unit,
					     mdesc->type, mdesc->len);
		if (mid < 0) {
			errno = -mid;
			if (error)
				asprintf(error, "metric %s, add failed: %d", sent->str, -mid);
			goto err_1;
		}
	}

	TAILQ_FOREACH(sent, mlist, entry) {
		mdesc = ldmsd_jobmgr_metric_lookup(sent->str);
		assert(mdesc);
		mid = ldms_record_metric_add(recdef, mdesc->name, mdesc->unit,
					     mdesc->type, mdesc->len);
		if (mid < 0) {
			errno = -mid;
			if (error)
				asprintf(error, "metric %s, add failed: %d", sent->str, -mid);
			goto err_1;
		}
		if (0 == strcmp("job_id", sent->str)) {
			q->job_id_midx = mid;
		}
	}

	/* let plugins know about this query */
	ldmsd_cfg_lock(LDMSD_CFGOBJ_JOBMGR);
	struct ldmsd_jobmgr_query_mgr_s *ent;
	ldmsd_cfgobj_t cfg;
	for (cfg = ldmsd_cfgobj_first(LDMSD_CFGOBJ_JOBMGR); cfg;
	     cfg = ldmsd_cfgobj_next(cfg)) {
		ent = calloc(1, sizeof(*ent));
		if (!ent) {
			ldmsd_cfg_unlock(LDMSD_CFGOBJ_JOBMGR);
			goto err_2;
		}
		ent->mgr = (void*)cfg;
		ldmsd_cfgobj_get(cfg, "qmgr_ent");
		rc = ent->mgr->api->on_query_new(cfg, q, &ent->qmgr_ctxt);
		if (rc) {
			ldmsd_cfg_unlock(LDMSD_CFGOBJ_JOBMGR);
			errno = rc;
			free(ent);
			goto err_2;
		}
		TAILQ_INSERT_TAIL(&q->mgr_tq, ent, entry);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_JOBMGR);

	goto out;

 err_2:
	while ((ent = TAILQ_FIRST(&q->mgr_tq))) {
		TAILQ_REMOVE(&q->mgr_tq, ent, entry);
		ent->mgr->api->on_query_free((void*)ent->mgr, q, ent->qmgr_ctxt);
		free(ent);
	}
 err_1:
	ldms_record_delete(recdef);

 err_0:
	free(q);
	q = NULL;
 out:
	return q;
}

int ldmsd_jobmgr_query_execute(ldmsd_jobmgr_query_t q,
				ldms_set_t set,
				int recdef_midx,
				int list_midx)
{
	enum ldms_value_type t;
	if (q->set)
		return EBUSY; /* already executed */

	t = ldms_metric_type_get(set, recdef_midx);
	if (t != LDMS_V_RECORD_TYPE)
		return EINVAL;

	t = ldms_metric_type_get(set, list_midx);
	if (t != LDMS_V_LIST)
		return EINVAL;

	ldms_set_ref_get(set, "query_execute"); /* put in close */
	q->set = set;
	q->recdef_midx = recdef_midx;
	q->list_midx = list_midx;
	q->rec_value_sz = ldms_record_value_size_get(q->recdef);

	QUERY_TQ_LOCK();
	ref_get(&q->ref, "query_tq");
	TAILQ_INSERT_TAIL(&query_tq, q, entry);
	q->list_ref++;
	QUERY_TQ_UNLOCK();
	return 0;
}

void ldmsd_jobmgr_query_close(ldmsd_jobmgr_query_t q)
{
	if (!q->set) /* has not been successfully executed yet */
		goto out;

	ref_get(&q->ref, "close");

	QUERY_TQ_LOCK();
	assert(q->list_ref > 0);
	q->list_ref--;
	if (0 == q->list_ref) {
		/* safe to remove from the list */
		TAILQ_REMOVE(&query_tq, q, entry);
		ref_put(&q->ref, "query_tq");
	}
	QUERY_TQ_UNLOCK();

	if (0 == q->list_ref) {
		__post_close(q); /* see close_cb */
	}

 out:
	ref_put(&q->ref, "init");
}

typedef struct jobmgr_qev_s {
	struct ldmsd_jobmgr_query_event_s qev;
	void *ctxt; /* event context from the plugin */
	struct ovis_event_s ovis_ev;
} *jobmgr_qev_t;

void close_cb(ovis_event_t oev)
{
	/* Close event, just deliver to app */
	jobmgr_qev_t jev = oev->param.ctxt;
	ldmsd_jobmgr_query_t q = jev->qev.q;
	struct ldmsd_jobmgr_query_mgr_s *ent;

	assert(0 == q->list_ref);

	/* Notify all plugins; then notify application */
	while ((ent = TAILQ_FIRST(&q->mgr_tq))) {
		TAILQ_REMOVE(&q->mgr_tq, ent, entry);
		ent->mgr->api->on_query_free((void*)ent->mgr, q, ent->qmgr_ctxt);
		ldmsd_cfgobj_put(&ent->mgr->cfg, "qmgr_ent");
		free(ent);
	}
	/* notify query consumer */
	struct ldmsd_jobmgr_query_event_s qev = {
		.event_type = LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE,
		.q = q,
	};
	q->cb_fn(q, &qev, q->cb_arg);
	ref_put(&q->ref, "close");

	free(jev);
}

void qev_cb(ovis_event_t oev)
{
	jobmgr_qev_t jev = oev->param.ctxt;
	TAILQ_HEAD(, ldmsd_jobmgr_query_s) rm_tq = TAILQ_HEAD_INITIALIZER(rm_tq);
	ldmsd_jobmgr_query_t q, _q;
	ldmsd_cfgobj_jobmgr_t mgr;
	int rc;
	void *q_ctxt;
	ldmsd_jobmgr_query_event_type_t event_type;
	struct ldmsd_jobmgr_query_mgr_s *ent;
	char job_id[LDMSD_JOBMGR_JOB_ID_LEN] = "";
	ldms_mval_t mv;

	event_type = jev->qev.event_type;

	mgr = container_of(jev->qev.mgr, struct ldmsd_cfgobj_jobmgr, cfg);
	QUERY_TQ_LOCK();
	q = TAILQ_FIRST(&query_tq);
	while (q) {
		assert(q->list_ref > 0);
		q->list_ref++;
		QUERY_TQ_UNLOCK();

		/* reuse qev for each query */
		jev->qev.q = q;
		q_ctxt = NULL;
		for (ent = TAILQ_FIRST(&q->mgr_tq); ent;
		     ent = TAILQ_NEXT(ent, entry)) {
			if (ent->mgr == mgr) {
				q_ctxt = ent->qmgr_ctxt;
				break;
			}
		}
		rc = mgr->api->make_qev(jev->qev.mgr, &jev->qev, q_ctxt, jev->ctxt);
		if (0 == rc) {
			if (event_type == LDMSD_JOBMGR_QUERY_EVENT_JOB_END && job_id[0] == 0) {
				/* cache job_id for job_end event to
				 * later schedule job cleanup */
				mv = ldms_record_metric_get(
						jev->qev.start_end.qrec,
						q->job_id_midx);
				snprintf(job_id, sizeof(job_id), "%s", mv->a_char);
			}
			q->cb_fn(q, &jev->qev, q->cb_arg);
		} else if (jev->qev.event_type == LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE) {
			q->cb_fn(q, &jev->qev, q->cb_arg);
		}

		/* recover qev */
		jev->qev.q = NULL;
		jev->qev.event_type = event_type;
		switch (event_type) {
		case LDMSD_JOBMGR_QUERY_EVENT_JOB_START:
		case LDMSD_JOBMGR_QUERY_EVENT_JOB_END:
		case LDMSD_JOBMGR_QUERY_EVENT_STEP_START:
		case LDMSD_JOBMGR_QUERY_EVENT_STEP_END:
		case LDMSD_JOBMGR_QUERY_EVENT_TASK_START:
		case LDMSD_JOBMGR_QUERY_EVENT_TASK_END:
			jev->qev.start_end.qrec = NULL;
			break;
		case LDMSD_JOBMGR_QUERY_EVENT_JOB_CLEANUP:
			assert( 0 == "Unexpected evnet");
			break;
		case LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE:
			assert( 0 == "Unexpected evnet");
			break;
		case LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE:
			assert( 0 == "Unexpected evnet");
			break;
		}

		QUERY_TQ_LOCK();
		q->list_ref--;
		if (0 == q->list_ref) {
			/* remove it! */
			_q = q;
			q = TAILQ_NEXT(q, entry);
			TAILQ_REMOVE(&query_tq, _q, entry);
			TAILQ_INSERT_TAIL(&rm_tq, _q, entry);
		} else {
			q = TAILQ_NEXT(q, entry);
		}
	}
	QUERY_TQ_UNLOCK();

	/* done with the event */
	mgr->api->qev_done(jev->qev.mgr, &jev->qev, jev->ctxt);

	if (event_type == LDMSD_JOBMGR_QUERY_EVENT_JOB_END) {
		/* schedule record cleanup here */
		ldmsd_jobmgr_job_cleanup_post(jev->qev.mgr, job_id, 10);
	}

	while ((q = TAILQ_FIRST(&rm_tq))) {
		TAILQ_REMOVE(&rm_tq, q, entry);
		__post_close(q); /* see close_cb */
	}
	assert(oev->param.type == OVIS_EVENT_ONESHOT);
	free(jev);
}

static int __post_close(ldmsd_jobmgr_query_t q)
{
	int rc;
	jobmgr_qev_t jev = calloc(1, sizeof(*jev));

	if (!jev)
		return ENOMEM;

	jev->qev.q = q;
	jev->qev.event_type = LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE;
	jev->ovis_ev.param.type  = OVIS_EVENT_ONESHOT;
	jev->ovis_ev.param.cb_fn = close_cb;
	jev->ovis_ev.param.ctxt  = jev;

	rc = ovis_scheduler_event_add(jobmgr_sched, &jev->ovis_ev);
	if (rc)
		free(jev);
	return rc;
}

/* calls by jobmgr plugin */
int ldmsd_jobmgr_qev_post(ldmsd_plug_handle_t mgr,
			  ldmsd_jobmgr_query_event_type_t event_type,
			  void *ctxt)
{
	int rc;
	jobmgr_qev_t jev = calloc(1, sizeof(*jev));

	if (!jev)
		return ENOMEM;

	jev->ctxt = ctxt;
	OVIS_EVENT_INIT(&jev->ovis_ev);

	jev->qev.mgr = mgr;
	jev->qev.event_type = event_type;

	jev->ovis_ev.param.type  = OVIS_EVENT_ONESHOT;
	jev->ovis_ev.param.cb_fn = qev_cb;
	jev->ovis_ev.param.ctxt  = jev;

	rc = ovis_scheduler_event_add(jobmgr_sched, &jev->ovis_ev);
	if (rc)
		free(jev);
	return rc;
}

/* protected by query_tq_lock */
static void job_list_cleanup(jobmgr_qev_t jev, ldmsd_jobmgr_query_t q)
{
	ldms_mval_t qrec, lh, qr, job_id;
	enum ldms_value_type type;
	size_t count;
	pthread_mutex_lock(&q->mutex);
	ldms_transaction_begin(q->set);
	lh = ldms_metric_get(q->set, q->list_midx);
	qrec = ldms_list_first(q->set, lh, &type, &count);
	while (qrec) {
		assert(type == LDMS_V_RECORD_INST);
		qr = qrec;
		qrec = ldms_list_next(q->set, qrec, &type, &count);
		job_id = ldms_record_metric_get(qr, q->job_id_midx);
		if (0 != strcmp(job_id->a_char, jev->qev.cleanup.job_id))
			continue;
		/* remove + free qr */
		ldms_list_remove_item(q->set, lh, qr);
	}
	ldms_transaction_end(q->set);
	pthread_mutex_unlock(&q->mutex);
}

static void job_cleanup_cb(ovis_event_t oev)
{
	jobmgr_qev_t jev = oev->param.ctxt;
	ldmsd_jobmgr_query_t q;
	if (oev->param.type == OVIS_EVENT_TIMEOUT) {
		ovis_scheduler_event_del(jobmgr_sched, oev);
	}

	QUERY_TQ_LOCK();
	for (q = TAILQ_FIRST(&query_tq); q; q = TAILQ_NEXT(q, entry)) {
		job_list_cleanup(jev, q);
	}
	QUERY_TQ_UNLOCK();

	free(jev);
}

int ldmsd_jobmgr_job_cleanup_post(ldmsd_plug_handle_t mgr, const char *job_id,
				  time_t delay_sec)
{
	int rc;
	jobmgr_qev_t jev = calloc(1, sizeof(*jev));

	if (!jev)
		return ENOMEM;
	OVIS_EVENT_INIT(&jev->ovis_ev);

	jev->qev.event_type = LDMSD_JOBMGR_QUERY_EVENT_JOB_CLEANUP;
	snprintf(jev->qev.cleanup.job_id, sizeof(jev->qev.cleanup.job_id), "%s", job_id);

	jev->ovis_ev.param.cb_fn = job_cleanup_cb;
	jev->ovis_ev.param.ctxt = jev;

	if (delay_sec > 0) {
		jev->ovis_ev.param.type = OVIS_EVENT_TIMEOUT;
		jev->ovis_ev.param.timeout.tv_sec = delay_sec;
	} else {
		jev->ovis_ev.param.type = OVIS_EVENT_ONESHOT;
	}

	rc = ovis_scheduler_event_add(jobmgr_sched, &jev->ovis_ev);
	if (rc)
		free(jev);
	return rc;
}

/* return 0 if m0 == m1
 * return 1 if m0 != m1
 * return -errno on error
 */
static int __mval_eq(enum ldms_value_type t,
		ldms_mval_t m0, size_t l0,
		ldms_mval_t m1, size_t l1)
{
	switch (t) {
	case LDMS_V_U8: case LDMS_V_S8: case LDMS_V_CHAR:
		return !!memcmp(m0, m1, sizeof(uint8_t));
	case LDMS_V_U16: case LDMS_V_S16:
		return !!memcmp(m0, m1, sizeof(uint16_t));
	case LDMS_V_U32: case LDMS_V_S32:
		return !!memcmp(m0, m1, sizeof(uint32_t));
	case LDMS_V_U64: case LDMS_V_S64:
		return !!memcmp(m0, m1, sizeof(uint64_t));
	case LDMS_V_F32:
		return !!memcmp(m0, m1, sizeof(float));
	case LDMS_V_D64:
		return !!memcmp(m0, m1, sizeof(double));
	case LDMS_V_CHAR_ARRAY:
		return !!strncmp(m0->a_char, m1->a_char, l0<l1?l0:l1);
	default:
		errno = EINVAL;
		return -EINVAL;
	}
	return 0;
}

/* return 0 if m0 == m1
 * return 1 if m0 != m1
 * return -errno on error
 */
static int __rec_key_eq(ldms_mval_t rec, ldmsd_jobmgr_qrec_key_t k, int blank_is_any)
{
	int i, c;
	size_t len;
	ldms_mval_t mv;
	enum ldms_value_type t;
	for (i = 0; i < k->n; i++) {
		mv = ldms_record_metric_get(rec, k->keys[i].midx);
		t = ldms_record_metric_type_get(rec, k->keys[i].midx, &len);
		if (t != k->keys[i].type)
			return EINVAL; /* type mismatch */
		if (blank_is_any && t == LDMS_V_CHAR_ARRAY && mv->a_char[0] == 0)
			continue; /* empty string matches any */
		c = __mval_eq(t, mv, len, k->keys[i].mval, k->keys[i].len);
		if (c)
			return c;
	}
	return 0;
}

static int __rec_key_fill(ldms_mval_t rec, ldmsd_jobmgr_qrec_key_t k)
{
	int i;
	size_t len;
	ldms_mval_t mv;
	size_t sz;
	enum ldms_value_type t;
	for (i = 0; i < k->n; i++) {
		mv = ldms_record_metric_get(rec, k->keys[i].midx);
		t = ldms_record_metric_type_get(rec, k->keys[i].midx, &len);
		if (t == LDMS_V_CHAR_ARRAY && mv->a_char[0])
			continue; /* don't modify non-empty str */
		if (t != k->keys[i].type)
			return EINVAL; /* type mismatch */
		if (k->keys[i].len > len)
			return EINVAL;
		sz = ldms_metric_value_size_get(t, k->keys[i].len);
		memcpy(mv, k->keys[i].mval, sz);
	}
	return 0;
}

static int __rec_key_assign(ldms_mval_t rec, ldmsd_jobmgr_qrec_key_t k)
{
	int i;
	size_t len;
	ldms_mval_t mv;
	size_t sz;
	enum ldms_value_type t;
	for (i = 0; i < k->n; i++) {
		mv = ldms_record_metric_get(rec, k->keys[i].midx);
		t = ldms_record_metric_type_get(rec, k->keys[i].midx, &len);
		if (t != k->keys[i].type)
			return EINVAL; /* type mismatch */
		if (k->keys[i].len > len)
			return EINVAL;
		sz = ldms_metric_value_size_get(t, k->keys[i].len);
		memcpy(mv, k->keys[i].mval, sz);
	}
	return 0;
}

ldms_mval_t ldmsd_jobmgr_qrec_get(ldmsd_jobmgr_query_t q,
					ldmsd_jobmgr_qrec_key_t k)
{
	ldms_mval_t qlist;
	ldms_mval_t qrec;
	int rc;

	qlist = ldms_metric_get(q->set, q->list_midx);
	if (!qlist)
		return NULL;

	pthread_mutex_lock(&q->mutex);
	for (qrec = ldms_list_first(q->set, qlist, NULL, NULL);
	     qrec; qrec = ldms_list_next(q->set, qrec, NULL, NULL)) {
		if (0 == __rec_key_eq(qrec, k, 1)) {
			/* since "" matches any, we need to fill the blanks */
			__rec_key_fill(qrec, k);
			goto out;
		}
	}

	ldms_transaction_begin(q->set);
	/* alloc */
	qrec = ldms_record_alloc(q->set, q->recdef_midx);
	if (!qrec) {
		errno = ENOMEM;
		ldms_transaction_end(q->set);
		goto out;
	}

	bzero(qrec->v_rec_inst.rec_data, q->rec_value_sz);

	__rec_key_assign(qrec, k);
	rc = ldms_list_append_record(q->set, qlist, qrec);
	assert(rc == 0);
	ldms_transaction_end(q->set);

 out:
	pthread_mutex_unlock(&q->mutex);
	return qrec;
}

ldms_mval_t
ldmsd_jobmgr_qrec_next(ldmsd_jobmgr_query_t q, ldmsd_jobmgr_qrec_key_t k,
		       ldms_mval_t qrec)
{
	/* caller held q->mutex */
	ldms_mval_t sqrec = ldms_list_next(q->set, qrec, NULL, NULL);
	while (sqrec) {
		if (0 == __rec_key_eq(sqrec, k, 0)) {
			/* found the match */
			goto out;
		}
		sqrec = ldms_list_next(q->set, sqrec, NULL, NULL);
	}
 out:
	return sqrec;
}

__attribute__((constructor))
void __init__()
{
	/* populate the default metric list */
	struct ldmsd_str_ent *ent;
	for (ent = &default_marray[0]; ent->str; ent++) {
		TAILQ_INSERT_TAIL(&default_mlist, ent, entry);
	}
}
