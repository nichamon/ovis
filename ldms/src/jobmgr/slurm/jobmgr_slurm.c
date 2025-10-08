/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2025 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2025 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of Sandia nor the names of any contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *      Neither the name of Open Grid Computing nor the names of any
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *      Modified source versions must be plainly marked as such, and
 *      must not be misrepresented as being the original software.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "jobmgr_slurm.h"

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_jobmgr.h"
#include "ldmsd_jobmgr_query.h"
#include "ldmsd_plug_api.h"

#include "ovis_json/ovis_json.h"

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) (sizeof(a)/sizeof(*a))
#endif /* ARRAY_LEN */

#define JOB_TAG_STR_LEN 256

enum jobmgr_slurm_metric_e {

	JOBMGR_SLURM_METRIC_JOB_ID = 0x1,
	JOBMGR_SLURM_METRIC_USER,
	JOBMGR_SLURM_METRIC_JOB_NAME,
	JOBMGR_SLURM_METRIC_JOB_UID,
	JOBMGR_SLURM_METRIC_JOB_GID,
	JOBMGR_SLURM_METRIC_JOB_START,
	JOBMGR_SLURM_METRIC_JOB_END,
	JOBMGR_SLURM_METRIC_NODE_COUNT,
	JOBMGR_SLURM_METRIC_JOB_TAG,

	JOBMGR_SLURM_METRIC_STEP_ID = 0x101,
	JOBMGR_SLURM_METRIC_STEP_START,
	JOBMGR_SLURM_METRIC_STEP_END,
	JOBMGR_SLURM_METRIC_TOTAL_TASKS,
	JOBMGR_SLURM_METRIC_LOCAL_TASKS,

	JOBMGR_SLURM_METRIC_TASK_ID = 0x201,
	JOBMGR_SLURM_METRIC_TASK_PID,
	JOBMGR_SLURM_METRIC_TASK_RANK,
	JOBMGR_SLURM_METRIC_TASK_START,
	JOBMGR_SLURM_METRIC_TASK_END,
	JOBMGR_SLURM_METRIC_TASK_EXIT_STATUS,

};

struct jobmgr_slurm_mdesc_s {
	const char *name;
	enum ldms_value_type v_type;
	const char *unit;
	int len;
	enum jobmgr_slurm_metric_e m_type;
};

/* This will be q-sorted by __constructor__ */
struct jobmgr_slurm_mdesc_s jobmgr_slurm_mdesc_tbl[] = {
	{ "job_id",   LDMS_V_CHAR_ARRAY, NULL,
		      LDMSD_JOBMGR_JOB_ID_LEN, JOBMGR_SLURM_METRIC_JOB_ID },

	{ "user",     LDMS_V_CHAR_ARRAY, NULL,
		      LDMSD_JOBMGR_USER_LEN, JOBMGR_SLURM_METRIC_USER },

	{ "job_name", LDMS_V_CHAR_ARRAY, NULL,
		      LDMSD_JOBMGR_JOB_NAME_LEN, JOBMGR_SLURM_METRIC_USER },

	{ "job_uid",    LDMS_V_U32,       NULL, 1, JOBMGR_SLURM_METRIC_JOB_UID    },
	{ "job_gid",    LDMS_V_U32,       NULL, 1, JOBMGR_SLURM_METRIC_JOB_GID    },
	{ "job_start",  LDMS_V_TIMESTAMP, NULL, 1, JOBMGR_SLURM_METRIC_JOB_START  },
	{ "job_end",    LDMS_V_TIMESTAMP, NULL, 1, JOBMGR_SLURM_METRIC_JOB_END    },
	{ "node_count", LDMS_V_U32,       NULL, 1, JOBMGR_SLURM_METRIC_NODE_COUNT },

	{ "step_id", LDMS_V_CHAR_ARRAY, NULL,
		     LDMSD_JOBMGR_STEP_ID_LEN, JOBMGR_SLURM_METRIC_STEP_ID },
	{ "step_start",       LDMS_V_TIMESTAMP,  NULL, 1, JOBMGR_SLURM_METRIC_STEP_START },
	{ "step_end",         LDMS_V_TIMESTAMP,  NULL, 1, JOBMGR_SLURM_METRIC_STEP_END },

	{ "total_tasks", LDMS_V_U32, NULL, 1, JOBMGR_SLURM_METRIC_TOTAL_TASKS },
	{ "local_tasks", LDMS_V_U32, NULL, 1, JOBMGR_SLURM_METRIC_LOCAL_TASKS },

	{ "task_id", LDMS_V_CHAR_ARRAY, NULL,
		     LDMSD_JOBMGR_TASK_ID_LEN, JOBMGR_SLURM_METRIC_TASK_ID },

	{ "task_pid",         LDMS_V_U64,        NULL, 1, JOBMGR_SLURM_METRIC_TASK_PID },
	{ "task_rank",        LDMS_V_U32,        NULL, 1, JOBMGR_SLURM_METRIC_TASK_RANK },
	{ "task_start",       LDMS_V_TIMESTAMP,  NULL, 1, JOBMGR_SLURM_METRIC_TASK_START },
	{ "task_end",         LDMS_V_TIMESTAMP,  NULL, 1, JOBMGR_SLURM_METRIC_TASK_END },
	{ "task_exit_status", LDMS_V_S32,        NULL, 1, JOBMGR_SLURM_METRIC_TASK_EXIT_STATUS },
};

static int __mdesc_key_cmp(const void *_k, const void *_e)
{
	const char *a = _k;
	const struct jobmgr_slurm_mdesc_s *e = _e;
	return strcmp(a, e->name);
}

static const struct jobmgr_slurm_mdesc_s *
jobmgr_slurm_mdesc_find(const char *name)
{
	const struct jobmgr_slurm_mdesc_s *ent;
	ent = bsearch(name, jobmgr_slurm_mdesc_tbl,
		sizeof(jobmgr_slurm_mdesc_tbl)/sizeof(jobmgr_slurm_mdesc_tbl[0]),
		sizeof(jobmgr_slurm_mdesc_tbl[0]),
		__mdesc_key_cmp);
	return ent;
}

static int job_id_str(char *buf, size_t bufsz, uint64_t job_id)
{
	return snprintf(buf, bufsz, "slurm_%lu", job_id);
}

static int step_id_str(char *buf, size_t bufsz, uint64_t job_id, uint64_t step_id)
{
	return snprintf(buf, bufsz, "step-%lu", step_id);
}

