/* -*- c-basic-offset: 8 -*-
 * cgroup_smplrp.c - cgroup v2 sampler designed for use with ldmsd_smplrp
 *
 * Unlike cgroup_sampler (which manages all jobs internally via per_job_sampler_base),
 * this plugin is a plain stateless sampler. ldmsd_smplrp loads one instance of
 * this plugin per job, passing the job_id as a config parameter. The plugin
 * resolves the job's cgroup path, discovers available metrics from the real
 * job cgroup (so memory.current, memory.events, memory.peak are all found),
 * creates one LDMS set, and samples it on the normal schedule.
 *
 * smplrp handles the job lifecycle: load -> config -> start on job_init,
 * stop -> term on job_exit. This plugin has no awareness of jobs beyond
 * the single job_id it receives at config time.
 *
 * Configuration (via smplrp json, see usage()):
 *   producer=<name>
 *   instance=<name>
 *   job_id=<id>                 Required. The job ID for this instance.
 *   [schema=<name>]             Default: "cgroup_job"
 *   [component_id=<int>]        Passed through to sampler_base.
 *   [cgroup_root=<path>]        Default: "/sys/fs/cgroup"
 *   [cgroup_path_pattern=<fmt>] Optional printf pattern with one %s for job_id.
 *                               If omitted, common layouts are auto-detected.
 *
 * Metrics:
 *   All keys found in cpu.stat and memory.stat under the job's cgroup directory
 *   are added to the schema dynamically at config time. No metric names are
 *   hardcoded. The exact set depends on the kernel version.
 *
 *   LDMS metric names: cpu.stat keys -> "cpu_<key>", memory.stat keys -> "mem_<key>"
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"
#include "sampler_base.h"

#include "cgroup_util.h"

#define SAMP "cgroup_smplrp"

typedef struct cgroup_smplrp_s {
	ovis_log_t log;
	base_data_t base;

	/* Resolved cgroup directory for this job instance */
	char cgroup_path[PATH_MAX];

	/* Discovered metrics -- built once at config time from the real
	 * job cgroup, so all files (cpu.stat, memory.stat) are present. */
	struct cgroup_metric_desc_list_s *metric_list;

	/* Per-metric schema indices, one per metric_list->descs[i].
	 * metric_offset is the index of the first cgroup metric in the set
	 * (after the standard base metrics component_id, job_id, etc.). */
	int metric_offset;

	/* Sample value scratch buffer -- allocated once, reused every sample */
	uint64_t *values;
} *cgroup_smplrp_t;

static const char *usage(ldmsd_plug_handle_t handle)
{
	return	"config name=" SAMP
		" producer=<name>"
		" instance=<name>"
		" job_id=<id>"
		" [schema=<name>]"
		" [component_id=<int>]"
		" [cgroup_root=<path>]"
		" [cgroup_path_pattern=<fmt>]\n"
		"\n"
		"    producer             Required. Producer name.\n"
		"    instance             Required. Set instance name.\n"
		"    job_id               Required. Job ID for this sampler instance.\n"
		"                         Typically passed as '%J' by smplrp.\n"
		"    schema               Schema name. Default: " SAMP "\n"
		"    component_id         Component ID. Default: 0\n"
		"    cgroup_root          cgroup v2 mount point. Default: /sys/fs/cgroup\n"
		"    cgroup_path_pattern  printf pattern with one %%s for job_id.\n"
		"                         Example: /sys/fs/cgroup/system.slice/\n"
		"                                  slurmstepd.scope/job_%%s\n"
		"                         If omitted, common layouts are auto-detected.\n";
}

static int create_metric_set(cgroup_smplrp_t cg, ldmsd_plug_handle_t handle)
{
	ldms_schema_t schema;
	int rc, i;

	schema = base_schema_new(cg->base);
	if (!schema) {
		ovis_log(cg->log, OVIS_LERROR,
			 SAMP ": failed to create schema '%s', errno=%d\n",
			 cg->base->schema_name, errno);
		return errno;
	}

	/* Record where our metrics start (after base metrics) */
	cg->metric_offset = ldms_schema_metric_count_get(schema);

	/* Add one metric per discovered key */
	for (i = 0; i < cg->metric_list->count; i++) {
		rc = ldms_schema_metric_add_with_unit(schema,
					cg->metric_list->descs[i].ldms_name,
					cg->metric_list->descs[i].unit,
					LDMS_V_U64);
		if (rc < 0) {
			ovis_log(cg->log, OVIS_LERROR,
				 SAMP ": failed to add metric '%s': %d\n",
				 cg->metric_list->descs[i].ldms_name, rc);
			return ENOMEM;
		}
	}

	cg->base->set = base_set_new(cg->base);
	if (!cg->base->set) {
		ovis_log(cg->log, OVIS_LERROR,
			 SAMP ": failed to create set, errno=%d\n", errno);
		return errno;
	}

	return 0;
}

static int config(ldmsd_plug_handle_t handle,
		  struct attr_value_list *kwl, struct attr_value_list *avl)
{
	cgroup_smplrp_t cg = ldmsd_plug_ctxt_get(handle);
	const char *job_id, *cgroup_root, *pattern;
	int rc;

