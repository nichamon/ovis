/* per_job_netdev.c - Per-job /proc/<pid>/net/dev sampler
 *
 * This plugin demonstrates how to use per_job_sampler_base with
 * additional lists beyond the standard pid_list.
 *
 * Schema structure:
 * - Standard per-job metrics (job_id, binding_key)
 * - pid_list: Simple PID records (just PID for correlation)
 * - netdev_list: Network interface records (one per interface per PID)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <ctype.h>

#include "per_job_sampler_base.h"

/* ============================================================================
 * Plugin Data Structures
 * ============================================================================ */

/* Plugin instance */
typedef struct per_job_netdev_s {
	per_job_base_t sampler;
	ovis_log_t log;
} *per_job_netdev_t;

/* Per-job context (cached indices) */
struct netdev_job_ctxt_s {
	/* PID record metrics */
	int pid_midx;

	/* netdev_list and record indices */
	int netdev_list_midx;
	int netdev_rec_midx;

	/* netdev record field indices */
	int ndev_pid_midx;
	int ndev_iface_midx;
	int ndev_rx_bytes_midx;
	int ndev_rx_packets_midx;
	int ndev_rx_errs_midx;
	int ndev_rx_drop_midx;
	int ndev_rx_fifo_midx;
	int ndev_rx_frame_midx;
	int ndev_rx_compressed_midx;
	int ndev_rx_multicast_midx;
	int ndev_tx_bytes_midx;
	int ndev_tx_packets_midx;
	int ndev_tx_errs_midx;
	int ndev_tx_drop_midx;
	int ndev_tx_fifo_midx;
	int ndev_tx_colls_midx;
	int ndev_tx_carrier_midx;
	int ndev_tx_compressed_midx;
};

/* ============================================================================
 * Metric Definitions
 * ============================================================================ */

/* PID record - just PID for correlation */
static struct ldms_metric_template_s pid_metrics[] = {
	{"pid", 0, LDMS_V_U64, "", 1},
	{0}
};

