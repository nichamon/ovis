/* cgroup_sampler.c -- Per-job cgroup v2 CPU and memory sampler plugin
 *
 * Uses dynamic metric discovery: at config() time, the plugin reads the
 * actual cgroup files present on this node (cpu.stat, memory.stat, etc.)
 * and builds the LDMS schema from whatever keys the kernel provides.
 * No metric names are hardcoded in the plugin itself.
 *
 * Configuration:
 *   producer=<name>           Required
 *   schema=<name>             Default: "cgroup_job"
 *   instance=<name>           Default: schema name
 *   cgroup_root=<path>        Default: "/sys/fs/cgroup"
 *   cgroup_template_path=<path>
 *                             A readable cgroup directory used for discovery.
 *                             Defaults to cgroup_root itself (cpu.stat and
 *                             memory.stat are usually present there even with
 *                             no jobs running; memory.current and memory.events
 *                             require a real job cgroup or will be skipped).
 *   cgroup_path_pattern=<fmt> printf pattern with one %s for job_id.
 *                             Overrides auto-detection at sample time.
 *                             Example: "/sys/fs/cgroup/system.slice/slurmstepd.scope/job_%s"
 *   tenant=<name>             Use an existing ldmsd_tenant. If omitted,
 *                             an internal tenant is created automatically.
 *
 * Schema:
 *   Metrics are named dynamically from the keys discovered at config time.
 *   cpu.stat keys are prefixed with "cpu_"; memory.* keys with "mem_".
 *   Dots in kernel key names become underscores.
 *
 *   Example on a kernel >= 5.19:
 *     cpu_usage_usec, cpu_user_usec, cpu_system_usec,
 *     cpu_nr_periods, cpu_nr_throttled, cpu_throttled_usec,
 *     cpu_nr_bursts, cpu_burst_usec,
 *     mem_current, mem_anon, mem_file, mem_kernel, mem_kernel_stack,
 *     mem_slab, mem_sock, mem_anon_thp, mem_pgfault, mem_pgmajfault,
 *     mem_pgrefill, mem_pgscan, mem_pgsteal, mem_pgactivate,
 *     mem_low, mem_high, mem_max, mem_oom, mem_oom_kill, ..., mem_peak
 *
 *   On an older kernel, nr_bursts/burst_usec and mem_peak will simply
 *   not appear in the schema -- no zeros, no placeholder fields.
 *
 * NOTE: PSI (pressure stall) metrics are intentionally excluded.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"

#include "per_job_sampler_base.h"
#include "cgroup_util.h"

/* -----------------------------------------------------------------------
 * Plugin-global context
 * ----------------------------------------------------------------------- */

struct cgroup_plugin_ctxt_s {
	ovis_log_t log;  /* retrieved from ldmsd_plug_log_get() in constructor */

	char cgroup_root[PATH_MAX];
	char cgroup_path_pattern[PATH_MAX]; /* optional; "" means auto-detect */

	/*
	 * Discovered metric descriptors -- built once at config() time during
	 * cgroup_discover_metrics(). Passed to the base via define_per_job_metrics
	 * so the base can add them to the schema during per_job_sampler_create().
	 *
	 * The base stores the assigned metric indices in
	 * per_job_sampler_s.per_job_metric_midx[], which job_init then copies
	 * into the per-job context for O(1) access at sample time.
	 */
	struct cgroup_metric_desc_list_s *metric_list;

	per_job_sampler_t sampler;
};


/* -----------------------------------------------------------------------
 * Per-job context
 * ----------------------------------------------------------------------- */

struct cgroup_job_ctxt_s {
	/* Resolved cgroup path for this job */
	char cgroup_path[PATH_MAX];
	int  cgroup_path_resolved;

	/*
	 * Per-metric indices in the LDMS set, in the same order as
	 * pctxt->metric_list->descs[]. Copied from
	 * job_base->per_job_metric_midx[] at job_init time.
	 * Allocated once per job; freed in job_cleanup.
	 */
	int     *metric_midx;
	int      metric_count;

	/* Scratch buffer for sample values; avoids per-sample malloc */
	uint64_t *values;
};

