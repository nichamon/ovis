/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2025 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2025 Open Grid Computing, Inc. All rights reserved.
 *
 * Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
 * license for use of this work by or on behalf of the U.S. Government.
 * Export of this program may require a license from the United States
 * Government.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of Sandia nor the names of any contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *      Neither the name of Open Grid Computing nor the names of any
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *      Modified source versions must be plainly marked as such, and
 *      must not be misrepresented as being the original software.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file ldmsd_tenant.h
 * \brief LDMS Daemon Multi-Tenant Interface
 *
 * This module provides functionality for managing tenant definitions and their
 * associated metrics within the LDMS daemon. Tenants represent distinct entities
 * (such as jobs, users, steps or tasks) that can be tracked and monitored through
 * a list of attributes retrieved from the job manager.
 *
 * # Overview
 *
 * The multi-tenant infrastructure allows:
 * - Definition of tenant types with custom attribute sets
 * - Sampling of tenant information from the job manager
 * - Storage of tenant data in LDMS record lists
 *
 * # Usage Pattern
 *
 * 1. Create a tenant definition with ldmsd_tenant_def_create()
 * 2. Add the tenant schema to an LDMS schema with ldmsd_tenant_schema_list_add()
 * 3. Sample tenant values periodically with ldmsd_tenant_values_sample()
 * 4. Release the definition when done with ldmsd_tenant_def_free()
 *
 * # Thread Safety
 *
 * The tenant definition list is protected by an internal mutex. Individual
 * tenant definitions use reference counting for safe concurrent access.
 */

#ifndef __LDMSD_TENANT_H__
#define __LDMSD_TENANT_H__

#include <pthread.h>

#include "ovis_ref/ref.h"
#include "ovis_log/ovis_log.h"

#include "ldmsd.h"
#include "ldmsd_jobmgr_query.h"

/** \brief Default initial number of tenants to allocate space for */
#define LDMSD_TENANT_NUM_DEFAULT 10

/** \brief Standard name for tenant record definition in LDMS schemas */
#define LDMSD_TENANT_REC_DEF_NAME "tenant_def"

/** \brief Standard name for tenant list metric in LDMS schemas */
#define LDMSD_TENANT_LIST_NAME "tenants"

/**
 * \brief Tenant metric structure
 *
 * Represents a single attribute (metric) within a tenant definition.
 * Each metric corresponds to a field that will be queried from the job
 * manager and stored in the tenant record.
 *
 * The metric template contains the core metadata (name, type, unit) that
 * defines the metric's schema. The __rent_id field provides an internal
 * reference for accessing the metric within the LDMS record definition.
 *
 * \note This structure is managed internally by the tenant system.
 *       Users typically don't interact with it directly.
 */
typedef struct ldmsd_tenant_metric_s {
	struct ldms_metric_template_s mtempl; /**< Metric template */
	/* Fields internal to tenant core logic */
	int __rent_id; /**< Reference to metric in LDMS record definition */
	TAILQ_ENTRY(ldmsd_tenant_metric_s) ent; /**< List linkage */

} *ldmsd_tenant_metric_t;

/**
 * \brief List type for tenant metrics
 *
 * Maintains an ordered list of metrics that comprise a tenant definition.
 * The order of metrics in this list determines their layout in the
 * tenant record.
 */
TAILQ_HEAD(ldmsd_tenant_metric_list, ldmsd_tenant_metric_s);

/**
 * \brief Tenant definition structure
 *
 * Represents a complete definition of a tenant type, including all its
 * attributes (metrics) and the necessary metadata for querying and storing
 * tenant information.
 *
 * A tenant definition serves as a template that can be reused across
 * multiple LDMS sets. It defines:
 * - The set of attributes to query from the job manager
 * - The structure of the LDMS record for storing tenant data
 * - The memory requirements for tenant storage
 *
 * Tenant definitions are reference-counted and can be safely shared
 * across multiple contexts.
 *
 * \note Once created, a tenant definition is immutable. To change the
 *       definition, create a new one with a different name.
 */
