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

extern int
prdcr_tree_cfg_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
extern int
prdcr_tree_cfg_rsp_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);
extern int
prdcr_cfg_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);
extern int
prdcr_connect_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);
extern int
prdcr_xprt_event_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);
extern int
prdcr_dir_complete_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

extern int
prdset_add_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

extern int
updtr_tree_prdset_add_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

extern int
strgp_tree_prdset_add_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

extern int
configfile_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

extern int
configfile_resp_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

extern int
failover_routine_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

extern int
failover_ib_rsp_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

extern int
failover_cfgobj_rsp_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

extern int
failover_cfg_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e);

int ldmsd_worker_init(void)
{
	int i;
	char s[128];

	logger_w = ev_worker_new("logger", log_actor);
	if (!logger_w)
		goto enomem;

	configfile_w = ev_worker_new("configfile", default_actor);
	if (!configfile_w)
		goto enomem;
	ev_dispatch(configfile_w, cfgfile_type, configfile_actor);
	ev_dispatch(configfile_w, ob_rsp_type, configfile_resp_actor);

	/* msg_tree */
	msg_tree_w = ev_worker_new("msg_tree", default_actor);
	if (!msg_tree_w)
		goto enomem;

	ev_dispatch(msg_tree_w, reqc_type, reqc_actor);
	ev_dispatch(msg_tree_w, recv_rec_type, recv_rec_actor);
	ev_dispatch(msg_tree_w, xprt_term_type, msg_tree_xprt_term_actor);
	ev_dispatch(msg_tree_w, deferred_start_type, deferred_start_actor);

	/* cfg worker */
	cfg_w = ev_worker_new("cfg", recv_cfg_actor);
	if (!cfg_w)
		goto enomem;

	/* prdcr_tree worker */
	prdcr_tree_w = ev_worker_new("prdcr_tree", default_actor);
	if (!prdcr_tree_w)
		goto enomem;

	ev_dispatch(prdcr_tree_w, cfg_type, prdcr_tree_cfg_actor);
	ev_dispatch(prdcr_tree_w, cfgobj_rsp_type, prdcr_tree_cfg_rsp_actor);

	/* prdcr worker pool */
	prdcr_pool = malloc(ldmsd_num_prdcr_workers_get() * sizeof(ev_worker_t));
	if (!prdcr_pool)
		goto enomem;
	for (i = 0; i < ldmsd_num_prdcr_workers_get(); i++) {
		snprintf(s, 127, "prdcr_w_%d", i + 1);
		prdcr_pool[i] = ev_worker_new(s, default_actor);
		if (!prdcr_pool[i])
			goto enomem;

		ev_dispatch(prdcr_pool[i], cfgobj_cfg_type, prdcr_cfg_actor);
		ev_dispatch(prdcr_pool[i], prdcr_connect_type, prdcr_connect_actor);
		ev_dispatch(prdcr_pool[i], prdcr_xprt_type, prdcr_xprt_event_actor);
		ev_dispatch(prdcr_pool[i], dir_complete_type, prdcr_dir_complete_actor);
	}

	prdset_pool = malloc(ldmsd_num_prdset_workers_get() * sizeof(ev_worker_t));
	if (!prdset_pool)
		goto enomem;
	for (i = 0; i < ldmsd_num_prdset_workers_get(); i++) {
		snprintf(s, 127, "prdset_w_%d", i + 1);
		prdset_pool[i] = ev_worker_new(s, default_actor);
		if (!prdset_pool[i])
			goto enomem;

		ev_dispatch(prdset_pool[i], prdset_add_type, prdset_add_actor);
	}

	/* updtr_tree worker */
	updtr_tree_w = ev_worker_new("updtr_tree", default_actor);
	if (!updtr_tree_w)
		goto enomem;
	ev_dispatch(updtr_tree_w, prdset_add_type, updtr_tree_prdset_add_actor);

	/* strgp_tree worker */
	strgp_tree_w = ev_worker_new("strgp_tree", default_actor);
	if (!strgp_tree_w)
		goto enomem;
	ev_dispatch(strgp_tree_w, prdset_add_type, strgp_tree_prdset_add_actor);

	/* failover worker */
	failover_w = ev_worker_new("failover", default_actor);
	if (!failover_w)
		goto enomem;
	ev_dispatch(failover_w, failover_routine_type, failover_routine_actor);
	ev_dispatch(failover_w, cfg_type, failover_cfg_actor);
	ev_dispatch(failover_w, rsp_type, failover_ib_rsp_actor);
	ev_dispatch(failover_w, cfgobj_rsp_type, failover_cfgobj_rsp_actor);

	return 0;
enomem:
	return ENOMEM;
}

