.. _ldms-flux_shell_ldms_notifier:

========================
flux_shell_ldms_notifier
========================

-----------------------------------------------------------
a flux shell plugin for job event notification to ``ldmsd``
-----------------------------------------------------------

:Date:   19 Aug 2025
:Manual section: 7
:Manual group: LDMS job plugin


SYNOPSIS
========

In ``[job-manager]`` section of ``system.toml``:

.. code:: toml
 ...

 [job-manager]

 plugins = [
   {{ load = "/path/to/libflux_jobtap_ldms_notifier.so", conf = {{ KEY = VAL, ... }} }},
 ]

 ...

``conf`` is optional. All ``ATTR`` in ``conf`` are also optional. See
``CONFIG ATTRIBUTES`` section below for available configuration attributes and
their default values.


CONFIG ATTRIBUTES
=================

``host``
  (default: "localhost")
  The host name (or IP address) of the ``ldmsd`` to notify.

``port``
  (default: 411)
  The port number of the ``ldmsd`` to notify.

``xprt``
  (default: "sock")
  The transport type of the ``ldmsd`` to notify.

``auth``
  (default: "munge")
  The LDMS authentication type of the ``xprt``.

``message_channel``
  (default: "flux")
  The name of the message channel for the notification.


EXAMPLE 1
=========

.. code:: toml
 ...

 [job-manager]

 plugins = [
   {{ load = "/path/to/libflux_jobtap_ldms_notifier.so" }},
 ]

 ...


EXAMPLE 2
=========

.. code:: toml
 ...

 [job-manager]

 plugins = [
   {{ load = "/path/to/libflux_jobtap_ldms_notifier.so", conf = {{ port = 412, message_channel = "hahaha" }} }},
 ]
 ...


SEE ALSO
========

:ref:`ldms-jobmgr_flux(7) <ldms-jobmgr_flux>`
