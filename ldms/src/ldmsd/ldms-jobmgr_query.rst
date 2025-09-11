.. _ldms-jobmgr_query:

============
jobmgr_query
============

---------------------------------------------------------------
getting job events and information from jobmgr service in LDMSD
---------------------------------------------------------------

:Date:  20 Nov 2025
:Manual section: 7
:Manual group: LDMSD


SYNOPSIS
========

.. code:: c

 #include "ldmsd_jobmgr_query.h"


 ldmsd_jobmgr_query_t
 ldmsd_jobmgr_query_new(struct ldmsd_str_list *metric_list,
                        ldmsd_jobmgr_query_cb_fn_t cb_fn, void *cb_arg,
                        char **error);

 int ldmsd_jobmgr_query_execute(ldmsd_jobmgr_query_t q,
                                ldms_set_t set,
                                int rec_def_midx,
                                int list_midx);

 void ldmsd_jobmgr_query_close(ldmsd_jobmgr_query_t q);

 typedef int (*ldmsd_jobmgr_query_cb_fn_t)(
                         ldmsd_jobmgr_query_t q,
                         ldmsd_jobmgr_query_event_t qev,
                         void *cb_arg);

 struct ldmsd_jobmgr_query_event_s {
    ldmsd_jobmgr_query_t q; /* The query handle associated to the event. */
    ldmsd_jobmgr_query_event_type_t event_type;

    ldmsd_plug_handle_t mgr; /* The job manager plugin that posted the event */

    union {
        /* Data for start/end event */
        struct {
            ldms_mval_t qrec; /* The query record associated with the event */
        } start_end;

        /* Data for no-space event */
        struct {
            int number_of_records; /* NT name too long */
        } no_space;

        struct {
            char job_id[LDMSD_JOBMGR_JOB_ID_LEN];
        } cleanup;
    };
 };

 typedef enum ldmsd_jobmgr_query_event_type_e {
     LDMSD_JOBMGR_QUERY_EVENT_JOB_START,
     LDMSD_JOBMGR_QUERY_EVENT_JOB_END,
     LDMSD_JOBMGR_QUERY_EVENT_STEP_START,
     LDMSD_JOBMGR_QUERY_EVENT_STEP_END,
     LDMSD_JOBMGR_QUERY_EVENT_TASK_START,
     LDMSD_JOBMGR_QUERY_EVENT_TASK_END,
     LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE, /* the last event delivered to the cb fn */
     LDMSD_JOBMGR_QUERY_EVENT_JOB_CLEANUP, /* job data deleted */
     LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE, /* the last event delivered to the cb fn */
 } ldmsd_jobmgr_query_event_type_t;

 struct ldmsd_jobmgr_query_s {
     ldms_record_t recdef; /* can be used in ldms_schema */
     ...
 };


DESCRIPTION
===========

To use ``jobmgr_query``:

1. Create a query ``q = ldmsd_jobmgr_query_new(mlist, cb_fn, cb_arg, &errstr)``
   providing ``mlist`` a list of job-related metrics to be queried and
   a callback function ``cb_fn``.

2. Add ``q->recdef`` record definition to a schema ``sch`` and create an LDMS
   set ``set`` with a ``list``. The ``query`` will populate/manipulate records
   in the ``list`` for query results.

3. Execute the query ``ldmsd_jobmgr_query_execute(q, set, recdef_midx, list_midx)``.
   Then, the callback function ``cb_fn`` provided in 1) will be called with a
   corresponding record in the ``list`` every time a job event has been posted
   to ``jobmgr`` subsystem. This keeps happening until,

4. Call ``ldmsd_jobmgr_query_close(q)`` to request query termination. ``cb_fn``
   will be called again for the last time to deliver
   ``LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE`` event, and *no* events will be
   delivered to this query after the close event.


``mlist`` provided in 1) is a list of job-related metrics (e.g. "job_id",
"task_pid") to be supplied in the query result when a job event occur (e.g.
``JOB_START``). If ``mlist`` is ``NULL``, a default list of metrics are chosen
("job_id", "step_id", "task_id", "job_start", "job_end", "step_start",
"step_end", "task_start", "task_end"). "job_id", "step_id" and "task_id" may be
prepended to the list if the query requested for metrics that may need the
"\*_id" but did not specified them. For example, mlist being [ "job_start",
"job_end" ], will be prepended to be [ "job_id", "job_start", "job_end" ].

Common metrics that are provided by all ``jobmgr`` plugins are:

    "job_id", "user", "job_name", "job_uid", "job_gid", "job_start", "job_end",
    "job_state", "step_id", "step_start", "step_end", "node_count",
    "total_tasks", "local_tasks", "task_id", "task_pid", "task_rank",
    "task_start", "task_end", "task_exit_status".

