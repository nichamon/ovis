/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2021 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2021 Open Grid Computing, Inc. All rights reserved.
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

#include "ovis_json/ovis_json.h"
#include "ovis_ev/ev.h"
#include "ldmsd_stream.h"
#include "ldmsd_request.h"
#include "ldmsd.h"


/* Worker */
ev_worker_t logger_w;
ev_worker_t cfg_w;
ev_worker_t logger_w;
ev_worker_t msg_tree_w;
ev_worker_t prdcr_tree_w;
ev_worker_t updtr_tree_w;
ev_worker_t strgp_tree_w;
ev_worker_t auth_tree_w;
ev_worker_t ldmsd_set_tree_w;
ev_worker_t stream_pub_w;
ev_worker_t stream_sub_w;

/* Pool of workers that own the same type of resources */
ev_worker_t prdcr_pool;
ev_worker_t prd_set_pool;
ev_worker_t updtr_pool;
ev_worker_t strgp_pool;
ev_worker_t auth_pool;

/* Event Types and data */
struct filter_data {
	char *filter;
	int cnt;
	ldmsd_req_ctxt_t reqc;
};

/* ldms xprt */
ev_type_t dir_add_type;
ev_type_t lookup_complete_type;
ev_type_t update_complete_type;

struct dir_data {
	ldms_dir_t dir;
};

struct lookup_data {
	ldmsd_prdcr_set_t prdset;
};

struct update_data {
	uint8_t is_failed;
	uint64_t gn;
	ldms_set_t set;
};

/* LDMSD log */
ev_type_t log_type;

struct log_data {
	uint8_t is_rotate;
	enum ldmsd_loglevel level;
	char *msg;
};

/* LDMSD messages & request contexts */
ev_type_t recv_rec_type;
ev_type_t reqc_type; /* add to msg_tree, rem to msg_tree, send to cfg */
ev_type_t deferred_start_type;

struct recv_rec_data {
	ldmsd_req_hdr_t rec;
	struct ldmsd_cfg_xprt_s xprt;
};

struct reqc_data {
	enum {
		REQC_EV_TYPE_ADD = 1,
		REQC_EV_TYPE_REM,
		REQC_EV_TYPE_CFG
	} type;
	ldmsd_req_ctxt_t reqc;
};

ev_type_t xprt_term_type;

struct xprt_term_data {
	ldms_t x;
};

/* streams */
ev_type_t stream_pub_type;
ev_type_t stream_sub_type;

struct stream_pub_data {
	char *stream_name;
	ldmsd_stream_type_t type;
	char *data; /* stream payload */
	size_t sz; /* payload size */
};

struct stream_sub_data {
	char *stream_name;
	ldms_t ldms;
};

/* prdcr config */
ev_type_t prdcr_new_type;
ev_type_t prdcr_del_type;
ev_type_t prdcr_start_type;
ev_type_t prdcr_stop_type;
ev_type_t prdcr_status_type;
ev_type_t prdcr_rem_type;

struct prdcr_def_data {
	json_entity_t def;
	ldmsd_req_ctxt_t reqc;
};

/* prdcr connect */
ev_type_t prdcr_connect_type;
ev_type_t prdcr_connected_type;
ev_type_t prdcr_disconnected_type;
ev_type_t prdcr_conn_error_type;
ev_type_t prdcr_rem_set_del_type;

struct prdcr_data {
	ldmsd_prdcr_t prdcr;
};

struct set_del_data {
	ldms_set_t set;
};

/* producer sets */
ev_type_t prdset_update_hint_type;
ev_type_t prdset_add_type;
ev_type_t prdset_strgp_add_type;
ev_type_t prdset_strgp_rem_type;
ev_type_t prdset_lookup_type;
ev_type_t prdset_update_type;
ev_type_t prdset_reg_push_type;
ev_type_t prdset_sched_update_type;

struct prdset_data {
	ldmsd_prdcr_set_t prdset;
	ldmsd_updtr_t updtr;
	ldmsd_strgp_t strgp;
};

/* Updaters */
ev_type_t updtr_new_type;
ev_type_t updtr_del_type;
ev_type_t updtr_rem_type;
ev_type_t updtr_start_type;
ev_type_t updtr_stop_type;
ev_type_t updtr_lookup_type;

struct updtr_def_data {
	json_entity_t def;
	ldmsd_req_ctxt_t reqc;
};

struct updtr_filter_data {
	char *prdcr_filter; /* TODO: subject to change depending on cfg protocol */
	uint8_t is_prdcr_regex;
	char *set_filter;
	uint8_t is_set_regex;
	ldmsd_req_ctxt_t reqc;
};

struct updtr_data {
	ldmsd_updtr_t updtr;
};

/* Storage Policies */
ev_type_t strgp_new_type;
ev_type_t strgp_del_type;
ev_type_t strgp_rem_type;
ev_type_t strgp_start_type;
ev_type_t strgp_stop_type;

struct strgp_def_data {
	json_entity_t def;
	ldmsd_req_ctxt_t reqc;
};

struct strgp_filter_data {
	char *prdcr_filter;
	uint8_t is_prdcr_regex;
	char *set_filter;
	uint8_t is_set_regex;
	ldmsd_req_ctxt_t reqc;
};

/* Sets */
ev_type_t pi_set_query_type;
ev_type_t ldmsd_set_reg_type;
ev_type_t ldmsd_set_dereg_type;

struct set_reg_data {
	char *pi_inst_name;
	char *set_inst_name;
};

int ldmsd_ev_init(void);
int ldmsd_worker_init(void);