static int task_id_str(char *buf, size_t bufsz, uint64_t job_id,
			uint64_t step_id, uint64_t task_id)
{
	return snprintf(buf, bufsz, "task-%lu", task_id);
}

int u32_node_cmp(void *a, const void *b)
{
	uint32_t l = *(uint32_t*)a;
	uint32_t r = *(uint32_t*)b;
	if (l < r)
		return -1;
	if (l > r)
		return 1;
	return 0;
}

int u64_node_cmp(void *a, const void *b)
{
	uint64_t l = *(uint64_t*)a;
	uint64_t r = *(uint64_t*)b;
	if (l < r)
		return -1;
	if (l > r)
		return 1;
	return 0;
}

typedef enum jobmgr_slurm_state_e {
	JOBMGR_SLURM_STOPPED,
	JOBMGR_SLURM_STOPPING,
	JOBMGR_SLURM_STARTING,
	JOBMGR_SLURM_STARTED,
	JOBMGR_SLURM_LAST,
} jobmgr_slurm_state_t;

static const char *jobmgr_slurm_state_text_tbl[] = {
	[JOBMGR_SLURM_STOPPED]  = "JOBMGR_SLURM_STOPPED",
	[JOBMGR_SLURM_STOPPING] = "JOBMGR_SLURM_STOPPING",
	[JOBMGR_SLURM_STARTING] = "JOBMGR_SLURM_STARTING",
	[JOBMGR_SLURM_STARTED]  = "JOBMGR_SLURM_STARTED",
};

static const char *jobmgr_slurm_state_text(enum jobmgr_slurm_state_e st)
{
	if (JOBMGR_SLURM_LAST <= st)
		return "UNKNOWN";
	return jobmgr_slurm_state_text_tbl[st];
}

struct jobmgr_slurm {
	/* ptr to plug should be safe */
	ldmsd_plug_handle_t plug;
	ldms_msg_client_t mc;
	pthread_mutex_t mutex;
	struct rbt job_tree;
	char *ch_name;
	ldms_schema_t sch;
	enum jobmgr_slurm_state_e state;
	int recdef_midx;
};
typedef struct jobmgr_slurm *jobmgr_slurm_t;

static const char *usage_str =
"config inst=NAME message_channel=CH_NAME\n"
;

static const char *usage(ldmsd_plug_handle_t p)
{
	return usage_str;
}

/* Conveneint logging macros */
#define LOG(p, LVL, FMT, ...) ovis_log(ldmsd_plug_log_get(p), LVL, FMT, ## __VA_ARGS__ )
#define LOG_CRITICAL(p, FMT, ...) LOG(p, OVIS_LCRITICAL, FMT, ## __VA_ARGS__)
#define LOG_ERROR(p, FMT, ...) LOG(p, OVIS_LERROR, FMT, ## __VA_ARGS__)
#define LOG_INFO(p, FMT, ...) LOG(p, OVIS_LINFO, FMT, ## __VA_ARGS__)
#define LOG_DEBUG(p, FMT, ...) LOG(p, OVIS_LDEBUG, FMT, ## __VA_ARGS__)

static int config(ldmsd_plug_handle_t p, struct attr_value_list *kwl, struct attr_value_list *avl)
{
	jobmgr_slurm_t js = ldmsd_plug_ctxt_get(p);
	char *val;
	val = av_value(avl, "message_channel");
	if (!val) {
		if (errno == ENOMEM)
			goto enomem;
		val = strdup("slurm"); /* default */
		if (!val) {
			goto enomem;
		}
	}
	js->ch_name = strdup(val);

	return 0;
 enomem:
	LOG_ERROR(p, "Not enough memory.\n");
	return ENOMEM;
}

static int constructor(ldmsd_plug_handle_t p)
{
	int rc;
	jobmgr_slurm_t js;
	struct ldmsd_jobmgr_query_mdesc_s desc = {
		"job_tag", LDMS_V_CHAR_ARRAY, NULL, JOB_TAG_STR_LEN, 0
	};

	rc = ldmsd_jobmgr_metric_register(&desc);
	if (rc)
		return rc;

	js = calloc(1, sizeof(*js));
	if (!js)
		return ENOMEM;
	pthread_mutex_init(&js->mutex, NULL);
	rbt_init(&js->job_tree, u64_node_cmp);
	js->state = JOBMGR_SLURM_STOPPED;
	ldmsd_plug_ctxt_set(p, js);
	js->plug = p;
	return 0;
}

static void destructor(ldmsd_plug_handle_t p)
{
	jobmgr_slurm_t js = ldmsd_plug_ctxt_get(p);
	if (!js)
		return;
	free(js->ch_name);
	assert(js->mc == 0);
}

