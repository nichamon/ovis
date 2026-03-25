.. _sampler_plugin_developer_documentation:

==========================================
Sampler Plugin Developer Documentation
==========================================

.. contents:: Table of Contents
   :local:
   :depth: 2

Section 1: Threading Overview
==============================

Execution Context
-----------------

Plugin interfaces (``constructor()``, ``config()``, ``sample()``, ``destructor()``) are
not always called from the same thread. Understanding which thread calls which interface
is important because it determines what operations are safe to perform and whether
additional synchronization is needed. ldmsd uses three categories of threads:

**Worker threads** are created and managed by ldmsd. Their responsibilities are:

- Schedule samples
- Call ``sample()``
- Handle configuration commands (``load``, ``config``, ``start``, ``stop``, ``term``)
  in configuration files either from ``-c`` or ``-y``

**Dedicated sampling thread** — if the plugin instance is configured to use a dedicated
thread, ldmsd creates one when ``start`` is called and deletes it when ``stop`` is
called. Its sole responsibility is to call ``sample()`` for that plugin instance.

**IO threads** (also referred to as transport threads) are created and managed by Zap.
Their responsibilities are:

- Handle configuration commands (``load``, ``config``, ``start``, ``stop``, ``term``)
  sent by a remote interface (``ldmsd_controller`` and maestro)
- Deliver Stream and Message to sampler plugins

Section 2: API Threading Guarantees
=====================================

Each plugin interface may be called by different threads depending on how the command
that triggers it is delivered — whether from a configuration file processed at startup or
from a remote client such as ``ldmsd_controller`` or maestro. The following describes
which thread is responsible for calling each interface.

.. figure:: ../../images/sampler_plugin_developer/sampler_plugin_command_interface_map.png
   :alt: Diagram mapping config commands to plugin interface calls
   :align: center

   Config Commands to Sampler Plugin Interface Calls

- ``constructor()`` is called by the thread handling the load configuration command:

  - worker thread when the load command is in config files
  - IO thread when the load command is sent by a remote client such as
    ``ldmsd_controller`` or maestro

- ``usage()`` is called by the thread handling the usage configuration command:

  - IO thread typically calls ``usage()`` because it makes most sense for users to use
    it in ``ldmsd_controller``
  - ``usage()`` is expected to return an immutable string of a short description of the
    plugin and how to configure it

- ``config()`` is called by:

  - worker thread when the command is in config files
  - IO thread when the command is sent by a remote client such as ``ldmsd_controller``
    or maestro

- ``sample()`` is only called by a single thread:

  - a worker thread shared with other ldmsd's operations (e.g., other sampler plugin's
    ``sample()`` calls)
  - a dedicated thread created by ldmsd at the config time if the plugin instance is
    configured to have a dedicated sampling thread

- ``destructor()`` can be called by either a worker thread or an IO thread. It is called
  when the reference of the plugin configuration object reaches zero.

Beyond knowing which thread calls which interface, plugin authors can rely on a set of
ordering and availability guarantees that ldmsd enforces across the plugin lifecycle.
These are safe to assume regardless of which thread is active:

What Sampler Plugin Authors CAN Assume
---------------------------------------

- ``constructor()`` will be the first interface to be called
- ``destructor()`` will be the last interface to be called
- Plugin instance log handle is available when ``constructor()`` is called, so
  ``ldmsd_plug_log_get()`` can be called in ``constructor()``
- The plugin instance log handle exists when ``destructor()`` is called
- The plugin instance has been configured at least once
  (``config name=<plugin instance name> ...``) before ``sample()`` is called
- All plugin interfaces are protected by mutex, except ``usage()``, which may be called
  concurrently with ``config()`` and ``sample()``

Section 3: Concurrency Scenarios
==================================

This section describes which plugin operations can run concurrently and which
are mutually exclusive. ldmsd serializes most plugin operations to prevent race
conditions. usage() is the exception — it is not serialized because it only
returns an immutable string and therefore does not modify any plugin state.

