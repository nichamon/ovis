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

#ifndef __LDMSD_JOBMGR_QUERY_H__
#define __LDMSD_JOBMGR_QUERY_H__

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_jobmgr.h"

typedef struct ldmsd_jobmgr_query_s *ldmsd_jobmgr_query_t;
typedef const struct ldmsd_jobmgr_query_s *const_ldmsd_jobmgr_query_t;

typedef struct ldmsd_jobmgr_query_event_s *ldmsd_jobmgr_query_event_t;

typedef int (*ldmsd_jobmgr_query_cb_fn_t)(
			ldmsd_jobmgr_query_t q,
			ldmsd_jobmgr_query_event_t qev,
			void *cb_arg
		);

struct ldmsd_jobmgr_query_mgr_s {
	TAILQ_ENTRY(ldmsd_jobmgr_query_mgr_s) entry;
	struct ldmsd_cfgobj_jobmgr *mgr;
	void *qmgr_ctxt;
};
typedef struct ldmsd_jobmgr_query_mgr_s *ldmsd_jobmgr_query_mgr_t;

struct ldmsd_jobmgr_query_s {
	ldms_record_t recdef; /* can be used in ldms_schema */

	/*
	 * These get filled by ldmsd_jobmgr_query_subscribe().
	 * They are managed by jobmgr service.
	 */
	ldmsd_jobmgr_query_cb_fn_t cb_fn;
	void *cb_arg;

	/* The set->list that will hold the query record  */
	ldms_set_t set;
	int list_midx;

	/* Metric ID of the recdef in the set */
	int recdef_midx;

	size_t rec_value_sz;

	/* Job ID index in the recdef */
	int job_id_midx;

	struct ref_s ref;
	TAILQ_ENTRY(ldmsd_jobmgr_query_s) entry;

	TAILQ_HEAD(, ldmsd_jobmgr_query_mgr_s) mgr_tq;

	int list_ref; /* protected by QUERY_TQ_LOCK */

	pthread_mutex_t mutex;
};

typedef enum ldmsd_jobmgr_query_event_type_e {
	/* the symbols are very long .... */
	LDMSD_JOBMGR_QUERY_EVENT_JOB_START,
	LDMSD_JOBMGR_QUERY_EVENT_JOB_END,
	LDMSD_JOBMGR_QUERY_EVENT_STEP_START,
	LDMSD_JOBMGR_QUERY_EVENT_STEP_END,
	LDMSD_JOBMGR_QUERY_EVENT_TASK_START,
	LDMSD_JOBMGR_QUERY_EVENT_TASK_END,
	LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE, /* the last event delivered to the cb fn */
	LDMSD_JOBMGR_QUERY_EVENT_JOB_CLEANUP, /* job data deleted */
	LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE, /* the last event delivered to the cb fn */
} ldmsd_jobmgr_query_event_type_t;

struct ldmsd_jobmgr_query_event_s {
	ldmsd_jobmgr_query_t q; /* The query handle associated to the event. */
	ldmsd_jobmgr_query_event_type_t event_type;

	ldmsd_plug_handle_t mgr; /* The job manager plugin that posted the event */

	union {
		/* Data for start/end event */
		struct {
			ldms_mval_t qrec; /* The query record associated with the event */
		} start_end;

		/* Data for no-space event */
		struct {
			int number_of_records; /* NT name too long */
		} no_space;

		struct {
			char job_id[LDMSD_JOBMGR_JOB_ID_LEN];
		} cleanup;
	};
};

/**
 * \brief Create a query handle.
 *
 * A jobmgr query handle is an object used for obtaining job information from
 * LDMS JobMgr service. The returned handle \c q contains a record definition
 * \c q->recdef that the caller use to add to an LDMS set schema. \c q->recdef
 * may contain more metrics than the requested in \c metric_list for record
 * instance identification. For example, if \c metric_list specified
 * "task_start", but did not include "task_id", the JobMgr service will added
 * "step_id" and "task_id" to \c q->recdef.
 *
 * If metric_list is NULL, the default \c q->recdef will be used.
 *
 * In the case of an error, \c NULL is returned, and  \c errno is set.
 * The \c *error string is also set to describe the error in human-readable
 * form. The caller is responsible for freeing the \c *error string.
 *
 * \param metric_list A list of metrics requested to query
 * \param cb_fn The callback function.
 * \param cb_arg The argument supplied to the callback.
 * \param[out] error An output parameter describing the error (if any) in
 *                   human-readble form.
 *
 */
ldmsd_jobmgr_query_t ldmsd_jobmgr_query_new(
		struct ldmsd_str_list *metric_list,
		ldmsd_jobmgr_query_cb_fn_t cb_fn,
		void *cb_arg,
		char **error);

/**
 * \brief Start the query event devliery.
 *
 * \retval 0 Success.
 * \retval EBUSY  The query handle has already been used in another subscription
 * \retval EINVAL Invalid parameters
 */
int ldmsd_jobmgr_query_execute(ldmsd_jobmgr_query_t q,
				ldms_set_t set,
				int rec_def_midx,
				int list_midx);

/**
 * \brief Close the query.
 *
 * Terminate event delivery to the query. The query consumer will receive the
 * last event \c LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE after this call, and
 * there will be no further events. The query handle \c q and associated jobmgr
 * resources are also released.
 */
void ldmsd_jobmgr_query_close(ldmsd_jobmgr_query_t q);



/**
 * For jobmgr plugin to post event to jobmgr service.
 */
int ldmsd_jobmgr_qev_post(ldmsd_plug_handle_t mgr,
			  ldmsd_jobmgr_query_event_type_t event_type,
			  void *ctxt);

/**
 * Tell \c jobmgr service to cleanup the records identified by \c job_id.
 */
int ldmsd_jobmgr_job_cleanup_post(ldmsd_plug_handle_t mgr, const char *job_id,
				  time_t delay_sec);

struct ldmsd_jobmgr_qrec_key_ent_s {
	int midx;
	size_t len;
	enum ldms_value_type type;
	ldms_mval_t mval;
};

struct ldmsd_jobmgr_qrec_key_s {
	int n;
	struct ldmsd_jobmgr_qrec_key_ent_s keys[];
};
typedef struct ldmsd_jobmgr_qrec_key_s *ldmsd_jobmgr_qrec_key_t;

/**
 * A common utility to get (find/alloc) a record in the list that matches
 * the key.
 */
ldms_mval_t ldmsd_jobmgr_qrec_get(ldmsd_jobmgr_query_t q, ldmsd_jobmgr_qrec_key_t k);

/**
 * Get the next query record that matches the key.
 */
ldms_mval_t ldmsd_jobmgr_qrec_next(ldmsd_jobmgr_query_t q, ldmsd_jobmgr_qrec_key_t k,
				   ldms_mval_t qrec);

/**
 * \brief Get the record field index for a named attribute in a query's recdef.
 *
 * Returns the index of a named field within the query's record definition.
 * Used by per_job_sampler_base to cache job_id, task_pid, and binding key
 * field indices at creation time for fast access during event handling.
 *
 * \param q     jobmgr query handle
 * \param name  Field name to look up (e.g. "job_id", "task_pid")
 *
 * \return Non-negative index on success
 * \return Negative value if the field is not found
 */
int ldmsd_jobmgr_query_field_index(ldmsd_jobmgr_query_t q, const char *name);

#endif