/* return 0 if transition failed */
static inline int jobmgr_slurm_transition(jobmgr_slurm_t js,
		enum jobmgr_slurm_state_e from,
		enum jobmgr_slurm_state_e to,
		enum jobmgr_slurm_state_e *prev)
{
	int success;
	success = __atomic_compare_exchange_n(&js->state, &from, to, 0,
					__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	if (prev)
		*prev = from;
	return success;
}

typedef struct job_data {
	enum slurm_job_state {
		JOB_FREE = 0,
		JOB_STARTING = 1,
		JOB_RUNNING = 2,
		JOB_STOPPING = 3,
		JOB_COMPLETE = 4
	} state;

	pthread_mutex_t mutex;

	uint64_t job_id;

	int exited;	/* True if this job is on the deleting list */
	/*
	 * \c exited_tasks_count is the number of tasks that have been exited already.
	 * When exited_tasks_count == v[TASK_COUNT], the plugin calls handle_job_exit().
	 * This is to handle the jobs started by srun.
	 */
	int exited_tasks_count;
	struct rbn rbn;

	char user[LDMSD_JOBMGR_USER_LEN];
	char job_name[LDMSD_JOBMGR_JOB_NAME_LEN];
	char job_tag[JOB_TAG_STR_LEN];
	uid_t job_uid;
	gid_t job_gid;
	struct ldms_timestamp job_start;
	struct ldms_timestamp job_end;
	int node_count;
	struct rbt step_rbt; /* key is &step->step_id */

} *job_data_t;

typedef struct step_data {
	struct rbn rbn; /* key is &step_id */
	job_data_t job; /* refers back to job */
	uint64_t step_id;
	struct ldms_timestamp step_start;
	struct ldms_timestamp step_end;
	int total_tasks;
	int local_tasks;
	struct rbt task_rbt; /* key is &task->task_id */
} *step_data_t;

typedef struct task_data {
	struct rbn rbn;

	step_data_t step; /* refers back to step */
	uint64_t task_id;
	uint64_t task_pid;
	uint32_t task_rank;
	struct ldms_timestamp task_start;
	struct ldms_timestamp task_end;
	int task_exit_status;

} *task_data_t;

struct jobmgr_slurm_ev_ctxt {
	job_data_t  job;
	step_data_t step;
	task_data_t task;
	int level;
};

static step_data_t step_data_get(job_data_t job, uint64_t step_id);

static inline
ldms_mval_t __task_metric(ldms_mval_t rec, int *idx, const char *name)
{
	if (*idx >= 0)
		return ldms_record_metric_get(rec, *idx);
	*idx = ldms_record_metric_find(rec, name);
	if (*idx < 0)
		return NULL;
	return ldms_record_metric_get(rec, *idx);
}

static inline void task_data_free(task_data_t task)
{
	free(task);
}

static task_data_t task_data_alloc(jobmgr_slurm_t js, job_data_t job, uint64_t step_id, uint64_t task_pid)
{
	step_data_t step;

	step = step_data_get(job, step_id);
	if (!step)
		return NULL;

	task_data_t task = calloc(1, sizeof(*task));
	if (!task)
		return NULL;

	task->task_pid = task_pid;
	rbn_init(&task->rbn, &task->task_pid);

	pthread_mutex_lock(&job->mutex);
	task->step = step;
	rbt_ins(&step->task_rbt, &task->rbn);
	pthread_mutex_unlock(&job->mutex);

	return task;
}

static void step_data_free(step_data_t step)
{
	task_data_t task;
	struct rbn *rbn;
	while ((rbn = rbt_min(&step->task_rbt))) {
		rbt_del(&step->task_rbt, rbn);
		task = container_of(rbn, struct task_data, rbn);
		task_data_free(task);
	}
	free(step);
}

static void job_data_free(jobmgr_slurm_t js, job_data_t job)
{
	step_data_t step;
	struct rbn *rbn;

	pthread_mutex_lock(&js->mutex);
	rbt_del(&js->job_tree, &job->rbn);
	pthread_mutex_unlock(&js->mutex);

	while ((rbn = rbt_min(&job->step_rbt))) {
		rbt_del(&job->step_rbt, rbn);
		step = container_of(rbn, struct step_data, rbn);
		step_data_free( step);
	}

	free(job);
}

static inline
ldms_mval_t __metric_by_name(ldms_set_t set, int *i, const char *name)
{
	if (*i >= 0)
		return ldms_metric_get(set, *i);
	*i = ldms_metric_by_name(set, name);
	if (*i < 0)
		return NULL;
	return ldms_metric_get(set, *i);
}

/* js->mutex is held */
static job_data_t __job_data_alloc(jobmgr_slurm_t js, uint64_t job_id)
{
	job_data_t job;

	job = calloc(1, sizeof(*job));
	if (!job)
		return NULL;
	pthread_mutex_init(&job->mutex, NULL);
	job->job_id = job_id;

	/* job structure */
	rbt_init(&job->step_rbt, u64_node_cmp);

	/* use u64 job_id as key */
	rbn_init(&job->rbn, &job->job_id);
	rbt_ins(&js->job_tree, &job->rbn);

	return job;
}

/* js->mutex is held */
static job_data_t __job_data_find(jobmgr_slurm_t js, uint64_t job_id)
{
	job_data_t job = NULL;
	struct rbn *rbn;
	rbn = rbt_find(&js->job_tree, &job_id);
	if (rbn)
		job = container_of(rbn, struct job_data, rbn);
	return job;
}

static job_data_t job_data_get(jobmgr_slurm_t js, uint64_t job_id)
{
	job_data_t job = NULL;

	pthread_mutex_lock(&js->mutex);
	job = __job_data_find(js, job_id);
	if (job)
		goto out;
	job = __job_data_alloc(js, job_id);
 out:
	pthread_mutex_unlock(&js->mutex);
	return job;
}

static task_data_t task_data_find(job_data_t job, uint64_t step_id, uint64_t task_pid)
{
	struct rbn *rbn = NULL;
	step_data_t step;
	step = step_data_get(job, step_id);
	if (!step)
		goto out;
	pthread_mutex_lock(&job->mutex);
	rbn = rbt_find(&step->task_rbt, &task_pid);
	pthread_mutex_unlock(&job->mutex);
 out:
	if (rbn)
		return container_of(rbn, struct task_data, rbn);
	return NULL;
}

static void handle_job_init(jobmgr_slurm_t js, job_data_t job, json_entity_t e)
{
	uint64_t timestamp;
	json_entity_t av, dict;
	ldmsd_plug_handle_t p = js->plug;

	LOG_DEBUG(p, "job %ld: Received 'init' event\n", job->job_id);

	av = json_value_find(e, "timestamp");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'timestamp' attribute "
				     "in 'init' event.\n", job->job_id);
		goto err;
	}
	timestamp = json_value_int(av);

	dict = json_value_find(e, "data");
	if (!dict) {
		LOG_ERROR(p, "job %ld: Missing 'data' attribute "
				     "in 'init' event.\n", job->job_id);
		goto err;
	}

	job->job_start.sec = timestamp;
	job->job_start.usec = 0;

	/* node count */
	av = json_value_find(dict, "nnodes");
	if (av) {
		job->node_count = json_value_int(av);
	}

	/* uid */
	av = json_value_find(dict, "uid");
	if (av) {
		job->job_uid = json_value_int(av);
	}

	/* gid */
	av = json_value_find(dict, "gid");
	if (av) {
		job->job_gid = json_value_int(av);
	}

	struct jobmgr_slurm_ev_ctxt *ev_ctxt;
	ev_ctxt = calloc(1, sizeof(*ev_ctxt));
	if (!ev_ctxt)
		return;
	ev_ctxt->job = job;
	ev_ctxt->level = 0;

	ldmsd_jobmgr_get(js->plug, "jobmgr_event");

	int rc = ldmsd_jobmgr_qev_post(js->plug, LDMSD_JOBMGR_QUERY_EVENT_JOB_START, ev_ctxt);
	if (rc) {
		free(ev_ctxt);
		ldmsd_jobmgr_put(js->plug, "jobmgr_event");
	}

	return;

 err:
	return;

}

