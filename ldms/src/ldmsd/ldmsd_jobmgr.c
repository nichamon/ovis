/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2025 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2025 Open Grid Computing, Inc. All rights reserved.
 *
 * Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
 * license for use of this work by or on behalf of the U.S. Government.
 * Export of this program may require a license from the United States
 * Government.
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


#define _GNU_SOURCE
#include "ldmsd_jobmgr.h"
#include "ldmsd_jobmgr_query.h"

#include "ovis_event/ovis_event.h"

extern ovis_log_t config_log;

ovis_scheduler_t jobmgr_sched;
static pthread_t jobmgr_thr;

int ldmsd_jobmgr_start(ldmsd_cfgobj_jobmgr_t jm)
{
	if (!jm->api->start)
		return ENOSYS;
	return jm->api->start(jm);
}

int ldmsd_jobmgr_stop(ldmsd_cfgobj_jobmgr_t jm)
{
	if (!jm->api->stop)
		return ENOSYS;
	return jm->api->stop(jm);
}

void *jobmgr_sched_proc(void *arg)
{
	ovis_scheduler_loop(jobmgr_sched, 0);
	assert(0 == "jobmgr_sched-proc unexpectedly terminated");
	return NULL;
}

int jobmgr_mtmp_cmp(void *tk, const void *k)
{
	return strcmp(tk, k);
}

pthread_mutex_t jobmgr_mtmp_rbt_mutex = PTHREAD_MUTEX_INITIALIZER;
struct rbt jobmgr_mtmp_rbt = RBT_INITIALIZER(jobmgr_mtmp_cmp);

#define JOBMGR_MTMP_RBT_LOCK() pthread_mutex_lock(&jobmgr_mtmp_rbt_mutex)
#define JOBMGR_MTMP_RBT_UNLOCK() pthread_mutex_unlock(&jobmgr_mtmp_rbt_mutex)

struct jobmgr_mtmp_s {
	struct rbn rbn;
	struct ldmsd_jobmgr_query_mdesc_s tmp;
};

int ldmsd_jobmgr_metric_register(ldmsd_jobmgr_query_mdesc_t m)
{
	struct rbn *rbn;
	struct jobmgr_mtmp_s *mtmp;
	size_t sz, name_len, unit_len;

	/* only support pritives, array of primitives and timestamp */
	switch (m->type) {
	case LDMS_V_CHAR: case LDMS_V_U8:  case LDMS_V_S8:
	case LDMS_V_U16:  case LDMS_V_S16: case LDMS_V_U32: case LDMS_V_S32:
	case LDMS_V_U64:  case LDMS_V_S64: case LDMS_V_F32: case LDMS_V_D64:
	case LDMS_V_CHAR_ARRAY:
	case LDMS_V_U8_ARRAY:  case LDMS_V_S8_ARRAY:
	case LDMS_V_U16_ARRAY: case LDMS_V_S16_ARRAY:
	case LDMS_V_U32_ARRAY: case LDMS_V_S32_ARRAY:
	case LDMS_V_U64_ARRAY: case LDMS_V_S64_ARRAY:
	case LDMS_V_F32_ARRAY: case LDMS_V_D64_ARRAY:
	case LDMS_V_TIMESTAMP:
		break;
	default:
		return EINVAL;
	}

	JOBMGR_MTMP_RBT_LOCK();

	rbn = rbt_find(&jobmgr_mtmp_rbt, m->name);

	if (!rbn)
		goto create;

	/* check if they're compatible */
	mtmp = container_of(rbn, struct jobmgr_mtmp_s, rbn);
	if (m->type != mtmp->tmp.type)
		goto eexist;
	if (m->len != mtmp->tmp.len)
		goto eexist;
	if (strcmp(m->unit, mtmp->tmp.unit))
		goto eexist;
	/* compatible, return 0 */
	goto out;

 create:
	name_len = strlen(m->name) + 1;
	unit_len = (m->unit)?(strlen(m->unit)+1):0;
	sz = sizeof(*mtmp) + name_len + unit_len;
	mtmp = calloc(1, sz);
	if (!mtmp)
		goto enomem;
	mtmp->tmp.type = m->type;
	mtmp->tmp.len = m->len;
	mtmp->tmp.level = m->level;
	mtmp->tmp.name = (char*)&mtmp[1];
	memcpy((void*)mtmp->tmp.name, m->name, name_len);
	if (unit_len) {
		mtmp->tmp.unit = mtmp->tmp.name + name_len;
		memcpy((void*)mtmp->tmp.unit, m->unit, unit_len);
	}

	rbn_init(&mtmp->rbn, (void*)mtmp->tmp.name);
	rbt_ins(&jobmgr_mtmp_rbt, &mtmp->rbn);

 out:
	JOBMGR_MTMP_RBT_UNLOCK();
	return 0;

 eexist:
	JOBMGR_MTMP_RBT_UNLOCK();
	return EEXIST;

 enomem:
	JOBMGR_MTMP_RBT_UNLOCK();
	return ENOMEM;
}