typedef struct ldmsd_tenant_def_s {
	struct ldmsd_cfgobj obj;              /**< Base configuration object */

	int deleted;                          /**< 1 if ldmsd_tenant_def_del() was called */
	int mcount;                           /**< Number of metrics in mlist */
	struct ldmsd_tenant_metric_list mlist; /**< List of tenant metrics */
	ldms_record_t recdef;                 /**< Record definition for tenant data */
	int tenant_recdef_mid;                /**< Metric ID of record definition in schema */
	int tenant_list_mid;                  /**< Metric ID of tenant list in schema */
	const char *schema_name;              /**< Name of the LDMS schema */
	ldms_schema_t schema;                 /**< LDMS schema object */
	int *mids;                            /**< Array of metric IDs in schema */
	const char *set_name;                 /**< Name of the LDMS set */
	ldms_set_t set;                       /**< LDMS set containing tenant records */
	size_t heap_sz;                       /**< Heap size allocated for set */
	size_t rec_def_heap_sz;               /**< Heap size needed per tenant record */
	ldmsd_jobmgr_query_t jquery;          /**< Job manager query for event subscription */

	/* Key field for binding tenants with sets/metrics */
	const char *key_field_name;           /**< Name of the binding key field */
	int key_mid;                          /**< Metric ID of the binding key field */

	/* Multi-tenant Notification */
	pthread_mutex_t event_consumer_lock;  /**< Mutex protecting event_consumer_list */
	TAILQ_HEAD(, tenant_event_consumer_s) event_consumer_list; /**< List of registered event consumers */
} *ldmsd_tenant_def_t;

/**
 * \brief Create tenant definition from attribute list
 *
 * Creates a new tenant definition that specifies which attributes should be
 * tracked for tenants. Each attribute in the list must be a valid metric name
 * recognized by the job manager.
 *
 * A tenant is defined as a unique combination of attribute values. Two tenants
 * are considered distinct if they differ in any attribute value specified in
 * the definition.
 *
 * \param name      Unique name for this tenant definition (used for lookup and reuse)
 * \param str_list  List of attribute names to include in the tenant definition.
 *                  Each string must match a valid job manager metric name.
 *
 * \return Handle to the newly created tenant definition on success.
 *         Returns NULL on failure with errno set to:
 *         - EEXIST: A definition with this name already exists
 *         - ENOMEM: Memory allocation failed
 *         - EINVAL: One or more attributes are not recognized by the job manager
 *         - Other errors from job manager initialization
 *
 * \note The returned handle has an initial reference count. Call
 *       ldmsd_tenant_def_free() when done using it.
 *
 * \warning If a definition with the same name exists, this function fails.
 *          Use ldmsd_tenant_def_find() to reuse existing definitions.
 */
struct ldmsd_tenant_def_s *
ldmsd_tenant_def_create(const char *name, struct ldmsd_str_list *str_list);


/**
 * \brief Create a tenant definition with complete configuration
 *
 * Creates a new tenant definition that specifies which attributes should be
 * tracked for tenants. Each attribute in the list must be a valid metric name
 * recognized by the job manager. This function allows full control over all
 * tenant definition parameters including permissions.
 *
 * A tenant is defined as a unique combination of attribute values. Two tenants
 * are considered distinct if they differ in any attribute value specified in
 * the definition.
 *
 * The key_name parameter specifies which attribute to use as a binding key
 * for correlating tenant data with other metrics or sets (e.g., SLURM_JOB_UUID).
 *
 * \param name          Unique name for this tenant definition (used for lookup and reuse)
 * \param dameon_name   Daemon name (typically the ldmsd instance name).
 *                      Used to construct the internal set name.
 * \param attr_list     List of attribute names to include in the tenant definition.
 *                      Each string must match a valid job manager metric name.
 * \param key           Key field name for binding with other metrics.
 *                      If NULL, the default key ("job_id") is used.
 * \param uid           User ID for access control on the tenant definition
 * \param gid           Group ID for access control on the tenant definition
 * \param perm          Permission bits (e.g., 0600) for access control
 *
 * \return Handle to the newly created tenant definition on success.
 *         errno is set on failure:
 *         - EEXIST: A definition with this name already exists
 *         - ENOMEM: Memory allocation failed
 *         - EINVAL: One or more attributes are not recognized by the job manager
 *         - Other errors: Job manager initialization failures
 *
 * \note The returned handle has an initial reference count. Call
 *       ldmsd_tenant_def_del() when done using it.
 *
 * \note The tenant definition is automatically registered with the
 *       configuration object system for discovery and management.
 *
 * \warning If a definition with the same name exists, this function fails
 *          with EEXIST. Use ldmsd_tenant_def_find() to reuse existing
 *          definitions instead of creating duplicates.
 *
 * \see ldmsd_tenant_def_find() - Find an existing definition by name
 * \see ldmsd_tenant_def_del() - Delete a definition
 * \see ldmsd_tenant_event_register() - Register to receive events
 */
ldmsd_tenant_def_t ldmsd_tenant_def_new(const char *name, const char *dameon_name,
					struct ldmsd_str_list *attr_list, const char *key,
					uid_t uid, gid_t gid, int perm);