static step_data_t step_data_get(job_data_t job, uint64_t step_id)
{
	struct rbn *rbn;
	step_data_t step;
	pthread_mutex_lock(&job->mutex);
	rbn = rbt_find(&job->step_rbt, &step_id);
	if (rbn) {
		step = container_of(rbn, struct step_data, rbn);
		goto out;
	}
	step = calloc(1, sizeof(*step));
	if (!step)
		goto out;
	step->job = job;
	step->step_id = step_id;
	rbt_init(&step->task_rbt, u64_node_cmp);
	rbn_init(&step->rbn, &step->step_id);
	rbt_ins(&job->step_rbt, &step->rbn);
 out:
	pthread_mutex_unlock(&job->mutex);
	return step;
}

static void handle_step_init(jobmgr_slurm_t js, job_data_t job, json_entity_t e)
{
	json_entity_t av, dict;
	uint64_t timestamp;
	ldmsd_plug_handle_t p = js->plug;
	uint64_t step_id;
	step_data_t step;

	LOG_DEBUG(p, "job %ld: Received 'step_init' event\n", job->job_id);

	av = json_value_find(e, "timestamp");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'timestamp' attribute "
				     "in 'init' event.\n", job->job_id);
		return;
	}
	timestamp = json_value_int(av);

	dict = json_value_find(e, "data");
	if (!dict) {
		LOG_ERROR(p, "job %ld: Missing 'data' attribute in "
				     "'step_init' event.\n", job->job_id);
		return;
	}


	/* step_id */
	av = json_value_find(dict, "step_id");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'step_id' attribute in "
				     "'step_init' event.\n", job->job_id);
		return;
	}
	step_id = json_value_int(av);
	step = step_data_get(job, step_id);
	if (!step) {
		LOG_ERROR(p, "job %ld: step data allocation error.\n", job->job_id);
		return;
	}
	step->step_start.sec = timestamp;
	step->step_start.usec = 0;

	/* user */
	av = json_value_find(dict, "job_user");
	if (av) {
		if (json_entity_type(av) == JSON_STRING_VALUE) {
			snprintf(job->user, LDMSD_JOBMGR_USER_LEN,
				 "%s", json_value_str(av)->str);
		}
	}

	/* job_name */
	av = json_value_find(dict, "job_name");
	if (av) {
		if (json_entity_type(av) == JSON_STRING_VALUE) {
			snprintf(job->job_name, LDMSD_JOBMGR_JOB_NAME_LEN,
				 "%s", json_value_str(av)->str);
		}
	}

	/* If subscriber data is present, look for an instance tag */
	json_entity_t subs_dict;
	subs_dict = json_value_find(dict, "subscriber_data");
	if (subs_dict) {
		if (json_entity_type(subs_dict) == JSON_DICT_VALUE) {
			av = json_value_find(subs_dict, "job_tag");
			if (av) {
				if (json_entity_type(av) == JSON_STRING_VALUE) {
					snprintf(job->job_tag, JOB_TAG_STR_LEN,
						"%s", json_value_str(av)->str);
				}
			}
		}
	}

	/* node count */
	av = json_value_find(dict, "nnodes");
	if (av) {
		job->node_count = json_value_int(av);
	}

	/* uid */
	av = json_value_find(dict, "uid");
	if (av) {
		job->job_uid = json_value_int(av);
	}


	/* gid */
	av = json_value_find(dict, "gid");
	if (av) {
		job->job_gid = json_value_int(av);
	}

	/* task count */
	av = json_value_find(dict, "total_tasks");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'total_tasks' attribute "
				     "in 'step_init' event.\n", job->job_id);
	} else {
		step->total_tasks = json_value_int(av);
	}

	/* task count */
	av = json_value_find(dict, "local_tasks");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'local_tasks' "
						"attribute in 'step_init' event.\n",
								  job->job_id);
	} else {
		step->local_tasks = json_value_int(av);
	}

	/* post STEP_START event */

	struct jobmgr_slurm_ev_ctxt *ev_ctxt;
	ev_ctxt = calloc(1, sizeof(*ev_ctxt));
	if (!ev_ctxt)
		return;
	ev_ctxt->job = job;
	ev_ctxt->step = step;
	ev_ctxt->level = 1;

	ldmsd_jobmgr_get(js->plug, "jobmgr_event");

	int rc = ldmsd_jobmgr_qev_post(js->plug, LDMSD_JOBMGR_QUERY_EVENT_STEP_START, ev_ctxt);
	if (rc) {
		free(ev_ctxt);
		ldmsd_jobmgr_put(js->plug, "jobmgr_event");
	}
}

static void handle_task_init(jobmgr_slurm_t js, job_data_t job, json_entity_t e)
{
	ldmsd_plug_handle_t p = js->plug;
	json_entity_t av, dict;
	uint64_t ts, task_pid, task_rank, step_id, task_id;
	task_data_t task;

	av = json_value_find(e, "timestamp");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'timestamp' "
				"attribute in 'task_init' event.\n", job->job_id);
		return;
	}
	ts = json_value_int(av);

	dict = json_value_find(e, "data");
	if (!dict) {
		LOG_ERROR(p, "job %ld: Missing 'data' attribute in "
				    "'task_init_priv' event.\n", job->job_id);
		return;
	}

	av = json_value_find(dict, "task_id");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'task_id' attribute "
				     "in 'task_init_priv' event.\n", job->job_id);
		return;
	}
	task_id = json_value_int(av);

	av = json_value_find(dict, "task_pid");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'task_pid' attribute "
				     "in 'task_init_priv' event.\n", job->job_id);
		return;
	}
	task_pid = json_value_int(av);

	LOG_DEBUG(p, "job %ld: task %ld: "
			    "Received 'task_init_priv' event\n",
			    job->job_id, task_pid);

	/* task rank */
	av = json_value_find(dict, "task_global_id");
	if (!av) {
		LOG_ERROR(p, "job %ld: task %ld: Missing "
				     "'task_global_id' attribute in "
				     "'task_init_priv' event.\n",
				     job->job_id, task_pid);
		return;
	}
	task_rank = json_value_int(av);
	/* step id */
	av = json_value_find(dict, "step_id");
	if (!av) {
		LOG_ERROR(p, "job %ld: task %ld: Missing "
				     "'step_id' attribute in "
				     "'task_init_priv' event.\n",
				     job->job_id, task_pid);
		return;
	}
	step_id = json_value_int(av);

	task = task_data_alloc(js, job, step_id, task_pid);
	if (!task) {
		LOG_CRITICAL(p, "Task memory allocation error.\n");
		return;
	}

	task->task_start.sec = ts;
	task->task_start.usec = 0;
	task->task_pid = task_pid;
	task->task_id = task_id;
	task->task_rank = task_rank;

	/* prepend the step_id to task_id */
	struct jobmgr_slurm_ev_ctxt *ev_ctxt;
	ev_ctxt = calloc(1, sizeof(*ev_ctxt));
	if (!ev_ctxt)
		return;
	ev_ctxt->job  = job;
	ev_ctxt->step = task->step;
	ev_ctxt->task = task;
	ev_ctxt->level = 2;

	ldmsd_jobmgr_get(js->plug, "jobmgr_event");

	int rc = ldmsd_jobmgr_qev_post(js->plug, LDMSD_JOBMGR_QUERY_EVENT_TASK_START, ev_ctxt);
	if (rc) {
		free(ev_ctxt);
		ldmsd_jobmgr_put(js->plug, "jobmgr_event");
	}
}

