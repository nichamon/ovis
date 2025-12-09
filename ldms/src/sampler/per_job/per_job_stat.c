/* per_job_stat.c - Per-job /proc/<pid>/stat sampler
 *
 * This plugin uses the per_job_sampler_base library to handle all the
 * complex job lifecycle, event handling, and threading concerns.
 *
 * Plugin author only needs to:
 * - Define PID metrics
 * - Parse /proc/<pid>/stat
 * - Implement sampling callback
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "per_job_sampler_base.h"

/* ============================================================================
 * Plugin Data Structures
 * ============================================================================ */

/* Plugin instance - simple! */
typedef struct per_job_stat_s {
	per_job_base_t base;  /* Base handles everything */
	ovis_log_t log;
} *per_job_stat_t;

/* Per-job context (cached indices for fast access) */
struct stat_job_ctxt_s {
	/* Cached PID record metric indices */
	int pid_midx;
	int comm_midx;
	int state_midx;
	int ppid_midx;
	int pgrp_midx;
	int session_midx;
	int utime_midx;
	int stime_midx;
	int cutime_midx;
	int cstime_midx;
	int num_threads_midx;
	int vsize_midx;
	int rss_midx;
	int processor_midx;
};

/* ============================================================================
 * PID Metrics Definition
 * ============================================================================ */

