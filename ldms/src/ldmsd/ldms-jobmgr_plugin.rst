.. _ldms-jobmgr_plugin:

=============
jobmgr_plugin
=============

---------------------------------------------------
a development guide on jobmgr_plugin implementation
---------------------------------------------------

:Date:  20 Nov 2025
:Manual section: 7
:Manual group: LDMSD


SYNOPSIS
========

.. code:: c

 #include "ldmsd.h"
 #include "ldmsd_plug_api.h"
 #include "ldmsd_jobmgr.h"
 #include "ldmsd_jobmgr_query.h"

 int ldmsd_jobmgr_metric_register(struct ldmsd_jobmgr_query_mdesc_s *m);

 struct ldmsd_jobmgr {

    struct ldmsd_plugin base; /* see `struct ldmsd_plugin` below */

    int (*start)(ldmsd_plug_handle_t p);
    int (*stop)(ldmsd_plug_handle_t p);

    int (*on_query_new)(ldmsd_plug_handle_t p, ldmsd_jobmgr_query_t q, void **q_ctxt_out);
    void (*on_query_free)(ldmsd_plug_handle_t p, ldmsd_jobmgr_query_t q, void *q_ctxt);

    int (*make_qev)(ldmsd_plug_handle_t p, struct ldmsd_jobmgr_query_event_s *ev, void *q_ctxt, void *ev_ctxt);
    void (*qev_done)(ldmsd_plug_handle_t p, ldmsd_jobmgr_query_event_type_t event_type, void *ev_ctxt);

 };

 struct ldmsd_plugin {
     enum ldmsd_plugin_type {
         ...
         LDMSD_PLUGIN_JOBMGR,
     } type;
     ...
     int (*config)(ldmsd_plug_handle_t handle,
     struct attr_value_list *kwl,
     struct attr_value_list *avl);
     const char *(*usage)(ldmsd_plug_handle_t handle);

     int (*constructor)(ldmsd_plug_handle_t handle);
     void (*destructor)(ldmsd_plug_handle_t handle);
     ...
 };

 int ldmsd_jobmgr_qev_post(ldmsd_plug_handle_t mgr,
			  ldmsd_jobmgr_query_event_type_t event_type,
			  void *ctxt);

 typedef enum ldmsd_jobmgr_query_event_type_e {
     LDMSD_JOBMGR_QUERY_EVENT_JOB_START,
     LDMSD_JOBMGR_QUERY_EVENT_JOB_END,
     LDMSD_JOBMGR_QUERY_EVENT_STEP_START,
     LDMSD_JOBMGR_QUERY_EVENT_STEP_END,
     LDMSD_JOBMGR_QUERY_EVENT_TASK_START,
     LDMSD_JOBMGR_QUERY_EVENT_TASK_END,
     ...
 } ldmsd_jobmgr_query_event_type_t;

 ldms_mval_t ldmsd_jobmgr_qrec_get(ldmsd_jobmgr_query_t q, ldmsd_jobmgr_qrec_key_t k);

 ldms_mval_t ldmsd_jobmgr_qrec_next(ldmsd_jobmgr_query_t q, ldmsd_jobmgr_qrec_key_t k,
				   ldms_mval_t qrec);



DESCRIPTION
===========

``jobmgr_plugin`` is an LDMSD plugin that interact with ``ldmsd_jobmgr``
subsystem to provide job-related information to components in ``ldmsd``.
The following is a list of stages of a ``jobmgr_plugin`` and how ``ldmsd``
interacts with it. All API calls are meant to be a short call. The plugin shall
not block the calling thread for a long time (e.g. blocking-wait indefinitely
for data to arrive in ``start()``).


1. ``load``. The ``jobmgr_plugin`` is ``dlopen()`` by ``ldmsd``. Like other
   ``ldmsd`` plugins, ``ldmsd`` looks for ``ldmsd_plugin_interface`` variable
   that describes the plugin. In our case, the variable shall be of type
   ``struct ldmsd_jobmgr``.

   Then, the ``constructor()`` interface is called when ``ldmsd`` creates a
   plugin instance (driven by ``load`` config command). The plugin shall create
   the instance context in ``constructor()`` and assign the context by calling
   ``ldmsd_plug_ctxt_set()``.

   Extra job metrics (in addition to the common job metric list) may be
   registered by calling ``ldmsd_jobmgr_metric_register()``. The function
   returns ``0`` (OK) if the registering metric does not conflict with the
   metrics already registered.


2. ``config``. Also like other plugins, ``config()`` API is called when
   ``config`` command is issued to ``ldmsd``. There are no common parameters for
   ``jobmgr_plugin`` configuration.

