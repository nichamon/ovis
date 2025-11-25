/* per_job_sampler_base.h */
#ifndef __PER_JOB_SAMPLER_BASE_H__
#define __PER_JOB_SAMPLER_BASE_H__

#include <pthread.h>
#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"
#include "ldmsd_tenant.h"
#include "ldmsd_jobmgr_query.h"

/**
 * \file per_job_sampler_base.h
 * \brief Base library for per-job sampler plugins
 *
 * This library provides infrastructure for developing per-job sampler plugins
 * that collect per-PID metrics correlated with job metadata. It handles:
 * - Configuration parsing (3 deployment modes)
 * - Tenant integration and event handling
 * - Per-job LDMS set lifecycle management
 * - Schema creation with PID metrics
 * - Field index caching
 * - Transaction management
 *
 * Plugin developers only need to:
 * 1. Define PID metrics
 * 2. Implement sample_pid() callback to parse /proc/<pid>/
 * 3. Call base APIs from standard plugin functions
 */

/**
 * Opaque handle for per-job sampler base.
 */
typedef struct per_job_base_s *per_job_base_t;

/**
 * Callback function type for sampling one PID.
 *
 * The base has already:
 * - Located the job set
 * - Located/created the PID record
 * - Started a transaction
 *
 * Plugin should:
 * - Read /proc/<pid>/ files
 * - Set metric values in the record
 * - Return 0 on success, errno on failure
 *
 * \param rec           PID record to populate
 * \param pid           Process ID to sample
 * \param plugin_ctxt   Plugin context (from per_job_base_set_pid_sampler)
 *
 * \return 0 on success, errno on failure
 */
typedef int (*per_job_base_sample_pid_fn_t)(ldms_mval_t rec,
                                             uint64_t pid,
                                             void *plugin_ctxt);

/**
 * \brief Parse configuration and initialize base infrastructure.
 *
 * Handles all three deployment modes:
 * - Self-contained: Creates internal tenant with default attributes
 * - Custom key: Creates internal tenant with specified key_field
 * - Shared tenant: References external tenant definition
 *
 * \param avl          Configuration attribute-value list
 * \param plugin_name  Name of the plugin (for logging)
 * \param def_schema   Default schema name if not specified
 * \param mylog        Plugin's log handle
 * \param[out] error   Human-readable error message (caller must free)
 *
 * \return Initialized per_job_base_t on success, NULL on failure (errno set)
 */
per_job_base_t per_job_base_config(struct attr_value_list *avl,
                                    const char *plugin_name,
                                    const char *def_schema,
                                    ovis_log_t mylog,
                                    char **error);

/**
 * \brief Create schema with standard fields + plugin-defined PID metrics.
 *
 * Standard fields added automatically:
 * - job_id (CHAR_ARRAY)
 * - binding_key (CHAR_ARRAY)
 *
 * \param base            Base handle from per_job_base_config()
 * \param pid_metrics     Array of metric templates for PID record
 *                        (terminated with {0})
 * \param max_num_pids    Maximum PIDs to track per job (default: 1024)
 *
 * \return A schema handle. NULL is returned and errno is set on errors
 */
ldms_schema_t per_job_base_schema_create(per_job_base_t base,
                                struct ldms_metric_template_s *pid_metrics,
                                int max_num_pids);

/**
 * \brief Register the PID sampling callback.
 *
 * This callback is invoked during per_job_base_sample() for each active PID
 * in each active job.
 *
 * \param base          Base handle
 * \param sample_fn     Function to sample one PID
 * \param plugin_ctxt   Context passed to sample_fn (usually plugin instance)
 *
 * \return 0 on success, errno on failure
 */
int per_job_base_set_pid_sampler(per_job_base_t base,
                                  per_job_base_sample_pid_fn_t sample_fn,
                                  void *plugin_ctxt);

/**
 * \brief Sample all active PIDs in all active jobs.
 *
 * For each job set:
 * - Iterate through PID records in the list
 * - Call plugin's sample_pid() callback
 * - Handle transactions automatically
 *
 * What it does:
 * 1. Lock job set list
 * 2. For each active job:
 *    a. Begin LDMS transaction
 *    b. Get PID record list
 *    c. For each PID record:
 *       - Extract PID value
 *       - Call plugin's sample_pid_fn(record, pid, plugin_context)
 *    d. End LDMS transaction
 * 3. Unlock job set list
 *
 * \param base  Base handle
 *
 * \return 0 on success, errno on failure
 */
int per_job_base_sample(per_job_base_t base);

/**
 * \brief Clean up and free all resources.
 *
 * - Unregister from tenant events
 * - Delete all per-job sets
 * - Delete internal tenant if owned
 * - Free schema and other resources
 *
 * \param base  Base handle (set to NULL after)
 */
void per_job_base_del(per_job_base_t base);

/**
 * \brief Get the producer name from configuration.
 *
 * \param base  Base handle
 * \return Producer name string (borrowed, don't free)
 */
const char *per_job_base_producer_get(per_job_base_t base);

#endif /* __PER_JOB_SAMPLER_BASE_H__ */
