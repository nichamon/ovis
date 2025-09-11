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
#ifndef __LDMSD_JOBMGR_H__
#define __LDMSD_JOBMGR_H__

#include "ldmsd.h"

#define LDMSD_JOBMGR_JOB_ID_LEN 128
#define LDMSD_JOBMGR_STEP_ID_LEN 128
#define LDMSD_JOBMGR_TASK_ID_LEN 128
#define LDMSD_JOBMGR_JOB_NAME_LEN 128
#define LDMSD_JOBMGR_USER_LEN     32
#define LDMSD_JOBMGR_JOB_STATE_LEN 16

typedef struct ldmsd_cfgobj_jobmgr *ldmsd_cfgobj_jobmgr_t;
typedef struct ldmsd_jobmgr_query_s *ldmsd_jobmgr_query_t;
typedef const struct ldmsd_jobmgr_query_s *const_ldmsd_jobmgr_query_t;

typedef struct ldmsd_jobmgr_query_event_s *ldmsd_jobmgr_query_event_t;

typedef struct ldmsd_jobmgr_query_mdesc_s *ldmsd_jobmgr_query_mdesc_t;
typedef enum ldmsd_jobmgr_query_event_type_e ldmsd_jobmgr_query_event_type_t;

/**
 * \brief Register a metric.
 *
 * The `m->flags` and `m->rec_def` are ignored.
 *
 * \retval 0 Success.
 * \retval EEXIST The metric existed and has different information (e.g.
 *                different type).
 * \retval EINVAL Invalid metric template (e.g. trying to add an invalid type).
 */
int ldmsd_jobmgr_metric_register(struct ldmsd_jobmgr_query_mdesc_s *m);

/**
 * \brief Lookup a job-related metric by \c name.
 *
 * \note The caller must NOT free the returned template.
 *
 * \retval tmp The metric template (can be used in schema_add).
 *             The caller must NOT free the template.
 * \retval NULL If there is an error (e.g. \c ENOENT if metric not found).
 */
const struct ldmsd_jobmgr_query_mdesc_s *ldmsd_jobmgr_metric_lookup(const char *name);

struct ldmsd_jobmgr_query_mdesc_s {
	/* describe metrics in the query results */
	const char *name;
	enum ldms_value_type type;
	const char *unit;
	uint32_t len; /* array_len for ARRAY type; otherwise 1 */
	int level;
};

/* jobmgr API */
struct ldmsd_jobmgr {
	struct ldmsd_plugin base;

	/* Called by `ldmsd` to start the plugin.
	 * The plugin can then start manipulating \c jobset. */
	int (*start)(ldmsd_plug_handle_t p);

	/* Called by `ldmsd` to stop the plugin.
	 * The plugin shall not manipulate \c jobset after this function is called. */
	int (*stop)(ldmsd_plug_handle_t p);

	/*
	 * The plugin is expected to return its context for the query \c q.
	 * The returned context is supplied to the plugin in
	 * \c on_query_ls() and \c on_query_free() calls.
	 */
	int (*on_query_new)(ldmsd_plug_handle_t p,
			ldmsd_jobmgr_query_t q,
			void **q_ctxt_out);

	/*
	 * The plugin is expected to clean up the \c q_ctxt it returned from
	 * \c on_query_new().
	 */
	void (*on_query_free)(ldmsd_plug_handle_t p,
			ldmsd_jobmgr_query_t q, void *q_ctxt);

	/*
	 * This function is called by ldmsd_jobmgr subsystem to fill in
	 * ev->qres given ev (with ev->q already specified).
	 *
	 * The `ev_ctxt` is the context provided to the
	 * ldmsd_jobmgr_qev_post() call.
	 *
	 * The `q_ctxt` is the query context.
	 */
	int (*make_qev)(ldmsd_plug_handle_t p,
			struct ldmsd_jobmgr_query_event_s *ev,
			void *q_ctxt,
			void *ev_ctxt);

	/*
	 * This is called when ldmsd_jobmgr is done with the ev.
	 * The plugin can then clean up the `ev_ctxt` provided to the
	 * ldmsd_jobmgr_qev_post() call.
	 */
	void (*qev_done)(ldmsd_plug_handle_t p,
			 ldmsd_jobmgr_query_event_type_t event_type,
			 void *ev_ctxt);

};

/* jobmgr cfgobj instance */
struct ldmsd_cfgobj_jobmgr {
	struct ldmsd_cfgobj cfg;
	struct ldmsd_plugin_generic *plugin;
	struct ldmsd_jobmgr *api;

	/* Set to 1 if the plugin has been configured */
	int configured;

	/* Private context pointer, managed by plugin */
	void *context;
	/* ovis_log handle to use when logging plugin messages */
	ovis_log_t log;
};

static inline ldmsd_cfgobj_jobmgr_t ldmsd_jobmgr_find_get(const char *cfg_name)
{
	struct ldmsd_cfgobj *obj = ldmsd_cfgobj_find_get(cfg_name, LDMSD_CFGOBJ_JOBMGR);
	if (!obj)
		return NULL;
	return container_of(obj, struct ldmsd_cfgobj_jobmgr, cfg);
}

#define ldmsd_jobmgr_find_put(j) ldmsd_cfgobj_find_put(((struct ldmsd_cfgobj*)j))

#define ldmsd_jobmgr_get(_s_, _r_) ((ldmsd_cfgobj_jobmgr_t)ldmsd_cfgobj_get(&((ldmsd_cfgobj_jobmgr_t)_s_)->cfg, _r_))
#define ldmsd_jobmgr_put(_s_, _r_) ldmsd_cfgobj_put(&((ldmsd_cfgobj_jobmgr_t)_s_)->cfg, _r_)

int ldmsd_jobmgr_start(ldmsd_cfgobj_jobmgr_t jm);
int ldmsd_jobmgr_stop(ldmsd_cfgobj_jobmgr_t jm);

#endif
