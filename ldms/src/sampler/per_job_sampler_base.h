/* per_job_sampler_base.h - Base library for per-job sampler plugins
 *
 * This library handles all the complex aspects of per-job sampling:
 * - Event registration with tenant infrastructure
 * - Job lifecycle management (create/destroy sets)
 * - PID tracking per job
 * - Transaction management
 * - Threading and synchronization
 *
 * Plugin authors only need to:
 * 1. Define their metrics (per-job and per-PID)
 * 2. Implement parsing/sampling callbacks
 * 3. Call base library functions
 */

#ifndef __PER_JOB_SAMPLER_BASE_H__
#define __PER_JOB_SAMPLER_BASE_H__

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_tenant.h"
#include "ovis_log/ovis_log.h"

/* Opaque types */
typedef struct per_job_base_s *per_job_base_t;
typedef struct per_job_sampler_s *per_job_sampler_t;

/* ============================================================================
 * Per-Job Metric Descriptor (for plugins to define per-job metrics)
 * ============================================================================ */

struct per_job_metric_desc_s {
	const char *name;
	enum ldms_value_type type;
	const char *unit;
	size_t count;  /* For arrays, 1 for scalars */
};

/* ============================================================================
 * Plugin Callbacks
 * ============================================================================
 *
 * Plugins implement these callbacks to define behavior.
 * All callbacks are optional unless marked as required.
 */

/**
 * Define per-job metrics to add to schema
 *
 * Called once during schema creation.
 * Plugin should return array of metric descriptors.
 *
 * \param metrics_out  [out] Pointer to array of metric descriptors
 * \param count_out    [out] Number of metrics in array
 * \param plugin_ctxt  Plugin's global context
 * \return 0 on success, error code on failure
 */
typedef int (*per_job_define_metrics_fn_t)(
	struct per_job_metric_desc_s **metrics_out,
	int *count_out,
	void *plugin_ctxt);

/**
 * Initialize per-job context when a job starts
 *
 * Called when first task of a job starts.
 * Plugin can allocate per-job context (e.g., cached metric indices).
 *
 * \param job_base      Handle to this job's base instance
 * \param job_ctxt_out  [out] Plugin's per-job context
 * \return 0 on success, error code on failure
 */
typedef int (*per_job_init_fn_t)(
	per_job_base_t job_base,
	void **job_ctxt_out);

/**
 * Clean up per-job context when a job ends
 *
 * Called when job ends.
 * Plugin should free job context allocated in job_init.
 *
 * \param job_base   Handle to this job's base instance
 * \param job_ctxt   Plugin's per-job context
 */
typedef void (*per_job_cleanup_fn_t)(
	per_job_base_t job_base,
	void *job_ctxt);

/**
 * Initialize a PID record when a task starts
 *
 * Called when a new task (PID) starts.
 * Plugin should parse /proc/<pid>/ files and populate the PID record.
 *
 * \param job_base   Handle to this job's base instance
 * \param pid        Process ID
 * \param pid_rec    PID record to populate
 * \param job_ctxt   Plugin's per-job context
 * \return 0 on success, error code on failure
 */
typedef int (*per_job_pid_added_fn_t)(
	per_job_base_t job_base,
	pid_t pid,
	ldms_mval_t pid_rec,
	void *job_ctxt);

/**
 * Handle a PID being removed (task ended)
 *
 * Called when a task ends.
 * Plugin can do cleanup if needed.
 *
 * \param job_base   Handle to this job's base instance
 * \param pid        Process ID that ended
 * \param job_ctxt   Plugin's per-job context
 */
typedef void (*per_job_pid_removed_fn_t)(
	per_job_base_t job_base,
	pid_t pid,
	void *job_ctxt);

