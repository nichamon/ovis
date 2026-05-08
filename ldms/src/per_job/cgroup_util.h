/* cgroup_util.h - cgroup v2 path resolution and metric discovery
 *
 * Only *.stat files are supported: cpu.stat and memory.stat.
 * Both files exist at the cgroup root (/sys/fs/cgroup) even with no jobs
 * running, so schema discovery happens at config() time without needing
 * a live job cgroup or any template path.
 *
 * At sample time, metrics are read from the job-specific cgroup directory
 * resolved via cgroup_resolve_job_path() using the job_id from jobmgr.
 */

#ifndef __CGROUP_UTIL_H__
#define __CGROUP_UTIL_H__

#include <stdint.h>
#include <sys/types.h>

/* Maximum key=value pairs from one stat file.
 * cpu.stat ~15 fields, memory.stat ~50+. 96 is safe headroom. */
#define CGROUP_MAX_KV 96

struct cgroup_kv_s {
	char     key[64];
	uint64_t val;
};

struct cgroup_stat_s {
	struct cgroup_kv_s kv[CGROUP_MAX_KV];
	int count;
};

/* -----------------------------------------------------------------------
 * Path resolution
 * ----------------------------------------------------------------------- */

/**
 * Resolve the cgroup v2 directory for a given job_id.
 *
 * If cgroup_path_pattern is non-NULL it is used directly as a printf
 * pattern with one %s substituted for job_id (e.g.
 * "/sys/fs/cgroup/system.slice/slurmstepd.scope/job_%s").
 *
 * Otherwise auto-detection tries these layouts in order:
 *   1. <root>/system.slice/slurmstepd.scope/job_<job_id>/   (Slurm)
 *   2. <root>/flux.service/job/<job_id>/                     (Flux)
 *   3. <root>/job_<job_id>/                                  (generic)
 *
 * \param cgroup_root         Base of the cgroup v2 hierarchy
 * \param cgroup_path_pattern Optional printf pattern; NULL for auto-detect
 * \param job_id              Job ID string from jobmgr
 * \param path_out            Buffer to receive the resolved path
 * \param path_out_len        Size of path_out
 *
 * \return 0 on success, ENOENT if not found
 */
int cgroup_resolve_job_path(const char *cgroup_root,
			    const char *cgroup_path_pattern,
			    const char *job_id,
			    char *path_out,
			    size_t path_out_len);

/* -----------------------------------------------------------------------
 * Stat file parsing
 * ----------------------------------------------------------------------- */

/**
 * Parse a "key value\n" stat file into a cgroup_stat_s.
 * Non-matching lines are silently skipped.
 * \return 0 on success, errno on failure
 */
int cgroup_parse_stat_file(const char *path, struct cgroup_stat_s *out);

/**
 * Look up a key in a parsed stat structure.
 * \return 0 if found, ENOENT if not found
 */
int cgroup_stat_get(const struct cgroup_stat_s *stat,
		    const char *key,
		    uint64_t *val);

/* -----------------------------------------------------------------------
 * Metric discovery
 *
 * Called once at config() time using cgroup_root as the probe path.
 * cpu.stat and memory.stat exist at the cgroup root on any cgroup v2
 * system, so no live job cgroup is needed for discovery.
 * ----------------------------------------------------------------------- */

/**
 * Which stat files to include in discovery. OR together for multiple.
 */
typedef enum cgroup_source_e {
	CGROUP_SRC_CPU_STAT = (1 << 0),   /* cpu.stat    */
	CGROUP_SRC_MEM_STAT = (1 << 1),   /* memory.stat */
	CGROUP_SRC_ALL      = (CGROUP_SRC_CPU_STAT | CGROUP_SRC_MEM_STAT),
} cgroup_source_t;

/**
 * One discovered metric -- enough to add it to an LDMS schema and to
 * read its value at sample time.
 *
 * LDMS metric name is "<prefix>_<key>" where prefix is "cpu" or "mem":
 *   cpu.stat  "usage_usec"  ->  "cpu_usage_usec"
 *   memory.stat "anon"      ->  "mem_anon"
 */
struct cgroup_metric_desc_s {
	char ldms_name[80];    /* LDMS metric name                       */
	char cgroup_file[32];  /* source file, e.g. "cpu.stat"           */
	char cgroup_key[64];   /* key within the file, e.g. "usage_usec" */
	char unit[16];         /* e.g. "usec", "bytes", ""               */
	cgroup_source_t source;
};

struct cgroup_metric_desc_list_s {
	struct cgroup_metric_desc_s *descs;
	int count;
};

/**
 * Discover available metrics by reading stat files from cgroup_path.
 *
 * Every "key value" line in the selected stat files becomes one metric
 * descriptor. Files not present on this kernel are silently skipped.
 *
 * Call once at config() time with cgroup_root as cgroup_path.
 *
 * \param cgroup_path  Directory to probe (cgroup_root at config time)
 * \param sources      Bitmask of CGROUP_SRC_* flags
 * \param list_out     [out] Allocated list; free with cgroup_metric_desc_list_free()
 *
 * \return 0 on success, ENOMEM on allocation failure
 */
int cgroup_discover_metrics(const char *cgroup_path,
			    cgroup_source_t sources,
			    struct cgroup_metric_desc_list_s **list_out);

/** Free a list from cgroup_discover_metrics(). Safe to call with NULL. */
void cgroup_metric_desc_list_free(struct cgroup_metric_desc_list_s *list);

/* -----------------------------------------------------------------------
 * Runtime sampling
 * ----------------------------------------------------------------------- */

/**
 * Read current values for all discovered metrics from a job's cgroup.
 *
 * Each stat file is opened at most once per call. On partial failure
 * (file disappeared mid-job), affected values are left as 0 and the
 * function returns 0 -- zeros are published rather than skipping the sample.
 *
 * \param job_cgroup_path  Job cgroup directory from cgroup_resolve_job_path()
 * \param list             Descriptor list from cgroup_discover_metrics()
 * \param values_out       Caller-allocated uint64_t[list->count];
 *                         values_out[i] corresponds to list->descs[i]
 *
 * \return 0 always
 */
int cgroup_read_metrics(const char *job_cgroup_path,
			const struct cgroup_metric_desc_list_s *list,
			uint64_t *values_out);

#endif /* __CGROUP_UTIL_H__ */