/* -----------------------------------------------------------------------
 * Path resolution (per-job, cached after first success)
 * ----------------------------------------------------------------------- */

static int resolve_job_path(struct cgroup_plugin_ctxt_s *pctxt,
			    struct cgroup_job_ctxt_s *jctxt,
			    const char *job_id)
{
	if (jctxt->cgroup_path_resolved)
		return 0;

	const char *pattern = pctxt->cgroup_path_pattern[0]
			    ? pctxt->cgroup_path_pattern : NULL;

	int rc = cgroup_resolve_job_path(pctxt->cgroup_root, pattern, job_id,
					 jctxt->cgroup_path,
					 sizeof(jctxt->cgroup_path));
	if (rc) {
		ovis_log(pctxt->log, OVIS_LWARNING,
			 "cgroup_sampler: cannot find cgroup dir "
			 "for job %s: %d\n", job_id, rc);
		return rc;
	}

	jctxt->cgroup_path_resolved = 1;
	ovis_log(pctxt->log, OVIS_LDEBUG,
		 "cgroup_sampler: job %s -> %s\n", job_id, jctxt->cgroup_path);
	return 0;
}

/* -----------------------------------------------------------------------
 * per_job_sampler_base callbacks
 * ----------------------------------------------------------------------- */

/*
 * define_per_job_metrics: called once during per_job_sampler_create() while
 * the schema is being built. We return a heap-allocated array of descriptors
 * derived from the already-discovered cgroup metric list. The base adds them
 * to the schema and frees the array.
 */
static int cgroup_define_per_job_metrics(struct per_job_metric_desc_s **metrics_out,
					 int *count_out,
					 void *plugin_ctxt)
{
	struct cgroup_plugin_ctxt_s *pctxt = plugin_ctxt;
	int n = pctxt->metric_list->count;

	struct per_job_metric_desc_s *descs = calloc(n, sizeof(*descs));
	if (!descs)
		return ENOMEM;

	for (int i = 0; i < n; i++) {
		descs[i].name  = pctxt->metric_list->descs[i].ldms_name;
		descs[i].unit  = pctxt->metric_list->descs[i].unit;
		descs[i].type  = LDMS_V_U64;
		descs[i].count = 1;
	}

	*metrics_out = descs;
	*count_out   = n;
	return 0;
}

/*
 * job_init: called when the first task of a new job starts and the job's
 * LDMS set has just been created. We copy the metric index array from
 * job_base (which the base populated during create_job_base from
 * sampler->per_job_metric_midx) and allocate the sample value scratch buffer.
 */
static int cgroup_job_init(per_job_base_t job_base, void **job_ctxt_out)
{
	struct cgroup_plugin_ctxt_s *pctxt = per_job_base_get_plugin_ctxt(job_base);
	int n = pctxt->metric_list->count;

	struct cgroup_job_ctxt_s *jctxt = calloc(1, sizeof(*jctxt));
	if (!jctxt)
		return ENOMEM;

	jctxt->metric_count = n;

	if (n > 0) {
		jctxt->metric_midx = malloc(n * sizeof(int));
		if (!jctxt->metric_midx) {
			free(jctxt);
			return ENOMEM;
		}

		/*
		 * Look up each metric index by name via the public API.
		 * per_job_base_s is opaque -- we cannot access its fields
		 * directly from plugin code.
		 */
		for (int i = 0; i < n; i++) {
			jctxt->metric_midx[i] = per_job_base_get_job_metric_index(
						job_base,
						pctxt->metric_list->descs[i].ldms_name);
		}

		jctxt->values = calloc(n, sizeof(uint64_t));
		if (!jctxt->values) {
			free(jctxt->metric_midx);
			free(jctxt);
			return ENOMEM;
		}
	}

	*job_ctxt_out = jctxt;
	return 0;
}

static void cgroup_job_cleanup(per_job_base_t job_base, void *job_ctxt)
{
	struct cgroup_job_ctxt_s *jctxt = job_ctxt;
	if (!jctxt)
		return;
	free(jctxt->metric_midx);
	free(jctxt->values);
	free(jctxt);
}