/**
 * \brief Delete a tenant definition
 *
 * Removes a tenant definition from the system and releases its resources.
 * This function marks the definition for deletion and closes its job manager
 * query subscription. The definition resources are freed once all references
 * are released.
 *
 * \param tdef    Tenant definition handle to delete
 *
 * \return 0 on success
 * \return Non-zero error code on failure
 *
 * \note This function should be called once for each definition created.
 *       After calling this function, the handle should not be used.
 *
 * \note The deletion is asynchronous. The definition is marked for deletion,
 *       but actual resource cleanup occurs when all references are released
 *       by the reference counting system.
 *
 * \warning After calling this function, do not attempt to use the handle
 *          or access the tenant definition in any way.
 *
 * \see ldmsd_tenant_def_new() - Create a definition
 * \see ldmsd_tenant_def_find() - Find a definition for access
 */
int ldmsd_tenant_def_del(ldmsd_tenant_def_t tdef);

/**
 * \brief Find an existing tenant definition by name
 *
 * Searches for a previously created tenant definition with the given name.
 * If found, increments the reference count and returns the handle.
 *
 * \param name  Name of the tenant definition to find
 *
 * \return Handle to the tenant definition if found
 * \return NULL if not found
 *
 * \note Must call ldmsd_tenant_def_find_put() to release the reference when done.
 *
 * \see ldmsd_tenant_def_new() - Create a new definition
 * \see ldmsd_tenant_def_find_put() - Release the reference
 */
static inline ldmsd_tenant_def_t ldmsd_tenant_def_find(const char *name)
{
	return (ldmsd_tenant_def_t)ldmsd_cfgobj_find_get(name, LDMSD_CFGOBJ_TENANT);
}

/**
 * \brief Release a reference obtained from ldmsd_tenant_def_find()
 *
 * Decrements the reference count of a tenant definition found via
 * ldmsd_tenant_def_find(). This must be called once for each successful
 * find() call.
 *
 * \param tdef  Tenant definition handle from ldmsd_tenant_def_find()
 *
 * \see ldmsd_tenant_def_find() - Find a definition
 */
static inline void ldmsd_tenant_def_find_put(ldmsd_tenant_def_t tdef)
{
	ldmsd_cfgobj_find_put(&tdef->obj);
}

/**
 * \brief Lock a tenant definition for exclusive access
 *
 * Acquires a lock on the tenant definition, preventing other threads
 * from accessing or modifying it. This should be called before accessing
 * tenant data that might be modified by other threads.
 *
 * \param tdef  Tenant definition to lock
 *
 * \note Locks are not reentrant. A thread cannot acquire the same lock twice.
 *
 * \see ldmsd_tenant_def_unlock() - Release the lock
 *
 * \warning Always pair lock() with unlock(). Use in-place locks or RAII
 *          patterns to ensure unlock is called even on error.
 */
static inline void ldmsd_tenant_def_lock(ldmsd_tenant_def_t tdef) {
	ldmsd_cfgobj_lock(&tdef->obj);
}

/**
 * \brief Unlock a tenant definition
 *
 * Releases a lock previously acquired with ldmsd_tenant_def_lock().
 *
 * \param tdef  Tenant definition to unlock
 *
 * \see ldmsd_tenant_def_lock() - Acquire the lock
 *
 * \warning Must only be called after a successful lock() call.
 */
static inline void ldmsd_tenant_def_unlock(ldmsd_tenant_def_t tdef) {
	ldmsd_cfgobj_unlock(&tdef->obj);
}

/**
 * \brief Increment reference count for a tenant definition
 *
 * Increments the reference count of a tenant definition with the specified
 * name/tag. Used to track named references to the definition.
 *
 * \param tdef  Tenant definition to reference
 * \param name  Reference name/tag (for tracking purposes)
 *
 * \return The input tdef handle
 *
 * \see ldmsd_tenant_def_put() - Decrement reference count
 */
static inline ldmsd_tenant_def_t ldmsd_tenant_def_get(ldmsd_tenant_def_t tdef, char *name) {
	ldmsd_cfgobj_get(&tdef->obj, name);
	return tdef;
}

/**
 * \brief Decrement reference count for a tenant definition
 *
 * Decrements the reference count of a tenant definition with the specified
 * name/tag. Must be called once for each successful get() call.
 *
 * \param tdef  Tenant definition to release
 * \param name  Reference name/tag (for tracking purposes)
 *
 * \see ldmsd_tenant_def_get() - Increment reference count
 */
static inline void ldmsd_tenant_def_put(ldmsd_tenant_def_t tdef, char *name) {
	ldmsd_cfgobj_put(&tdef->obj, name);
}