/* Network interface record - one per interface per PID */
static struct ldms_metric_template_s netdev_metrics[] = {
	{"pid",             0, LDMS_V_U64,        "",         1},   /* PID for correlation */
	{"iface",           0, LDMS_V_CHAR_ARRAY, "",         16},  /* Interface name */
	{"rx_bytes",        0, LDMS_V_U64,        "bytes",    1},
	{"rx_packets",      0, LDMS_V_U64,        "packets",  1},
	{"rx_errs",         0, LDMS_V_U64,        "errors",   1},
	{"rx_drop",         0, LDMS_V_U64,        "dropped",  1},
	{"rx_fifo",         0, LDMS_V_U64,        "errors",   1},
	{"rx_frame",        0, LDMS_V_U64,        "errors",   1},
	{"rx_compressed",   0, LDMS_V_U64,        "packets",  1},
	{"rx_multicast",    0, LDMS_V_U64,        "packets",  1},
	{"tx_bytes",        0, LDMS_V_U64,        "bytes",    1},
	{"tx_packets",      0, LDMS_V_U64,        "packets",  1},
	{"tx_errs",         0, LDMS_V_U64,        "errors",   1},
	{"tx_drop",         0, LDMS_V_U64,        "dropped",  1},
	{"tx_fifo",         0, LDMS_V_U64,        "errors",   1},
	{"tx_colls",        0, LDMS_V_U64,        "collisions", 1},
	{"tx_carrier",      0, LDMS_V_U64,        "errors",   1},
	{"tx_compressed",   0, LDMS_V_U64,        "packets",  1},
	{0}
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Parse /proc/<pid>/net/dev and add entries to netdev_list
 *
 * Format:
 * Inter-|   Receive                                                |  Transmit
 *  face |bytes    packets errs drop fifo frame compressed multicast|bytes ...
 *     lo: 2776770   11307    0    0    0     0          0         0  2776770 ...
 */
static int parse_proc_netdev(pid_t pid, ldms_set_t set,
			      struct netdev_job_ctxt_s *jctxt,
			      ovis_log_t log)
{
	char path[256];
	FILE *f;
	char line[256];
	int line_num = 0;
	ldms_mval_t netdev_list, netdev_rec;

	snprintf(path, sizeof(path), "/proc/%d/net/dev", pid);
	f = fopen(path, "r");
	if (!f) {
		/* PID exited or no network namespace - not an error */
		return 0;
	}

	/* Get netdev list head */
	netdev_list = ldms_metric_get(set, jctxt->netdev_list_midx);
	if (!netdev_list) {
		ovis_log(log, OVIS_LERROR, "Failed to get netdev list\n");
		fclose(f);
		return EINVAL;
	}

	/* Parse each line (skip headers) */
	while (fgets(line, sizeof(line), f)) {
		char iface[16];
		uint64_t rx_bytes, rx_packets, rx_errs, rx_drop, rx_fifo;
		uint64_t rx_frame, rx_compressed, rx_multicast;
		uint64_t tx_bytes, tx_packets, tx_errs, tx_drop, tx_fifo;
		uint64_t tx_colls, tx_carrier, tx_compressed;
		char *ptr;
		int rc;

		line_num++;

		/* Skip first two header lines */
		if (line_num <= 2)
			continue;

		/* Parse interface name (before colon) */
		ptr = strchr(line, ':');
		if (!ptr)
			continue;

		*ptr = '\0';
		ptr++;

		/* Trim leading whitespace from interface name */
		char *iface_start = line;
		while (isspace(*iface_start))
			iface_start++;

		snprintf(iface, sizeof(iface), "%s", iface_start);

		/* Parse 16 numeric fields */
		rc = sscanf(ptr, "%lu %lu %lu %lu %lu %lu %lu %lu "
			    "%lu %lu %lu %lu %lu %lu %lu %lu",
			    &rx_bytes, &rx_packets, &rx_errs, &rx_drop,
			    &rx_fifo, &rx_frame, &rx_compressed, &rx_multicast,
			    &tx_bytes, &tx_packets, &tx_errs, &tx_drop,
			    &tx_fifo, &tx_colls, &tx_carrier, &tx_compressed);

		if (rc != 16) {
			ovis_log(log, OVIS_LDEBUG,
				 "Failed to parse /proc/%d/net/dev line %d "
				 "(got %d fields)\n", pid, line_num, rc);
			continue;
		}

		/* Allocate netdev record */
		netdev_rec = ldms_record_alloc(set, jctxt->netdev_rec_midx);
		if (!netdev_rec) {
			ovis_log(log, OVIS_LERROR,
				 "Failed to allocate netdev record for %s\n", iface);
			fclose(f);
			return ENOMEM;
		}

		/* Populate record */
		ldms_record_set_u64(netdev_rec, jctxt->ndev_pid_midx, pid);
		ldms_record_array_set_str(netdev_rec, jctxt->ndev_iface_midx, iface);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_rx_bytes_midx, rx_bytes);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_rx_packets_midx, rx_packets);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_rx_errs_midx, rx_errs);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_rx_drop_midx, rx_drop);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_rx_fifo_midx, rx_fifo);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_rx_frame_midx, rx_frame);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_rx_compressed_midx, rx_compressed);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_rx_multicast_midx, rx_multicast);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_tx_bytes_midx, tx_bytes);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_tx_packets_midx, tx_packets);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_tx_errs_midx, tx_errs);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_tx_drop_midx, tx_drop);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_tx_fifo_midx, tx_fifo);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_tx_colls_midx, tx_colls);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_tx_carrier_midx, tx_carrier);
		ldms_record_set_u64(netdev_rec, jctxt->ndev_tx_compressed_midx, tx_compressed);

		/* Append to list */
		rc = ldms_list_append_record(set, netdev_list, netdev_rec);
		if (rc) {
			ovis_log(log, OVIS_LERROR,
				 "Failed to append netdev record: %d\n", rc);
			fclose(f);
			return rc;
		}
	}

	fclose(f);
	return 0;
}

/* ============================================================================
 * Plugin Callbacks
 * ============================================================================ */

/**
 * Initialize per-job context - cache all metric indices
 */