static void handle_task_exit(jobmgr_slurm_t js, job_data_t job, json_entity_t e)
{
	ldmsd_plug_handle_t p = js->plug;
	json_entity_t dict, av;
	uint64_t ts, step_id, task_pid;
	task_data_t task;

	av = json_value_find(e, "timestamp");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'timestamp' "
				"attribute in 'task_exit' event.\n", job->job_id);
		return;
	}
	ts = json_value_int(av);

	dict = json_value_find(e, "data");
	if (!dict) {
		LOG_ERROR(p, "job %ld: Missing 'data' attribute in "
				    "'task_exit' event.\n", job->job_id);
		return;
	}

	av = json_value_find(dict, "task_pid");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'task_pid' attribute "
				     "in 'task_exit' event.\n", job->job_id);
		return;
	}
	task_pid = json_value_int(av);
	LOG_DEBUG(p, "job %ld: task %ld: Received 'task_exit' event\n",
					     job->job_id, task_pid);

	av = json_value_find(dict, "step_id");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'step_id' attribute "
				     "in 'task_exit' event.\n", job->job_id);
		return;
	}
	step_id = json_value_int(av);

	task = task_data_find(job, step_id, task_pid);
	if (!task) {
		LOG_ERROR(p, "job %ld: task %ld: Cannot find "
				     "the task_data in 'task_exit' event.\n", job->job_id, task_pid);
		return;
	}

	av = json_value_find(dict, "task_exit_status");
	if (!av) {
		LOG_ERROR(p, "job %ld: task %ld: Missing "
				     "'task_exit_status' attribute "
				     "in 'task_exit' event.\n", job->job_id,
				     task->task_pid);
		return;
	}
	job->exited_tasks_count += 1;

	task->task_end.sec  = ts;
	task->task_end.usec = 0;
	task->task_exit_status = json_value_int(av);

	struct jobmgr_slurm_ev_ctxt *ev_ctxt;
	ev_ctxt = calloc(1, sizeof(*ev_ctxt));
	if (!ev_ctxt)
		return;
	ev_ctxt->job  = job;
	ev_ctxt->step = task->step;
	ev_ctxt->task = task;
	ev_ctxt->level = 2;

	ldmsd_jobmgr_get(js->plug, "jobmgr_event");

	int rc = ldmsd_jobmgr_qev_post(js->plug, LDMSD_JOBMGR_QUERY_EVENT_TASK_END, ev_ctxt);
	if (rc) {
		free(ev_ctxt);
		ldmsd_jobmgr_put(js->plug, "jobmgr_event");
	}
}

static void handle_step_exit(jobmgr_slurm_t js, job_data_t job, json_entity_t e)
{
	json_entity_t dict, av;
	uint64_t step_id;
	ldmsd_plug_handle_t p = js->plug;
	int rc;
	step_data_t step;

	LOG_DEBUG(p, "job %ld: Received 'step_exit' event.\n",
							 job->job_id);

	dict = json_value_find(e, "data");
	if (!dict) {
		LOG_ERROR(p, "job %ld: Missing 'data' attribute in "
				    "'step_exit' event.\n", job->job_id);
		return;
	}

	av = json_value_find(dict, "step_id");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'step_id' attribute "
				     "in 'task_exit' event.\n", job->job_id);
		return;
	}
	step_id = json_value_int(av);

	av = json_value_find(e, "timestamp");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'timestamp' "
				"attribute in 'step_exit' event.\n", job->job_id);
		return;
	}

	step = step_data_get(job, step_id);
	if (!step) {
		LOG_ERROR(p, "job %ld: step data allocation error.\n", job->job_id);
		return;
	}

	step->step_end.sec = json_value_int(av);
	step->step_end.usec = 0;

	struct jobmgr_slurm_ev_ctxt *ev_ctxt;
	ev_ctxt = calloc(1, sizeof(*ev_ctxt));
	if (!ev_ctxt)
		return;
	ev_ctxt->job  = job;
	ev_ctxt->step = step;
	ev_ctxt->level = 1;

	ldmsd_jobmgr_get(js->plug, "jobmgr_event");

	rc = ldmsd_jobmgr_qev_post(js->plug, LDMSD_JOBMGR_QUERY_EVENT_STEP_END, ev_ctxt);
	if (rc) {
		free(ev_ctxt);
		ldmsd_jobmgr_put(js->plug, "jobmgr_event");
	}
}

static void handle_job_exit(jobmgr_slurm_t js, job_data_t job, json_entity_t e)
{
	json_entity_t av;
	ldmsd_plug_handle_t p = js->plug;
	int rc;

	LOG_DEBUG(p, "job %ld: Received 'job_exit' event.\n",
							 job->job_id);

	av = json_value_find(e, "timestamp");
	if (!av) {
		LOG_ERROR(p, "job %ld: Missing 'timestamp' "
				"attribute in 'job_exit' event.\n", job->job_id);
		return;
	}

	job->job_end.sec = json_value_int(av);
	job->job_end.usec = 0;

	struct jobmgr_slurm_ev_ctxt *ev_ctxt;
	ev_ctxt = calloc(1, sizeof(*ev_ctxt));
	if (!ev_ctxt)
		return;
	ev_ctxt->job  = job;
	ev_ctxt->level = 0;

	ldmsd_jobmgr_get(js->plug, "jobmgr_event");

	rc = ldmsd_jobmgr_qev_post(js->plug, LDMSD_JOBMGR_QUERY_EVENT_JOB_END, ev_ctxt);
	if (rc) {
		free(ev_ctxt);
		ldmsd_jobmgr_put(js->plug, "jobmgr_event");
	}

	job_data_free(js, job);
}