const struct ldmsd_jobmgr_query_mdesc_s *
ldmsd_jobmgr_metric_lookup(const char *name)
{
	struct rbn *rbn;
	struct jobmgr_mtmp_s *mtmp;
	struct ldmsd_jobmgr_query_mdesc_s *tmp;
	JOBMGR_MTMP_RBT_LOCK();
	rbn = rbt_find(&jobmgr_mtmp_rbt, name);
	if (!rbn) {
		errno = ENOENT;
		tmp = NULL;
	} else {
		mtmp = container_of(rbn, struct jobmgr_mtmp_s, rbn);
		tmp = &mtmp->tmp;
	}
	JOBMGR_MTMP_RBT_UNLOCK();
	return tmp;
}

extern struct rbt *cfgobj_trees[]; /* defined in ldmsd_cfgobj.c */

struct ldmsd_jobmgr_query_mdesc_s jobmgr_common_metrics[] = {
	{ "job_id",   LDMS_V_CHAR_ARRAY, NULL, LDMSD_JOBMGR_JOB_ID_LEN,   0 },
	{ "user",     LDMS_V_CHAR_ARRAY, NULL, LDMSD_JOBMGR_USER_LEN,     0 },
	{ "job_name", LDMS_V_CHAR_ARRAY, NULL, LDMSD_JOBMGR_JOB_NAME_LEN, 0 },

	{ "job_uid",   LDMS_V_U32,        NULL, 1,  0 },
	{ "job_gid",   LDMS_V_U32,        NULL, 1,  0 },
	{ "job_start", LDMS_V_TIMESTAMP,  NULL, 1,  0 },
	{ "job_end",   LDMS_V_TIMESTAMP,  NULL, 1,  0 },

	{ "job_state", LDMS_V_CHAR_ARRAY, NULL, 16, 0 },

	{ "step_id",    LDMS_V_CHAR_ARRAY, NULL, LDMSD_JOBMGR_STEP_ID_LEN, 1 },
	{ "step_start", LDMS_V_TIMESTAMP,  NULL, 1,                        1 },
	{ "step_end",   LDMS_V_TIMESTAMP,  NULL, 1,                        1 },

	{ "node_count",  LDMS_V_U32, NULL, 1, 1 },
	{ "total_tasks", LDMS_V_U32, NULL, 1, 1 },
	{ "local_tasks", LDMS_V_U32, NULL, 1, 1 },

	{ "task_id",     LDMS_V_CHAR_ARRAY, NULL, LDMSD_JOBMGR_TASK_ID_LEN, 2 },

	{ "task_pid",         LDMS_V_U64,       NULL, 1, 2 },
	{ "task_rank",        LDMS_V_U32,       NULL, 1, 2 },
	{ "task_start",       LDMS_V_TIMESTAMP, NULL, 1, 2 },
	{ "task_end",         LDMS_V_TIMESTAMP, NULL, 1, 2 },
	{ "task_exit_status", LDMS_V_S32,       NULL, 1, 2 },

	{0} /* terminator */
};

__attribute__((constructor))
static void jobmgr_once()
{
	/* register common metrics */
	struct ldmsd_jobmgr_query_mdesc_s *tmp;
	for (tmp = &jobmgr_common_metrics[0]; tmp->name; tmp++) {
		ldmsd_jobmgr_metric_register(tmp);
	}

	jobmgr_sched = ovis_scheduler_new();
	pthread_create(&jobmgr_thr, NULL, jobmgr_sched_proc, NULL);
	pthread_setname_np(jobmgr_thr, "jobmgr");
}