static int job_init(per_job_sampler_t job_base, void **job_ctxt_out)
{
	struct netdev_job_ctxt_s *jctxt = calloc(1, sizeof(*jctxt));
	ldms_set_t set;
	ldms_schema_t schema;

	if (!jctxt)
		return ENOMEM;

	set = per_job_base_get_set(job_base);
	schema = per_job_base_get_schema(job_base);

	/* Cache PID record indices */
	jctxt->pid_midx = per_job_base_get_pid_metric_index(job_base, "pid");

	/* Cache netdev list/record indices */
	jctxt->netdev_list_midx = ldms_metric_by_name(set, "netdev_list");
	jctxt->netdev_rec_midx = ldms_metric_by_name(set, "netdev_rec");

	/* Cache netdev record field indices */
	ldms_record_t netdev_rec_def = ldms_schema_record_get(schema,
							       jctxt->netdev_rec_midx);
	jctxt->ndev_pid_midx           = ldms_record_metric_find(netdev_rec_def, "pid");
	jctxt->ndev_iface_midx         = ldms_record_metric_find(netdev_rec_def, "iface");
	jctxt->ndev_rx_bytes_midx      = ldms_record_metric_find(netdev_rec_def, "rx_bytes");
	jctxt->ndev_rx_packets_midx    = ldms_record_metric_find(netdev_rec_def, "rx_packets");
	jctxt->ndev_rx_errs_midx       = ldms_record_metric_find(netdev_rec_def, "rx_errs");
	jctxt->ndev_rx_drop_midx       = ldms_record_metric_find(netdev_rec_def, "rx_drop");
	jctxt->ndev_rx_fifo_midx       = ldms_record_metric_find(netdev_rec_def, "rx_fifo");
	jctxt->ndev_rx_frame_midx      = ldms_record_metric_find(netdev_rec_def, "rx_frame");
	jctxt->ndev_rx_compressed_midx = ldms_record_metric_find(netdev_rec_def, "rx_compressed");
	jctxt->ndev_rx_multicast_midx  = ldms_record_metric_find(netdev_rec_def, "rx_multicast");
	jctxt->ndev_tx_bytes_midx      = ldms_record_metric_find(netdev_rec_def, "tx_bytes");
	jctxt->ndev_tx_packets_midx    = ldms_record_metric_find(netdev_rec_def, "tx_packets");
	jctxt->ndev_tx_errs_midx       = ldms_record_metric_find(netdev_rec_def, "tx_errs");
	jctxt->ndev_tx_drop_midx       = ldms_record_metric_find(netdev_rec_def, "tx_drop");
	jctxt->ndev_tx_fifo_midx       = ldms_record_metric_find(netdev_rec_def, "tx_fifo");
	jctxt->ndev_tx_colls_midx      = ldms_record_metric_find(netdev_rec_def, "tx_colls");
	jctxt->ndev_tx_carrier_midx    = ldms_record_metric_find(netdev_rec_def, "tx_carrier");
	jctxt->ndev_tx_compressed_midx = ldms_record_metric_find(netdev_rec_def, "tx_compressed");

	*job_ctxt_out = jctxt;
	return 0;
}

/**
 * Clean up per-job context
 */
static void job_cleanup(per_job_sampler_t job_base, void *job_ctxt)
{
	free(job_ctxt);
}

/**
 * Initialize PID record when task starts
 */
static int pid_added(per_job_sampler_t job_base, pid_t pid,
		     ldms_mval_t pid_rec, void *job_ctxt)
{
	struct netdev_job_ctxt_s *jctxt = job_ctxt;

	/* Just set PID in the record */
	ldms_record_set_u64(pid_rec, jctxt->pid_midx, pid);

	return 0;
}

/**
 * PID iterator callback - parse netdev for one PID
 */
struct sample_netdev_ctxt_s {
	struct netdev_job_ctxt_s *jctxt;
	ldms_set_t set;
	ovis_log_t log;
};

static int sample_netdev_for_pid(per_job_sampler_t job_base, pid_t pid,
				  ldms_mval_t pid_rec, void *arg)
{
	struct sample_netdev_ctxt_s *ctx = arg;

	/* Parse /proc/<pid>/net/dev and add entries to netdev_list */
	return parse_proc_netdev(pid, ctx->set, ctx->jctxt, ctx->log);
}

/**
 * Sample this job
 * Called for each active job during sample()
 */
static int sample_job(per_job_sampler_t job_base, void *job_ctxt)
{
	struct netdev_job_ctxt_s *jctxt = job_ctxt;
	ldms_set_t set = per_job_base_get_set(job_base);
	per_job_netdev_t pjn = ldmsd_plug_ctxt_get(
		ldmsd_plug_handle_get_from_ctxt(set));
	ldms_mval_t netdev_list;

	/* Clear existing netdev entries */
	netdev_list = ldms_metric_get(set, jctxt->netdev_list_midx);
	if (netdev_list) {
		ldms_list_purge(set, netdev_list);
	}

	/* Parse netdev for each PID */
	struct sample_netdev_ctxt_s ctx = {
		.jctxt = jctxt,
		.set = set,
		.log = pjn->log,
	};

	return per_job_base_foreach_pid(job_base, sample_netdev_for_pid, &ctx);
}

/* ============================================================================
 * LDMSD Plugin Interface
 * ============================================================================ */

static int constructor(ldmsd_plug_handle_t handle)
{
	per_job_netdev_t pjn = calloc(1, sizeof(*pjn));
	if (!pjn)
		return ENOMEM;

	pjn->log = ldmsd_plug_log_get(handle);
	ldmsd_plug_ctxt_set(handle, pjn);

	return 0;
}

static int config(ldmsd_plug_handle_t handle,
		  struct attr_value_list *kwl,
		  struct attr_value_list *avl)
{
	per_job_netdev_t pjn = ldmsd_plug_ctxt_get(handle);
	ldms_record_t pid_rec_def, netdev_rec_def;
	int rc;

