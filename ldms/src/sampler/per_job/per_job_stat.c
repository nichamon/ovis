/* per_job_stat.c - Per-job /proc/<pid>/stat sampler */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "per_job_sampler_base.h"

/* Plugin instance */
typedef struct per_job_stat_s
{
	per_job_base_t base;
	ovis_log_t log;
} *per_job_stat_t;

/* Define PID metrics from /proc/<pid>/stat */
static struct ldms_metric_template_s pid_metrics[] = {
		{"pid", 0, LDMS_V_U64, "", 1},	     /* Always include PID */
		{"comm", 0, LDMS_V_CHAR_ARRAY, "", 16},  /* Command name */
		{"state", 0, LDMS_V_CHAR, "", 1},	     /* Process state */
		{"ppid", 0, LDMS_V_U64, "", 1},	     /* Parent PID */
		{"pgrp", 0, LDMS_V_U64, "", 1},	     /* Process group ID */
		{"session", 0, LDMS_V_U64, "", 1},	     /* Session ID */
		{"utime", 0, LDMS_V_U64, "jiffies", 1},  /* User time */
		{"stime", 0, LDMS_V_U64, "jiffies", 1},  /* System time */
		{"cutime", 0, LDMS_V_U64, "jiffies", 1}, /* Children user time */
		{"cstime", 0, LDMS_V_U64, "jiffies", 1}, /* Children system time */
		{"num_threads", 0, LDMS_V_U32, "", 1},   /* Number of threads */
		{"vsize", 0, LDMS_V_U64, "bytes", 1},    /* Virtual memory size */
		{"rss", 0, LDMS_V_U64, "pages", 1},	     /* Resident set size */
		{"processor", 0, LDMS_V_U32, "", 1},     /* CPU number */
		{0}
};

/* Metric field indices */
enum {
	MID_PID = 0,
	MID_COMM,
	MID_STATE,
	MID_PPID,
	MID_PGRP,
	MID_SESSION,
	MID_UTIME,
	MID_STIME,
	MID_CUTIME,
	MID_CSTIME,
	MID_NUM_THREADS,
	MID_VSIZE,
	MID_RSS,
	MID_PROCESSOR,
};

/**
 * Parse /proc/<pid>/stat and extract metrics.
 *
 * Format of /proc/<pid>/stat:
 * pid (comm) state ppid pgrp session tty_nr tpgid flags minflt cminflt
 * majflt cmajflt utime stime cutime cstime priority nice num_threads
 * itrealvalue starttime vsize rss ... processor ...
 */
static int parse_proc_stat(uint64_t pid, ldms_mval_t rec, ovis_log_t log)
{
	char path[256];
	FILE *f;
	char comm[16];
	char state;
	uint64_t ppid, pgrp, session, utime, stime, cutime, cstime, vsize, rss;
	uint32_t num_threads, processor;
	int rc;

	snprintf(path, sizeof(path), "/proc/%lu/stat", pid);
	f = fopen(path, "r");
	if (!f)
	{
		/* PID may have exited - not an error */
		return 0;
	}

	/* Parse the stat file
	 * Note: We skip many fields with %*d or %*u
	 * Fields we parse:
	 * - comm: command name (in parentheses)
	 * - state: process state (R, S, D, Z, T, etc.)
	 * - ppid, pgrp, session: process identifiers
	 * - utime, stime, cutime, cstime: CPU times
	 * - num_threads: number of threads
	 * - vsize: virtual memory size
	 * - rss: resident set size
	 * - processor: CPU number
	 */
	rc = fscanf(f, "%*d (%15[^)]) %c %lu %lu %lu %*d %*d %*u %*u %*u %*u %*u "
		       "%lu %lu %lu %lu %*d %*d %u %*d %*u %lu %lu %*u %*u %*u %*u "
		       "%*u %*u %*u %*u %*u %*u %*u %*u %*d %*d %u",
		    comm, &state, &ppid, &pgrp, &session,
		    &utime, &stime, &cutime, &cstime, &num_threads,
		    &vsize, &rss, &processor);

	fclose(f);

	if (rc != 13)
	{
		ovis_log(log, OVIS_LDEBUG,
			 "Failed to parse /proc/%lu/stat (got %d fields)\n", pid, rc);
		return 0;
	}

	/* Set metric values in the record */
	ldms_record_set_u64(rec, MID_PID, pid);
	ldms_record_array_set_str(rec, MID_COMM, comm);
	ldms_record_set_char(rec, MID_STATE, state);
	ldms_record_set_u64(rec, MID_PPID, ppid);
	ldms_record_set_u64(rec, MID_PGRP, pgrp);
	ldms_record_set_u64(rec, MID_SESSION, session);
	ldms_record_set_u64(rec, MID_UTIME, utime);
	ldms_record_set_u64(rec, MID_STIME, stime);
	ldms_record_set_u64(rec, MID_CUTIME, cutime);
	ldms_record_set_u64(rec, MID_CSTIME, cstime);
	ldms_record_set_u32(rec, MID_NUM_THREADS, num_threads);
	ldms_record_set_u64(rec, MID_VSIZE, vsize);
	ldms_record_set_u64(rec, MID_RSS, rss);
	ldms_record_set_u32(rec, MID_PROCESSOR, processor);

	return 0;
}