static void handle_msg_recv(ldmsd_plug_handle_t p, jobmgr_slurm_t js, ldms_msg_event_t ev)
{
	json_entity_t event, dict, av;

	if (ev->recv.type != LDMS_MSG_JSON) {
		LOG_DEBUG(p, "Unexpected stream type data...ignoring\n");
		LOG_DEBUG(p, "%s\n", ev->recv.data);
		return;
	}

	event = json_value_find(ev->recv.json, "event");
	if (!event) {
		LOG_ERROR(p, "'event' attribute missing\n");
		goto err_0;
	}

	json_str_t event_name = json_value_str(event);
	dict = json_value_find(ev->recv.json, "data");
	if (!dict) {
		LOG_ERROR(p, "'%s' event is missing "
		       "the 'data' attribute\n", event_name->str);
		goto err_0;
	}
	av = json_value_find(dict, "job_id");
	if (!av) {
		LOG_ERROR(p, "'%s' event is missing the "
		       "'job_id' attribute.\n", event_name->str);
		goto err_0;
	}
	uint64_t job_id = json_value_int(av);

	job_data_t job;
	if (0 == strncmp(event_name->str, "init", 4)) {
		job = job_data_get(js, job_id);
		if (!job) {
			LOG_CRITICAL(p, "Memory allocation error when "
					"creating a job data object.\n");
			goto err_0;
		}
		handle_job_init(js, job, ev->recv.json);
	} else if (0 == strncmp(event_name->str, "step_init", 9)) {
		job = job_data_get(js, job_id);
		if (!job) {
			LOG_CRITICAL(p,
				"Memory allocation error when "
				"creating a job data object.\n");
			goto err_0;
		}
		handle_step_init(js, job, ev->recv.json);
	} else if (0 == strncmp(event_name->str, "task_init_priv", 14)) {
		job = job_data_get(js, job_id);
		if (!job) {
			LOG_ERROR(p, "'%s' event was received "
					"for job %ld with no job_data.\n",
					event_name->str, job_id);
			goto err_0;
		}
		handle_task_init(js, job, ev->recv.json);
	} else if (0 == strncmp(event_name->str, "task_exit", 9)) {
		job = job_data_get(js, job_id);
		if (!job) {
			LOG_ERROR(p, "'%s' event was received "
					"for job %ld with no job_data.\n",
					event_name->str, job_id);
			goto err_0;
		}
		handle_task_exit(js, job, ev->recv.json);
	} else if (0 == strncmp(event_name->str, "step_exit", 4)) {
		job = job_data_get(js, job_id);
		if (!job) {
			LOG_ERROR(p, "'%s' event was received "
					"for job %ld with no job_data.\n",
					event_name->str, job_id);
			goto err_0;
		}
		handle_step_exit(js, job, ev->recv.json);
	} else if (0 == strncmp(event_name->str, "exit", 4)) {
		job = job_data_get(js, job_id);
		if (!job) {
			LOG_ERROR(p, "'%s' event was received "
					"for job %ld with no job_data.\n",
					event_name->str, job_id);
			goto err_0;
		}
		handle_job_exit(js, job, ev->recv.json);
	} else {
		LOG_DEBUG(p, "ignoring event '%s'\n", event_name->str);
	}
err_0:
	/* no-op */;
}

static int msg_cb(ldms_msg_event_t ev, void *cb_arg)
{
	ldmsd_plug_handle_t p = cb_arg;
	jobmgr_slurm_t js = ldmsd_plug_ctxt_get(p);
	int succ;

	switch (ev->type) {
	case LDMS_MSG_EVENT_RECV:
		handle_msg_recv(p, js, ev);
		break;
	case LDMS_MSG_EVENT_CLIENT_CLOSE:
		assert(ev->close.client == js->mc);
		js->mc = NULL;
		succ = jobmgr_slurm_transition(js, JOBMGR_SLURM_STOPPING,
					       JOBMGR_SLURM_STOPPED, NULL);
		if (!succ) {
			assert(0 == "UNEXPECTED STATE");
		}
		ldmsd_jobmgr_put(p, "msg_subscribe");
		break;
	default:
		/* no-op */ ;
	}

	return 0;
}

static int start(ldmsd_plug_handle_t p)
{
	jobmgr_slurm_t js = ldmsd_plug_ctxt_get(p);
	enum jobmgr_slurm_state_e st;
	int succ, rc = 0;

	/* transition to STARTING */
	if (0 == jobmgr_slurm_transition(js, JOBMGR_SLURM_STOPPED,
					     JOBMGR_SLURM_STARTING, &st)) {
		/* failed transition */
		LOG_ERROR(p, "Cannot start jobmgr '%s', state: %s\n",
				ldmsd_plug_name_get(p),
				jobmgr_slurm_state_text(st));
		return EBUSY;
	}

	if (!js->ch_name) {
		LOG_ERROR(p, "jobmgr '%s' is not configured.\n",
				ldmsd_plug_name_get(p));
		rc = EINVAL;
		goto err;
	}

	assert(js->mc == NULL);

	ldmsd_jobmgr_get(p, "msg_subscribe");

	js->mc = ldms_msg_subscribe(js->ch_name, 0, msg_cb,
				    p, "jobmgr_slurm");
	if (!js->mc) {
		ldmsd_jobmgr_put(p, "msg_subscribe");
		LOG_ERROR(p, "msg subscription failed, errno: %d\n", errno);
		rc = errno;
		goto err;
	}

	succ = jobmgr_slurm_transition(js, JOBMGR_SLURM_STARTING, JOBMGR_SLURM_STARTED, &st);
	if (!succ) {
		assert(0 == "BAD STATE");
	}

	return 0;
 err:
	succ = jobmgr_slurm_transition(js, JOBMGR_SLURM_STARTING, JOBMGR_SLURM_STOPPED, &st);
	if (!succ) {
		assert(0 == "BAD STATE");
	}
	return rc;
}