	if (cg->base) {
		ovis_log(cg->log, OVIS_LERROR, SAMP ": already configured.\n");
		return EBUSY;
	}

	/* job_id is required -- smplrp passes it via %J expansion */
	job_id = av_value(avl, "job_id");
	if (!job_id) {
		ovis_log(cg->log, OVIS_LERROR,
			 SAMP ": 'job_id' is required.\n");
		return EINVAL;
	}

	cgroup_root = av_value(avl, "cgroup_root");
	if (!cgroup_root)
		cgroup_root = "/sys/fs/cgroup";

	pattern = av_value(avl, "cgroup_path_pattern");

	/* Resolve the job's actual cgroup directory */
	rc = cgroup_resolve_job_path(cgroup_root, pattern, job_id,
				     cg->cgroup_path, sizeof(cg->cgroup_path));
	if (rc) {
		ovis_log(cg->log, OVIS_LERROR,
			 SAMP ": cannot find cgroup dir for job '%s' "
			 "under '%s': %d\n", job_id, cgroup_root, rc);
		return rc;
	}

	ovis_log(cg->log, OVIS_LINFO,
		 SAMP ": job %s -> %s\n", job_id, cg->cgroup_path);

	/*
	 * Discover metrics from the real job cgroup.
	 * Because this is a live job directory, all files are present --
	 * cpu.stat, memory.stat -- giving us the full metric set.
	 */
	rc = cgroup_discover_metrics(cg->cgroup_path, CGROUP_SRC_ALL,
				     &cg->metric_list);
	if (rc) {
		ovis_log(cg->log, OVIS_LERROR,
			 SAMP ": metric discovery failed: %d\n", rc);
		return rc;
	}

	if (cg->metric_list->count == 0) {
		ovis_log(cg->log, OVIS_LWARNING,
			 SAMP ": no metrics discovered from '%s'\n",
			 cg->cgroup_path);
	}

	ovis_log(cg->log, OVIS_LINFO,
		 SAMP ": discovered %d metrics from '%s'\n",
		 cg->metric_list->count, cg->cgroup_path);

	/* Allocate sample value scratch buffer */
	if (cg->metric_list->count > 0) {
		cg->values = calloc(cg->metric_list->count, sizeof(uint64_t));
		if (!cg->values) {
			rc = ENOMEM;
			goto err_list;
		}
	}

	/* Configure sampler_base (handles producer, instance, component_id, etc.) */
	cg->base = base_config(avl, ldmsd_plug_cfg_name_get(handle),
			       SAMP, cg->log);
	if (!cg->base) {
		rc = errno;
		goto err_values;
	}

	rc = create_metric_set(cg, handle);
	if (rc)
		goto err_base;

	return 0;

err_base:
	base_del(cg->base);
	cg->base = NULL;
err_values:
	free(cg->values);
	cg->values = NULL;
err_list:
	cgroup_metric_desc_list_free(cg->metric_list);
	cg->metric_list = NULL;
	return rc;
}

static int sample(ldmsd_plug_handle_t handle)
{
	cgroup_smplrp_t cg = ldmsd_plug_ctxt_get(handle);
	int rc, i;

	if (!cg->base || !cg->base->set) {
		ovis_log(cg->log, OVIS_LDEBUG, SAMP ": not configured\n");
		return EINVAL;
	}

	if (cg->metric_list->count == 0)
		return 0;

	rc = cgroup_read_metrics(cg->cgroup_path, cg->metric_list, cg->values);
	if (rc) {
		ovis_log(cg->log, OVIS_LWARNING,
			 SAMP ": read_metrics failed: %d\n", rc);
		/* Non-fatal: publish zeros rather than skip the sample */
	}

	base_sample_begin(cg->base);
	for (i = 0; i < cg->metric_list->count; i++) {
		ldms_metric_set_u64(cg->base->set,
				    cg->metric_offset + i,
				    cg->values[i]);
	}
	base_sample_end(cg->base);

	return 0;
}

static int constructor(ldmsd_plug_handle_t handle)
{
	cgroup_smplrp_t cg = calloc(1, sizeof(*cg));
	if (!cg)
		return ENOMEM;
	cg->log = ldmsd_plug_log_get(handle);
	ldmsd_plug_ctxt_set(handle, cg);
	return 0;
}

static void destructor(ldmsd_plug_handle_t handle)
{
	cgroup_smplrp_t cg = ldmsd_plug_ctxt_get(handle);
	if (!cg)
		return;
	if (cg->base) {
		base_set_delete(cg->base);
		base_del(cg->base);
	}
	cgroup_metric_desc_list_free(cg->metric_list);
	free(cg->values);
	free(cg);
}

struct ldmsd_sampler ldmsd_plugin_interface = {
	.base.type        = LDMSD_PLUGIN_SAMPLER,
	.base.flags       = LDMSD_PLUGIN_MULTI_INSTANCE,
	.base.config      = config,
	.base.usage       = usage,
	.base.constructor = constructor,
	.base.destructor  = destructor,
	.sample           = sample,
};