// /**
//  * \brief Calculate heap size required for tenant storage
//  *
//  * Computes the total heap memory needed to store a specified number of
//  * tenant records in an LDMS set. This should be used when creating or
//  * resizing LDMS sets to ensure adequate heap space.
//  *
//  * \param tdef         Tenant definition handle
//  * \param num_tenants  Expected number of concurrent tenants
//  *
//  * \return Size in bytes required for heap allocation
//  *
//  * \note If the actual number of tenants exceeds num_tenants at runtime,
//  *       ldmsd_tenant_values_sample() will return ENOMEM and the set
//  *       will need to be resized.
//  */
// size_t ldmsd_tenant_heap_sz_get(struct ldmsd_tenant_def_s *tdef, int num_tenants);

// /**
//  * \brief Add tenant metrics to an LDMS schema
//  *
//  * Adds the necessary record definition and list metric to an LDMS schema
//  * for storing tenant information. This creates:
//  * 1. A record definition describing the structure of individual tenant records
//  * 2. A list metric that will contain the actual tenant instances
//  *
//  * \param tdef                  Tenant definition handle
//  * \param schema                LDMS schema to add metrics to
//  * \param num_tenants           Estimated maximum number of concurrent tenants
//  * \param _tenant_rec_mid       [out] Optional pointer to receive the metric index
//  *                              of the tenant record definition
//  * \param _tenants_mid          [out] Optional pointer to receive the metric index
//  *                              of the tenant list metric
//  *
//  * \return 0 on success. On failure, returns a positive error code:
//  *         - ENOMEM: Memory allocation failed
//  *         - Other positive error codes from LDMS schema operations
//  *
//  * \note The returned metric indices should be saved for use with
//  *       ldmsd_tenant_values_sample().
//  */
// int ldmsd_tenant_schema_list_add(struct ldmsd_tenant_def_s *tdef,
// 				 ldms_schema_t schema, int num_tenants,
// 				 int *_tenant_rec_mid, int *_tenants_mid);

// /**
//  * \brief Sample and update tenant values in an LDMS set
//  *
//  * \param tdef            Tenant definition handle
//  * \param set             LDMS set to update
//  * \param tenant_rec_mid  Metric ID of the tenant record definition
//  *                        (obtained from ldmsd_tenant_schema_list_add())
//  * \param tenants_mid     Metric ID of the tenant list metric
//  *                        (obtained from ldmsd_tenant_schema_list_add())
//  *
//  * \return 0 on success. On failure, returns a positive error code:
//  *         - ENOENT: Could not retrieve the tenant list metric from the set
//  *         - ENOMEM: Insufficient heap space in the set for the current number
//  *                   of tenants. The set should be resized with a larger heap
//  *                   and this function called again.
//  *
//  * \warning When ENOMEM is returned, the tenant list may be partially updated.
//  *          The caller should resize the set and retry the sampling operation.
//  *
//  * \note If no tenants are currently running, the function succeeds and leaves
//  *       the tenant list empty (after purging any previous contents).
//  */
// int ldmsd_tenant_values_sample(struct ldmsd_tenant_def_s *tdef, ldms_set_t set,
// 			       int tenant_rec_mid, int tenants_mid);

/**
 * \brief Convert a tenant definition to JSON string
 *
 * Serializes a tenant definition to a JSON-formatted string and appends it
 * to the provided request context's line buffer.
 *
 * \param tdef     Tenant definition handle
 * \param summary  Summary flag (non-zero = summary mode, zero = full details)
 * \param reqc     [in,out] Request context to receive the formatted string
 *
 * \return 0 on success
 * \return ENOMEM if the buffer could not be reallocated for the output
 *
 * \note The formatted string is appended to reqc's existing buffer content.
 *       The caller is responsible for managing the request context lifetime.
 */
int ldmsd_tenant_def_str(ldmsd_tenant_def_t tdef, int summary, ldmsd_req_ctxt_t reqc);

/**
 * \brief Get the key field name from a tenant definition
 *
 * Retrieves the key field name that is used for binding this tenant definition
 * with other metrics or sets. The key field is specified when the tenant
 * definition is created.
 *
 * \param tdef       Tenant definition handle
 * \param key_name   [out] Pointer to receive the key field name string.
 *                   The returned string is borrowed from the tenant definition
 *                   and must NOT be freed by the caller.
 *
 * \return Index of the key field metric on success
 * \return -ENOENT if the tenant definition has no key field defined
 *
 * \note The returned key_name pointer is valid only while the tenant
 *       definition exists. Do not use it after the definition is deleted.
 *
 * \see ldmsd_tenant_def_new() - Key field is specified at creation time
 */