3. ``jobgr_start``. After the ``jobmgr_plugin`` is configured, ``jobmgr_start``
   configuration command is expected from the user to tell the ``jobmgr_plugin``
   to start working on job data. When this happened, ``ldmsd`` calls
   ``jobmgr_plugin``'s ``start()`` interface. The plugin can then start
   collecting/processing job data. The plugin can return ``EBUSY`` if it is not
   ready to start.

   See ``JOB EVENTS AND QUERY DATA`` section on how ``jobmgr_plugin`` notify
   ``jobmgr`` about job events and how ``jobmgr_plugin`` provide information on
   those events to ``jobmgr_query``.

4. ``jobgr_stop``. ``jobmgr_plugin``'s ``stop()`` interface is called when the
   user issues ``jobmgr_stop`` configuration command. The plugin may returns
   ``EBUSY`` if it cannot stop at the time of the call.

5. ``term``. When ``term`` configuration command is issued to ``ldmsd``,
   plugin's ``destructor()`` interface is called. This is a revert of
   ``constructor()``.


JOB EVENTS AND QUERY DATA
=========================

``jobmgr`` is a subsystem in ``ldmsd`` that acts as a middle man managing events
posted by ``jobmgr_plugin`` and notifying (multiple) ``jobmgr_query`` who are
the consumer of job information. ``jobmgr`` subsystem keeps track of all
``jobmgr_query`` in ``ldmsd`` and the loaded ``jobmgr_plugin``. The
``jobmgr_query`` provides query spec as metric list and LDMS ``set`` to store
the query result (as records in the set). The ``jobmgr_plugin`` provides
information and manipulate ``jobmgr_query``'s ``set`` when ``jobmgr`` subsystem
asked.

When a ``jobmgr_query`` is created, the ``on_query_new(p, q, q_ctxt_out)``
interface is called (``p`` being plugin handle, ``q`` being query handle and
``q_ctxt_out`` being output parameter query context for the plugin) to let the
plugin prepare relevant information for the query (e.g. looking up internal
metric descriptors in the plugin). The ``*q_ctxt_out`` context will later be
supplied to subsequent ``make_qev()`` interface call that will be discussed
further later on in this section.

After ``start()``, ``jobmgr_plugin`` may start processing job events and data.
When a job event occur, let's say ``JOB_START``, ``jobmgr_plugin`` shall inform
``jobmgr`` subsystem by calling

.. code:: c

   ldmsd_jobmgr_qev_post(plug, LDMSD_JOBMGR_QUERY_EVENT_JOB_START, ev_ctxt)


The ``ev_ctxt`` is a context created by the plugin for this particular event. It
may, for example, contains a reference to the plugin's job structure related to
the job. The ``ev_ctxt`` shall not be freed (yet) as it will supplied to the
subsequence ``make_qev()`` calls.

When the ``jobmgr`` subsystem received the event post from the plugin, it
iterates through all ``jobmgr_query`` objects, and for each ``jobmgr_query``
object ``jobmgr`` calls:

.. code:: c

   make_qev(p, qev, q_ctxt, ev_ctxt)

where ``p`` is the plugin handle, ``qev`` is the query event to be supplied to
query callback, ``q_ctxt`` is the query context ``*q_ctxt_out`` from
``on_query_new()`` call, and ``ev_ctxt`` is the event context from
``ldmsd_jobmgr_qev_post()`` call. The plugin is expected to manipulate ``qev``
event as follows:

- ``qev->q`` is already prepopulated to point to the query handle, the plugin
  should leave it alone.

- ``qev->type`` is pre-assigned by ``jobmgr`` subsystem to let the plugin knows
  about the event type (e.g. ``JOB_START``).

- ``qev->start_end.qrec`` shall point to the record in the set that corresponds
  to the event.

The plugin may use ``ldmsd_jobmgr_qrec_get(q, key)`` utility function to get the
first matching record, or create one if none is found, and then manipulate the
returned record. In the case that the plugin failed to allocate a new record due
to not enough set heap, the plugin can modify ``qev->type`` to
``LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE``, set ``qev->no_space.number_of_records``
and return a non-zero status. ``jobmgr`` will call ``jobmgr_query`` callback
function to let the query client know that the set does not have enough heap
memory.

When the event posted by the ``jobmgr_plugin`` has been processed by all
``jobmgr_query``\ 's, ``qev_done(p, ev_type, ev_ctxt)`` interface will be
called, where ``p`` (plugin handle), ``ev_type`` (event type) and ``ev_ctxt``
(event context) are exactly the same values specified in the
``ldmsd_jobmgr_qev_post(p, ev_type, ev_ctxt)`` call. In ``qev_done()`` function,
the plugin can release resources (e.g. ``ev_ctxt``) related to the event.


EXAMPLES
========

Please see ``jobmgr_slurm.c`` and ``jobmgr_flux.c`` for examples.


SEE ALSO
========
:ref:`ldms-jobmgr(7) <ldms-jobmgr>`,
:ref:`ldms-jobmgr_slurm(7) <ldms-jobmgr_slurm>`,
:ref:`ldms-jobmgr_flux(7) <ldms-jobmgr_flux>`,
:ref:`ldms-jobmgr_query(7) <ldms-jobmgr_query>`
