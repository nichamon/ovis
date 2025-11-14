#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"
#include "ldmsd_jobmgr_query.h"

#define JOB_ID_NAME_LEN 512

#define DEFAULT_SCHEMA_NAME "test_per_job_metrics"
#define DEFAULT_MAX_NUM_PIDS 10
/*
 * The sampler plugin creates a set per job
 */

typedef struct pid_src_s {
	FILE *mf;
	TAILQ_ENTRY(pid_src_s) ent;
} *pid_src_t;

typedef struct job_set_s {
	ldms_set_t set;
	int is_exited; /* 1 means job has been exited. */
	TAILQ_HEAD(pid_src_list, pid_src_s);
	TAILQ_ENTRY(job_set_s) set_ent;
} *job_set_t;

typedef struct job_metrics_s {
	ovis_log_t log;
	const char *producer;
	ldmsd_jobmgr_query_t jquery;
	ldms_schema_t schema;
	ldms_record_t recdef; /* record definition including jobmgr's record entries */
	int pid_rec_ent_mids;
	int recdef_mid;

	int pid_mrec_list_mid;
	int pid_rec_list_mid;

	int *mids;
	TAILQ_HEAD(job_set_list, job_set_s) jset_list; /* List of per-job sets */
} *job_metrics_t;

av_list_t *default_job_av;

static const char *usage()
{
	return "";
}

static int constructor(ldmsd_plug_handle_t handle)
{
	int rc;
	job_metrics_t inst = calloc(1, sizeof(*inst));
	if (!inst)
		return ENOMEM;
	ldmsd_plug_ctxt_set(handle, inst);
	inst->log = ldmsd_plug_log_get(handle);

	default_job_av = av_value_list("job_id,step_id,task_id,task_pid", ",");
	if (!default_job_av) {
		rc = ENOMEM;
		goto err;
	}

	TAILQ_INIT(&inst->jset_list);
	return 0;
err:
	free(inst);
	return rc;
}

static ldms_set_t __create_set(job_metrics_t jm_handle, const char *job_id, const char *job_key)
{
	ldms_set_t set;
	ldms_mval_t mv;
	int i = 0;
	char *set_name;
	size_t set_name_len;

	set_name_len = strlen(jm_handle->producer) + strlen(job_id);
	set_name = malloc(set_name_len);
	snprintf(set_name, set_name_len, "%s/%s", jm_handle->producer, job_id);


	set = ldms_set_create(set_name, jm_handle->schema, 0, 0, 0, 0); /* TODO: fix this */
	if (!set)
		return NULL;

	ldms_transaction_begin(set);
	/* TODO: clean this up mids[0] it isn't robust */
	mv = ldms_metric_get(set, jm_handle->mids[0]); /* Get the job_id metric; TODO: make this line more robust */
	snprintf(mv->a_char, JOB_ID_NAME_LEN, "%s", job_id);
	/* TODO: clean this up mids[0] it isn't robust */
	mv = ldms_metric_get(set, jm_handle->mids[1]);
	snprintf(mv->a_char, JOB_ID_NAME_LEN, "%s", job_key);
	ldms_transaction_end(set);
	return set;
}

static int __job_start_handle(job_metrics_t jm_handle, ldmsd_jobmgr_query_event_t e)
{
	int i = 0;
	job_set_t jset;
	ldms_mval_t qrec = e->start_end.qrec;
	char *key, *value;
	size_t len;
	ldms_mval_t mv;
	char *job_id, *job_key;

	jset = calloc(1, sizeof(*jset));
	if (!jset) {
		ovis_log(jm_handle->log, OVIS_LCRIT, "Memory allocation failure.\n");
		return ENOMEM;
	}

	for (i = 0; i < ldms_record_card(qrec); i++) {
		key = ldms_record_metric_name_get(qrec, i);
		value = ldms_record_metric_type_get(qrec, i, &len);
		mv = ldms_record_metric_get(qrec, i);
		if (0 == strcmp(key, "job_id"))
			job_id = value;
		else if (0 == strcmp(key, "job_key"))
			job_key = value;
	}

	jset->set = __create_set(jm_handle, job_id, job_key);
	if (!jset->set) {
		/* TODO: handle this */
	}

	TAILQ_INSERT_TAIL(&jm_handle->jset_list, jset, set_ent);

	ldms_set_publish(jset->set);
	return 0;
}

