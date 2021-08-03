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
ev_worker_t configfile_w;
ev_worker_t msg_tree_w;
ev_worker_t prdcr_tree_w;
ev_worker_t updtr_tree_w;
ev_worker_t strgp_tree_w;
ev_worker_t auth_tree_w;
ev_worker_t ldmsd_set_tree_w;
ev_worker_t stream_pub_w;
ev_worker_t stream_sub_w;
ev_worker_t failover_w;

/* Pool of workers that own the same type of resources */
ev_worker_t *prdcr_pool;
ev_worker_t *prdset_pool;
ev_worker_t *updtr_pool;
ev_worker_t strgp_pool;
ev_worker_t auth_pool;

/* event data */

/* Event Types and data */
struct filt_ent {
	const char *str;
	regex_t regex;
	TAILQ_ENTRY(filt_ent) ent;
};
TAILQ_HEAD(str_list, filt_ent);

struct filter_data {
	uint8_t is_regex;
	int cnt;
	struct str_list filt;

	/*
	 * If this is not NULL,
	 * receiver must report success/failure to the request by
	 * preparing the response and calling ldmsd_send_req_resp().
	 */
	struct ldmsd_req_ctxt *reqc;

	/* internal */
	struct filt_ent *_cur_ent;
	void *_ctxt;
};

struct log_data {
	uint8_t is_rotate;
	enum ldmsd_loglevel level;
	char *msg;
};

typedef int (*req_filter_fn_t)(ldmsd_cfg_xprt_t, ldmsd_req_hdr_t, void *);
struct cfgfile_data {
	const char *path;
	int *line_no;
	int trust;
	req_filter_fn_t filter_fn;
	void *ctxt;
};

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

struct cfg_data { /* TODO: rename this to recv_cfg_data */
	ldmsd_req_ctxt_t reqc;
	void *ctxt;
};

struct rsp_data { /* TODO: rename this to recv_rsp_data */
	ldmsd_req_cmd_t rcmd;
	void *ctxt;
};

/* Outbound response data */
struct ob_rsp_data {
	ldmsd_req_hdr_t hdr;
	void *ctxt;
};

struct cfgobj_data {
	ldmsd_cfgobj_t obj;
	void *ctxt;
};

struct cfgobj_rsp_data {
	void *rsp; /* Response of each cfgobj */
	void *ctxt; /* Context from the cfgobj_cfg event */
};

struct xprt_term_data {
	ldms_t x;
};

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

struct prdcr_data {
	ldmsd_prdcr_t prdcr;
};

struct set_del_data {
	ldms_set_t set;
};

struct prdset_data {
	struct ldmsd_prdcr_set *prdset;
	enum ldmsd_prdcr_set_state state;
	char *prdcr_name;
	char *updtr_name;
	struct ldmsd_updtr_schedule update_schedule;
	char *strgp_name;
	ldmsd_cfgobj_t obj;
};

struct updtr_def_data {
	json_entity_t def;
	ldmsd_req_ctxt_t reqc;
};

struct updtr_info {
	struct ref_s ref;
	struct ldmsd_updtr_schedule sched;
	uint8_t push_flags;
	uint8_t is_auto;
	struct ldmsd_filter prdcr_list;
	struct ldmsd_match_queue match_list;
};

struct updtr_state_data {
	enum ldmsd_updtr_state state;
	struct updtr_info *updtr_info;
	void *obj;
};

struct prdcr_n_set_filter_data {
	struct filter_data prdcr_filter;
	struct filter_data set_filter;
};

struct updtr_stop_data {
	struct prdcr_n_set_filter_data filter;
	uint8_t is_push;
};

struct updtr_data {
	ldmsd_updtr_t updtr;
};

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

struct set_reg_data {
	char *pi_inst_name;
	char *set_inst_name;
};

struct dir_data {
	ldms_dir_t dir;
	ldmsd_prdcr_t prdcr;
};

struct lookup_data {
	enum ldms_lookup_status status;
	int more;
	ldms_set_t set;
	ldmsd_prdcr_set_t prdset;
};

struct update_data {
	uint8_t is_failed;
	uint64_t gn;
	ldms_set_t set;
};

#ifdef LDMSD_FAILOVER
typedef struct ldmsd_failover *ldmsd_failover_t;
struct failover_data {
	ldmsd_failover_t f;
	void *ctxt;
};
#endif /* LDMSD_FAILOVER */

/* Event Types */

/* ldms xprt */
ev_type_t dir_complete_type;
ev_type_t lookup_complete_type;
ev_type_t update_complete_type;

/* LDMSD log */
ev_type_t log_type;

/* LDMSD messages & request contexts */
ev_type_t recv_rec_type;
ev_type_t reqc_type; /* add to msg_tree, rem to msg_tree, send to cfg */
ev_type_t deferred_start_type;
ev_type_t cfg_type; /* TODO: rename this to recv_cfg_type */
ev_type_t rsp_type; /* TODO: rename this to recv_rsp_type */
ev_type_t ob_rsp_type; /* Outbound response type */

ev_type_t xprt_term_type;

/* Configuration */
ev_type_t cfgfile_type;

ev_type_t cfgobj_cfg_type;
ev_type_t cfgobj_rsp_type;

/* Producers */
ev_type_t prdcr_cfg_type;
ev_type_t prdcr_rem_type;
ev_type_t prdcr_start_rsp_type;

/* prdcr connect */
ev_type_t prdcr_connect_type;
ev_type_t prdcr_xprt_type;

/* producer sets */
ev_type_t prdset_update_hint_type;
ev_type_t prdset_add_type;
ev_type_t prdset_state_type;
ev_type_t prdset_strgp_add_type;
ev_type_t prdset_strgp_rem_type;
ev_type_t prdset_lookup_type;
ev_type_t prdset_update_type;
ev_type_t prdset_reg_push_type;
ev_type_t prdset_sched_update_type;

/* Updaters */
ev_type_t updtr_new_type;
ev_type_t updtr_state_type;
ev_type_t updtr_del_type;
ev_type_t updtr_rem_type;
ev_type_t updtr_start_type;
ev_type_t updtr_stop_type;
ev_type_t updtr_lookup_type;

/* Storage Policies */
ev_type_t strgp_new_type;
ev_type_t strgp_del_type;
ev_type_t strgp_rem_type;
ev_type_t strgp_start_type;
ev_type_t strgp_stop_type;

/* streams */
ev_type_t stream_pub_type;
ev_type_t stream_sub_type;

/* Sets */
ev_type_t pi_set_query_type;
ev_type_t ldmsd_set_reg_type;
ev_type_t ldmsd_set_dereg_type;

#ifdef LDMSD_FAILOVER
/* Failover */
ev_type_t failover_routine_type;
ev_type_t failover_xprt_type;
#endif /* LDMSD_FAILOVER */

int ldmsd_ev_init(void);
int ldmsd_worker_init(void);
struct filt_ent *ldmsd_filter_first(struct filter_data *filt);
struct filt_ent *ldmsd_filter_next(struct filter_data *filt);

void prdset_data_cleanup(struct prdset_data *data);
int prdset_data_copy(struct prdset_data *src, struct prdset_data *dst);