int ldmsd_ev_init(void)
{
	/* Event type */
	log_type = ev_type_new("ldmsd:log", sizeof(struct log_data));
	if (!log_type)
		return ENOMEM;

	cfgfile_type = ev_type_new("ldmsd:configfile", sizeof(struct cfgfile_data));

	xprt_term_type = ev_type_new("ldms:xprt_term", sizeof(struct xprt_term_data));
	if (!xprt_term_type)
		return ENOMEM;

	dir_complete_type = ev_type_new("ldms_xprt:dir_add", sizeof(struct dir_data));
	lookup_complete_type = ev_type_new("ldms_xprt:lookup_complete",
					  sizeof(struct lookup_data));
	update_complete_type = ev_type_new("ldms_xprt:update_complete",
					  sizeof(struct update_data));
	recv_rec_type = ev_type_new("ldms_xprt:recv", sizeof(struct recv_rec_data));
	reqc_type = ev_type_new("ldmsd:reqc_ev", sizeof(struct reqc_data));
	deferred_start_type = ev_type_new("ldmsd:deferred_start", 0);
	cfg_type = ev_type_new("recv_cfg", sizeof(struct cfg_data));
	rsp_type = ev_type_new("recv_rsp", sizeof(struct rsp_data));
	ob_rsp_type = ev_type_new("outbound_rsp", sizeof(struct ob_rsp_data));

	cfgobj_cfg_type = ev_type_new("cfgobj_tree:cfgobj:cfg_req", sizeof(struct cfgobj_data));
	cfgobj_rsp_type = ev_type_new("cfgobj:cfgobj_tree:cfg_rsp", sizeof(struct cfgobj_rsp_data));

	prdcr_connect_type = ev_type_new("prdcr:connect", sizeof(struct cfgobj_data));
	prdcr_xprt_type = ev_type_new("ldms_xprt:prdcr:xprt_event", sizeof(struct cfgobj_data));

	prdset_add_type = ev_type_new("prdcr:prdset:add", sizeof(struct prdset_data));

	failover_routine_type = ev_type_new("failover:routine", sizeof(struct failover_data));
	failover_xprt_type = ev_type_new("ldms_xprt:failover:xprt_event", sizeof(struct failover_data));
	return 0;
}

ev_worker_t __assign_worker(ev_worker_t *pool, int *_idx, int count)
{
	int i = *_idx;
	ev_worker_t w = pool[i];
	i++;
	i = i%count;
	*_idx = i;
	return w;
}

ev_worker_t assign_prdcr_worker()
{
	/*
	 * TODO: Use a more sophisticated than a round-robin
	 */
	static int i = 0;
	return __assign_worker(prdcr_pool, &i, ldmsd_num_prdcr_workers_get());
}

ev_worker_t assign_prdset_worker()
{
	/*
	 * TODO: Use a more sophisticated than a round-robin
	 */
	static int i = 0;
	return __assign_worker(prdcr_pool, &i, ldmsd_num_prdcr_workers_get());
}

ev_worker_t assign_failover_worker()
{
	return failover_w;
}

struct filt_ent *ldmsd_filter_first(struct filter_data *filt)
{
	filt->_cur_ent = TAILQ_FIRST(&filt->filt);
	return filt->_cur_ent;
}

struct filt_ent *ldmsd_filter_next(struct filter_data *filt)
{
	filt->_cur_ent = TAILQ_NEXT(filt->_cur_ent, ent);
	return filt->_cur_ent;
}