static int cgroup_sample_job(per_job_base_t job_base, void *job_ctxt)
{
	struct cgroup_plugin_ctxt_s *pctxt = per_job_base_get_plugin_ctxt(job_base);
	struct cgroup_job_ctxt_s *jctxt = job_ctxt;
	const char *job_id = per_job_base_get_job_id(job_base);
	int rc;

	/* Resolve path on first sample -- cgroup may not exist at job_init time */
	rc = resolve_job_path(pctxt, jctxt, job_id);
	if (rc)
		return 0;  /* Not yet visible; skip this sample silently */

	rc = cgroup_read_metrics(jctxt->cgroup_path,
				 pctxt->metric_list,
				 jctxt->values);
	if (rc) {
		ovis_log(pctxt->log, OVIS_LWARNING,
			 "cgroup_sampler: read_metrics failed for job %s: %d\n",
			 job_id, rc);
		return 0;  /* Non-fatal */
	}

	/* Write values into the set -- base handles the transaction */
	for (int i = 0; i < jctxt->metric_count; i++) {
		if (jctxt->metric_midx[i] >= 0)
			per_job_base_set_u64(job_base,
					     jctxt->metric_midx[i],
					     jctxt->values[i]);
	}

	return 0;
}

/* -----------------------------------------------------------------------
 * Plugin config
 * ----------------------------------------------------------------------- */

static int cgroup_config(ldmsd_plug_handle_t pi,
			 struct attr_value_list *kwl,
			 struct attr_value_list *avl)
{
	struct cgroup_plugin_ctxt_s *pctxt = ldmsd_plug_ctxt_get(pi);
	const char *val;
	int rc = 0;

	if (pctxt->sampler) {
		ovis_log(pctxt->log, OVIS_LERROR,
			 "cgroup_sampler: already configured.\n");
		return EBUSY;
	}

	/* cgroup path config */
	val = av_value(avl, "cgroup_root");
	strncpy(pctxt->cgroup_root,
		val ? val : "/sys/fs/cgroup",
		sizeof(pctxt->cgroup_root) - 1);

	val = av_value(avl, "cgroup_path_pattern");
	if (val)
		strncpy(pctxt->cgroup_path_pattern, val,
			sizeof(pctxt->cgroup_path_pattern) - 1);
	/* else cgroup_path_pattern stays "" -> auto-detect at sample time */

	/* Plugin identity */
	const char *producer = av_value(avl, "producer");
	const char *schema   = av_value(avl, "schema");
	const char *instance = av_value(avl, "instance");
	const char *tenant   = av_value(avl, "tenant");

	if (!producer) {
		ovis_log(pctxt->log, OVIS_LERROR,
			 "cgroup_sampler: 'producer' is required\n");
		return EINVAL;
	}
	if (!schema)   schema   = "cgroup_job";
	if (!instance) instance = schema;

	/*
	 * Step 1: Discover metrics from cgroup_root.
	 * cpu.stat and memory.stat are present at the root on any cgroup v2
	 * system -- no live job or template path needed.
	 */
	rc = cgroup_discover_metrics(pctxt->cgroup_root,
				     CGROUP_SRC_ALL,
				     &pctxt->metric_list);
	if (rc) {
		ovis_log(pctxt->log, OVIS_LERROR,
			 "cgroup_sampler: metric discovery failed: %d\n", rc);
		return rc;
	}

	if (pctxt->metric_list->count == 0) {
		ovis_log(pctxt->log, OVIS_LWARNING,
			 "cgroup_sampler: no metrics discovered from '%s'. "
			 "Check that cgroup_root is a cgroup v2 mount with "
			 "cpu.stat and memory.stat present.\n",
			 pctxt->cgroup_root);
	}

	ovis_log(pctxt->log, OVIS_LINFO,
		 "cgroup_sampler: discovered %d metrics from '%s'\n",
		 pctxt->metric_list->count, pctxt->cgroup_root);

	for (int i = 0; i < pctxt->metric_list->count; i++) {
		ovis_log(pctxt->log, OVIS_LDEBUG,
			 "  [%d] %s  (file=%s key=%s unit=%s)\n", i,
			 pctxt->metric_list->descs[i].ldms_name,
			 pctxt->metric_list->descs[i].cgroup_file,
			 pctxt->metric_list->descs[i].cgroup_key,
			 pctxt->metric_list->descs[i].unit);
	}

