.. _ldms-jobmgr:

======
jobmgr
======

-----------------------------
Job Manager Service for LDMSD
-----------------------------

:Date:  20 Nov 2025
:Manual section: 7
:Manual group: LDMSD


DESCRIPTION
===========

``jobmgr`` is a service inside ``ldmsd`` that provides normalized Job events and
information to other components in ``ldmsd`` via ``jobmgr_query`` APIs.
``jobmgr`` plugins (e.g. ``jobmgr_slurm`` and ``jobmgr_flux``) are the ones
obtaiing job events and information from the actual Job Manager system and
submit the events to ``jobmgr``. The following diagram shows an overview of the
relations among the components.


::

              ╔═════════════╗
              ║ Job Manager ║------.
              ╚═════════════╝      |
                                   |
      ╔═════════════════════════╗  | provide data
      ║          ldmsd          ║  |
      ║                         ║  |
      ║ ┌─────────────┐         ║  |
      ║ │jobmgr_plugin│<--------+--'
      ║ └─────────────┘         ║
      ║    | post events        ║
      ║    v                    ║
      ║ ┌──────┐                ║
      ║ │jobmgr│                ║
      ║ └──────┘                ║
      ║    | calls cb_fn() in each jobmgr_query
      ║    v                    ║
      ║ ┌────────────┐          ║
      ║ │jobmgr_query│          ║
      ║ └────────────┘          ║
      ║                         ║
      ║                         ║
      ╚═════════════════════════╝


For more information about how to use ``jobmgr_query``, see
``ldms-jobmgr_query(7)`` man page.

For more information about how to develop ``jobmgr_plugin``, see
``ldms-jobmgr_plugin(7)`` man page.

For SLURM jobmgr plugin usage, see ``ldms-jobmgr_slurm(7)``.

For Flux jobmgr plugin usage, see ``ldms-jobmgr_flux(7)``.

SEE ALSO
========
:ref:`ldms-jobmgr_slurm(7) <ldms-jobmgr_slurm>`,
:ref:`ldms-jobmgr_flux(7) <ldms-jobmgr_flux>`,
:ref:`ldms-jobmgr_plugin(7) <ldms-jobmgr_plugin>`,
:ref:`ldms-jobmgr_query(7) <ldms-jobmgr_query>`