/**
 * Sample this job (called during sample())
 *
 * REQUIRED callback.
 *
 * Called for each active job during sample().
 * Plugin should:
 * 1. Iterate PIDs and update per-PID metrics
 * 2. Compute and update per-job aggregate metrics
 *
 * Base handles transactions - plugin doesn't need to call
 * ldms_transaction_begin/end.
 *
 * \param job_base   Handle to this job's base instance
 * \param job_ctxt   Plugin's per-job context
 * \return 0 on success, error code on failure
 */
typedef int (*per_job_sample_fn_t)(
	per_job_base_t job_base,
	void *job_ctxt);

/* Plugin callbacks structure */
struct per_job_plugin_callbacks_s {
	/* Schema definition (optional) */
	per_job_define_metrics_fn_t define_per_job_metrics;

	/* Job lifecycle (optional) */
	per_job_init_fn_t job_init;
	per_job_cleanup_fn_t job_cleanup;

	/* PID lifecycle (optional) */
	per_job_pid_added_fn_t pid_added;
	per_job_pid_removed_fn_t pid_removed;

	/* Sampling (REQUIRED) */
	per_job_sample_fn_t sample_job;
};

/* ============================================================================
 * Configuration and Lifecycle
 * ============================================================================ */

/**
 * Create per-job sampler instance
 *
 * Handles all three deployment modes:
 * - Self-contained: tenant_name=NULL, key_field=NULL or "job_id"
 * - Custom key: tenant_name=NULL, key_field=<custom_field>
 * - Shared tenant: tenant_name=<tenant>, key_field ignored
 *
 * \param pi             Plugin handle
 * \param producer       Producer name
 * \param schema_name    Schema name
 * \param instance       Instance name (for self-contained mode)
 * \param tenant_name    Tenant definition name (NULL for self-contained)
 * \param key_field      Binding key field name (NULL for job_id)
 * \param pid_rec_def    PID record definition (from ldms_record_from_template)
 * \param max_pids       Maximum PIDs per job
 * \param callbacks      Plugin callbacks
 * \param plugin_ctxt    Plugin's global context (passed to callbacks)
 * \return Sampler instance on success, NULL on failure (errno set)
 */
per_job_sampler_t per_job_sampler_create(
	ldmsd_plug_handle_t pi,
	const char *producer,
	const char *schema_name,
	const char *instance,
	const char *tenant_name,
	const char *key_field,
	ldms_record_t pid_rec_def,
	int max_pids,
	struct per_job_plugin_callbacks_s *callbacks,
	void *plugin_ctxt);

/**
 * Destroy per-job sampler instance
 *
 * Cleans up all jobs, unregisters from tenant, destroys schema.
 *
 * \param sampler  Sampler instance
 */
void per_job_sampler_destroy(per_job_sampler_t sampler);

/**
 * Sample all active jobs
 *
 * Call this from plugin's sample() function.
 * Base handles:
 * - Iterating all active jobs
 * - Transaction management
 * - Calling plugin's sample_job callback for each job
 *
 * \param sampler  Sampler instance
 * \return 0 on success, error code on failure
 */
int per_job_sampler_sample(per_job_sampler_t sampler);

/* ============================================================================
 * Per-Job Base APIs (used inside callbacks)
 * ============================================================================
 *
 * These functions operate on a single job's base instance.
 * They are called from within plugin callbacks.
 */

/**
 * Get the LDMS set for this job
 *
 * \param job_base  Job base instance
 * \return LDMS set (do not free)
 */
ldms_set_t per_job_base_get_set(per_job_base_t job_base);

/**
 * Get the schema for this job
 *
 * \param job_base  Job base instance
 * \return Schema (do not free)
 */
ldms_schema_t per_job_base_get_schema(per_job_base_t job_base);

/**
 * Get job ID for this job
 *
 * \param job_base  Job base instance
 * \return Job ID string (do not free)
 */
const char *per_job_base_get_job_id(per_job_base_t job_base);

/**
 * Get binding key for this job
 *
 * \param job_base  Job base instance
 * \return Binding key string (do not free)
 */