Concurrent Operations
----------------------

``usage()`` may run concurrently with the other operations between ``constructor()`` and
``destructor()`` calls.

Mutually Exclusive Operations
------------------------------

``constructor()``, ``config()``, ``sample()``, and ``destructor()`` are mutually
exclusive. All these operations are serialized by ldmsd — only one executes at a time.

Section 4: Synchronization Mechanism
=======================================

This section describes how to write thread-safe sampler plugins. It covers two
distinct mechanisms: plugin-level locking for protecting plugin-owned data when
the plugin manages its own threads, and the LDMS set transaction API for
protecting metric consistency during updates.

Guidance for Plugin Authors
----------------------------

Sampler plugin authors do not need locks to synchronize between plugin operations
(``constructor()``, ``config()``, ``sample()``, ``destructor()``) because ldmsd
serializes all these operations — only one executes at a time. Plugin authors need to use
locks if and only if the plugin itself has multiple threads of execution.

What Locking Primitives to Use
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- Use standard ``pthread_mutex_t`` for protecting plugin-owned data
- Do **NOT** attempt to use ldmsd's internal cfgobj lock
- Initialize mutex in ``constructor()``, destroy in ``destructor()``

Lock Order Guideline
~~~~~~~~~~~~~~~~~~~~~

- If your plugin uses multiple locks, document your own lock acquisition order
- Never call ldmsd APIs (like ``ldmsd_set_deregister``) while holding plugin locks
- Be mindful of holding locks for unnecessary duration to optimize the sampler plugin
  performance
- Release all plugin-specific locks before returning any interfaces

When Locks Are NOT Needed
~~~~~~~~~~~~~~~~~~~~~~~~~~

- Protecting data that is only accessed by plugin operations (``constructor()``,
  ``config()``, ``sample()``, ``destructor()``)
- Changing metric values in LDMS sets do not need any locks

LDMS sets do not require explicit locking by plugin authors — thread safety is
handled by the transaction API.

Thread Safety of LDMS Sets
--------------------------

- LDMS sets have built-in transaction safety via "consistent/inconsistent" flags
- ``sample()`` must call ``ldms_transaction_begin()`` before updating a metric value and
  call ``ldms_transaction_end()`` after updating all metric values
- Set readers can call ``ldms_set_is_consistent()`` to verify that a set is not
  currently being updated
- Sampler plugin authors do **NOT** need additional locks for the LDMS set data itself
- LDMS transaction APIs are only needed to enable external set readers to verify inconsistent
  metric values. For example, an ldmsd aggregator automatically checks if the set
  is inconsistent so that it will not store a set that is in the inconsistent state

Available Locks
----------------

Do not call ``ldmsd_cfgobj_lock()``. The cfgobj lock protects internal ldmsd properties.
Sampler plugin authors should not use or depend on this lock. Use ``pthread_mutex_t`` for
plugin data.

Section 5: Plugin Lifecycle
============================

This section describes the lifecycle of a sampler plugin instance. A plugin
instance transitions through a defined set of states in response to
configuration commands. The state determines which plugin interfaces ldmsd may
call and which commands are valid at that point. Note that a plugin instance
must be stopped before it can be terminated — the term command is only valid
from the INIT or CONFIGURED states.

State Diagram
--------------

.. figure:: ../../images/sampler_plugin_developer/sampler_plugin_state_diagram.png
   :alt: Plugin lifecycle state diagram
   :align: center

   Sampler Plugin Lifecycle State Diagram

State Descriptions
-------------------

The table below describes each state in the plugin lifecycle, how it is entered, and
which operations are permitted in that state.

