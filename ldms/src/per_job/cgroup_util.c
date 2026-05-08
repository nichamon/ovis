/* cgroup_util.c - cgroup v2 path resolution, parsing, and metric discovery */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include "cgroup_util.h"

/* -----------------------------------------------------------------------
 * Path resolution
 * ----------------------------------------------------------------------- */

static int dir_exists(const char *path)
{
	struct stat st;
	return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

int cgroup_resolve_job_path(const char *cgroup_root,
			    const char *cgroup_path_pattern,
			    const char *job_id,
			    char *path_out,
			    size_t path_out_len)
{
	if (cgroup_path_pattern) {
		/* User-specified pattern: one %s substituted with job_id */
		snprintf(path_out, path_out_len, cgroup_path_pattern, job_id);
		if (dir_exists(path_out))
			return 0;
		path_out[0] = '\0';
		return ENOENT;
	}

	/* Auto-detection: try known scheduler layouts in order */
	static const char *templates[] = {
		"%s/system.slice/slurmstepd.scope/job_%s",  /* Slurm */
		"%s/flux.service/job/%s",                    /* Flux  */
		"%s/job_%s",                                 /* generic */
		NULL
	};

	for (int i = 0; templates[i]; i++) {
		snprintf(path_out, path_out_len, templates[i],
			 cgroup_root, job_id);
		if (dir_exists(path_out))
			return 0;
	}

	path_out[0] = '\0';
	return ENOENT;
}

/* -----------------------------------------------------------------------
 * Stat file parsing
 * ----------------------------------------------------------------------- */

int cgroup_parse_stat_file(const char *path, struct cgroup_stat_s *out)
{
	FILE *f;
	char line[256];

	memset(out, 0, sizeof(*out));

	f = fopen(path, "r");
	if (!f)
		return errno;

	while (fgets(line, sizeof(line), f) && out->count < CGROUP_MAX_KV) {
		char key[64];
		uint64_t val;
		if (sscanf(line, "%63s %lu", key, &val) == 2) {
			strncpy(out->kv[out->count].key, key,
				sizeof(out->kv[out->count].key) - 1);
			out->kv[out->count].val = val;
			out->count++;
		}
	}

	int rc = ferror(f) ? errno : 0;
	fclose(f);
	return rc;
}

int cgroup_stat_get(const struct cgroup_stat_s *stat,
		    const char *key,
		    uint64_t *val)
{
	for (int i = 0; i < stat->count; i++) {
		if (strcmp(stat->kv[i].key, key) == 0) {
			*val = stat->kv[i].val;
			return 0;
		}
	}
	return ENOENT;
}

/* -----------------------------------------------------------------------
 * Discovery internals
 * ----------------------------------------------------------------------- */

static void sanitize_ldms_name(char *s)
{
	/* Replace characters invalid in LDMS metric names */
	for (; *s; s++) {
		if (*s == '.' || *s == '-')
			*s = '_';
	}
}

static int append_desc(struct cgroup_metric_desc_list_s *list,
		       const char *ldms_name,
		       const char *cgroup_file,
		       const char *cgroup_key,
		       const char *unit,
		       cgroup_source_t source)
{
	int n = list->count;
	struct cgroup_metric_desc_s *tmp =
		realloc(list->descs, (n + 1) * sizeof(*list->descs));
	if (!tmp)
		return ENOMEM;
	list->descs = tmp;

	struct cgroup_metric_desc_s *d = &list->descs[n];
	memset(d, 0, sizeof(*d));
	strncpy(d->ldms_name,   ldms_name,   sizeof(d->ldms_name)   - 1);
	strncpy(d->cgroup_file, cgroup_file, sizeof(d->cgroup_file) - 1);
	strncpy(d->cgroup_key,  cgroup_key,  sizeof(d->cgroup_key)  - 1);
	strncpy(d->unit,        unit,        sizeof(d->unit)        - 1);
	d->source = source;
	list->count++;
	return 0;
}

/*
 * Infer unit from a cpu.stat key name.
 * Keys ending in "_usec" are microseconds; everything else is unitless.
 */
static const char *cpu_stat_unit(const char *key)
{
	size_t len = strlen(key);
	if (len > 5 && strcmp(key + len - 5, "_usec") == 0)
		return "usec";
	return "";
}

static int discover_stat_file(const char *cgroup_path,
			      const char *filename,
			      const char *ldms_prefix,
			      const char *unit_default,
			      int infer_unit,
			      cgroup_source_t source,
			      struct cgroup_metric_desc_list_s *list)
{
	char path[PATH_MAX];
	struct cgroup_stat_s raw;
	int rc;

	snprintf(path, sizeof(path), "%s/%s", cgroup_path, filename);

	rc = cgroup_parse_stat_file(path, &raw);
	if (rc == ENOENT || rc == EACCES)
		return 0;  /* File absent on this kernel -- skip silently */
	if (rc)
		return rc;

	for (int i = 0; i < raw.count; i++) {
		char ldms_name[80];
		char sanitized_key[64];

		strncpy(sanitized_key, raw.kv[i].key, sizeof(sanitized_key) - 1);
		sanitize_ldms_name(sanitized_key);
		snprintf(ldms_name, sizeof(ldms_name), "%s_%s",
			 ldms_prefix, sanitized_key);

		const char *unit = infer_unit
				 ? cpu_stat_unit(raw.kv[i].key)
				 : unit_default;

		rc = append_desc(list, ldms_name, filename,
				 raw.kv[i].key, unit, source);
		if (rc)
			return rc;
	}
	return 0;
}

/* -----------------------------------------------------------------------
 * Public: cgroup_discover_metrics
 * ----------------------------------------------------------------------- */

int cgroup_discover_metrics(const char *cgroup_path,
			    cgroup_source_t sources,
			    struct cgroup_metric_desc_list_s **list_out)
{
	int rc = 0;

	struct cgroup_metric_desc_list_s *list = calloc(1, sizeof(*list));
	if (!list)
		return ENOMEM;

	/*
	 * cpu.stat keys: usage_usec, user_usec, system_usec, nr_periods,
	 *   nr_throttled, throttled_usec, nr_bursts (>=5.14), burst_usec (>=5.14)
	 * Time fields inferred from "_usec" suffix; count fields are unitless.
	 */
	if (sources & CGROUP_SRC_CPU_STAT) {
		rc = discover_stat_file(cgroup_path, "cpu.stat",
					"cpu", "", 1 /* infer unit */,
					CGROUP_SRC_CPU_STAT, list);
		if (rc) goto err;
	}

	/*
	 * memory.stat keys: anon, file, kernel, kernel_stack, slab, sock,
	 *   anon_thp, pgfault, pgmajfault, pgrefill, pgscan, pgsteal, ...
	 * Byte-valued fields dominate; pg* fields are page counts but we
	 * annotate them "bytes" too for simplicity in this PoC.
	 */
	if (sources & CGROUP_SRC_MEM_STAT) {
		rc = discover_stat_file(cgroup_path, "memory.stat",
					"mem", "bytes", 0 /* use default */,
					CGROUP_SRC_MEM_STAT, list);
		if (rc) goto err;
	}

	*list_out = list;
	return 0;
err:
	cgroup_metric_desc_list_free(list);
	return rc;
}

void cgroup_metric_desc_list_free(struct cgroup_metric_desc_list_s *list)
{
	if (!list)
		return;
	free(list->descs);
	free(list);
}

/* -----------------------------------------------------------------------
 * Runtime sampling
 * ----------------------------------------------------------------------- */

/*
 * Per-call file cache: each stat file is opened at most once even if
 * multiple metrics share it. We only have 2 files (cpu.stat, memory.stat)
 * so a small linear array is fine.
 */
#define MAX_CACHED_FILES 4

struct file_cache_s {
	char filename[32];
	struct cgroup_stat_s stat;
	int load_rc;
};

static struct cgroup_stat_s *get_cached_stat(struct file_cache_s *cache,
					     int *cache_count,
					     const char *job_cgroup_path,
					     const char *filename)
{
	for (int i = 0; i < *cache_count; i++) {
		if (strcmp(cache[i].filename, filename) == 0)
			return cache[i].load_rc ? NULL : &cache[i].stat;
	}

	if (*cache_count >= MAX_CACHED_FILES)
		return NULL;

	struct file_cache_s *e = &cache[*cache_count];
	strncpy(e->filename, filename, sizeof(e->filename) - 1);

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", job_cgroup_path, filename);
	e->load_rc = cgroup_parse_stat_file(path, &e->stat);
	(*cache_count)++;

	return e->load_rc ? NULL : &e->stat;
}

int cgroup_read_metrics(const char *job_cgroup_path,
			const struct cgroup_metric_desc_list_s *list,
			uint64_t *values_out)
{
	struct file_cache_s cache[MAX_CACHED_FILES];
	int cache_count = 0;

	memset(cache, 0, sizeof(cache));
	memset(values_out, 0, list->count * sizeof(uint64_t));

	for (int i = 0; i < list->count; i++) {
		const struct cgroup_metric_desc_s *d = &list->descs[i];
		struct cgroup_stat_s *stat =
			get_cached_stat(cache, &cache_count,
					job_cgroup_path, d->cgroup_file);
		if (stat)
			cgroup_stat_get(stat, d->cgroup_key, &values_out[i]);
		/* else: file unreadable (job ended?) -- value stays 0 */
	}

	return 0;
}