/* Define PID metrics from /proc/<pid>/stat */
static struct ldms_metric_template_s pid_metrics[] = {
	{"pid",         0, LDMS_V_U64,        "",        1},   /* Process ID */
	{"comm",        0, LDMS_V_CHAR_ARRAY, "",        16},  /* Command name */
	{"state",       0, LDMS_V_CHAR,       "",        1},   /* Process state */
	{"ppid",        0, LDMS_V_U64,        "",        1},   /* Parent PID */
	{"pgrp",        0, LDMS_V_U64,        "",        1},   /* Process group */
	{"session",     0, LDMS_V_U64,        "",        1},   /* Session ID */
	{"utime",       0, LDMS_V_U64,        "jiffies", 1},   /* User time */
	{"stime",       0, LDMS_V_U64,        "jiffies", 1},   /* System time */
	{"cutime",      0, LDMS_V_U64,        "jiffies", 1},   /* Children user time */
	{"cstime",      0, LDMS_V_U64,        "jiffies", 1},   /* Children system time */
	{"num_threads", 0, LDMS_V_U32,        "",        1},   /* Number of threads */
	{"vsize",       0, LDMS_V_U64,        "bytes",   1},   /* Virtual memory size */
	{"rss",         0, LDMS_V_U64,        "pages",   1},   /* Resident set size */
	{"processor",   0, LDMS_V_U32,        "",        1},   /* CPU number */
	{0}
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Parse /proc/<pid>/stat and populate PID record
 *
 * Format of /proc/<pid>/stat:
 * pid (comm) state ppid pgrp session tty_nr tpgid flags minflt cminflt
 * majflt cmajflt utime stime cutime cstime priority nice num_threads
 * itrealvalue starttime vsize rss ... processor ...
 */
static int parse_proc_stat(pid_t pid, ldms_mval_t pid_rec,
			   struct stat_job_ctxt_s *jctxt,
			   ovis_log_t log)
{
	char path[256];
	FILE *f;
	char comm[16];
	char state;
	uint64_t ppid, pgrp, session, utime, stime, cutime, cstime, vsize, rss;
	uint32_t num_threads, processor;
	int rc;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	f = fopen(path, "r");
	if (!f) {
		/* PID may have exited - not an error */
		return 0;
	}

	/* Parse the stat file - skip many fields with %*d or %*u */
	rc = fscanf(f, "%*d (%15[^)]) %c %lu %lu %lu %*d %*d %*u %*u %*u %*u %*u "
		       "%lu %lu %lu %lu %*d %*d %u %*d %*u %lu %lu %*u %*u %*u %*u "
		       "%*u %*u %*u %*u %*u %*u %*u %*u %*d %*d %u",
		    comm, &state, &ppid, &pgrp, &session,
		    &utime, &stime, &cutime, &cstime, &num_threads,
		    &vsize, &rss, &processor);

	fclose(f);

	if (rc != 13) {
		/* Partial parse - log but don't fail */
		ovis_log(log, OVIS_LDEBUG,
			 "Partial parse of /proc/%d/stat (got %d fields)\n", pid, rc);
		return 0;
	}

	/* Set metric values in the PID record */
	ldms_record_set_u64(pid_rec, jctxt->pid_midx, pid);
	ldms_record_array_set_str(pid_rec, jctxt->comm_midx, comm);
	ldms_record_set_char(pid_rec, jctxt->state_midx, state);
	ldms_record_set_u64(pid_rec, jctxt->ppid_midx, ppid);
	ldms_record_set_u64(pid_rec, jctxt->pgrp_midx, pgrp);
	ldms_record_set_u64(pid_rec, jctxt->session_midx, session);
	ldms_record_set_u64(pid_rec, jctxt->utime_midx, utime);
	ldms_record_set_u64(pid_rec, jctxt->stime_midx, stime);
	ldms_record_set_u64(pid_rec, jctxt->cutime_midx, cutime);
	ldms_record_set_u64(pid_rec, jctxt->cstime_midx, cstime);
	ldms_record_set_u32(pid_rec, jctxt->num_threads_midx, num_threads);
	ldms_record_set_u64(pid_rec, jctxt->vsize_midx, vsize);
	ldms_record_set_u64(pid_rec, jctxt->rss_midx, rss);
	ldms_record_set_u32(pid_rec, jctxt->processor_midx, processor);

	return 0;
}

/* ============================================================================
 * Plugin Callbacks
 * ============================================================================ */

/**
 * Initialize per-job context
 * Called once when job starts - cache metric indices for fast access
 */
static int job_init(per_job_base_t job_base, void **job_ctxt_out)
{
	struct stat_job_ctxt_s *jctxt = calloc(1, sizeof(*jctxt));
	if (!jctxt)
		return ENOMEM;

	/* Cache all PID metric indices for O(1) access during sampling */
	jctxt->pid_midx         = per_job_base_get_pid_metric_index(job_base, "pid");
	jctxt->comm_midx        = per_job_base_get_pid_metric_index(job_base, "comm");
	jctxt->state_midx       = per_job_base_get_pid_metric_index(job_base, "state");
	jctxt->ppid_midx        = per_job_base_get_pid_metric_index(job_base, "ppid");
	jctxt->pgrp_midx        = per_job_base_get_pid_metric_index(job_base, "pgrp");
	jctxt->session_midx     = per_job_base_get_pid_metric_index(job_base, "session");
	jctxt->utime_midx       = per_job_base_get_pid_metric_index(job_base, "utime");
	jctxt->stime_midx       = per_job_base_get_pid_metric_index(job_base, "stime");
	jctxt->cutime_midx      = per_job_base_get_pid_metric_index(job_base, "cutime");
	jctxt->cstime_midx      = per_job_base_get_pid_metric_index(job_base, "cstime");
	jctxt->num_threads_midx = per_job_base_get_pid_metric_index(job_base, "num_threads");
	jctxt->vsize_midx       = per_job_base_get_pid_metric_index(job_base, "vsize");
	jctxt->rss_midx         = per_job_base_get_pid_metric_index(job_base, "rss");
	jctxt->processor_midx   = per_job_base_get_pid_metric_index(job_base, "processor");

	*job_ctxt_out = jctxt;
	return 0;
}

/**
 * Clean up per-job context
 * Called when job ends
 */
static void job_cleanup(per_job_base_t job_base, void *job_ctxt)
{
	free(job_ctxt);
}

/**
 * Initialize a PID record when task starts
 * Called when new task (PID) appears
 */
static int pid_added(per_job_base_t job_base, pid_t pid,
		     ldms_mval_t pid_rec, void *job_ctxt)
{
	struct stat_job_ctxt_s *jctxt = job_ctxt;
	per_job_stat_t pjs = ldmsd_plug_ctxt_get(
		ldmsd_plug_handle_get_from_ctxt(per_job_base_get_set(job_base)));

	/* Parse /proc/<pid>/stat and populate record */
	return parse_proc_stat(pid, pid_rec, jctxt, pjs->log);
}

/**
 * PID iterator callback - updates one PID's metrics
 */
struct sample_pid_ctxt_s {
	struct stat_job_ctxt_s *jctxt;
	ovis_log_t log;
};

static int sample_pid_callback(per_job_base_t job_base, pid_t pid,
				ldms_mval_t pid_rec, void *arg)
{
	struct sample_pid_ctxt_s *ctx = arg;
	return parse_proc_stat(pid, pid_rec, ctx->jctxt, ctx->log);
}

/**
 * Sample this job
 * Called for each active job during sample()
 * Base has already started transaction
 */
static int sample_job(per_job_base_t job_base, void *job_ctxt)
{
	struct stat_job_ctxt_s *jctxt = job_ctxt;
	per_job_stat_t pjs = ldmsd_plug_ctxt_get(
		ldmsd_plug_handle_get_from_ctxt(per_job_base_get_set(job_base)));

	struct sample_pid_ctxt_s ctx = {
		.jctxt = jctxt,
		.log = pjs->log,
	};

	/* Update all PIDs for this job */
	return per_job_base_foreach_pid(job_base, sample_pid_callback, &ctx);
}

/* ============================================================================
 * LDMSD Plugin Interface
 * ============================================================================ */

static int constructor(ldmsd_plug_handle_t handle)
{
	per_job_stat_t pjs = calloc(1, sizeof(*pjs));
	if (!pjs)
		return ENOMEM;

	pjs->log = ldmsd_plug_log_get(handle);
	ldmsd_plug_ctxt_set(handle, pjs);

	return 0;
}

static int config(ldmsd_plug_handle_t handle,
		  struct attr_value_list *kwl,
		  struct attr_value_list *avl)
{
	per_job_stat_t pjs = ldmsd_plug_ctxt_get(handle);
	ldms_record_t pid_rec_def;
	int rc;