	/* Get configuration parameters */
	char *producer = av_value(avl, "producer");
	if (!producer) {
		ovis_log(pjn->log, OVIS_LERROR, "producer is required\n");
		return EINVAL;
	}

	char *schema = av_value(avl, "schema");
	if (!schema)
		schema = "per_job_netdev";

	char *instance = av_value(avl, "instance");
	char *tenant = av_value(avl, "tenant");
	char *key_field = av_value(avl, "key_field");

	char *max_pids_str = av_value(avl, "max_pids");
	int max_pids = max_pids_str ? atoi(max_pids_str) : 100;

	/* Create PID record definition (simple, just PID) */
	pid_rec_def = ldms_record_from_template("netdev_pid_rec", pid_metrics, NULL);
	if (!pid_rec_def) {
		ovis_log(pjn->log, OVIS_LERROR,
			 "Failed to create PID record definition\n");
		return ENOMEM;
	}

	/* Create netdev record definition */
	netdev_rec_def = ldms_record_from_template("netdev_rec", netdev_metrics, NULL);
	if (!netdev_rec_def) {
		ovis_log(pjn->log, OVIS_LERROR,
			 "Failed to create netdev record definition\n");
		ldms_record_delete(pid_rec_def);
		return ENOMEM;
	}

	/* Set up callbacks */
	struct per_job_plugin_callbacks_s callbacks = {
		.job_init = job_init,
		.job_cleanup = job_cleanup,
		.pid_added = pid_added,
		.sample_job = sample_job,
	};

	/* Create sampler - base handles standard metrics and pid_list */
	pjn->sampler = per_job_sampler_create(
		handle,
		producer,
		schema,
		instance,
		tenant,
		key_field,
		pid_rec_def,
		max_pids,
		&callbacks,
		NULL
	);

	if (!pjn->sampler) {
		rc = errno;
		ovis_log(pjn->log, OVIS_LERROR,
			 "Failed to create sampler: %d\n", rc);
		ldms_record_delete(netdev_rec_def);
		ldms_record_delete(pid_rec_def);
		return rc;
	}

	/* Add netdev_list to the schema
	 * Assume max 10 interfaces per PID
	 */
	rc = per_job_sampler_add_list(pjn->sampler, "netdev_list",
				       netdev_rec_def, max_pids * 10);
	if (rc < 0) {
		ovis_log(pjn->log, OVIS_LERROR,
			 "Failed to add netdev_list: %d\n", -rc);
		per_job_sampler_destroy(pjn->sampler);
		ldms_record_delete(netdev_rec_def);
		return -rc;
	}

	return 0;
}

static int sample(ldmsd_plug_handle_t handle)
{
	per_job_netdev_t pjn = ldmsd_plug_ctxt_get(handle);

	/* Base handles everything */
	return per_job_sampler_sample(pjn->sampler);
}

static void destructor(ldmsd_plug_handle_t handle)
{
	per_job_netdev_t pjn = ldmsd_plug_ctxt_get(handle);

	if (!pjn)
		return;

	/* Base cleans up everything */
	per_job_sampler_destroy(pjn->sampler);
	free(pjn);
}

static const char *usage(ldmsd_plug_handle_t handle)
{
	return "config name=per_job_netdev producer=<n> [schema=<schema>] "
	       "[instance=<instance>] [tenant=<tenant>] [key_field=<field>] "
	       "[max_pids=<max_pids>]\n"
	       "\n"
	       "Required:\n"
	       "    producer    - Producer name\n"
	       "\n"
	       "Optional:\n"
	       "    schema      - Schema name (default: per_job_netdev)\n"
	       "    instance    - Instance name for self-contained mode\n"
	       "    tenant      - Tenant definition name (for shared mode)\n"
	       "    key_field   - Binding key field (default: job_id)\n"
	       "    max_pids    - Max PIDs per job (default: 100)\n"
	       "\n"
	       "Schema structure:\n"
	       "  - Per-job metrics: job_id, binding_key\n"
	       "  - pid_list: PID records (just PID for correlation)\n"
	       "  - netdev_list: Network interface records\n"
	       "    Each netdev record includes:\n"
	       "      pid, iface, rx_bytes, rx_packets, rx_errs, rx_drop,\n"
	       "      rx_fifo, rx_frame, rx_compressed, rx_multicast,\n"
	       "      tx_bytes, tx_packets, tx_errs, tx_drop, tx_fifo,\n"
	       "      tx_colls, tx_carrier, tx_compressed\n"
	       "\n"
	       "Three deployment modes (same as per_job_stat):\n"
	       "  1. Self-contained\n"
	       "  2. Custom binding key\n"
	       "  3. Shared tenant\n";
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
