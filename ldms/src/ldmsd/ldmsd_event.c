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

#include "ovis_ev/ev.h"
#include "ldmsd_event.h"

int default_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev)
{
	ldmsd_log(LDMSD_LINFO, "Unhandled Event: type=%s, id=%d\n",
		  ev_type_name(ev_type(ev)), ev_type_id(ev_type(ev)));
	ldmsd_log(LDMSD_LINFO, "    status  : %s\n", status ? "FLUSH" : "OK" );
	ldmsd_log(LDMSD_LINFO, "    src     : %s\n", (src)?ev_worker_name(src):"");
	ldmsd_log(LDMSD_LINFO, "    dst     : %s\n", (dst)?ev_worker_name(dst):"");
	return 0;
}

extern int log_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
extern int reqc_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
extern int recv_rec_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
extern int msg_tree_xprt_term_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
extern int deferred_start_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);

extern int recv_cfg_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);

int ldmsd_worker_init(void)
{
	logger_w = ev_worker_new("logger", log_actor);
	if (!logger_w)
		return ENOMEM;

	/* msg_tree */
	msg_tree_w = ev_worker_new("msg_tree", default_actor);
	if (!msg_tree_w)
		return ENOMEM;

	ev_dispatch(msg_tree_w, reqc_type, reqc_actor);
	ev_dispatch(msg_tree_w, recv_rec_type, recv_rec_actor);
	ev_dispatch(msg_tree_w, xprt_term_type, msg_tree_xprt_term_actor);
	ev_dispatch(msg_tree_w, deferred_start_type, deferred_start_actor);

	/* cfg worker */
	cfg_w = ev_worker_new("cfg", recv_cfg_actor);
	if (!cfg_w)
		return ENOMEM;

	return 0;
}

int ldmsd_ev_init(void)
{
	/* Event type */
	log_type = ev_type_new("ldmsd:log", sizeof(struct log_data));
	if (!log_type)
		return ENOMEM;

	xprt_term_type = ev_type_new("ldms:xprt_term", sizeof(struct xprt_term_data));
	if (!xprt_term_type)
		return ENOMEM;

	dir_add_type = ev_type_new("ldms_xprt:dir_add", sizeof(struct dir_data));
	lookup_complete_type = ev_type_new("ldms_xprt:lookup_complete",
					  sizeof(struct lookup_data));
	update_complete_type = ev_type_new("ldms_xprt:update_complete",
					  sizeof(struct update_data));
	recv_rec_type = ev_type_new("ldms_xprt:recv", sizeof(struct recv_rec_data));
	reqc_type = ev_type_new("ldmsd:reqc_ev", sizeof(struct reqc_data));
	deferred_start_type = ev_type_new("ldmsd:deferred_start", 0);
	cfg_type = ev_type_new("msg_tree:recv_cfg", sizeof(struct reqc_data));

	stream_pub_type = ev_type_new("cfg:stream", sizeof(struct stream_pub_data));
	stream_sub_type = ev_type_new("cfg:stream_sub", sizeof(struct stream_sub_data));

	prdcr_new_type = ev_type_new("cfg:prdcr_add", sizeof(struct prdcr_def_data));
	prdcr_del_type = ev_type_new("cfg:prdcr_del", sizeof(struct filter_data));
	prdcr_start_type = ev_type_new("cfg:prdcr_start", sizeof(struct filter_data));
	prdcr_stop_type = ev_type_new("cfg:prdcr_stop", sizeof(struct filter_data));
	prdcr_status_type = ev_type_new("cfg:prdcr_status", sizeof(struct filter_data));
	prdcr_rem_type = ev_type_new("prdcr:rem", sizeof(struct prdcr_data));

	prdcr_connect_type = ev_type_new("prdcr:connect", sizeof(struct prdcr_data));
	prdcr_connected_type = ev_type_new("ldms_xprt:connected", sizeof(struct prdcr_data));
	prdcr_disconnected_type = ev_type_new("ldms_xprt:disconnected", sizeof(struct prdcr_data));
	prdcr_conn_error_type = ev_type_new("ldms_xprt:conn_err", sizeof(struct prdcr_data));
	prdcr_rem_set_del_type = ev_type_new("ldms_xprt:rem_set", sizeof(struct set_del_data));

	prdset_update_hint_type = ev_type_new("prdset:update_hint", sizeof(struct prdset_data));
	prdset_add_type = ev_type_new("prdset:add", sizeof(struct prdset_data));
	prdset_strgp_add_type = ev_type_new("prdset:strgp_add", sizeof(struct prdset_data));
	prdset_strgp_rem_type = ev_type_new("prdset:strgp_rem", sizeof(struct prdset_data));
	prdset_lookup_type = ev_type_new("prdset:lookup", sizeof(struct prdset_data));
	prdset_update_type = ev_type_new("prdset:update", sizeof(struct prdset_data));
	prdset_reg_push_type = ev_type_new("prdset:reg_push", sizeof(struct prdset_data));
	prdset_sched_update_type = ev_type_new("prdset:sched_update", sizeof(struct prdset_data));

	updtr_new_type = ev_type_new("cfg:updtr_add", sizeof(struct updtr_def_data));
	updtr_del_type = ev_type_new("cfg:updtr_del", sizeof(struct filter_data));
	updtr_rem_type = ev_type_new("updtr:rem", sizeof(struct updtr_data));
	updtr_start_type = ev_type_new("cfg:start", sizeof(struct filter_data));
	updtr_stop_type = ev_type_new("cfg:stop", sizeof(struct filter_data));
	updtr_lookup_type = ev_type_new("updtr:lookup", sizeof(struct updtr_data));

	strgp_new_type = ev_type_new("cfg:strgp_add", sizeof(struct strgp_def_data));
	return 0;
}