static int stop(ldmsd_plug_handle_t p)
{
	jobmgr_slurm_t js = ldmsd_plug_ctxt_get(p);
	enum jobmgr_slurm_state_e st;

	if (0 == jobmgr_slurm_transition(js, JOBMGR_SLURM_STARTED,
					     JOBMGR_SLURM_STOPPING, &st)) {
		/* failed transition */
		LOG_ERROR(p, "Cannot stop jobmgr '%s', state: %s\n",
				ldmsd_plug_name_get(p),
				jobmgr_slurm_state_text(st));
		return EBUSY;
	}

	assert(js->mc);
	ldms_msg_client_close(js->mc);
	/* Transition from STOPPING to STOPPED when CLOSE event is delivered */

	return 0;
}

struct query_ctxt {
	int n;
	const struct ldmsd_jobmgr_query_s *q; /* for convenient */
	int job_id_idx;
	int step_id_idx;
	int task_id_idx;
	int job_end_idx;
	int step_end_idx;
	int level; /* 0 for job, 1 for step, 2 for task */
	const struct jobmgr_slurm_mdesc_s *mdesc[OVIS_FLEX];
};

static int on_query_new(ldmsd_plug_handle_t p,
			ldmsd_jobmgr_query_t q,
			void **q_ctxt_out)
{
	int i;
	int mx = 0, rc;
	struct query_ctxt *ctxt;
	struct ldms_metric_template_s mtmp;
	int n_metrics = ldms_record_metric_card(q->recdef);

	ctxt = calloc(1, sizeof(*ctxt) + n_metrics*sizeof(ctxt->mdesc[0]));
	if (!ctxt)
		return ENOMEM;
	ctxt->n = n_metrics;
	ctxt->q = q;
	ctxt->job_id_idx = -1;
	ctxt->step_id_idx = -1;
	ctxt->task_id_idx = -1;
	ctxt->job_end_idx = -1;
	ctxt->step_end_idx = -1;
	for (i = 0; i < n_metrics; i++) {
		rc = ldms_record_metric_template_get(q->recdef, i, &mtmp);
		assert(rc == 0);
		ctxt->mdesc[i] = jobmgr_slurm_mdesc_find(mtmp.name);
		/* it is OK if we may not have all of the metrics */
		if (!ctxt->mdesc[i])
			continue;
		if (ctxt->mdesc[i]->m_type > mx)
			mx = ctxt->mdesc[i]->m_type;
		switch (ctxt->mdesc[i]->m_type) {
		case JOBMGR_SLURM_METRIC_JOB_ID:
			ctxt->job_id_idx = i;
			break;
		case JOBMGR_SLURM_METRIC_STEP_ID:
			ctxt->step_id_idx = i;
			break;
		case JOBMGR_SLURM_METRIC_TASK_ID:
			ctxt->task_id_idx = i;
			break;
		case JOBMGR_SLURM_METRIC_JOB_END:
			ctxt->job_end_idx = i;
			break;
		case JOBMGR_SLURM_METRIC_STEP_END:
			ctxt->step_end_idx = i;
			break;
		default:
			/* no-op */ ;
		}
	}
	if (mx >= 0x200) {
		ctxt->level = 2;
	} else if (mx >= 0x100) {
		ctxt->level = 1;
	} else {
		ctxt->level = 0;
	}
	*q_ctxt_out = ctxt;
	return 0;
}

static void on_query_free(ldmsd_plug_handle_t p,
			  ldmsd_jobmgr_query_t q, void *q_ctxt)
{
	struct query_ctxt *ctxt = q_ctxt;
	free(ctxt);
}

void mv_assign(const struct jobmgr_slurm_mdesc_s *mdesc,
	       ldms_mval_t mv, job_data_t job, step_data_t step, task_data_t task)
{
	switch (mdesc->m_type) {
	case JOBMGR_SLURM_METRIC_JOB_ID:
		job_id_str(mv->a_char, LDMSD_JOBMGR_JOB_ID_LEN, job->job_id);
		break;
	case JOBMGR_SLURM_METRIC_USER:
		snprintf(mv->a_char, LDMSD_JOBMGR_USER_LEN,
			 "%s", job->user);
		break;
	case JOBMGR_SLURM_METRIC_JOB_NAME:
		snprintf(mv->a_char, LDMSD_JOBMGR_JOB_NAME_LEN,
			 "%s", job->job_name);
		break;
	case JOBMGR_SLURM_METRIC_JOB_UID:
		mv->v_u32 = job->job_uid;
		break;
	case JOBMGR_SLURM_METRIC_JOB_GID:
		mv->v_u32 = job->job_gid;
		break;
	case JOBMGR_SLURM_METRIC_JOB_START:
		mv->v_ts = job->job_start;
		break;
	case JOBMGR_SLURM_METRIC_JOB_END:
		mv->v_ts = job->job_end;
		break;
	case JOBMGR_SLURM_METRIC_NODE_COUNT:
		mv->v_u32 = job->node_count;
		break;
	case JOBMGR_SLURM_METRIC_JOB_TAG:
		snprintf(mv->a_char, JOB_TAG_STR_LEN, "%s", job->job_tag);
		break;

	case JOBMGR_SLURM_METRIC_STEP_ID:
		if (step) {
			step_id_str(mv->a_char, LDMSD_JOBMGR_STEP_ID_LEN,
				    job->job_id, step->step_id);
		}
		break;
	case JOBMGR_SLURM_METRIC_STEP_START:
		if (step)
			mv->v_ts = step->step_start;
		break;
	case JOBMGR_SLURM_METRIC_STEP_END:
		if (step)
			mv->v_ts = step->step_end;
		break;
	case JOBMGR_SLURM_METRIC_TOTAL_TASKS:
		if (step)
			mv->v_u32 = step->total_tasks;
		break;
	case JOBMGR_SLURM_METRIC_LOCAL_TASKS:
		if (step)
			mv->v_u32 = step->local_tasks;
		break;

	case JOBMGR_SLURM_METRIC_TASK_ID:
		if (step && task) {
			task_id_str(mv->a_char, LDMSD_JOBMGR_TASK_ID_LEN,
				    job->job_id, step->step_id, task->task_id);
		}
		break;
	case JOBMGR_SLURM_METRIC_TASK_PID:
		if (task)
			mv->v_u64 = task->task_pid;
		break;
	case JOBMGR_SLURM_METRIC_TASK_RANK:
		if (task)
			mv->v_u32 = task->task_rank;
		break;
	case JOBMGR_SLURM_METRIC_TASK_START:
		if (task)
			mv->v_ts = task->task_start;
		break;
	case JOBMGR_SLURM_METRIC_TASK_END:
		if (task)
			mv->v_ts = task->task_end;
		break;
	case JOBMGR_SLURM_METRIC_TASK_EXIT_STATUS:
		if (task)
			mv->v_s32 = task->task_exit_status;
		break;
	}
}