static int jm_jquery_cb(ldmsd_jobmgr_query_t q, ldmsd_jobmgr_query_event_t e, void *arg)
{
	job_metrics_t jm_handle = (job_metrics_t)arg;

	switch (e->event_type) {
	case LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE:
		/* TODO: complete this */
		assert(ENOSYS);
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_START:
		/* nothing to do */
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_END:
		/* TODO: search for the set and mark as exited */
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_START:
		/* TODO: confirm if we have anything to do */
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_END:
		/* TODO: confirm if we have anything to do */
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_START:
		/* TODO: get the PID from the event
		 *       begin transaction
		 *       create a record and append to the list
		 *       Open /proc/<pid>/....
		 *       assign first metric values, e.g., /proc/<pid>/statm....
		 * ......end transaction
		 */
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_END:
		/* TODO: see if we need to do anyting */
		break;
	}
}


/* Forward declarations */
static int extract_multi_value_metrics(const char *key, const char *value_str,
				       struct ldms_metric_template_s *specs, int max_specs);
static int determine_metric_specs(const char *key, const char *value_str,
				  struct ldms_metric_template_s *specs, int max_specs);

/* Determine metric specifications based on field name and value
 * Uses background knowledge to properly type and size metrics
 * Returns number of metric specs added
 */
static int determine_metric_specs(const char *key, const char *value_str,
				  struct ldms_metric_template_s *specs, int max_specs)
{
	uint64_t num;
	char *endptr;
	char *unit_start;
	char unit_buf[32];
	size_t unit_len;
	int count = 0;

	if (!key || !value_str || count >= max_specs)
		return 0;

	/* Multi-value fields: parse each number separately */
	if (strcmp(key, "Uid") == 0 || strcmp(key, "Gid") == 0) {
		return extract_multi_value_metrics(key, value_str, specs, max_specs);
	}

	/* SigQ has 2 values: pending/max */
	if (strcmp(key, "SigQ") == 0) {
		return extract_multi_value_metrics(key, value_str, specs, max_specs);
	}

	/* Try to parse as numeric with optional unit */
	num = strtoull(value_str, &endptr, 10);

	if (endptr != value_str && (*endptr == '\0' || isspace(*endptr) || !isdigit(*endptr))) {
		/* Numeric value - extract unit if present */
		unit_buf[0] = '\0';

		/* Skip whitespace */
		while (*endptr && isspace(*endptr))
			endptr++;

		/* Extract unit */
		if (*endptr != '\0') {
			unit_start = endptr;
			unit_len = 0;
			while (*endptr && !isspace(*endptr)) {
				unit_len++;
				endptr++;
			}
			if (unit_len < sizeof(unit_buf)) {
				strncpy(unit_buf, unit_start, unit_len);
				unit_buf[unit_len] = '\0';
			}
		}

		specs[count].name = key;
		specs[count].type = LDMS_V_U64;
		specs[count].unit = (unit_buf[0] != '\0') ? strdup(unit_buf) : "";
		specs[count].len = 0;
		specs[count].flags = 0;
		specs[count].rec_def = NULL;
		count++;
	} else {
		/* String value - determine reasonable max length */
		size_t str_len = strlen(value_str);
		uint32_t array_len = 256; /* Default for most strings */

		/* Use background knowledge for specific fields */
		if (strcmp(key, "Name") == 0 || strcmp(key, "State") == 0) {
			array_len = 64; /* Process names and states are typically short */
		} else if (strncmp(key, "Cap", 3) == 0) {
			array_len = 32; /* Capability masks are short hex strings */
		} else if (strncmp(key, "Cpus_allowed", 12) == 0 || strncmp(key, "Mems_allowed", 12) == 0) {
			array_len = 256; /* CPU/memory masks can be long */
		} else if (str_len > 0) {
			/* Use actual length + some padding for dynamic content */
			array_len = (str_len + 16) < 512 ? (str_len + 16) : 512;
		}

		specs[count].name = key;
		specs[count].type = LDMS_V_CHAR_ARRAY;
		specs[count].unit = "";
		specs[count].len = array_len;
		specs[count].flags = 0;
		specs[count].rec_def = NULL;
		count++;
	}

