.. _ldms-jobmgr_slurm:

============
jobmgr_slurm
============

-----------------------------------------
Man page for the LDMS jobmgr_slurm plugin
-----------------------------------------

:Date:   19 Aug 2025
:Manual section: 7
:Manual group: LDMS job plugin

SYNOPSIS
========

``load`` name=\ `INST` plugin=\ ``jobmgr_slurm``

``config`` name=\ `INST` [message_channel=\ `CHANNEL`] [component_id=\ `COMP_ID`]

``jobmgr_start`` name=\ `INST`

``jobmgr_stop``  name=\ `INST`

``term`` name=\ `INST`


DESCRIPTION
===========

``jobmgr_slurm`` is an LDMSD Job Manager Plugin that receives ``slurm`` events
from ``slurm_notifier`` via ``ldms_msg`` from the channel specified by
``message_channel``, create corresponding job sets, and post ``jobmgr`` events
to the Job Manager event subscribers in ``ldmsd``.

Please see CONFIG ATTRIBUTES section for descriptions of all available
attributes to ``config`` command for this plugin.

This plugin supports multi instances.


CONFIG ATTRIBUTES
=================

``name``\ =\ `INST`
  The instance name of the plugin.


[ ``message_channel``\ =\ `CHANNEL` ]
  The message channel to receive Slurm job data from ``slurm_notiifer``. If
  this is not specified, the default value is "slurm".


EXAMPLES
========

Within ldmsd_controller or a configuration file:

::

   load name=jobmgr_slurm
   config name=jobmgr_slurm message_channel=slurm
   jobmgr_start name=jobmgr_slurm

   jobmgr_stop name=jobmgr_slurm


AVAILABLE METRICS
=================

The common metrics that ``jobmgr_slurm`` supported for ``jobmgr_query`` are:
"job_id", "user", "job_name", "job_uid", "job_gid", "job_start", "job_end",
"node_count", "job_state", "step_id", "step_start", "step_end", "total_tasks",
"local_tasks", "task_id", "task_pid", "task_rank", "task_start", "task_end", and
"task_exit_status".

"job_tag" is an additonal metric supported by ``jobmgr_slurm``.


SEE ALSO
========

:ref:`ldms-sampler_slurm_notifier(8) <slurm_notifier>` ,
:ref:`ldmsd(8) <ldmsd>` ,
:ref:`ldms_quickstart(7) <ldms_quickstart>` ,
:ref:`ldmsd_controller(8) <ldmsd_controller>`,
:ref:`ldms-jobmgr(8) <ldms-jobmgr>`,