struct slurm_qrec_key_s {
	struct ldmsd_jobmgr_qrec_key_s k;
	struct ldmsd_jobmgr_qrec_key_ent_s keys[3];
	char job_id[LDMSD_JOBMGR_JOB_ID_LEN];
	char step_id[LDMSD_JOBMGR_STEP_ID_LEN];
	char task_id[LDMSD_JOBMGR_TASK_ID_LEN];
};

static int make_qev(ldmsd_plug_handle_t p,
		struct ldmsd_jobmgr_query_event_s *qev,
		void *_q_ctxt, void *_ev_ctxt)
{
	struct query_ctxt *q_ctxt = _q_ctxt;
	struct jobmgr_slurm_ev_ctxt *ev_ctxt = _ev_ctxt;
	int i;
	ldms_mval_t qrec, mv, sqrec, smv;
	ldmsd_jobmgr_query_t q = qev->q;
	struct slurm_qrec_key_s k = {
		.k.n = 1 + ev_ctxt->level,
		.keys = {
			{q_ctxt->job_id_idx , LDMSD_JOBMGR_JOB_ID_LEN,
				LDMS_V_CHAR_ARRAY, (ldms_mval_t)&k.job_id},
			{q_ctxt->step_id_idx , LDMSD_JOBMGR_STEP_ID_LEN,
				LDMS_V_CHAR_ARRAY, (ldms_mval_t)&k.step_id},
			{q_ctxt->task_id_idx , LDMSD_JOBMGR_TASK_ID_LEN,
				LDMS_V_CHAR_ARRAY, (ldms_mval_t)&k.task_id},
		},
	};

	/* make key */
	mv_assign(q_ctxt->mdesc[q_ctxt->job_id_idx],
			k.keys[0].mval, ev_ctxt->job, ev_ctxt->step, ev_ctxt->task);
	if (q_ctxt->level >= 1) {
		mv_assign(q_ctxt->mdesc[q_ctxt->step_id_idx],
			k.keys[1].mval, ev_ctxt->job, ev_ctxt->step, ev_ctxt->task);
	}
	if (q_ctxt->level >= 2) {
		mv_assign(q_ctxt->mdesc[q_ctxt->task_id_idx],
			k.keys[2].mval, ev_ctxt->job, ev_ctxt->step, ev_ctxt->task);
	}

	/* get record */
	qrec = ldmsd_jobmgr_qrec_get(qev->q, &k.k);
	if (!qrec) {
		qev->event_type = LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE;
		ldms_mval_t lh = ldms_metric_get(qev->q->set, qev->q->list_midx);
		qev->no_space.number_of_records = ldms_list_len(qev->q->set, lh);
		return errno;
	}

	/* assign values */
	pthread_mutex_lock(&q->mutex);
	ldms_transaction_begin(q_ctxt->q->set);
	for (i = 0; i < q_ctxt->n; i++) {
		if (!q_ctxt->mdesc[i])
			continue; /* we don't have this metric */
		mv = ldms_record_metric_get(qrec, i);
		mv_assign(q_ctxt->mdesc[i], mv, ev_ctxt->job, ev_ctxt->step, ev_ctxt->task);
	}

	int end_idx;

	if (qev->event_type == LDMSD_JOBMGR_QUERY_EVENT_JOB_END &&
	    q_ctxt->job_end_idx >= 0) {
		/* do the job_end update */
		end_idx = q_ctxt->job_end_idx;
	} else if (qev->event_type == LDMSD_JOBMGR_QUERY_EVENT_STEP_END &&
		   q_ctxt->step_end_idx >= 0) {
		end_idx = q_ctxt->step_end_idx;
	} else {
		goto skip;
	}

	/* update subsequent records that shares the same umbrella
	 * e.g. `step_end` event should be updated in all tasks under the same
	 * step. */
	mv = ldms_record_metric_get(qrec, end_idx);
	sqrec = ldmsd_jobmgr_qrec_next(qev->q, &k.k, qrec);
	while (sqrec) {
		smv = ldms_record_metric_get(sqrec, end_idx);
		smv->v_ts = mv->v_ts;
		sqrec = ldmsd_jobmgr_qrec_next(qev->q, &k.k, sqrec);
	}

 skip:
	ldms_transaction_end(q_ctxt->q->set);
	pthread_mutex_unlock(&q->mutex);
	qev->start_end.qrec = qrec;

	return 0;
}

void qev_done(ldmsd_plug_handle_t p,
		struct ldmsd_jobmgr_query_event_s *qev,
		void *ctxt)
{
	struct jobmgr_slurm_ev_ctxt *ev_ctxt = ctxt;
	free(ev_ctxt);
}

struct ldmsd_jobmgr ldmsd_plugin_interface = {
	.base = {
		.type = LDMSD_PLUGIN_JOBMGR,
		.config = config,
		.flags = LDMSD_PLUGIN_MULTI_INSTANCE,
		.usage = usage,
		.constructor = constructor,
		.destructor = destructor,
	},
	.start = start,
	.stop  = stop,
	.on_query_new = on_query_new,
	.on_query_free = on_query_free,
	.make_qev = make_qev,
	.qev_done = qev_done,
};

static int __mdesc_cmp(const void *_a, const void *_b)
{
	const struct jobmgr_slurm_mdesc_s *a = _a;
	const struct jobmgr_slurm_mdesc_s *b = _b;
	return strcmp(a->name, b->name);
}

__attribute__((constructor))
static void __init__()
{
	/* library init */
	qsort(jobmgr_slurm_mdesc_tbl,
	      sizeof(jobmgr_slurm_mdesc_tbl)/sizeof(jobmgr_slurm_mdesc_tbl[0]),
	      sizeof(jobmgr_slurm_mdesc_tbl[0]),
	      __mdesc_cmp);
}