const char *per_job_base_get_binding_key(per_job_base_t job_base);

/* ============================================================================
 * Per-Job Metric Access APIs
 * ============================================================================ */

/**
 * Get metric index for a per-job metric by name
 *
 * Use this in job_init to cache indices for fast access.
 *
 * \param job_base     Job base instance
 * \param metric_name  Metric name
 * \return Metric index (>=0) on success, -1 if not found
 */
int per_job_base_get_job_metric_index(per_job_base_t job_base,
				       const char *metric_name);

/**
 * Get metric index for a PID record metric by name
 *
 * Use this in job_init to cache indices for fast access.
 *
 * \param job_base     Job base instance
 * \param metric_name  Metric name in PID record
 * \return Metric index (>=0) on success, -1 if not found
 */
int per_job_base_get_pid_metric_index(per_job_base_t job_base,
				       const char *metric_name);

/**
 * Set a per-job metric (U64)
 */
void per_job_base_set_u64(per_job_base_t job_base, int metric_idx, uint64_t value);

/**
 * Set a per-job metric (U32)
 */
void per_job_base_set_u32(per_job_base_t job_base, int metric_idx, uint32_t value);

/**
 * Set a per-job metric (S64)
 */
void per_job_base_set_s64(per_job_base_t job_base, int metric_idx, int64_t value);

/**
 * Set a per-job metric (D64)
 */
void per_job_base_set_d64(per_job_base_t job_base, int metric_idx, double value);

/**
 * Get a per-job metric value
 *
 * \param job_base    Job base instance
 * \param metric_idx  Metric index
 * \return Metric value (do not free)
 */
ldms_mval_t per_job_base_get_metric(per_job_base_t job_base, int metric_idx);

/* ============================================================================
 * PID Iteration APIs
 * ============================================================================ */

/**
 * PID iterator callback
 *
 * \param job_base  Job base instance
 * \param pid       Process ID
 * \param pid_rec   PID record
 * \param arg       User argument
 * \return 0 to continue iteration, non-zero to stop
 */
typedef int (*per_job_pid_iterator_fn_t)(
	per_job_base_t job_base,
	pid_t pid,
	ldms_mval_t pid_rec,
	void *arg);

/**
 * Iterate over all PIDs in this job
 *
 * Calls callback for each PID record.
 *
 * \param job_base  Job base instance
 * \param callback  Iterator callback
 * \param arg       User argument passed to callback
 * \return 0 on success, error code on failure
 */
int per_job_base_foreach_pid(per_job_base_t job_base,
			      per_job_pid_iterator_fn_t callback,
			      void *arg);

/**
 * Get a specific PID record
 *
 * \param job_base  Job base instance
 * \param pid       Process ID
 * \return PID record on success, NULL if not found
 */
ldms_mval_t per_job_base_get_pid_record(per_job_base_t job_base, pid_t pid);

/**
 * Get number of PIDs in this job
 *
 * \param job_base  Job base instance
 * \return Number of active PIDs
 */
int per_job_base_get_pid_count(per_job_base_t job_base);

/* ============================================================================
 * Helper: Schema Creation with Additional Lists
 * ============================================================================
 *
 * For plugins like per_job_netdev that need additional lists beyond pid_list.
 */

/**
 * Add an additional list to the schema
 *
 * Call this AFTER per_job_sampler_create() but BEFORE any sets are created.
 * Typically called at end of plugin's config().
 *
 * \param sampler       Sampler instance
 * \param list_name     List metric name
 * \param rec_def       Record definition for list entries
 * \param max_entries   Maximum number of entries
 * \return Metric index on success, negative error code on failure
 */
int per_job_sampler_add_list(per_job_sampler_t sampler,
			      const char *list_name,
			      ldms_record_t rec_def,
			      int max_entries);

#endif /* __PER_JOB_SAMPLER_BASE_H__ */