.. list-table::
   :header-rows: 1
   :widths: 15 20 20 25 20

   * - State
     - How to Enter
     - Plugin Status
     - Allowed Operations
     - Operation Calling Thread
   * - INIT
     - load command calls ``constructor()``
     - Plugin loaded but not configured
     - ``config()``, ``usage()``
     - Worker or IO
   * - CONFIGURED
     - ``config`` command calls ``config()``, or ``stop`` command after ``start`` command
     - Plugin configured
     - ``config()`` (reconfig & multi-config lines), ``usage()``
     - Worker or IO
   * - RUNNING
     - ``start`` command initiates sampling
     - Plugin actively sampling data
     - ``sample()``, ``usage()``
     - Worker or dedicated sampling thread (for ``sample()``); IO (for ``usage()``)
   * - TERMINATING
     - ``term`` command
     - Plugin being destroyed
     - ``destructor()``
     - Worker or IO (whoever drops last reference)

Section 6: Using the LDMS Message Service in Sampler plugins
============================================================

Terminology
-----------

**LDMS Message Bus** (declared in ``ldms_msg.h``, API prefix ``ldms_msg_``)
   The pub/sub routing layer in LDMS. Each message is published with a tag name;
   subscribers register callbacks against a tag name or regex pattern to receive
   matching messages. The Message Bus manages the routing and delivery of messages
   to authorized subscribers.

**Message channel** (declared in ``ldms_msg_chan.h``, API prefix ``ldms_msg_chan_``)
   A resilient mechanism that sits on top of the LDMS Message Bus. A message
   channel manages multiple message buses, reconnects if a peer disconnects,
   and queues data while the peer is down.

Checking Whether the Message Service Is Enabled
------------------------------------------------

``ldms_msg_is_enabled()`` returns whether the message service is active in this
ldmsd instance. This check applies to both the LDMS Message Bus API (``ldms_msg_``)
and the message channel API (``ldms_msg_chan_``). A plugin that uses either API should
call it before attempting to subscribe, publish, or create a channel:

.. code-block:: c

   if (!ldms_msg_is_enabled()) {
       /* handle disabled case */
   }

How to handle a disabled message service depends on the plugin:

- If the plugin requires the message service to function at all, return an error
  from ``constructor()`` to prevent the plugin from loading.
- If the plugin has modes that do not use the message service, return an error
  from ``config()`` only when the user configures a mode that requires it.

Plugins must not call ``ldms_msg_enable()`` to enable the service themselves.
Whether the message service is enabled is an ldmsd instance-level decision, not a
per-plugin one.

Overview: Which API Should a Plugin Use?
-----------------------------------------

The choice depends on whether the plugin needs its own connection to a peer.

A plugin that relies on connections already managed by ldmsd
uses the LDMS Message Bus API (``ldms_msg_``) directly. This covers publishing
messages within the current ldmsd process and subscribing to messages that arrive
on existing connections — no connection management is required from the plugin.

A plugin that needs its own dedicated and reliable connection to a peer uses the
message channel API (``ldms_msg_chan_``). The channel manages the connection
lifecycle — connecting, reconnecting, and buffering messages while the peer is
unreachable — whether the plugin is the active side connecting out or the passive
side listening for incoming connections.

.. list-table::
   :widths: 50 50
   :header-rows: 1

   * - Use case
     - API to use
   * - Plugin does not need its own connection to a peer
     - ``ldms_msg_subscribe()`` and/or ``ldms_msg_publish(NULL, ...)`` directly
   * - Plugin needs its own connection to a peer
     - ``ldms_msg_chan``

Key points:

- Within each case, the plugin can subscribe, publish, or both as needed.
- A single message channel can publish on any number of different tag names.
  A plugin has no reason to create more than one channel.
- Without a message channel, if the peer the plugin depends on disconnects,
  there is no reconnect logic and the plugin will not recover.

LDMS Message Bus API (``ldms_msg_``)
-------------------------------------

Functions Relevant to Sampler Plugins
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :widths: 55 45
   :header-rows: 1

   * - Function
     - Purpose
   * - ``ldms_msg_subscribe(match, is_regex, cb_fn, cb_arg, desc)``
     - Register a subscriber callback for a tag name or regex
   * - ``ldms_msg_client_close(client)``
     - Unsubscribe and begin teardown
   * - ``ldms_msg_publish(x, name, type, cred, perm, data, len)``
     - Publish a message; pass ``NULL`` for ``x`` to deliver within this ldmsd