int ldmsd_tenant_def_get_key_name(ldmsd_tenant_def_t tdef, const char **key_name);

/**
 * \brief Get the metric index of an attribute in a tenant definition
 *
 * Looks up an attribute (metric) name in the tenant definition's job manager
 * query record and returns its index. This index can be used with LDMS record
 * functions to access the metric value.
 *
 * \param tdef       Tenant definition handle
 * \param attr_name  Name of the attribute to look up (e.g., "job_id", "task_pid")
 *
 * \return Non-negative index if the attribute is found
 * \return Negative error code if not found:
 *         - -EINVAL if tdef is invalid or query not initialized
 *         - Other negative values for query lookup failures
 *
 * \see ldmsd_jobmgr_query_field_index() - Similar functionality for queries
 *
 * \note The index is valid only while the tenant definition exists.
 */
int ldmsd_tenant_def_attr_index(ldmsd_tenant_def_t tdef, const char *attr_name);

/* Multi-tenant Event Notification API */

/**
 * \brief Callback function type for tenant-level job events
 *
 * This callback is invoked when job/step/task lifecycle events occur within
 * the tenant. The function is called with complete job metadata through the
 * qrec parameter, allowing consumers to react to job lifecycle changes.
 *
 * \param tdef        Tenant definition that originated the event
 * \param event_type  Type of event (job_start, job_end, task_start, etc.)
 * \param qrec        Record containing all jobmgr metrics for this event.
 *                    This includes job_id, task_pid, timestamps, and other
 *                    metrics configured in the tenant definition.
 * \param cb_arg      User context pointer passed to ldmsd_tenant_event_register()
 *
 * \return 0 on success
 * \return Non-zero error code on failure (handler should log the error)
 *
 * \note The qrec record is only valid during the callback execution.
 *       Do not save pointers to its contents.
 *
 * \warning This callback should complete quickly. Long-running operations
 *          may block other event processing.
 *
 * \see ldmsd_tenant_event_register() - Register a callback function
 * \see ldmsd_jobmgr_query_event_type_e - Event type definitions
 */
typedef int (*ldmsd_tenant_event_cb_fn_t)(ldmsd_tenant_def_t tdef,
					  ldmsd_jobmgr_query_event_type_t event_type,
					  ldms_mval_t qrec,
					  void *cb_arg
);



typedef struct tenant_event_consumer_s *ldmsd_tenant_event_consumer_t;

/**
 * \brief Register a consumer for tenant-level job events
 *
 * Registers a callback function to receive job lifecycle event notifications
 * from a tenant definition. The callback will be invoked for all events
 * (job_start, job_end, task_start, task_end, etc.) that occur within the
 * tenant.
 *
 * Multiple consumers can be registered for the same tenant definition.
 * Each registered consumer will receive all events.
 *
 * \param tdef        Tenant definition to monitor for events
 * \param event_cb    Callback function to invoke on events.
 *                    Must not be NULL.
 * \param cb_arg      User context pointer to pass to the callback.
 *                    Can be NULL if not needed.
 *
 * \return Handle for later unregistration on success
 * \return NULL on error (e.g., memory allocation failure)
 *
 * \note The returned handle should be saved if you plan to later call
 *       ldmsd_tenant_event_unregister() to stop receiving events.
 *
 * \see ldmsd_tenant_event_unregister() - Unregister a consumer
 * \see ldmsd_tenant_event_cb_fn_t - Callback function type definition
 *
 * \warning The callback function may be invoked from any thread processing
 *          job manager events. Callback implementations should be
 *          thread-safe.
 */
ldmsd_tenant_event_consumer_t
ldmsd_tenant_event_register(ldmsd_tenant_def_t tdef,
			    ldmsd_tenant_event_cb_fn_t event_cb,
			    void *cb_arg);

/**
 * \brief Unregister a consumer from tenant events
 *
 * Stops a previously registered consumer from receiving events. After this
 * call, the consumer will no longer receive any event callbacks, and the
 * returned handle becomes invalid.
 *
 * \param consumer    Handle returned from ldmsd_tenant_event_register().
 *                    Must not be NULL.
 *
 * \return 0 on success
 * \return Negative error code on failure
 *
 * \note After unregistration, the handle should not be reused.
 *       It is the caller's responsibility to not invoke this function
 *       twice with the same handle.
 *
 * \see ldmsd_tenant_event_register() - Register a consumer
 */
int ldmsd_tenant_event_unregister(ldmsd_tenant_event_consumer_t consumer);

#endif