	return count;
}

/* Extract multiple numeric values from fields like Uid, Gid, SigQ
 * Creates separate metrics for each value with descriptive names
 * Return the number of metrics, or -errno on error
 */
static int extract_multi_value_metrics(const char *key, const char *value_str,
				       struct ldms_metric_template_s *specs, int max_specs)
{
	char *copy, *token, *saveptr;
	uint64_t values[max_specs];
	int value_count = 0;
	int spec_count = 0;

	copy = strdup(value_str);
	if (!copy)
		return -ENOMEM;

	/* Parse space-separated numeric values */
	token = strtok_r(copy, " \t", &saveptr);
	while (token && value_count < max_specs) {
		char *endptr;
		values[value_count] = strtoull(token, &endptr, 10);
		if (endptr != token) {
			value_count++;
		}
		token = strtok_r(NULL, " \t", &saveptr);
	}
	free(copy);

	if (value_count == 0)
		return 0;

	/* Create metric specs with descriptive names */
	if (strcmp(key, "Uid") == 0) {
		const char *uid_names[] = { "real_uid", "effective_uid", "saved_uid", "filesystem_uid" };
		for (int i = 0; i < value_count && spec_count < max_specs; i++) {
			specs[spec_count].name = uid_names[i];
			specs[spec_count].type = LDMS_V_U64;
			specs[spec_count].unit = "";
			specs[spec_count].len = 0;
			specs[spec_count].flags = 0;
			specs[spec_count].rec_def = NULL;
			spec_count++;
		}
	} else if (strcmp(key, "Gid") == 0) {
		const char *gid_names[] = { "real_gid", "effective_gid", "saved_gid", "filesystem_gid" };
		for (int i = 0; i < value_count && spec_count < max_specs; i++) {
			specs[spec_count].name = gid_names[i];
			specs[spec_count].type = LDMS_V_U64;
			specs[spec_count].unit = "";
			specs[spec_count].len = 0;
			specs[spec_count].flags = 0;
			specs[spec_count].rec_def = NULL;
			spec_count++;
		}
	} else if (strcmp(key, "SigQ") == 0) {
		const char *sigq_names[] = { "sigq_pending", "sigq_max" };
		for (int i = 0; i < value_count && spec_count < max_specs; i++) {
			specs[spec_count].name = sigq_names[i];
			specs[spec_count].type = LDMS_V_U64;
			specs[spec_count].unit = "";
			specs[spec_count].len = 0;
			specs[spec_count].flags = 0;
			specs[spec_count].rec_def = NULL;
			spec_count++;
		}
	}

	return spec_count;
}

/* Parse a single line from /proc/pid/status and generate metric templates
 * Returns number of metrics added to the template array, or -1 on error
 */
int pid_status_line_parse(job_metrics_t jm_handle, char *line,
			  struct ldms_metric_template_s *template_array,
			  int max_metrics, int *idx)
{
	char *key, *value_str, *colon;
	char *line_copy, *trimmed_value;
	struct ldms_metric_template_s specs[4];
	int spec_count;
	size_t len;
	int added = 0;

	if (!line || !template_array || !idx) {
		return -1;
	}

	/* Make a copy since we'll modify it */
	line_copy = strdup(line);
	if (!line_copy)
		return -1;

	/* Remove trailing newline */
	len = strlen(line_copy);
	if (len > 0 && line_copy[len - 1] == '\n')
		line_copy[len - 1] = '\0';

	/* Skip empty lines */
	if (strlen(line_copy) == 0) {
		free(line_copy);
		return 0;
	}

	/* Split on colon */
	colon = strchr(line_copy, ':');
	if (!colon) {
		free(line_copy);
		return 0;
	}

	/* Extract key */
	*colon = '\0';
	key = line_copy;

	/* Extract and trim value */
	value_str = colon + 1;
	while (*value_str && isspace(*value_str))
		value_str++;

	trimmed_value = value_str;
	len = strlen(trimmed_value);
	while (len > 0 && isspace(trimmed_value[len - 1]))
		len--;

	/* Determine what metrics this line produces */
	spec_count = determine_metric_specs(key, trimmed_value, specs, 4);

	/* Add each metric spec to the template array */
	for (int i = 0; i < spec_count; i++) {
		template_array[*idx] = specs[i];
		(*idx)++;
		added++;
	}

	free(line_copy);
	return added;
}

