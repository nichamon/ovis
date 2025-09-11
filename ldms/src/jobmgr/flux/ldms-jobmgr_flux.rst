.. _ldms-jobmgr_flux:

============
jobmgr_flux
============

-----------------------------------------
Man page for the LDMS jobmgr_flux plugin
-----------------------------------------

:Date:   19 Aug 2025
:Manual section: 7
:Manual group: LDMS job plugin

SYNOPSIS
========

``load`` name=\ `INST` plugin=\ ``jobmgr_flux``

``config`` name=\ `INST` [message_channel=\ `CHANNEL`]

``jobmgr_start`` name=\ `INST`

``jobmgr_stop``  name=\ `INST`

``term`` name=\ `INST`


DESCRIPTION
===========

``jobmgr_flux`` is an LDMSD Job Manager Plugin that receives ``flux`` shell
events from ``flux_shell_ldms_notifier`` via ``ldms_msg`` from the channel
specified by ``message_channel``, create corresponding job sets, and post
``jobmgr`` events to the Job Manager event subscribers in ``ldmsd``.

Please see CONFIG ATTRIBUTES section for descriptions of all available
attributes to ``config`` command for this plugin.

This plugin supports multi instances.


CONFIG ATTRIBUTES
=================

``name``\ =\ `INST`
  The instance name of the plugin.


[ ``message_channel``\ =\ `CHANNEL` ]
  The message channel to receive Flux job data from
  ``flux_shell_ldms_notiifer`` (Flux shell plugin). If this is not specified,
  the default value is "flux".


EXAMPLES
========

Within ldmsd_controller or a configuration file:

::

   load name=jobmgr_flux
   config name=jobmgr_flux message_channel=flux
   jobmgr_start name=jobmgr_flux

   jobmgr_stop name=jobmgr_flux


AVAILABLE METRICS
=================

``jobmgr_flux`` does not have extra metrics for ``jobmgr_query``. The common
metrics that ``jobmgr_flux`` supported are: "job_id", "user", "job_name",
"job_uid", "job_gid", "job_start", "job_end", "node_count", "job_state",
"step_id", "step_start", "step_end", "total_tasks", "local_tasks", "task_id",
"task_pid", "task_rank", "task_start", "task_end", and "task_exit_status".


SEE ALSO
========

:ref:`ldms-flux_shell_ldms_notifier(7) <ldms-flux_shell_ldms_notifier>` ,
:ref:`ldmsd(8) <ldmsd>` ,
:ref:`ldms_quickstart(7) <ldms_quickstart>` ,
:ref:`ldmsd_controller(8) <ldmsd_controller>`,
:ref:`ldms-jobmgr(8) <ldms-jobmgr>`,