	/*
	 * Step 2: Create the per-job sampler base instance.
	 * Metrics are added to the schema via the define_per_job_metrics
	 * callback -- no separate step 3 needed.
	 */
	struct per_job_plugin_callbacks_s cbs = {
		.define_per_job_metrics = cgroup_define_per_job_metrics,
		.job_init    = cgroup_job_init,
		.job_cleanup = cgroup_job_cleanup,
		.sample_job  = cgroup_sample_job,
		.pid_added   = NULL,
		.pid_removed = NULL,
	};

	pctxt->sampler = per_job_sampler_create(
		pi,
		producer,
		schema,
		instance,
		tenant,
		NULL,   /* key_field: default (job_id) */
		NULL,   /* pid_rec_def: no PID list */
		0,      /* max_pids */
		&cbs,
		pctxt);

	if (!pctxt->sampler) {
		ovis_log(pctxt->log, OVIS_LERROR,
			 "cgroup_sampler: per_job_sampler_create failed: %d\n",
			 errno);
		rc = errno;
		goto err_list;
	}

	return 0;

err_list:
	cgroup_metric_desc_list_free(pctxt->metric_list);
	pctxt->metric_list = NULL;
	return rc;
}

static int cgroup_sample(ldmsd_plug_handle_t pi)
{
	struct cgroup_plugin_ctxt_s *pctxt = ldmsd_plug_ctxt_get(pi);
	if (!pctxt->sampler) {
		ovis_log(pctxt->log, OVIS_LDEBUG,
			 "cgroup_sampler: plugin not configured\n");
		return EINVAL;
	}
	return per_job_sampler_sample(pctxt->sampler);
}

static int cgroup_constructor(ldmsd_plug_handle_t pi)
{
	struct cgroup_plugin_ctxt_s *pctxt = calloc(1, sizeof(*pctxt));
	if (!pctxt)
		return ENOMEM;
	pctxt->log = ldmsd_plug_log_get(pi);
	ldmsd_plug_ctxt_set(pi, pctxt);
	return 0;
}

static void cgroup_destructor(ldmsd_plug_handle_t pi)
{
	struct cgroup_plugin_ctxt_s *pctxt = ldmsd_plug_ctxt_get(pi);
	if (!pctxt)
		return;
	if (pctxt->sampler)
		per_job_sampler_destroy(pctxt->sampler);
	cgroup_metric_desc_list_free(pctxt->metric_list);
	free(pctxt);
}

static const char *cgroup_usage(ldmsd_plug_handle_t pi)
{
	return
	"config name=cgroup_sampler"
	" producer=<name>"
	" [schema=<name>]"
	" [instance=<name>]"
	" [cgroup_root=<path>]"
	" [cgroup_path_pattern=<fmt>]"
	" [tenant=<name>]\n"
	"\n"
	"  producer               Required. Producer name.\n"
	"  schema                 Schema name. Default: cgroup_job\n"
	"  instance               Base instance name. Default: schema name\n"
	"  cgroup_root            cgroup v2 mount point. Default: /sys/fs/cgroup\n"
	"                         Also used at startup to discover which metrics\n"
	"                         exist on this kernel (cpu.stat, memory.stat).\n"
	"  cgroup_path_pattern    printf pattern with one %%s for job_id used to\n"
	"                         locate each job's cgroup at sample time.\n"
	"                         Example: /sys/fs/cgroup/system.slice/"
		"slurmstepd.scope/job_%%s\n"
	"                         If omitted, common layouts are auto-detected.\n"
	"  tenant                 Name of existing ldmsd_tenant. If omitted,\n"
	"                         an internal tenant is created automatically.\n";
}

struct ldmsd_sampler ldmsd_plugin_interface = {
	.base.type        = LDMSD_PLUGIN_SAMPLER,
	.base.flags       = LDMSD_PLUGIN_MULTI_INSTANCE,
	.base.config      = cgroup_config,
	.base.usage       = cgroup_usage,
	.base.constructor = cgroup_constructor,
	.base.destructor  = cgroup_destructor,
	.sample           = cgroup_sample,
};
