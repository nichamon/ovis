==============================================
NOTE for OGC about Job Manager Plugin (jobmgr)
==============================================

The original proposal can be found in `<JOB.rst>`_ file. The actual
implementation may not 100% follow the proposal as we may have to change
something along the way to make it fit.


``jobmgr`` is a new service in ``ldmsd`` that allows other components of
``ldmsd`` to subscribe for job events. Job data is managed using ``ldms_set``,
which is managed by ``ldmsd_jobmgr`` plugin (a new ``ldmsd_plugin`` type).
``ldmsd_jobmgr`` plugin is also responsible for posting job events (and ``jobmgr``
service will deliver them to the subscriber inside ``ldmsd`` process).

`<ldms/src/ldmsd/ldmsd_jobmgr.h>`_ contains the APIs for interacting with
``jobmgr`` service as well as defining ``ldmsd_jobmgr`` plugin APIs. (NOTE: The
full documentation regarding plugin development is still a WIP).

`<ldms/src/ldmsd/ldmsd_jobmgr.c>`_ contains the code implementing the service.

`<ldms/src/jobmgr/slurm/jobmgr_slurm.c>`_ is an ``ldmsd_jobmgr`` plugin for
``slurm``. The plugin uses ``ldms_msg`` data from ``slurm_notifier`` Slurm/Spank
plugin. In other words, ``jobmgr_slurm`` is in a sense ``slurm_sampler`` with a
new ``jobset`` format and ``job_event`` posting capability.

`<ldms/src/sampler/examples/test_sampler/test_jobmgr_sampler.c>`_ is another
test sampler showing how to subscribe for the job events from a sampler. It
prints received events into a file (for testing / debugging purposes).

The following is an example of ``ldmsd.conf`` that uses ``jobmgr_slurm`` plugin
and ``test_jobmgr_sampler`` plugin.

.. code:: conf

  msg_enable

  listen xprt=sock port=411

  load name=jobmgr_slurm
  config name=jobmgr_slurm
  jobmgr_start name=jobmgr_slurm

  load name=test_jobmgr_sampler
  config name=test_jobmgr_sampler component_id=1 instance=node-1/test_jobmgr_sampler producer=node-1 file=/db/jobev-node-1.log
  # no need to start test_jobmgr_samler since its `sample()` is a no-op
