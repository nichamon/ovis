.. _ldmsd_multi_tenant:

==================
ldmsd_multi_tenant
==================

-------------------------------------
Manual for LDMSD Multi-Tenant Feature
-------------------------------------

:Date: September 2025
:Manual section: 7
:Manual group: LDMSD

SYNOPSIS
========

tenant_add
   name=\ *NAME* attributes=\ *ATTRIBUTES* [producer=\ *PRODUCER*] [key=\ *KEY ATTRIBUTE*]

tenant_del
   name=\ *NAME*

DESCRIPTION
===========

The multi-tenant feature enables users to track and monitor multiple concurrent
tenants (such as jobs, users, or tasks). Users can define custom tenant
definitions specifying which attributes to track. The multi-tenant system then
creates dedicated LDMS sets to store tenant-specific data, independent of other
metric sets.

The multi-tenant system integrates with the LDMS job manager interface to query
tenant attributes dynamically. Each tenant definition creates its own set where
the list of active tenants and their attribute values are stored and updated.

Key Concepts
------------

**Tenant Definition**
  A named template that specifies which attributes identify a tenant. Examples:

  - A job-level tenant: ``job_id``
  - A job-step tenant: ``job_id, step_id``
  - A task-level tenant: ``job_id, step_id, task_id``

**Tenant Attributes**
  Individual properties that describe a tenant, such as ``job_id``, ``user_id``,
  ``step_id``, or ``task_id``. These must be valid attributes recognized by the
  job manager.

**Tenant Instance**
  A specific combination of attribute values representing one tenant. For example,
  with a definition of ``job_id, step_id``, the instance ``{job_id=12345, step_id=2}``
  represents one tenant.

**Tenant List**
  A list metric within an LDMS set containing all currently active tenant instances.
  This list is updated each time the sampler runs.

Workflow
--------

1. **Define tenant types** using ``tenant_add`` to specify which attributes
   constitute a tenant

2. **Tenant sets are created automatically** - each tenant definition has its own
   dedicated set for storing tenant instances

3. **Tenant data is collected independently** - the system automatically queries and
   updates tenant information on each sample interval

CONFIGURATION
=============

Tenant Definition Command
--------------------------

**tenant_add** creates a new tenant definition.

   name=NAME
      Unique identifier for this tenant definition. This name is referenced when
      configuring samplers.

   attributes=ATTRIBUTES
      Comma-separated list of attribute names that define a tenant. Each attribute
      must be a valid attribute available from the job manager. The order of attributes
      matters - two tenants are considered different if they differ in any attribute
      value.
  [producer=PRODUCER]
      Producer name. This is the same as `producer` at the `config` command. Default is <hostname>:<listening port>.
  [key=KEY_ATTRIBUTE]
      An attribute name that is the identifier of a tenant. It must be a valid
      attribute available from the job manager. It may or may not be in the
      comma-separated list. Default is 'job_id'.

**tenant_del** deletes a tenant definition

   name=NAME
      The name of the tenant definition to be deleted

**Example:**

.. code-block:: bash

   # Define a tenant by job, step, and task
   tenant_add name=job_step_task_tenant attributes=job_id,step_id,task_id

   # Define a tenant by job only
   tenant_add name=job_tenant attributes=job_id

   # Define a tenant by job and user
   tenant_add name=job_user_tenant attributes=job_id,user_id

Sampler Integration
-------------------

Tenant tracking is managed independently through tenant definitions. Once a tenant
definition is created with ``tenant_add``, a dedicated set is automatically
created to store tenant data. Samplers do not require special configuration to
participate in tenant tracking - the system automatically handles tenant data
collection for all defined tenant types.

EXAMPLES
========

Basic Configuration
-------------------

Track jobs with their steps and tasks:

.. code-block:: bash

   # Define tenant structure
   tenant_add name=job_step_task_tenant attributes=job_id,step_id,task_id

   # The system automatically creates a dedicated set for tenant tracking
   # No additional sampler configuration needed

Multiple Tenant Definitions
----------------------------

Create multiple tenant definitions for different tracking granularities:

.. code-block:: bash

   # Define multiple tenant types
   tenant_add name=job_tenant attributes=job_id
   tenant_add name=job_step_tenant attributes=job_id,step_id

   # Each definition creates its own dedicated set
   # The system automatically manages tenant data collection for both

Complete Configuration File
----------------------------

.. code-block:: bash

   # samplerd.conf

   # Define what a tenant is for this system
   tenant_add name=job_step_task_tenant attributes=job_id,step_id,task_id

   # Load and configure sampler plugins (no tenant parameter needed)
   load name=meminfo
   config name=meminfo producer=nid0001 instance=nid0001/meminfo
   start name=meminfo interval=1s

   load name=vmstat
   config name=vmstat producer=nid0001 instance=nid0001/vmstat
   start name=vmstat interval=1s

DATA STRUCTURE
==============

Each tenant definition creates its own dedicated LDMS set containing:

**tenant_def**
  Record definition describing the structure of tenant records (includes all
  attributes specified in the tenant definition)

**tenants**
  List of active tenant instances, where each element is a record containing
  the values of all tenant attributes

The tenant list is updated periodically. Tenants that have terminated are
automatically removed, and new tenants are added.

NOTES
=====

- Tenant definitions must be created via ``tenant_add`` commands

- Each tenant definition automatically creates its own dedicated set

- Multiple tenant definitions can coexist and operate independently

- Tenant attributes must be valid attributes provided by the configured job manager

- If more tenants exist than initially estimated, the set may need to be
  resized automatically (this is handled internally)

- If no tenants are active at update time, the tenant list will be empty

- Tenant tracking has minimal performance overhead - only a job manager query
  per update interval

SEE ALSO
========

:ref:`ldmsd(8) <ldmsd>`, :ref:`ldmsd_controller(8) <ldmsd_controller>`