Plugins may provide additional metrics, please consult their man pages for
more infirmation.



EXAMPLES
========

This example is excerpted from ``test_jobmgr_sampler.c``:

.. code:: c

 #include "ldmsd_jobmgr.h"

 static int config(ldmsd_plug_handle_t handle,
		  struct attr_value_list *kwl, struct attr_value_list *avl)
 {
    ...

    test_jobmgr_samp_t j;
    int redcdef_midx, list_midx;

    j->q = ldmsd_jobmgr_query_new(metrics, jobmgr_cb, j, &err_str);
    sch = ldms_schema_new(schema);
    recdef_midx = ldms_schema_record_add(sch, j->q->recdef);
    list_midx = ldms_schema_metric_list_add(sch, "job_rec", NULL, j->heap_sz);
    set = ldms_set_new_with_auth(inst, sch, getuid(), getgid(), 0440);
    ldmsd_jobmgr_query_execute(j->q, set, recdef_midx, list_midx);

    ...
 }

 int jobmgr_cb(ldmsd_jobmgr_query_t q, ldmsd_jobmgr_query_event_t qev, void *cb_arg)
 {
     test_jobmgr_samp_t j = cb_arg;
     char buf[4096];

     switch (qev->event_type) {
     case LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE:
         assert(0);
         fprintf(j->f, "no_space\n");
         return 0;
     case LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE:
         fprintf(j->f, "client_close\n");
         return 0;
     default:
         break;
     }

     qrec_print(qev, buf, sizeof(buf));

     switch (qev->event_type) {
     case LDMSD_JOBMGR_QUERY_EVENT_JOB_START:
         fprintf(j->f, "job_start: %s\n", buf);
         break;
     case LDMSD_JOBMGR_QUERY_EVENT_TASK_START:
         fprintf(j->f, "task_start: %s\n", buf);
         break;
     case LDMSD_JOBMGR_QUERY_EVENT_TASK_END:
         fprintf(j->f, "task_end: %s\n", buf);
         break;
     case LDMSD_JOBMGR_QUERY_EVENT_JOB_END:
         fprintf(j->f, "job_end: %s\n", buf);
         break;
     case LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE:
         fprintf(j->f, "client_close\n");
         ref_put(&j->ref, "jobmgr_cb");
         break;
     case LDMSD_JOBMGR_QUERY_EVENT_STEP_START:
         fprintf(j->f, "step_start: %s\n", buf);
         break;
     case LDMSD_JOBMGR_QUERY_EVENT_STEP_END:
         fprintf(j->f, "step_end: %s\n", buf);
         break;
     default:
         break;
     }

     return 0;
 }

 int qrec_print(ldmsd_jobmgr_query_event_t qev, char *buf, size_t bufsz)
 {
     /* print record into buf as json dict */
     ...

     qrec = qev->start_end.qrec;
     n_metrics = ldms_record_card(qev->start_end.qrec);
     rc = 0;
     i = 0;
     off += snprintf(buf+off, bufsz-off, "{");
 loop:
     if (i >= n_metrics)
         goto done;
     mv_name = ldms_record_metric_name_get(qrec, i);
     mv_type = ldms_record_metric_type_get(qrec, i, &len);
     mv = ldms_record_metric_get(qrec, i);
     switch (mv_type) {
     case LDMS_V_S8:
         off += snprintf(buf+off, bufsz-off, "%s\"%s\":%hhd", sep, mv_name,mv->v_s8);
         break;
     case LDMS_V_U8:
         off += snprintf(buf+off, bufsz-off, "%s\"%s\":%hhu", sep, mv_name,mv->v_u8);
         break;
     case LDMS_V_S16:
         off += snprintf(buf+off, bufsz-off, "%s\"%s\":%hd", sep, mv_name,mv->v_s16);
         break;
         ...
     default:
         goto next;
     }
     sep = ",";
     if (off >= bufsz) {
         rc = ENOBUFS;
         goto out;
     }
 next:
     i += 1;
     goto loop;
 done:
     off += snprintf(buf+off, bufsz-off, "}");
     if (off >= bufsz)
     rc = ENOBUFS;
 out:
     return rc;
 }


SEE ALSO
========

:ref:`ldms-jobmgr(8) <ldms-jobmgr>`,
:ref:`ldms-jobmgr(8) <ldms-jobmgr_plugin>`,
:ref:`ldms-jobmgr(8) <ldms-jobmgr_slurm>`,
:ref:`ldms-jobmgr(8) <ldms-jobmgr_flux>`