/**
 * Sample one PID - called by base for each active PID.
 */
static int sample_pid(ldms_mval_t rec, uint64_t pid, void *ctxt)
{
	per_job_stat_t pjs = (per_job_stat_t)ctxt;
	return parse_proc_stat(pid, rec, pjs->log);
}

/* ========== LDMSD Plugin Interface ========== */

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
	ldms_schema_t schema;
	char *error = NULL;
	int rc;

	/* Base handles all config parsing (3 modes) */
	pjs->base = per_job_base_config(avl, "per_job_stat",
					"per_job_stat", pjs->log, &error);
	if (!pjs->base)
	{
		ovis_log(pjs->log, OVIS_LERROR, "Config failed: %s\n", error);
		free(error);
		return errno;
	}

	/* Create schema with stat metrics */
	schema = per_job_base_schema_create(pjs->base, pid_metrics, 1024); /* TODO: */
	if (!schema) {
		rc = errno;
		ovis_log(pjs->log, OVIS_LERROR,
			 "Schema creation failed: %d\n", rc);
		return rc;
	}

	/* Register our sampling function */
	rc = per_job_base_set_pid_sampler(pjs->base, sample_pid, pjs);
	if (rc)
	{
		ovis_log(pjs->log, OVIS_LERROR,
			 "Failed to set sampler: %d\n", rc);
		return rc;
	}

	return 0;
}

static int sample(ldmsd_plug_handle_t handle)
{
	per_job_stat_t pjs = ldmsd_plug_ctxt_get(handle);

	/* Base handles everything:
	 * - Iterate through all active jobs
	 * - For each job, iterate through all PID records
	 * - Call our sample_pid() for each PID
	 * - Handle transactions
	 */
	return per_job_base_sample(pjs->base);
}

static void destructor(ldmsd_plug_handle_t handle)
{
	per_job_stat_t pjs = ldmsd_plug_ctxt_get(handle);

	if (!pjs)
		return;

	/* Base cleans up everything */
	per_job_base_del(pjs->base);
	free(pjs);
}

static const char *usage(ldmsd_plug_handle_t handle)
{
	return "config name=per_job_stat producer=<name> "
	       "[tenant=<tenant_def>] [key_field=<field>]\n"
	       "\n"
	       "    producer   - Producer name (required)\n"
	       "    tenant     - Name of tenant definition (optional)\n"
	       "    key_field  - Binding key field name (optional)\n"
	       "\n"
	       "Collects per-PID metrics from /proc/<pid>/stat:\n"
	       "  pid, comm, state, ppid, pgrp, session, utime, stime,\n"
	       "  cutime, cstime, num_threads, vsize, rss, processor\n"
	       "\n"
	       "Examples:\n"
	       "  # Simple self-contained mode\n"
	       "  config name=per_job_stat producer=node1\n"
	       "\n"
	       "  # With custom binding key\n"
	       "  config name=per_job_stat producer=node1 \\\n"
	       "         key_field=SLURM_JOB_UUID\n"
	       "\n"
	       "  # With shared tenant\n"
	       "  config name=per_job_stat producer=node1 \\\n"
	       "         tenant=job_tenant\n";
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