/* Parse the entire /proc/pid/status file and populate metric template array
 * Returns the number of metrics parsed, or -1 on error
 */
int pid_status_parse(job_metrics_t jm_handle, FILE *fin,
		     struct ldms_metric_template_s *template_array, int max_metrics)
{
	char line[1024];
	int idx = 0;
	int rc;

	if (!fin || !template_array || max_metrics <= 0)
		return -1;

	while (fgets(line, sizeof(line), fin)) {
		if (idx >= max_metrics) {
			ovis_log(jm_handle->log, OVIS_LWARNING,
				 "Warning: the number of metrics in /proc/<pid>/status " \
				 "exceeded metric template array size (%d)\n", max_metrics);
			break;
		}

		rc = pid_status_line_parse(jm_handle, line, template_array, max_metrics - idx, &idx);
		if (rc < 0) {
			ovis_log(jm_handle->log, OVIS_LERROR,
				 "Error parsing line: %s\n", line);
			return -EINVAL;
		}
	}

	return idx;
}

static struct ldms_metric_template_s per_job_common_metrics[] = {
	{ "job_id",	0,	LDMS_V_CHAR_ARRAY,	"",	JOB_ID_NAME_LEN },
	{0},
};

static struct ldms_metric_template_s test_recdef[] = {
	{ "pid",	0,	LDMS_V_CHAR_ARRAY,	"",		512 },
	{ "mem",	0,	LDMS_V_U64,		"bytes",	1 },
	{ "cpu",	0,	LDMS_V_U64,		"bytes",	1 },
	{ "str",	0,	LDMS_V_CHAR_ARRAY,	"",		512 },
	{0}
};

static int config(ldmsd_plug_handle_t handle,
		  struct attr_value_list *kwl, struct attr_value_list *avl)
{
	int rc;
	size_t heap_sz;
	int max_num_pids = DEFAULT_MAX_NUM_PIDS;
	char *jquery_errstr = NULL;
	job_metrics_t jm_handle = ldmsd_plug_ctxt_get(handle);

	jm_handle->jquery = ldmsd_jobmgr_query_new(default_job_av, jm_jquery_cb,
						   jm_handle, &jquery_errstr);

	jm_handle->recdef = ldms_record_create("pid_recdef");
	jm_handle->recdef = ldms_record_from_template("pid_recdef", test_recdef, jm_handle->pid_rec_ent_mids);

	jm_handle->schema = ldms_schema_from_template("per_job_schema", per_job_common_metrics, jm_handle->mids);
	jm_handle->recdef_mid = ldms_schema_record_add(jm_handle->schema, jm_handle->recdef);

	heap_sz = ldms_record_heap_size_get(jm_handle->jquery->recdef) * max_num_pids;
	jm_handle->pid_mrec_list_mid =  ldms_schema_metric_list_add(jm_handle->schema, "pid_mrec_list", "", heap_sz);

	TAILQ_INIT(&jm_handle->jset_list);
	return 0;
}

static int sample(ldmsd_plug_handle_t handle)
{
	job_metrics_t jm_handle = ldmsd_plug_ctxt_get(handle);
	int rc;

	/* TODO: complete this */

	return 0;
}

static void destructor(ldmsd_plug_handle_t handle)
{
	job_metrics_t jm_handle = ldmsd_plug_ctxt_get(handle);
	/* TODO: Clear up any memory/file that have been allocated/opened in the plugin */
}

struct ldmsd_sampler ldmsd_plugin_interface = {
	.base.type = LDMSD_PLUGIN_SAMPLER,
	.base.flags = LDMSD_PLUGIN_MULTI_INSTANCE,
	.base.config = config,
	.base.usage = usage,
	.base.constructor = constructor,
	.base.destructor = destructor,

	.sample = sample,
}