	/* Get configuration parameters */
	char *producer = av_value(avl, "producer");
	if (!producer) {
		ovis_log(pjs->log, OVIS_LERROR, "producer is required\n");
		return EINVAL;
	}

	char *schema = av_value(avl, "schema");
	if (!schema)
		schema = "per_job_stat";

	char *instance = av_value(avl, "instance");
	char *tenant = av_value(avl, "tenant");
	char *key_field = av_value(avl, "key_field");

	char *max_pids_str = av_value(avl, "max_pids");
	int max_pids = max_pids_str ? atoi(max_pids_str) : 1024;

	/* Create PID record definition */
	pid_rec_def = ldms_record_from_template("stat_pid_rec", pid_metrics, NULL);
	if (!pid_rec_def) {
		ovis_log(pjs->log, OVIS_LERROR,
			 "Failed to create PID record definition\n");
		return ENOMEM;
	}

	/* Set up callbacks */
	struct per_job_plugin_callbacks_s callbacks = {
		.job_init = job_init,
		.job_cleanup = job_cleanup,
		.pid_added = pid_added,
		.sample_job = sample_job,
	};

	/* Create sampler - base handles ALL complexity! */
	pjs->base = per_job_sampler_create(handle,
					   producer,
					   schema,
					   instance,
					   tenant,
					   key_field,
					   pid_rec_def,
					   max_pids,
					   &callbacks,
					   pjs);

	if (!pjs->base) {
		rc = errno;
		ovis_log(pjs->log, OVIS_LERROR,
			 "Failed to create sampler: %d\n", rc);
		ldms_record_delete(pid_rec_def);
		return rc;
	}

	return 0;
}

static int sample(ldmsd_plug_handle_t handle)
{
	per_job_stat_t pjs = ldmsd_plug_ctxt_get(handle);

	/* Base handles everything:
	 * - Iterate all active jobs
	 * - For each job, start transaction
	 * - Call our sample_job callback
	 * - End transaction
	 */
	return per_job_sampler_sample(pjs->base);
}

static void destructor(ldmsd_plug_handle_t handle)
{
	per_job_stat_t pjs = ldmsd_plug_ctxt_get(handle);

	if (!pjs)
		return;

	/* Base cleans up everything:
	 * - All jobs and sets
	 * - Tenant registration
	 * - Schema
	 */
	per_job_sampler_destroy(pjs->base);
	free(pjs);
}

static const char *usage(ldmsd_plug_handle_t handle)
{
	return "config name=per_job_stat producer=<name> [schema=<schema>] "
	       "[instance=<instance>] [tenant=<tenant>] [key_field=<field>] "
	       "[max_pids=<max_pids>]\n"
	       "\n"
	       "Required:\n"
	       "    producer    - Producer name\n"
	       "\n"
	       "Optional:\n"
	       "    schema      - Schema name (default: per_job_stat)\n"
	       "    instance    - Instance name for self-contained mode\n"
	       "    tenant      - Tenant definition name (for shared mode)\n"
	       "    key_field   - Binding key field (default: job_id)\n"
	       "    max_pids    - Max PIDs per job (default: 1024)\n"
	       "\n"
	       "Three deployment modes:\n"
	       "  1. Self-contained (simplest):\n"
	       "     config name=per_job_stat producer=node1\n"
	       "\n"
	       "  2. Custom binding key:\n"
	       "     config name=per_job_stat producer=node1 \\\n"
	       "            key_field=JOB_ID\n"
	       "\n"
	       "  3. Shared tenant:\n"
	       "     config name=per_job_stat producer=node1 \\\n"
	       "            tenant=job_tenant\n"
	       "\n"
	       "Collected metrics from /proc/<pid>/stat:\n"
	       "  pid, comm, state, ppid, pgrp, session, utime, stime,\n"
	       "  cutime, cstime, num_threads, vsize, rss, processor\n";
}

struct ldmsd_sampler ldmsd_plugin_interface = {
	.base.type = LDMSD_PLUGIN_SAMPLER,
	.base.flags = LDMSD_PLUGIN_MULTI_INSTANCE,
	.base.config = config,
	.base.usage = usage,
	.base.constructor = constructor,
	.base.destructor = destructor,
	.sample = sample,
};