Thread Safety
~~~~~~~~~~~~~

All three functions are thread-safe. They use internal locks (``__msg_rwlock`` and
per-tag rwlocks) that are independent of ldmsd's worker, IO, and sampling threads.
No plugin-held lock is acquired by these functions.

Appropriateness by Plugin Interface
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :widths: 34 13 13 13 13 14
   :header-rows: 1

   * - API
     - ``constructor()``
     - ``config()``
     - ``sample()``
     - ``destructor()``
     - callback
   * - ``ldms_msg_subscribe()``
     - Yes
     - Yes
     - Not appropriate [#msg1]_
     - Not appropriate [#msg1]_
     - Yes [#msg2]_
   * - ``ldms_msg_client_close()``
     - No
     - Yes
     - Not appropriate [#msg1]_
     - Yes
     - Yes [#msg3]_
   * - ``ldms_msg_publish(NULL,...)``
     - Yes
     - Yes
     - Yes
     - No
     - Yes [#msg4]_

.. rubric:: Notes

.. [#msg1] Not a thread safety issue. Subscribing or unsubscribing on every
   ``sample()`` call would create duplicate subscriptions or repeated teardowns.
   Subscriptions are lifecycle operations that belong in ``constructor()`` and
   ``config()``.

.. [#msg2] Safe from a locking standpoint. Be aware of teardown ordering if
   calling ``ldms_msg_client_close()`` from within a callback — see
   `Subscription Teardown and Resource Safety`_.

.. [#msg3] Safe from a locking standpoint. See
   `Subscription Teardown and Resource Safety`_ for correct usage.

.. [#msg4] Safe from a locking standpoint. However, ``ldms_msg_publish(NULL, ...)``
   invokes subscriber callbacks inline before returning. If the plugin holds an
   internal lock when calling ``ldms_msg_publish()``, and any subscriber's callback
   tries to acquire the same lock, a deadlock results. See
   `Message Event Callback`_ below.

Subscribing to Message Tags
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Call ``ldms_msg_subscribe()`` to register a callback for a tag name or regex. This
is typically done in ``constructor()`` if the tag name is fixed, or in ``config()``
when the tag name is provided by the user.

.. code-block:: c

   plugin->client = ldms_msg_subscribe(
			"job_events",   /* tag name or regex pattern */
			0,              /* is_regex: 0 = exact match, 1 = regex */
			my_callback,
			plugin,         /* cb_arg passed to callback */
			"myplugin job event subscription"  /* description, for diagnostics */
			);
   if (!plugin->client) {
       /* handle error */
   }

Pass ``is_regex=0`` for exact tag name matching. Pass ``is_regex=1`` to match all
tags whose names match the provided regular expression.

A plugin may subscribe to multiple message tags by calling ``ldms_msg_subscribe()``
multiple times, once per tag or regex. Each call returns a separate client handle.
All client handles must be closed when they are no longer needed. Failing to close
a subscription means the callback continues to be invoked after the plugin no
longer expects it.

When ``config()`` is called, the plugin should evaluate which subscriptions are
still needed, close any that are no longer needed with ``ldms_msg_client_close()``,
and open any new ones with ``ldms_msg_subscribe()``. Closing a subscription is
asynchronous — see `Subscription Teardown and Resource Safety`_ for how to wait
safely before re-subscribing or freeing resources.

Publishing Message Tags Within This ldmsd
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Pass ``NULL`` as the first argument to deliver the message to all subscribers
within this ldmsd whose pattern matches the tag name. The call invokes matching
subscriber callbacks before returning. If no subscriber is registered for the tag,
the call returns 0 silently.

.. code-block:: c

   int rc = ldms_msg_publish(
			NULL,            /* x: NULL = deliver within this ldmsd */
			"job_events",    /* tag name */
			LDMS_MSG_JSON,   /* message type */
			NULL,            /* cred: NULL = use process euid/egid */
			0660,            /* permission bits */
			buf, len);

Message Event Callback
~~~~~~~~~~~~~~~~~~~~~~~

The thread invoking the callback depends on the origin of the message. For messages
originating from a remote peer, the callback is invoked by an IO thread. For
messages published within this ldmsd, the callback is invoked by whichever thread
called ``ldms_msg_publish()`` — which may be a worker thread or a dedicated
sampling thread.

**No** ``ldms_msg`` **locks are held when the callback is invoked.** All
``ldms_msg_*`` functions are safe to call from within the callback.

The callback receives an ``ldms_msg_event_t`` pointer. Always check ``ev->type``
first:

- ``LDMS_MSG_EVENT_RECV``: a message has arrived. Read message content from
  ``ev->recv``.
- ``LDMS_MSG_EVENT_CLIENT_CLOSE``: the subscription has been fully torn down.
  This is the last event guaranteed to be delivered to this client. Resources
  associated with the subscription may be safely freed at this point.

Fields available on ``LDMS_MSG_EVENT_RECV``:

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Field
     - Description
   * - ``ev->recv.name``
     - Tag name the message was published on
   * - ``ev->recv.data``
     - Message payload
   * - ``ev->recv.data_len``
     - Payload length in bytes
   * - ``ev->recv.type``
     - ``LDMS_MSG_STRING``, ``LDMS_MSG_JSON``, or ``LDMS_MSG_AVRO_SER``
   * - ``ev->recv.json``
     - Pre-parsed JSON root; valid only for ``LDMS_MSG_JSON`` messages delivered
       within this ldmsd; ``NULL`` otherwise
   * - ``ev->recv.cred``
     - uid/gid of the publisher
   * - ``ev->recv.perm``
     - Permission bits
   * - ``ev->hop_num``
     - Number of hops the message traversed

**Lock discipline when publishing from the callback:**
``ldms_msg_publish(NULL, ...)`` invokes subscriber callbacks inline. If the plugin
holds an internal lock when calling ``ldms_msg_publish()`` from within a callback,
and any subscriber of that published tag also tries to acquire the same lock, a
deadlock results. Avoid holding plugin-internal locks across a
``ldms_msg_publish()`` call inside a callback.

Subscription Teardown and Resource Safety
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``ldms_msg_client_close()`` begins teardown but returns before it is complete.
After it returns:

- No new ``LDMS_MSG_EVENT_RECV`` callbacks will be dispatched for this client.
- The ``LDMS_MSG_EVENT_CLIENT_CLOSE`` event will be delivered asynchronously by an
  internal thread. The plugin must not free resources used by the callback until
  this event arrives.
- The client handle must not be used after ``ldms_msg_client_close()`` is called.

Call ``ldms_msg_client_close()`` whenever a subscription is no longer needed — for
example, when ``config()`` replaces or removes a subscription, or when
``destructor()`` tears down all subscriptions before the plugin is unloaded.

A typical teardown pattern using a condition variable:

.. code-block:: c

   static int my_callback(ldms_msg_event_t ev, void *cb_arg) {
       struct myplugin_s *plugin = cb_arg;
       switch (ev->type) {
       case LDMS_MSG_EVENT_RECV:
           /* process message */
           break;
       case LDMS_MSG_EVENT_CLIENT_CLOSE:
           pthread_mutex_lock(&plugin->lock);
           plugin->client = NULL;
           pthread_cond_signal(&plugin->close_cond);
           pthread_mutex_unlock(&plugin->lock);
           /* Free resources specific to this subscription here */
           break;
       }
       return 0;
   }

   /* Call this whenever a subscription needs to be torn down */
   static void close_subscription(struct myplugin_s *plugin) {
       if (!plugin->client)
           return;
       ldms_msg_client_close(plugin->client);
       /* Wait for CLIENT_CLOSE before proceeding */
       pthread_mutex_lock(&plugin->lock);
       while (plugin->client != NULL)
           pthread_cond_wait(&plugin->close_cond, &plugin->lock);
       pthread_mutex_unlock(&plugin->lock);
       /* Safe to proceed: no more callbacks will be invoked */
   }

Call ``close_subscription()`` from ``config()`` before replacing a subscription,
and from ``destructor()`` for each active subscription before freeing plugin state.

Message Channel API (``ldms_msg_chan_``)
-----------------------------------------

Use a message channel when the plugin needs to manage its own reliable connection
to a peer — either connecting out to a remote peer or listening for an incoming
connection. The channel manages the transport automatically: it connects on
creation, reconnects after disconnection, and queues messages while the peer is
unreachable.

Functions Relevant to Sampler Plugins
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :widths: 60 40
   :header-rows: 1

   * - Function
     - Purpose
   * - ``ldms_msg_chan_new(...)``
     - Create a channel and begin connecting or listening
   * - ``ldms_msg_chan_publish(chan, tag, uid, gid, perm, type, msg, len)``
     - Enqueue a message for delivery to the remote peer
   * - ``ldms_msg_chan_close(chan, cancel)``
     - Close the channel
   * - ``ldms_msg_chan_set_q_limit(chan, bytes)``
     - Set the maximum queue depth in bytes (default 1 MB)

Thread Safety
~~~~~~~~~~~~~

All functions are thread-safe with respect to ldmsd's internal locks. They use
``chan->lock`` (a per-channel mutex) and the global ``chan_lock``, neither of which
conflict with ldmsd's worker, IO, or sampling threads.

Appropriateness by Plugin Interface
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :widths: 34 13 13 13 13 14
   :header-rows: 1

   * - API
     - ``constructor()``
     - ``config()``
     - ``sample()``
     - ``destructor()``
     - callback
   * - ``ldms_msg_chan_new()``
     - Yes
     - Yes
     - Not appropriate [#chan1]_
     - No
     - No
   * - ``ldms_msg_chan_publish()``
     - No
     - Yes
     - Yes [#chan2]_
     - No
     - Yes
   * - ``ldms_msg_chan_close()``
     - No
     - Yes
     - Not appropriate [#chan1]_
     - Yes
     - No
   * - ``ldms_msg_chan_set_q_limit()``
     - Yes
     - Yes
     - No
     - No
     - No

.. rubric:: Notes

.. [#chan1] Not a thread safety issue. Creating or closing a channel on every
   ``sample()`` call makes no design sense and wastes resources. These are
   lifecycle operations.

.. [#chan2] Appropriate, but be aware that the call may block if the queue is
   full. See `Blocking Behavior When the Queue Is Full`_ below.

Creating a Channel
~~~~~~~~~~~~~~~~~~~

Create the channel in ``constructor()`` or ``config()``. A channel in publish mode
requires the remote host and port. A single channel can publish on any number of
different tag names — there is no reason to create more than one channel per
plugin.

.. code-block:: c

   plugin->chan = ldms_msg_chan_new(
			"myplugin",          /* application name, for diagnostics */
			LDMS_MSG_CHAN_MODE_PUBLISH,
			"sock",              /* transport type */
			"aggregator-host",   /* remote host */
			411,                 /* remote port */
			NULL, 0,             /* local host/port: unused in publish mode */
			"none", NULL,        /* auth plugin and options */
			5                    /* reconnect interval in seconds */
   );
   if (!plugin->chan) {
       /* handle error */
   }

Creating the first channel in a process launches a scheduler thread
(``msg_chan_sched``) shared across all channels process-wide. Each channel also
gets its own I/O thread (``msg_chan_io``). The channel begins attempting to connect
to the remote peer immediately after creation.

Publishing Through a Channel
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   int rc = ldms_msg_chan_publish(
			plugin->chan,
			"job_events",        /* tag name */
			geteuid(), getegid(),
			0660,
			LDMS_MSG_JSON,
			buf, len);

The message is copied into the outbound queue and the I/O thread delivers it to
the remote peer when connected. If the peer is not yet connected, messages
accumulate in the queue and are sent once the connection is established.

Blocking Behavior When the Queue Is Full
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If the queue is full, ``ldms_msg_chan_publish()`` blocks the caller until space
becomes available in the queue or the channel is closed. The queue fills when the
remote peer is slow or disconnected and the publishing rate exceeds the send rate.

To reduce the likelihood of blocking in ``sample()``:

- Keep published message size and rate within the queue capacity for the expected
  reconnect duration.
- Adjust the queue limit with ``ldms_msg_chan_set_q_limit()`` if the default 1 MB
  is insufficient for the expected message rate and acceptable buffering window.

Closing a Channel
~~~~~~~~~~~~~~~~~~

Call ``ldms_msg_chan_close()`` when the channel is no longer needed. This applies
in ``destructor()`` when the plugin is being unloaded, and also in ``config()``
when the user changes the remote peer address and the plugin needs to replace the
existing channel with a new one pointing to a different peer.

.. code-block:: c

   ldms_msg_chan_close(plugin->chan, 0);  /* cancel=0: wait for queued messages */
   plugin->chan = NULL;

With ``cancel=0``, the call blocks until all queued messages have been delivered
to the peer and the channel is fully closed. With ``cancel=1``, queued messages
are abandoned and the channel closes immediately.

Why Not Use a Message Channel for Subscribing Inside ldmsd
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When a message channel is created in subscribe mode
(``LDMS_MSG_CHAN_MODE_SUBSCRIBE`` or ``LDMS_MSG_CHAN_MODE_BIDIR``), it creates a
listening transport endpoint bound to a local host and port. This is designed for
out-of-process programs that need a remote publisher to establish a transport
connection to them.

Inside ldmsd, this is unnecessary. Message tags published within this ldmsd are
delivered directly to ``ldms_msg_subscribe()`` callbacks without any transport
involvement. Using subscribe mode inside ldmsd creates a listener endpoint that
serves no purpose, consuming a port, a file descriptor, and transport resources.
For subscribing to message tags within ldmsd, use ``ldms_msg_subscribe()``
directly.

Property Comparison
--------------------

.. list-table::
   :widths: 40 30 30
   :header-rows: 1

   * - Property
     - LDMS Message Bus (``ldms_msg_``)
     - Message channel (``ldms_msg_chan_``)
   * - Header to include
     - ``ldms_msg.h``
     - ``ldms_msg_chan.h``
   * - Delivers to subscribers in this ldmsd
     - Yes
     - No
   * - Delivers to a remote peer over transport
     - No (sampler plugins do not manage ldmsd transport endpoints)
     - Yes (manages the connection)
   * - Auto-reconnect
     - No
     - Yes
   * - Outbound message queue
     - No
     - Yes (default 1 MB)
   * - Blocks when queue full
     - N/A
     - Yes, until space is available or channel is closed
   * - Regex subscription
     - Yes (``is_regex=1``)
     - Always regex
   * - Exact-match subscription
     - Yes (``is_regex=0``)
     - No
   * - ``CLIENT_CLOSE`` event delivered to plugin callback
     - Yes
     - No (handled internally)
   * - Threads created
     - None
     - 1 scheduler thread (shared process-wide) + 1 I/O thread per channel

Section 7: LDMSD Internal Implementation Details (For Maintainers Only)
=========================================================================

This section documents ldmsd-internal implementation details for ldmsd
maintainers. Plugin authors do not need to read this section.

Lock Acquisition Order
-----------------------

The following describes the acquisition order for the plugin cfgobj lock:

- Take when handling ``start``; release before replying back
- Take before calling ``sample()``; release after returning
- Take when handling ``stop``; release before replying back
- Take when handling ``term``:

  - Release before calling ``ldmsd_set_deregister()``
  - Re-acquire the lock
  - Release before removing the cfgobj from the tree

SEE ALSO
=========

:ref:`ldmsd(8) <ldmsd>` :ref:`ldmsd_controller(8) <ldmsd_controller>`
