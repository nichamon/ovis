/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2015-2018 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2015-2018 Open Grid Computing, Inc. All rights reserved.
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
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <coll/rbt.h>
#include <pthread.h>
#include <ovis_util/util.h>
#include <json/json_util.h>
#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plugin.h"
#include "ldmsd_sampler.h"
#include "ldmsd_store.h"
#include "ldmsd_request.h"
#include "ldmsd_stream.h"
#include "ldms_xprt.h"

/*
 * This file implements an LDMSD control protocol. The protocol is
 * message oriented and has message boundary markers.
 *
 * Every message has a unique msg_no identifier. Every record that is
 * part of the same message has the same msg_no value. The flags field
 * is a bit field as follows:
 *
 * 1 - Start of Message
 * 2 - End of Message
 *
 * The rec_len field is the size of the record including the header.
 * It is assumed that when reading from the socket that the next
 * message starts at cur_ptr + rec_len when cur_ptr starts at 0 and is
 * incremented by the read length for each socket operation.
 *
 * When processing protocol records, the header is stripped off and
 * all reqresp strings that share the same msg_no are concatenated
 * together until the record in which flags | End of Message is True
 * is received and then delivered to the ULP as a single message
 *
 */

int ldmsd_req_debug = 0; /* turn on / off using gdb or edit src to
                                 * see request/response debugging messages */

/*
 * TODO: Uncomment this when refactoring the quit handler
 */
static int cleanup_requested = 0;

void __ldmsd_log(enum ldmsd_loglevel level, const char *fmt, va_list ap);

__attribute__((format(printf, 1, 2)))
static inline
void __dlog(const char *fmt, ...)
{
	if (!ldmsd_req_debug)
		return;
	va_list ap;
	va_start(ap, fmt);
	__ldmsd_log(LDMSD_LALL, fmt, ap);
	va_end(ap);
}

static int msg_comparator(void *a, const void *b)
{
	ldmsd_msg_key_t ak = (ldmsd_msg_key_t)a;
	ldmsd_msg_key_t bk = (ldmsd_msg_key_t)b;
	int rc;

	rc = ak->conn_id - bk->conn_id;
	if (rc)
		return rc;
	return ak->msg_no - bk->msg_no;
}
struct rbt req_msg_tree = RBT_INITIALIZER(msg_comparator);
struct rbt rsp_msg_tree = RBT_INITIALIZER(msg_comparator);
pthread_mutex_t req_msg_tree_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rsp_msg_tree_lock = PTHREAD_MUTEX_INITIALIZER;

static
void ldmsd_req_ctxt_sec_get(ldmsd_req_ctxt_t rctxt, ldmsd_sec_ctxt_t sctxt)
{
	switch (rctxt->xprt->type) {
	case LDMSD_CFG_XPRT_CONFIG_FILE:
		ldmsd_sec_ctxt_get(sctxt);
		break;
	case LDMSD_CFG_XPRT_LDMS:
		ldms_xprt_cred_get(rctxt->xprt->xprt, NULL, &sctxt->crd);
		break;
	}
}

typedef int (*ldmsd_obj_handler_t)(ldmsd_req_ctxt_t reqc);
struct request_handler_entry {
	int req_id;
	ldmsd_obj_handler_t handler;
	int flag; /* Lower 12 bit (mask 0777) for request permisson.
		   * The rest is reserved for ldmsd_request use. */
};

struct obj_handler_entry {
	const char *name;
	ldmsd_obj_handler_t handler;
	int flag; /* Lower 12 bit (mask 0777) for request permission.
	   * The rest is reserved for ldmsd_request use. */
};

/*
 * Command object handlers
 */
static int daemon_status_handler(ldmsd_req_ctxt_t reqc);
static int example_handler(ldmsd_req_ctxt_t req_ctxt);
static int greeting_handler(ldmsd_req_ctxt_t req_ctxt);
static int include_handler(ldmsd_req_ctxt_t req_ctxt);
static int plugin_list_handler(ldmsd_req_ctxt_t reqc);
static int plugin_query_handler(ldmsd_req_ctxt_t reqc);
static int plugin_sets_handler(ldmsd_req_ctxt_t reqc);
static int plugin_status_handler(ldmsd_req_ctxt_t reqc);
static int plugin_usage_handler(ldmsd_req_ctxt_t reqc);
static int prdcr_set_status_handler(ldmsd_req_ctxt_t reqc);
static int prdcr_status_handler(ldmsd_req_ctxt_t reqc);
static int set_route_handler(ldmsd_req_ctxt_t req_ctxt);
static int smplr_status_handler(ldmsd_req_ctxt_t reqc);
static int strgp_status_handler(ldmsd_req_ctxt_t reqc);
static int stream_subscribe_handler(ldmsd_req_ctxt_t reqc);
static int updtr_status_handler(ldmsd_req_ctxt_t reqc);
static int version_handler(ldmsd_req_ctxt_t reqc);
static int export_config_handler(ldmsd_req_ctxt_t reqc);

/*
 * Configuration object handlers
 */
static int auth_handler(ldmsd_req_ctxt_t reqc);
static int env_handler(ldmsd_req_ctxt_t reqc);
static int listen_handler(ldmsd_req_ctxt_t reqc);
static int plugin_instance_handler(ldmsd_req_ctxt_t reqc);
static int prdcr_handler(ldmsd_req_ctxt_t reqc);
static int setgroup_handler(ldmsd_req_ctxt_t reqc);
static int smplr_handler(ldmsd_req_ctxt_t req_ctxt);
static int strgp_handler(ldmsd_req_ctxt_t reqc);
static int updtr_handler(ldmsd_req_ctxt_t reqc);

/*
 * Action object handlers
 */
static int auth_action_handler(ldmsd_req_ctxt_t reqc);
static int daemon_exit_handler(ldmsd_req_ctxt_t reqc);
static int plugin_instance_action_handler(ldmsd_req_ctxt_t reqc);
static int prdcr_action_handler(ldmsd_req_ctxt_t reqc);
static int smplr_action_handler(ldmsd_req_ctxt_t reqc);
static int set_udata_handler(ldmsd_req_ctxt_t req_ctxt);
static int setgroup_action_handler(ldmsd_req_ctxt_t reqc);
static int strgp_action_handler(ldmsd_req_ctxt_t reqc);
static int updtr_action_handler(ldmsd_req_ctxt_t reqc);
static int verbosity_change_handler(ldmsd_req_ctxt_t reqc);

//static int oneshot_handler(ldmsd_req_ctxt_t req_ctxt);
//static int logrotate_handler(ldmsd_req_ctxt_t req_ctxt);

///* these are implemented in ldmsd_failover.c */
//int failover_config_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_peercfg_start_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_peercfg_stop_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_mod_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_status_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_pair_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_reset_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_cfgprdcr_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_cfgupdtr_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_cfgstrgp_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_ping_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_peercfg_handler(ldmsd_req_ctxt_t req);
//
//int failover_start_handler(ldmsd_req_ctxt_t req_ctxt);
//int failover_stop_handler(ldmsd_req_ctxt_t req_ctxt);
//
//static int auth_del_handler(ldmsd_req_ctxt_t reqc);

/* executable for all */
#define XALL 0111
/* executable for user, and group */
#define XUG 0110

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

static int handler_entry_comp(const void *a, const void *b)
{
	struct obj_handler_entry *_a = (struct obj_handler_entry *)a;
	struct obj_handler_entry *_b = (struct obj_handler_entry *)b;
	return strcmp(_a->name, _b->name);
}

static struct obj_handler_entry cfg_obj_handler_tbl[] = {
		{ "auth",		auth_handler,			XUG },
		{ "env",		env_handler,			XUG },
		{ "listen",		listen_handler,			XUG },
		{ "plugin_instance",	plugin_instance_handler,	XUG },
		{ "prdcr",		prdcr_handler,			XUG },
		{ "setgroup",		setgroup_handler,		XUG },
		{ "smplr",		smplr_handler,			XUG },
		{ "strgp",		strgp_handler,			XUG },
		{ "updtr",		updtr_handler,			XUG },
};

static struct obj_handler_entry cmd_obj_handler_tbl[] = {
		{ "daemon_status",	daemon_status_handler,	XALL },
		{ "example", 	example_handler, 	XALL },
		{ "export_config",	export_config_handler,	XUG | LDMSD_PERM_FAILOVER_ALLOWED },
		{ "greeting",	greeting_handler,	XALL },
		{ "include",	include_handler,	XUG },	/* TODO: This is a special one */
		{ "plugin_list",	plugin_list_handler,	XALL },
		{ "plugin_query",	plugin_query_handler,	XALL },
		{ "plugin_sets",	plugin_sets_handler,	XALL },
		{ "plugin_status",	plugin_status_handler,	XALL },
		{ "plugin_usage",	plugin_usage_handler,	XALL },
		{ "prdcr_set_status",	prdcr_set_status_handler, XALL },
		{ "prdcr_status",	prdcr_status_handler,	XALL },
		{ "set_route",	set_route_handler, 	XUG },
		{ "smplr_status",	smplr_status_handler,	XALL },
		{ "stream_subscribe",	stream_subscribe_handler,	XUG },
		{ "strgp_status",	strgp_status_handler,	XALL },
		{ "updtr_status",	updtr_status_handler,	XALL },
		{ "version",	version_handler,		XALL },
};

static struct obj_handler_entry act_obj_handler_tbl[] = {
		{ "auth",	auth_action_handler,	XUG },
		{ "daemon_exit",	daemon_exit_handler,	XUG },
		{ "plugin_instance",	plugin_instance_action_handler,	XUG },
		{ "prdcr",	prdcr_action_handler,	XUG },
		{ "setgroup",	setgroup_action_handler,XUG },
		{ "set_udata",	set_udata_handler,	XUG },
		{ "smplr", 	smplr_action_handler,	XUG },
		{ "strgp",	strgp_action_handler,	XUG },
		{ "updtr",	updtr_action_handler,	XUG },
		{ "verbosity_change",	verbosity_change_handler, XUG },
};

/*
 * The process request function takes records and collects
 * them into messages. These messages are then delivered to the req_id
 * specific handlers.
 *
 * The assumptions are the following:
 * 1. msg_no is unique on the socket
 * 2. There may be multiple messages outstanding on the same socket
 */
static ldmsd_req_ctxt_t find_req_ctxt(struct ldmsd_msg_key *key, int type)
{
	ldmsd_req_ctxt_t rm = NULL;
	struct rbt *tree;
	struct rbn *rbn;

	if (LDMSD_REQ_CTXT_RSP == type)
		tree = &rsp_msg_tree;
	else
		tree = &req_msg_tree;
	rbn = rbt_find(tree, key);
	if (rbn)
		rm = container_of(rbn, struct ldmsd_req_ctxt, rbn);
	return rm;
}

void ldmsd_req_ctxt_tree_lock(int type)
{
	if (LDMSD_REQ_CTXT_REQ == type)
		pthread_mutex_lock(&req_msg_tree_lock);
	else
		pthread_mutex_lock(&rsp_msg_tree_lock);
}

void ldmsd_req_ctxt_tree_unlock(int type)
{
	if (LDMSD_REQ_CTXT_REQ == type)
		pthread_mutex_unlock(&req_msg_tree_lock);
	else
		pthread_mutex_unlock(&rsp_msg_tree_lock);
}

/*
 * Caller must hold the tree lock
 */
ldmsd_req_ctxt_t ldmsd_req_ctxt_first(int type)
{
	ldmsd_req_ctxt_t reqc;
	struct rbn *rbn;
	struct rbt *tree = (type == LDMSD_REQ_CTXT_REQ)?&req_msg_tree:&rsp_msg_tree;
	rbn = rbt_min(tree);
	if (!rbn)
		return NULL;
	reqc = container_of(rbn, struct ldmsd_req_ctxt, rbn);
	return reqc;
}

/*
 * Caller must hold the tree lock
 */
ldmsd_req_ctxt_t ldmsd_req_ctxt_next(ldmsd_req_ctxt_t reqc)
{
	ldmsd_req_ctxt_t next;
	struct rbn *rbn;
	rbn = rbn_succ(&reqc->rbn);
	if (!rbn)
		return NULL;
	next = container_of(rbn, struct ldmsd_req_ctxt, rbn);
	return next;
}

/* The caller must _not_ hold the msg_tree lock. */
void __req_ctxt_del(ldmsd_req_ctxt_t reqc)
{
	ldmsd_cfg_xprt_ref_put(reqc->xprt, "req_ctxt");
	if (reqc->recv_buf)
		ldmsd_req_buf_free(reqc->recv_buf);
	if (reqc->send_buf)
		ldmsd_req_buf_free(reqc->send_buf);
	free(reqc);
}

int cfg_msg_ctxt_free_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev)
{
	ldmsd_req_ctxt_t reqc;
	reqc = EV_DATA(ev, struct msg_ctxt_free_data)->reqc;
	ldmsd_req_ctxt_ref_put(reqc, "create");
	return 0;
}

void __msg_key_get(ldmsd_cfg_xprt_t xprt, uint32_t msg_no,
						ldmsd_msg_key_t key_)
{
	key_->msg_no = msg_no;
	if (xprt->type == LDMSD_CFG_XPRT_LDMS) {
		/*
		 * Don't use the cfg_xprt directly because
		 * a new cfg_xprt get allocated
		 * every time LDMSD receives a record.
		 */
		key_->conn_id = (uint64_t)(unsigned long)xprt->ldms.ldms;
	} else {
		key_->conn_id = (uint64_t)(unsigned long)xprt;
	}
}

/*
 * max_msg_len must be a positive number.
 *
 * The caller must hold the msg_tree lock.
 */
ldmsd_req_ctxt_t
__req_ctxt_alloc(ldmsd_msg_key_t key, ldmsd_cfg_xprt_t xprt, int type)
{
	ldmsd_req_ctxt_t reqc;

	reqc = calloc(1, sizeof *reqc);
	if (!reqc)
		return NULL;

	reqc->recv_buf = ldmsd_req_buf_alloc(xprt->max_msg);
	if (!reqc->recv_buf)
		goto err;

	reqc->send_buf = ldmsd_req_buf_alloc(xprt->max_msg);
	if (!reqc->send_buf)
		goto err;

	ldmsd_cfg_xprt_ref_get(xprt, "req_ctxt");
	reqc->xprt = xprt;

	reqc->free_ev = ev_new(cfg_msg_ctxt_free_type);
	EV_DATA(reqc->free_ev, struct msg_ctxt_free_data)->reqc = reqc;

	ref_init(&reqc->ref, "create", (ref_free_fn_t)__req_ctxt_del, reqc);
	reqc->key = *key;
	rbn_init(&reqc->rbn, &reqc->key);
	reqc->type = type;
	if (LDMSD_REQ_CTXT_RSP == type) {
		rbt_ins(&rsp_msg_tree, &reqc->rbn);
	} else {
		rbt_ins(&req_msg_tree, &reqc->rbn);
	}
	return reqc;
 err:
 	__req_ctxt_del(reqc);
	return NULL;
}

/**
 * Allocate a request message context.
 */
ldmsd_req_ctxt_t
ldmsd_req_ctxt_alloc(struct ldmsd_msg_key *key, ldmsd_cfg_xprt_t xprt)
{
	return __req_ctxt_alloc(key, xprt, LDMSD_REQ_CTXT_RSP);
}

int __ldmsd_req_ctxt_free_nolock(ldmsd_req_ctxt_t reqc)
{
	if (LDMSD_REQ_CTXT_REQ == reqc->type)
		rbt_del(&req_msg_tree, &reqc->rbn);
	else
		rbt_del(&rsp_msg_tree, &reqc->rbn);
	return ev_post(cfg, cfg, reqc->free_ev, NULL);
}

int ldmsd_req_ctxt_free(ldmsd_req_ctxt_t reqc)
{
	ldmsd_req_ctxt_tree_lock(reqc->type);
	if (LDMSD_REQ_CTXT_REQ == reqc->type)
		rbt_del(&req_msg_tree, &reqc->rbn);
	else
		rbt_del(&rsp_msg_tree, &reqc->rbn);
	ldmsd_req_ctxt_tree_unlock(reqc->type);
	return ev_post(cfg, cfg, reqc->free_ev, NULL);
}

int __ldmsd_append_buffer(ldmsd_cfg_xprt_t xprt, struct ldmsd_msg_key *key,
			ldmsd_req_buf_t buf,
			int msg_flags, int msg_type,
			const char *data, size_t data_len)
{
	return ldmsd_append_msg_buffer(xprt, xprt->max_msg, key,
				(ldmsd_msg_send_fn_t)xprt->send_fn,
				buf, msg_flags, msg_type, data, data_len);
}

int __ldmsd_append_buffer_va(ldmsd_cfg_xprt_t xprt, struct ldmsd_msg_key *key,
				ldmsd_req_buf_t buf, int msg_flags,
				int msg_type, const char *fmt, va_list _ap)
{
	char *str = NULL;
	va_list ap;
	size_t cnt;

	va_copy(ap, _ap);
	cnt = vsnprintf(str, 0, fmt, ap);
	va_end(ap);
	str = malloc(cnt + 1);
	if (!str)
		return ENOMEM;
	va_copy(ap, _ap);
	cnt = vsnprintf(str, cnt + 1, fmt, ap);
	va_end(ap);
	return ldmsd_append_msg_buffer(xprt, xprt->max_msg, key,
					(ldmsd_msg_send_fn_t)xprt->send_fn,
					buf, msg_flags, msg_type, str, cnt);
}

int ldmsd_append_response_va(ldmsd_req_ctxt_t reqc, int msg_flags, const char *fmt, ...)
{
	int rc;
	va_list ap;
	va_start(ap, fmt);
	rc = __ldmsd_append_buffer_va(reqc->xprt, &reqc->key, reqc->send_buf,
					msg_flags, LDMSD_MSG_TYPE_RESP, fmt, ap);
	va_end(ap);
	return rc;
}

int ldmsd_append_response(ldmsd_req_ctxt_t reqc, int msg_flags,
				const char *data, size_t data_len)
{
	return __ldmsd_append_buffer(reqc->xprt, &reqc->key, reqc->send_buf,
			msg_flags, LDMSD_MSG_TYPE_RESP, data, data_len);
}

int ldmsd_append_request_va(ldmsd_req_ctxt_t reqc, int msg_flags, const char *fmt, ...)
{
	int rc;
	va_list ap;
	va_start(ap, fmt);
	rc = __ldmsd_append_buffer_va(reqc->xprt, &reqc->key, reqc->send_buf,
			msg_flags, LDMSD_MSG_TYPE_REQ, fmt, ap);
	va_end(ap);
	return rc;
}

int ldmsd_append_request(ldmsd_req_ctxt_t reqc, int msg_flags,
				const char *data, size_t data_len)
{
	return __ldmsd_append_buffer(reqc->xprt, &reqc->key, reqc->send_buf,
			msg_flags, LDMSD_MSG_TYPE_REQ, data, data_len);
}

int __send_error(ldmsd_cfg_xprt_t xprt, struct ldmsd_msg_key *key,
				ldmsd_req_buf_t buf, uint32_t errcode,
				const char *errmsg_fmt, va_list errmsg_ap)
{
	int rc;
	char errcode_s[16];
	char *str;

	str = "{\"type\":\"err_obj\",\"errcode\":";
	rc = __ldmsd_append_buffer(xprt, key, buf,
				LDMSD_REC_SOM_F, LDMSD_MSG_TYPE_RESP,
				str, strlen(str));
	if (rc)
		return rc;
	snprintf(errcode_s, 16, "%d", errcode);
	rc = __ldmsd_append_buffer(xprt, key, buf, 0, LDMSD_MSG_TYPE_RESP,
			errcode_s, strlen(errcode_s));
	if (rc)
		return rc;
	if (0 == errcode) {
		rc = __ldmsd_append_buffer(xprt, key, buf, LDMSD_REC_EOM_F,
				LDMSD_MSG_TYPE_RESP, "}", 1);
	} else {
		str = ",\"msg\":\"";
		rc = __ldmsd_append_buffer(xprt, key, buf, 0,
				LDMSD_MSG_TYPE_RESP, str, strlen(str));
		if (rc)
			return rc;
		rc = __ldmsd_append_buffer_va(xprt, key, buf, 0,
				LDMSD_MSG_TYPE_RESP, errmsg_fmt, errmsg_ap);
		if (rc)
			return rc;
		rc = __ldmsd_append_buffer(xprt, key, buf, LDMSD_REC_EOM_F,
				LDMSD_MSG_TYPE_RESP, "\"}", 2);
	}
	return rc;
}

/*
 * Any errors occur in any handler function must call \c ldmsd_send_error instead.
 *
 * if \c buf is NULL, the function will create its own buffer.
 *
 * Call the function only once to construct and send an error.
 */
int __ldmsd_send_error(ldmsd_cfg_xprt_t xprt, uint32_t msg_no,
				ldmsd_req_buf_t _buf, uint32_t errcode,
				char *errmsg_fmt, ...)
{
	va_list errmsg_ap;
	ldmsd_req_buf_t buf;
	struct ldmsd_msg_key key;
	int rc = 0;

	__msg_key_get(xprt, msg_no, &key);
	if (_buf) {
		buf = _buf;
	} else {
		buf = ldmsd_req_buf_alloc(xprt->max_msg);
		if (!buf)
			return ENOMEM;
	}

	va_start(errmsg_ap, errmsg_fmt);
	rc = __send_error(xprt, &key, buf, errcode, errmsg_fmt, errmsg_ap);
	if (!_buf)
		ldmsd_req_buf_free(buf);
	va_end(errmsg_ap);
	return rc;
}

/*
 * A convenient function that constructs and sends the error JSON object string.
 *
 * { "type": "error",
 *   "errcode": \c errcode,
 *   "msg": <string the same as the string printed by printf(errmsg_fmt, ...)>
 * }
 */

int ldmsd_send_error(ldmsd_req_ctxt_t reqc, uint32_t errcode, char *errmsg_fmt, ...)
{
	va_list errmsg_ap;
	int rc = 0;

	/*
	 * Clear any existing content in the send buffer.
	 */
	ldmsd_req_buf_reset(reqc->send_buf);
	va_start(errmsg_ap, errmsg_fmt);
	rc = __send_error(reqc->xprt, &reqc->key, reqc->send_buf, errcode,
					errmsg_fmt, errmsg_ap);
	va_end(errmsg_ap);
	return rc;
}

/*
 * A convenient function that constructs and sends the error response
 * caused by a required attribute is missing.
 */
int ldmsd_send_missing_attr_err(ldmsd_req_ctxt_t reqc,
					const char *obj_name,
					const char *missing_attr)
{
	int rc;

	rc = ldmsd_send_error(reqc, EINVAL, "'%s' is missing from the '%s' request.",
			missing_attr, obj_name);
	return rc;
}

int ldmsd_send_type_error(ldmsd_req_ctxt_t reqc, const char *obj_name,
							const char *type)
{
	return ldmsd_send_error(reqc, EINVAL, "Wrong JSON value type. %s must be %s.", obj_name, type);
}

int __ldmsd_send_missing_mandatory_attr(ldmsd_req_ctxt_t reqc,
					const char *obj_type,
					const char *missing)
{
	int rc;
	ldmsd_log(LDMSD_LERROR, "'%s' (%s) is missing from "
			"the message number %d:%" PRIu64,
			missing, obj_type,
			reqc->key.msg_no, reqc->key.conn_id);
	rc = ldmsd_send_error(reqc, EINVAL,
			"'%s' (%s) is missing from "
			"the message number %d:%" PRIu64,
			missing, obj_type,
			reqc->key.msg_no, reqc->key.conn_id);
	return rc;
}

int
ldmsd_send_err_rec_adv(ldmsd_cfg_xprt_t xprt, uint32_t msg_no, uint32_t rec_len)
{
	size_t cnt, hdr_sz, len;
	int rc;
	char *buf;
	ldmsd_rec_hdr_t hdr;

	hdr_sz = sizeof(struct ldmsd_rec_hdr_s);
	len = hdr_sz + 1024;

	buf = calloc(1, len);
	if (!buf)
		return ENOMEM;
	hdr = (ldmsd_rec_hdr_t)buf;
	cnt = snprintf(&buf[hdr_sz], len - hdr_sz, "{\"rsp\":\"rec_adv\","
						   " \"spec\": {\"errcode\":%d,"
						   "\"rec_len\":%d}}",
						   E2BIG, rec_len);
	if (cnt >= len - hdr_sz) {
		free(buf);
		buf = malloc(len + cnt);
		if (!buf)
			return ENOMEM;
	}
	cnt = snprintf(&buf[hdr_sz], len - hdr_sz, "{\"rsp\":\"rec_adv\","
						   " \"spec\": {\"errcode\":%d,"
						   "\"rec_len\":%d}}",
						   E2BIG, rec_len);
	hdr->msg_no = msg_no;
	hdr->rec_len = cnt + hdr_sz;
	hdr->type = LDMSD_MSG_TYPE_RESP;
	hdr->flags = LDMSD_REC_SOM_F | LDMSD_REC_EOM_F;
	ldmsd_hton_rec_hdr(hdr);
	rc = xprt->send_fn(xprt, (char *)hdr, cnt + hdr_sz);
	if (rc) {
		/* The content in reqc->rep_buf hasn't been sent. */
		ldmsd_log(LDMSD_LERROR, "failed to send the record length advice "
				"to the ldms xprt 0x%p\n", xprt->ldms.ldms);
		goto out;
	}
out:
	if (buf)
		free(buf);
	return rc;
}

ldmsd_req_ctxt_t ldmsd_handle_record(ldmsd_rec_hdr_t rec, ldmsd_cfg_xprt_t xprt)
{
	ldmsd_req_ctxt_t reqc = NULL;
	char *oom_errstr = "ldmsd out of memory";
	int rc = 0;
	int req_ctxt_type = LDMSD_REQ_CTXT_REQ;
	struct ldmsd_msg_key key;
	size_t data_len = rec->rec_len - sizeof(*rec);

	if (LDMSD_MSG_TYPE_RESP == rec->type)
		req_ctxt_type = LDMSD_REQ_CTXT_RSP;
	else
		req_ctxt_type = LDMSD_REQ_CTXT_REQ;

	__msg_key_get(xprt, req->msg_no, &key);
	ldmsd_req_ctxt_tree_lock(req_ctxt_type);

	reqc = find_req_ctxt(&key, req_ctxt_type);

	if (LDMSD_MSG_TYPE_RESP == rec->type) {
		/* Response messages */
		if (!reqc) {
			ldmsd_log(LDMSD_LERROR, "Cannot find the original request of "
					"a response number %d:%" PRIu64 "\n",
					rec->key.msg_no, rec->key.conn_id);
			rc = __ldmsd_send_error(xprt, &rec->key, NULL, ENOENT,
				"Cannot find the original request of "
				"a response number %d:%" PRIu64,
				rec->key.msg_no, rec->key.conn_id);
			if (rc == ENOMEM)
				goto oom;
			else
				goto err;
		}
	} else {
		/* request & stream messages */
		if (rec->flags & LDMSD_REC_SOM_F) {
			if (reqc) {
				rc = ldmsd_send_error(reqc, EADDRINUSE,
					"Duplicate message number %d:%" PRIu64 "received",
					key.msg_no, key.conn_id);
				if (rc == ENOMEM)
					goto oom;
				else
					goto err;
			}
			reqc = __req_ctxt_alloc(&key, xprt, LDMSD_REQ_CTXT_REQ);
			if (!reqc)
				goto oom;
		} else {
			if (!reqc) {
				rc = __ldmsd_send_error(xprt, &rec->key, NULL, ENOENT,
						"The message no %" PRIu32
						" was not found.", key.msg_no);
				ldmsd_log(LDMSD_LERROR, "The message no %" PRIu32 ":%" PRIu64
						" was not found.\n",
						key.msg_no, key.conn_id);
				goto err;
			}
		}
	}

	if (reqc->recv_buf->len - reqc->recv_buf->off < data_len) {
		reqc->recv_buf = ldmsd_req_buf_realloc(reqc->recv_buf,
					2 * (reqc->recv_buf->off + data_len));
		if (!reqc->recv_buf)
			goto oom;
	}
	memcpy(&reqc->recv_buf->buf[reqc->recv_buf->off], (char *)(rec + 1), data_len);
	reqc->recv_buf->off += data_len;

	ldmsd_req_ctxt_tree_unlock(req_ctxt_type);

	if (!(rec->flags & LDMSD_REC_EOM_F)) {
		/*
		 * LDMSD hasn't received the whole message.
		 */
		return NULL;
	}
	return reqc;

oom:
	rc = ENOMEM;
	ldmsd_log(LDMSD_LCRITICAL, "%s\n", oom_errstr);
err:
	errno = rc;
	ldmsd_req_ctxt_tree_unlock(req_ctxt_type);
	if (reqc)
		ldmsd_req_ctxt_free(reqc);
	return NULL;
}

extern void cleanup(int x, char *reason);
int ldmsd_process_msg_request(ldmsd_req_ctxt_t reqc)
{
	json_parser_t parser;
	int rc;
	char *str_repl;

	/* Replace environment variables */
	str_repl = str_repl_env_vars(reqc->recv_buf->buf);
	if (!str_repl) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return ENOMEM;
	}
	free(reqc->recv_buf->buf);
	reqc->recv_buf->buf = str_repl;
	reqc->recv_buf->len = reqc->recv_buf->off = strlen(reqc->recv_buf->buf) + 1;

	parser = json_parser_new(0);
	if (!parser) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return ENOMEM;
	}


	rc = json_parse_buffer(parser, reqc->recv_buf->buf,
			reqc->recv_buf->off, &reqc->json);
	json_parser_free(parser);
	if (rc) {
		ldmsd_log(LDMSD_LCRITICAL, "Failed to parse a JSON object string\n");
		ldmsd_send_error(reqc, rc, "Failed to parse a JSON object string");
		return rc;
	}

	rc = ldmsd_process_json_obj(reqc);

	if (cleanup_requested)
		cleanup(0, "user quit");
	return rc;
}

int ldmsd_process_msg_response(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_parser_t parser;
	char *str_repl;

	/* Replace environment variables */
	str_repl = str_repl_env_vars(reqc->recv_buf->buf);
	if (!str_repl) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return ENOMEM;
	}
	free(reqc->recv_buf->buf);
	reqc->recv_buf->buf = str_repl;
	reqc->recv_buf->len = reqc->recv_buf->off = strlen(reqc->recv_buf->buf) + 1;

	parser = json_parser_new(0);
	if (!parser) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return ENOMEM;
	}

	rc = json_parse_buffer(parser, reqc->recv_buf->buf,
			reqc->recv_buf->off, &reqc->json);
	json_parser_free(parser);
	if (rc) {
		ldmsd_log(LDMSD_LCRITICAL, "Failed to parse a JSON object string\n");
		ldmsd_send_error(reqc, rc, "Failed to parse a JSON object string");
		return rc;
	}

	if (reqc->resp_handler) {
		rc = reqc->resp_handler(reqc);
	} else {
		rc = ldmsd_process_json_obj(reqc);
	}

	return rc;
}

int ldmsd_process_msg_stream(ldmsd_req_ctxt_t reqc)
{
	size_t offset = 0;
	int rc = 0;
	char *stream_name, *data;
	enum ldmsd_stream_type_e stream_type;
	json_entity_t entity = NULL;
	json_parser_t p = NULL;


	__ldmsd_stream_extract_hdr(reqc->recv_buf->buf, &stream_name,
					&stream_type, &data, &offset);

	if (LDMSD_STREAM_JSON == stream_type) {
		p = json_parser_new(0);
		if (!p) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		}
		rc = json_parse_buffer(p, data,
				reqc->recv_buf->off - offset, &entity);
		if (rc) {
			ldmsd_log(LDMSD_LERROR, "Failed to parse a JSON stream '%s'.\n",
					stream_name);
			goto out;
		}
	}

	ldmsd_stream_deliver(stream_name, stream_type, data,
					reqc->recv_buf->off - offset, entity);
out:
	if (p)
		json_parser_free(p);
	if (entity)
		json_entity_free(entity);
	ldmsd_req_ctxt_free(reqc);
	/* Not sending any response back to the publisher */
	return rc;
}

int ldmsd_process_err_obj(ldmsd_req_ctxt_t reqc)
{
	json_entity_t errcode, msg;
	char *s;

	errcode = json_value_find(reqc->json, "errcode");
	if (!errcode) {
		ldmsd_log(LDMSD_LERROR, "Msg ID %d:%" PRIu64 ": "
				"The response err_obj has no errcode.\n",
				reqc->key.msg_no, reqc->key.conn_id);
		return 0;
	}
	if (json_value_int(errcode) != 0) {
		msg = json_value_find(reqc->json, "msg");
		if (msg)
			s = json_value_str(msg)->str;
		else
			s = NULL;
		ldmsd_log(LDMSD_LERROR, "Msg ID %d:%" PRIu64 ": received "
				"an error response '%" PRIu64 "': '%s'.\n",
				reqc->key.msg_no, reqc->key.conn_id,
				json_value_int(errcode), s);
	}
	ldmsd_req_ctxt_free(reqc);
	return 0;
}

int process_unexpected_info_obj(ldmsd_req_ctxt_t reqc)
{
	json_entity_t name;

	name = json_value_find(reqc->json, "name");
	if (!name || (JSON_STRING_VALUE != json_entity_type(name))) {
		ldmsd_log(LDMSD_LERROR, "Received an unexpected info object without a name.\n");
	} else {
		ldmsd_log(LDMSD_LERROR, "Received an unexpected info object '%s'\n",
							json_value_str(name)->str);
	}
	__dlog("%s\n", reqc->recv_buf->buf);
	ldmsd_req_ctxt_free(reqc);
	return 0;
}

int ldmsd_process_cfg_obj(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t cfg_obj, spec, op;
	struct obj_handler_entry *handler;
	char *cfg_obj_s, *op_s;

	cfg_obj = json_value_find(reqc->json, "cfg_obj");
	if (!cfg_obj) {
		rc = __ldmsd_send_missing_mandatory_attr(reqc, "cfg_obj", "cfg_obj");
		return rc;
	}

	if (JSON_STRING_VALUE != json_entity_type(cfg_obj)) {
		rc = ldmsd_send_error(reqc, EINVAL, "cfg_obj:cfg_obj must be a JSON string.");
		return rc;
	}

	cfg_obj_s = json_value_str(cfg_obj)->str;
	spec = json_value_find(reqc->json, "spec");
	if (!spec) {
		rc = __ldmsd_send_missing_mandatory_attr(reqc, "cfg_obj", "spec");
		return rc;
	}
	op = json_value_find(reqc->json, "op");
	if (op) {
		if (JSON_STRING_VALUE != json_entity_type(op)) {
			rc = ldmsd_send_error(reqc, EINVAL, "cfg_obj:op must be a string.");
			return rc;
		}
		op_s = json_value_str(op)->str;
		if ((0 != strncmp("create", op_s, 6)) && (0 != strncmp("update", op_s, 6))) {
			rc = ldmsd_send_error(reqc, EINVAL, "cfg_obj:op invalid value (%s)", op_s);
			return rc;
		}
	}

	handler = bsearch(&cfg_obj_s, cfg_obj_handler_tbl,
			ARRAY_SIZE(cfg_obj_handler_tbl),
			sizeof(*handler), handler_entry_comp);
	if (!handler) {
		ldmsd_log(LDMSD_LERROR, "Received an unrecognized "
				"configuration object '%s'\n", cfg_obj_s);
		rc = ldmsd_send_error(reqc, ENOTSUP, "Received an unrecognized "
				"configuration object '%s'\n", cfg_obj_s);
		return rc;
	}

	rc = handler->handler(reqc);
	ldmsd_req_ctxt_free(reqc);
	return rc;
}

int ldmsd_process_cmd_obj(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t cmd, spec;
	struct obj_handler_entry *handler;
	char *cmd_s;

	cmd = json_value_find(reqc->json, "cmd");
	if (!cmd) {
		rc = __ldmsd_send_missing_mandatory_attr(reqc, "cmd_obj", "cmd");
		return rc;
	}
	if (JSON_STRING_VALUE != json_entity_type(cmd)) {
		rc = ldmsd_send_error(reqc, EINVAL, "cmd_obj:cmd must be a JSON string.");
		return rc;
	}
	cmd_s = json_value_str(cmd)->str;
	spec = json_value_find(reqc->json, "spec");
	if (spec && (JSON_DICT_VALUE != json_entity_type(spec))) {
		rc = ldmsd_send_type_error(reqc, "cmd_obj:spec", "a dictionary");
		return rc;
	}

	handler = bsearch(&cmd_s, cmd_obj_handler_tbl,
			ARRAY_SIZE(cmd_obj_handler_tbl),
			sizeof(*handler), handler_entry_comp);
	if (!handler) {
		ldmsd_log(LDMSD_LERROR, "Received an unrecognized "
				"command object '%s'\n", cmd_s);
		rc = ldmsd_send_error(reqc, ENOTSUP, "Received an unrecognized "
				"command object '%s'\n", cmd_s);
		return rc;
	}

	rc = handler->handler(reqc);
	ldmsd_req_ctxt_free(reqc);
	return rc;
}

int ldmsd_process_act_obj(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t value, names, regex;
	char *action_s, *cfgobj_s;
	struct obj_handler_entry *handler;
	size_t len;

	/* cfg_obj */
	value = json_value_find(reqc->json, "cfg_obj");
	if (!value) {
		rc = __ldmsd_send_missing_mandatory_attr(reqc,
				"act_obj", "cfg_obj");
		return rc;
	}
	if (JSON_STRING_VALUE != json_entity_type(value)) {
		rc = ldmsd_send_type_error(reqc,
				"act_obj:cfg_obj", "a string");
		return rc;
	}
	cfgobj_s = json_value_str(value)->str;

	/* names */
	names = json_value_find(reqc->json, "names");
	if (names && (JSON_LIST_VALUE != json_entity_type(names))) {
		rc = ldmsd_send_type_error(reqc, "act_obj:names", "a list");
		return rc;
	}

	/* regex */
	regex = json_value_find(reqc->json, "regex");
	if (regex && (JSON_LIST_VALUE != json_entity_type(regex))) {
		rc = ldmsd_send_type_error(reqc, "act_obj:regex", "a list");
		return rc;
	}
	if (!names && !regex) {
		rc = ldmsd_send_error(reqc, EINVAL,
			"act_obj: Either 'names' or 'regex' must be given.");
		return rc;
	}

	/* action */
	value = json_value_find(reqc->json, "action");
	if (!value) {
		rc = __ldmsd_send_missing_mandatory_attr(reqc, "act_obj", "action");
		return rc;
	}
	if (JSON_STRING_VALUE != json_entity_type(value)) {
		rc = ldmsd_send_type_error(reqc, "act_obj:action", "a string");
		return rc;
	}
	action_s = json_value_str(value)->str;
	len = strlen(action_s);

	if ((0 != strncmp(action_s, "start", len)) &&
			(0 != strncmp(action_s, "stop", len)) &&
			(0 != strncmp(action_s, "delete", len))) {
		rc = ldmsd_send_error(reqc, EINVAL,
				"Unrecognized act_obj:action '%s'", action_s);
		return rc;
	}

	if (!ldmsd_is_initialized()) {
		/*
		 * Do not process the action object
		 * before LDMSD is initialized.
		 */
		return 0;
	}

	handler = bsearch(&cfgobj_s, act_obj_handler_tbl,
			ARRAY_SIZE(act_obj_handler_tbl),
			sizeof(*handler), handler_entry_comp);
	if (!handler) {
		ldmsd_log(LDMSD_LERROR, "Received an unsupported "
					"configuration object "
					"by action objects'%s'\n",
					cfgobj_s);
		rc = ldmsd_send_error(reqc, ENOTSUP,
				"Received an unsupported "
				"configuration object "
				"by action objects'%s'\n",
				cfgobj_s);
		return rc;
	}

	rc = handler->handler(reqc);
//	ldmsd_req_ctxt_free(reqc);
	return rc;

}

char __get_opt(char *name)
{
	if (0 == strncmp("banner", name, strlen(name)))
		return 'B';
	else if (0 == strncmp("daemon-name", name, strlen(name)))
		return 'n';
	else if (0 == strncmp("default-auth:plugin", name, strlen(name)))
		return 'a';
	else if (0 == strncmp("default-auth:args", name, strlen(name)))
		return 'A';
	else if (0 == strncmp("hostname", name, strlen(name)))
		return 'H';
	else if (0 == strncmp("log:dst", name, strlen(name)))
		return 'l';
	else if (0 ==strncmp("log:level", name, strlen(name)))
		return 'v';
	else if (0 == strncmp("pidfile", name, strlen(name)))
		return 'r';
	else if (0 == strncmp("kernel:publish", name, strlen(name)))
		return 'k';
	else if (0 == strncmp("kernel:file", name, strlen(name)))
		return 's';
	else if (0 == strncmp("mem", name, strlen(name)))
		return 'm';
	else if (0 == strncmp("workers", name, strlen(name)))
		return 'P';
	else {
		return '-';
	}
}

static inline int __daemon_type_error(ldmsd_req_ctxt_t reqc,
					const char *name, const char *type)
{
	char n[512];
	snprintf(n, 512, "daemon:%s", name);
	return ldmsd_send_type_error(reqc, n, type);
}

static inline int __daemon_proc_error(ldmsd_req_ctxt_t reqc,
					const char *name, int errcode)
{
	return ldmsd_send_error(reqc, errcode, "daemon:%s caused an error.", name);
}

static int __daemon_default_auth(ldmsd_req_ctxt_t reqc, json_entity_t auth)
{
	char *name;
	json_entity_t value, attr;
	json_str_t n, av;
	char *v;
	char tmp[512];
	int rc;

	if (JSON_DICT_VALUE != json_entity_type(auth))
		return __daemon_type_error(reqc, "default-auth", "a dictionary");

	/* default-auth:plugin */
	name = "default-auth:plugin";
	value = json_value_find(auth, "plugin");
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value))
			return __daemon_type_error(reqc, name, "a string");
		v = json_value_str(value)->str;
		rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
		if (rc)
			return __daemon_proc_error(reqc, name, rc);

		/* default-auth:args */
		name = "default-auth:args";
		value = json_value_find(auth, "args");
		if (!value)
			goto out;

		if (JSON_DICT_VALUE != json_entity_type(value))
			return __daemon_type_error(reqc, name, "a dictionary");
		for (attr = json_attr_first(value); attr; attr = json_attr_next(attr)) {
			n = json_attr_name(attr);
			value = json_attr_value(attr);
			if (JSON_STRING_VALUE != json_entity_type(value)) {
				snprintf(tmp, 512, "%s:%s", name, n->str);
				return __daemon_type_error(reqc, tmp, "a string");
			}
			av = json_value_str(value);

			snprintf(tmp, 512, "%s=%s", n->str, av->str);
			rc = ldmsd_process_cmd_line_arg(__get_opt(name), tmp);
			if (rc) {
				snprintf(tmp, 512, "%s:%s=%s", name, n->str, av->str);
				return __daemon_proc_error(reqc, tmp, rc);
			}
		}
	}
out:
	return 0;
}

static int __daemon_log(ldmsd_req_ctxt_t reqc, json_entity_t dict)
{
	json_entity_t value;
	char *name, *v;
	int rc;

	if (JSON_DICT_VALUE != json_entity_type(dict))
		return __daemon_type_error(reqc, "log", "a dictionary");

	/* log:dst */
	name = "log:dst";
	value = json_value_find(dict, "dst");
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value))
			return __daemon_type_error(reqc, name, "a string");

		v = json_value_str(value)->str;
		rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
		if (rc)
			return __daemon_proc_error(reqc, name, rc);
	}

	/* log:level */
	name = "log:level";
	value = json_value_find(dict, "level");
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value))
			return __daemon_type_error(reqc, name, "a string");
		v = json_value_str(value)->str;
		rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
		if (rc)
			return __daemon_proc_error(reqc, name, rc);
	}
	return 0;
}

static int __daemon_kernel(ldmsd_req_ctxt_t reqc, json_entity_t kernel)
{
	json_entity_t value;
	char *v, *name;
	int rc;

	if (JSON_DICT_VALUE != json_entity_type(kernel))
		return __daemon_type_error(reqc, "kernel", "a dictionary");

	/* kernel:publish */
	name = "kernek:publish";
	value = json_value_find(kernel, name);
	if (value) {
		if (JSON_BOOL_VALUE != json_entity_type(value))
			return __daemon_type_error(reqc, name, "true/false");
		if (!json_value_bool(value))
			goto out;

		rc = ldmsd_process_cmd_line_arg(__get_opt(name), "true");
		if (rc)
			return __daemon_proc_error(reqc, name, rc);
		/* kernel:file */
		name = "kernel:file";
		value = json_value_find(kernel, "file");
		if (value) {
			if (JSON_STRING_VALUE != json_entity_type(value))
				return __daemon_type_error(reqc, name, "a string");
			v = json_value_str(value)->str;
			rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
			if (rc)
				return __daemon_proc_error(reqc, name, rc);
		}
	}
out:
	return 0;
}

static int ldmsd_process_daemon_obj(ldmsd_req_ctxt_t reqc)
{
	json_entity_t spec, value;
	char *v, *name, *type;
	char buf[512];
	int rc = 0;

	if (ldmsd_is_initialized()) {
		/*
		 * No changes to command-line options are allowed
		 * after LDMSD is initialized.
		 *
		 * The only exception is loglevel which can be changed
		 * using loglevel command.
		 *
		 */
		rc = ldmsd_send_error(reqc, EPERM, "LDMSD is already initialized."
				"The daemon configuration cannot be altered.");
		return rc;
	}

	spec = json_value_find(reqc->json, "spec");

	/* log */
	name = "log";
	value = json_value_find(spec, name);
	if (value) {
		rc = __daemon_log(reqc, value);
		if (rc)
			return rc;
	}

	/* Default authentication */
	name = "default-auth";
	value = json_value_find(spec, name);
	if (value) {
		rc = __daemon_default_auth(reqc, value);
		if (rc)
			return rc;
	}

	/* memory */
	name = "mem";
	value = json_value_find(spec, name);
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value))
			return __daemon_type_error(reqc, name, "a string");
		v = json_value_str(value)->str;
		rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
		if (rc)
			return __daemon_proc_error(reqc, name, rc);
	}

	/* workers */
	name = "workers";
	value = json_value_find(spec, name);
	if (value) {
		if (JSON_INT_VALUE == json_entity_type(value)) {
			snprintf(buf, 512, "%" PRIu64, json_value_int(value));
			v = buf;
		} else if (JSON_STRING_VALUE != json_entity_type(value)) {
			type = "a string or an integer";
			return __daemon_type_error(reqc, name, type);
		} else {
			v = json_value_str(value)->str;
		}
		rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
		if (rc)
			return __daemon_proc_error(reqc, name, rc);
	}

	/* pidfile */
	name = "pidfile";
	value = json_value_find(spec, name);
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value))
			return __daemon_type_error(reqc, name, "a string");
		v = json_value_str(value)->str;
		rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
		if (rc)
			return __daemon_proc_error(reqc, name, rc);
	}

	/* banner */
	name = "banner";
	value = json_value_find(spec, name);
	if (value) {
		if (JSON_BOOL_VALUE == json_entity_type(value)) {
			v = json_value_bool(value)?"1":"0";
		} else if (JSON_INT_VALUE == json_entity_type(value)) {
			v = json_value_int(value)?"1":"0";
		} else {
			return __daemon_type_error(reqc, name, "true/false or 0/1");
		}
		rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
		if (rc)
			return __daemon_proc_error(reqc, name, rc);
	}

	/* hostname */
	name = "hostname";
	value = json_value_find(spec, name);
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value))
			return __daemon_type_error(reqc, name, "a string");
		v = json_value_str(value)->str;
		rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
		if (rc)
			return __daemon_proc_error(reqc, name, rc);
	}

	/* daemon-name */
	name = "daemon-name";
	value = json_value_find(spec, name);
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value))
			return __daemon_type_error(reqc, name, "a string");
		v = json_value_str(value)->str;
		rc = ldmsd_process_cmd_line_arg(__get_opt(name), v);
		if (rc)
			return __daemon_proc_error(reqc, name, rc);
	}

	/* kernel */
	value = json_value_find(spec, "kernel");
	if (value) {
		rc = __daemon_kernel(reqc, value);
		if (rc)
			return rc;
	}

	return 0;
}

int ldmsd_process_json_obj(ldmsd_req_ctxt_t reqc)
{
	json_entity_t type;
	char *type_s;
	int rc = 0;

	type = json_value_find(reqc->json, "type");
	if (!type) {
		ldmsd_log(LDMSD_LERROR, "The 'type' attribute is missing from "
				"message number %d:%" PRIu64 "\n",
				reqc->lcl_key.msg_no, reqc->lcl_key.conn_id);
		rc = ldmsd_send_error(reqc, EINVAL,
				"The 'type' attribute is missing from "
				"message number %d:%" PRIu64 "\n",
				reqc->rem_key.msg_no, reqc->rem_key.conn_id);
		goto out;
	}
	if (JSON_STRING_VALUE != json_entity_type(type)) {
		ldmsd_log(LDMSD_LERROR, "message number %d:%" PRIu64
				": The 'type' attribute is not a string.\n",
				reqc->lcl_key.msg_no, reqc->lcl_key.conn_id);
		rc = ldmsd_send_error(reqc, EINVAL, "message number %d:%" PRIu64
				": The 'type' attribute is not a string.",
				reqc->rem_key.msg_no, reqc->rem_key.conn_id);
		goto out;;
	}
	type_s = json_value_str(type)->str;
	if (0 == strncmp("cfg_obj", type_s, 7)) {
		rc = ldmsd_process_cfg_obj(reqc);
	} else if (0 == strncmp("act_obj", type_s, 7)) {
		rc = ldmsd_process_act_obj(reqc);
	} else if (0 == strncmp("cmd_obj", type_s, 7)) {
		rc = ldmsd_process_cmd_obj(reqc);
	} else if (0 == strncmp("daemon_obj", type_s, 10)) {
		rc = ldmsd_process_daemon_obj(reqc);
	} else if (0 == strncmp("err_obj", type_s, 7)) {
		rc = ldmsd_process_err_obj(reqc);
	} else if (0 == strncmp("info_obj", type_s, 7)) {
		rc = process_unexpected_info_obj(reqc);
	} else {
		ldmsd_log(LDMSD_LERROR, "Message number %d:%" PRIu64
				"has an unrecognized object type '%s'\n",
				reqc->lcl_key.msg_no, reqc->lcl_key.conn_id, type_s);
		rc = ldmsd_send_error(reqc, ENOTSUP, "message number %d:%" PRIu64
				" has ba unrecognized object type '%s'.",
				reqc->rem_key.msg_no, reqc->rem_key.conn_id, type_s);
	}
out:
	return rc;
}

int ldmsd_append_info_obj_hdr(ldmsd_req_ctxt_t reqc, const char *info_name)
{
	return ldmsd_append_response_va(reqc, LDMSD_REC_SOM_F,
					"{\"type\":\"info\","
					" \"name\":\"%s\","
					" \"info\":", info_name);
}

int ldmsd_append_cmd_obj_hdr(ldmsd_req_ctxt_t reqc, const char *cmd_name)
{
	return ldmsd_append_request_va(reqc, LDMSD_REC_SOM_F,
					"{\"type\":\"cmd_obj\","
					"\"cmd\":\"%s\"", cmd_name);
}

int __find_perm(ldmsd_req_ctxt_t reqc, json_entity_t spec, int *perm_value)
{
	int rc;
	json_entity_t perm;
	perm = json_value_find(spec, "perm");
	if (perm) {
		if (JSON_STRING_VALUE != json_entity_type(perm)) {
			rc = ldmsd_send_type_error(reqc, "smplr:perm", "a string");
			return rc;
		}
		*perm_value = strtol(json_value_str(perm)->str, NULL, 0);
	}
	return 0;
}

/*
 * This handler is an example of how to get the received attributes through
 * the 'spec' dictionary from reqc->json. It also an example of how to
 * send a rsp_obj back to the sender by constructing a rsp_obj JSON-formatted string.
 *
 * What the handler does, for example,
 *   the received 'spec' dictionary is
 *
 *   { "attr1": "A",
 *     "attr2": "B"
 *   }
 *
 *   It constructs the rsp_obj JSON-formatted string and sends it back
 *   by calling the convenient function \c ldmsd_append_rsp_obj.
 *
 *   { "rsp":"example","spec":{"attr1":"A","attr2":"B"}}
 *
 */
static int example_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	char *str, attr_str[1024];
	size_t cnt;
	json_entity_t recv_attrs, attr;
	json_str_t name, value;
	int attr_num = 0;

	recv_attrs = json_value_find(reqc->json, "spec");

	rc = ldmsd_append_info_obj_hdr(reqc, "example");
	if (rc)
		return rc;
	rc = ldmsd_append_response(reqc, 0, "{", 1);
	if (rc)
		goto out;
	for (attr = json_attr_first(recv_attrs); attr; attr = json_attr_next(attr)) {

		if (attr_num > 0) {
			/* Append the comma that separates two attributes in the dictionary */
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				goto out;
		}

		name = json_attr_name(attr);
		value = json_value_str(json_attr_value(attr));

		/*
		 * Construct the string
		 * "attr_name":"attr_value"
		 */
		cnt = snprintf(attr_str, 1024, "\"%s\":\"%s\"", name->str, value->str);

		/* Append to the rsp obj */
		rc = ldmsd_append_response(reqc, 0, attr_str, cnt);
		if (rc)
			goto out;
		attr_num++;
	}

	str = "}}"; /* The close parentheses of the 'spec' dict and the rsp_obj dict */
	rc = ldmsd_append_response(reqc, LDMSD_REC_EOM_F, str, strlen(str));
out:
	return rc;
}

static int
__prdcr_handler(ldmsd_req_ctxt_t reqc, json_entity_t spec, json_entity_t inst_ele,
		enum ldmsd_prdcr_type type, char *xprt, char *auth,
		unsigned long interval_us, json_entity_t streams, int perm)
{
	int rc;
	json_entity_t value;
	char *name, *host;
	unsigned short port_no;
	ldmsd_prdcr_t prdcr;
	char *port_s;

	/* host */
	value = json_value_find(spec, "host");
	if (!value) {
		if (inst_ele) {
			/*
			 * Not found in spec look at the instance element.
			 */
			value = json_value_find(inst_ele, "host");
		}
		if (!value) {
			rc = ldmsd_send_missing_attr_err(reqc,
					"prdcr:spec/instances[]", "host");
			return rc;
		}
	}
	host = json_value_str(value)->str;

	/* port */
	value = json_value_find(spec, "port");
	if (!value) {
		if (inst_ele)
			value = json_value_find(inst_ele, "port");
		if (!value) {
			rc = ldmsd_send_missing_attr_err(reqc,
					"prdcr:spec/instances[]", "port");
			return rc;
		}
	}
	if (JSON_STRING_VALUE == json_entity_type(value)) {
		port_s = json_value_str(value)->str;
		port_no = atoi(port_s);
	} else if (JSON_INT_VALUE == json_entity_type(value)) {
		port_no = (int)json_value_int(value);
	} else {
		rc = ldmsd_send_type_error(reqc, "prdcr:spec/instances[0]:port",
				"a string or an integer");
		return rc;
	}

	if (port_no < 1 || port_no > USHRT_MAX) {
		rc = ldmsd_send_error(reqc, EINVAL,
				"'%s' transport with invalid port '%s'",
				xprt, port_s);
		return rc;
	}

	/* name */
	/*
	 * 'name' must be in either 'spec' or an instances element.
	 */
	if (!inst_ele)
		inst_ele = spec;
	value = json_value_find(inst_ele, "name");
	if (!value) {
		rc = ldmsd_send_missing_attr_err(reqc,
				"prdcr:spec/instances[]", "name");
		return rc;
	}
	name = json_value_str(value)->str;

	struct ldmsd_sec_ctxt sctxt;
	ldmsd_req_ctxt_sec_get(reqc, &sctxt);
	uid_t uid = sctxt.crd.uid;
	gid_t gid = sctxt.crd.gid;

	prdcr = ldmsd_prdcr_new_with_auth(name, xprt, host, port_no, type,
					  interval_us, auth, uid, gid, perm);
	if (!prdcr) {
		if (errno == EEXIST) {
			rc = ldmsd_send_error(reqc, EEXIST,
					"Producer '%s' already exists.", name);
			return rc;
		} else if (errno == EAFNOSUPPORT) {
			rc = ldmsd_send_error(reqc, EAFNOSUPPORT,
					"Error resolving hostname '%s'", host);
			return rc;
		} else if (errno == ENOENT) {
			rc = ldmsd_send_error(reqc, ENOENT,
					"prdcr '%s': unrecognized "
					"authentication '%s'", auth);
			return rc;
		} else if (errno == ENOMEM){
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		} else {
			rc = ldmsd_send_error(reqc, errno,
					"Failed to create producer '%s', error %d",
					name, errno);
			return rc;

		}
	}

	if (streams) {
		for (value = json_item_first(streams); value; value = json_item_next(value)) {
			if (JSON_STRING_VALUE != json_entity_type(value)) {
				rc = ldmsd_send_type_error(reqc,
					"cfg_obj:prdcr:spec:streams[]", "a string");
				ldmsd_prdcr_put(prdcr); /* Put 'create' ref */
				return rc;
			}
			rc = ldmsd_prdcr_subscribe(prdcr, json_value_str(value)->str);
			if (rc) {
				rc = ldmsd_send_error(reqc, rc, "cfg_obj:prdcr - "
						"Producer '%s' failed to subscribe "
						"stream '%s'.", name,
						json_value_str(value)->str);
				ldmsd_prdcr_put(prdcr);
				return rc;
			}
		}
	}

	return 0;

}

static int prdcr_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t spec, value, inst, ele, streams;
	char *type_s, *xprt, *auth;
	enum ldmsd_prdcr_type type;
	unsigned long interval_us;
	int perm;

	spec = json_value_find(reqc->json, "spec");

	/* type */
	value = json_value_find(spec, "type");
	if (!value) {
		type = LDMSD_PRDCR_TYPE_ACTIVE;
	} else {
		if (JSON_STRING_VALUE != json_entity_type(value)) {
			rc = ldmsd_send_type_error(reqc, "prdcr:spec:type", "a string");
			return rc;
		}
		type_s = json_value_str(value)->str;
		type = ldmsd_prdcr_str2type(type_s);
		if ((int)type < 0) {
			rc = ldmsd_send_error(reqc, EINVAL,
					"prdcr: type '%s' is invalid.",
					type_s);
			return rc;
		}
		if (type == LDMSD_PRDCR_TYPE_LOCAL) {
			rc = ldmsd_send_error(reqc, ENOTSUP,
					"prdcr: type 'local' is "
					"not supported.");
			return rc;
		}
	}

	/* xprt */
	value = json_value_find(spec, "xprt");
	if (!value) {
		rc = ldmsd_send_missing_attr_err(reqc, "prdcr:spec", "xprt");
		return rc;
	}
	xprt = json_value_str(value)->str;

	/* re-connect interval */
	value = json_value_find(spec, "interval");
	if (!value) {
		rc = ldmsd_send_missing_attr_err(reqc, "prdcr:spec", "interval");
		return rc;
	}
	if (JSON_STRING_VALUE == json_entity_type(value)) {
		interval_us = ldmsd_time_str2us(json_value_str(value)->str);
	} else if (JSON_INT_VALUE == json_entity_type(value)) {
		interval_us = json_value_int(value);
	} else {
		rc = ldmsd_send_type_error(reqc, "prdcr:spec:interval",
				"a string or an integer");
		return rc;
	}

	/* authentication */
	value = json_value_find(spec, "auth");
	if (!value) {
		auth = DEFAULT_AUTH;
	} else {
		if (JSON_STRING_VALUE != json_entity_type(value)) {
			rc = ldmsd_send_type_error(reqc, "prdcr;spec:auth", "a string");
			return rc;
		}
		auth = json_value_str(value)->str;
	}

	/* streams */
	streams = json_value_find(spec, "streams");

	/* Permission */
	perm = 0770;
	rc = __find_perm(reqc, spec, &perm);
	if (rc)
		return rc;

	/* instance list */
	inst = json_value_find(reqc->json, "instances");
	if (!inst) {
		rc = __prdcr_handler(reqc, spec, NULL, type, xprt, auth,
				interval_us, streams, perm);
		if (rc)
			return rc;
	} else {
		for (ele = json_item_first(inst); ele; ele = json_item_next(ele)) {
			if (JSON_DICT_VALUE != json_entity_type(ele)) {
				rc = ldmsd_send_type_error(reqc,
						"prdcr:instances[]", "a dictionary");
				return rc;
			}
			rc = __prdcr_handler(reqc, spec, ele, type, xprt,
					auth, interval_us, streams, perm);
			if (rc)
				return rc;
		}
	}

	rc = ldmsd_send_error(reqc, 0, NULL);
	return rc;
}

static int prdcr_action_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t action, names, name, regex;
	char *action_s, *name_s;
	struct ldmsd_sec_ctxt sctxt;

	ldmsd_req_ctxt_sec_get(reqc, &sctxt);

	action = json_value_find(reqc->json, "action");
	action_s = json_value_str(action)->str;
	names = json_value_find(reqc->json, "names");
	regex = json_value_find(reqc->json, "regex");

	if (0 == strncmp(action_s, "start", 5)) {
		if (!names)
			goto start_regex;
		for (name = json_item_first(names); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_prdcr_start(name_s, NULL, &sctxt, 0);
			switch (rc) {
			case 0:
				break;
			case EBUSY:
				return ldmsd_send_error(reqc, rc,
					"The producer '%s' is already running.",
									name_s);
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"The producer '%s' does not exist.", name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"The producer '%s': permission denied", name_s);
			default:
				return ldmsd_send_error(reqc, rc,
					"Failed to start the producer '%s': %s",
					name_s, ovis_errno_abbvr(rc));
			}
		}
	start_regex:
		if (!regex)
			goto out;
		for (name = json_item_first(regex); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			ldmsd_req_buf_reset(reqc->recv_buf);
			rc = ldmsd_prdcr_start_regex(name_s, 0, reqc->recv_buf->buf,
					reqc->recv_buf->len, &sctxt, 0);
			if (rc) {
				return ldmsd_send_error(reqc, rc,
					"Failed to start producers with regex '%s'. %s",
					name_s, reqc->recv_buf->buf);

			}
		}
	} else if (0 == strncmp(action_s, "stop", 4)) {
		/* names */
		if (!names)
			goto stop_regex;
		for (name = json_item_first(names); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_prdcr_stop(name_s, &sctxt);
			switch (rc) {
			case 0:
				break;
			case EBUSY:
				return ldmsd_send_error(reqc, rc,
					"The producer '%s' is already stopped.",
					name_s);
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"The producer '%s' does not exist.", name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"Permission denied to access "
					"the producer '%s'.", name_s);
			default:
				return ldmsd_send_error(reqc, rc,
					"Failed to stop the producer '%s': %s",
					name_s, ovis_errno_abbvr(rc));
			}
		}
		/* regex */
	stop_regex:
		if (!regex)
			goto out;
		for (name = json_item_first(regex); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			ldmsd_req_buf_reset(reqc->recv_buf);
			rc = ldmsd_prdcr_stop_regex(name_s, reqc->recv_buf->buf,
					reqc->recv_buf->len, &sctxt);
			if (rc) {
				return ldmsd_send_error(reqc, rc,
					"Failed to stop producers with regex '%s'. %s",
					name_s, reqc->recv_buf->buf);

			}
		}
	} else {
		for (name = json_item_first(names); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_prdcr_del(name_s, &sctxt);
			switch (rc) {
			case 0:
				break;
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"The producer '%s' does not exist.", name_s);
			case EBUSY:
				return ldmsd_send_error(reqc, rc,
					"The producer '%s' is in use.", name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"Permission denied to access "
					"the producer '%s'.", name_s);
			default:
				return ldmsd_send_error(reqc, rc,
					"Failed to delete the producer '%s': %s",
					name_s, ovis_errno_abbvr(rc));
			}
		}
		if (regex) {
			return ldmsd_send_error(reqc, ENOTSUP,
					"act_obj:prdcr:delete not support 'regex'.");
		}
	}
out:
	rc = ldmsd_send_error(reqc, 0, NULL);
	return rc;
}

//static int prdcr_subscribe_regex_handler(ldmsd_req_ctxt_t reqc)
//{
//	char *prdcr_regex;
//	char *stream_name;
//	struct ldmsd_sec_ctxt sctxt;
//
//	reqc->errcode = 0;
//
//	prdcr_regex = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_REGEX);
//	if (!prdcr_regex) {
//		reqc->errcode = EINVAL;
//		Snprintf(&reqc->recv_buf, &reqc->recv_len,
//				"The attribute 'regex' is required by prdcr_stop_regex.");
//		goto send_reply;
//	}
//
//	stream_name = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_STREAM);
//	if (!stream_name) {
//		reqc->errcode = EINVAL;
//		Snprintf(&reqc->recv_buf, &reqc->recv_len,
//				"The attribute 'stream' is required by prdcr_subscribe_regex.");
//		goto send_reply;
//	}
//
//	ldmsd_req_ctxt_sec_get(reqc, &sctxt);
//	reqc->errcode = ldmsd_prdcr_subscribe_regex(prdcr_regex,
//						    stream_name,
//						    reqc->recv_buf,
//						    reqc->recv_len, &sctxt);
//	/* on error, reqc->line_buf will be filled */
//
//send_reply:
//	ldmsd_send_req_response(reqc, reqc->recv_buf);
//	if (prdcr_regex)
//		free(prdcr_regex);
//	return 0;
//}
//
int __prdcr_status_json_obj(ldmsd_req_ctxt_t reqc, ldmsd_prdcr_t prdcr)
{
	extern const char *prdcr_state_str(enum ldmsd_prdcr_state state);
	ldmsd_prdcr_set_t prv_set;
	int set_count = 0;
	int rc = 0;

	ldmsd_prdcr_lock(prdcr);
	rc = ldmsd_append_response_va(reqc, 0,
			"{ \"name\":\"%s\","
			"\"type\":\"%s\","
			"\"host\":\"%s\","
			"\"port\":%hu,"
			"\"transport\":\"%s\","
			"\"reconnect_us\":\"%ld\","
			"\"state\":\"%s\","
			"\"sets\": [",
			prdcr->obj.name, ldmsd_prdcr_type2str(prdcr->type),
			prdcr->host_name, prdcr->port_no, prdcr->xprt_name,
			prdcr->conn_intrvl_us,
			prdcr_state_str(prdcr->conn_state));
	if (rc)
		goto out;

	set_count = 0;
	for (prv_set = ldmsd_prdcr_set_first(prdcr); prv_set;
	     prv_set = ldmsd_prdcr_set_next(prv_set)) {
		if (set_count) {
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				goto out;
		}

		rc = ldmsd_append_response_va(reqc, 0,
			"{ \"inst_name\":\"%s\","
			"\"schema_name\":\"%s\","
			"\"state\":\"%s\"}",
			prv_set->inst_name,
			(prv_set->schema_name ? prv_set->schema_name : ""),
			ldmsd_prdcr_set_state_str(prv_set->state));
		if (rc)
			goto out;
		set_count++;
	}
	rc = ldmsd_append_response(reqc, 0, "]}", 2);
out:
	ldmsd_prdcr_unlock(prdcr);
	return rc;
}

static int prdcr_status_handler(ldmsd_req_ctxt_t reqc)
{
	int rc = 0;
	ldmsd_prdcr_t prdcr = NULL;
	char *name_s;
	int count;
	json_entity_t name, spec;

	spec = json_value_find(reqc->json, "spec");
	if (spec) {
		name = json_value_find(spec, "name");
		if (JSON_STRING_VALUE != json_entity_type(name)) {
			rc = ldmsd_send_type_error(reqc,
					"prdcr_status:spec:name", "a string");
			return rc;
		}
		name_s = json_value_str(name)->str;
		prdcr = ldmsd_prdcr_find(name_s);
		if (!prdcr) {
			rc = ldmsd_send_error(reqc, ENOENT,
					"prdcr '%s' doesn't exist.", name_s);
			return rc;
		}
	}

	/* Construct the json object of the producer(s) */

	rc = ldmsd_append_info_obj_hdr(reqc, "prdcr_status");
	if (rc)
		goto out;

	rc = ldmsd_append_response(reqc, 0, "[", 1);
	if (rc)
		goto out;

	if (prdcr) {
		rc = __prdcr_status_json_obj(reqc, prdcr);
		if (rc)
			goto out;
		ldmsd_prdcr_put(prdcr);
	} else {
		count = 0;
		ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
		for (prdcr = ldmsd_prdcr_first(); prdcr;
				prdcr = ldmsd_prdcr_next(prdcr)) {
			if (count) {
				rc = ldmsd_append_response(reqc, 0, ",", 1);
				if (rc)
					goto unlock_out;
			}
			rc = __prdcr_status_json_obj(reqc, prdcr);
			if (rc)
				goto unlock_out;
			count++;
		}
		ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
	}

	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}", 2);

unlock_out:
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
out:
	if (prdcr)
		ldmsd_prdcr_put(prdcr);
	return rc;
}

size_t __prdcr_set_status(ldmsd_req_ctxt_t reqc, ldmsd_prdcr_set_t prd_set)
{
	struct ldms_timestamp ts = { 0, 0 }, dur = { 0, 0 };
	const char *producer_name = "";
	char intrvl_hint[32];
	char offset_hint[32];
	if (prd_set->set) {
		ts = ldms_transaction_timestamp_get(prd_set->set);
		dur = ldms_transaction_duration_get(prd_set->set);
		producer_name = ldms_set_producer_name_get(prd_set->set);
	}
	if (prd_set->updt_hint.intrvl_us) {
		snprintf(intrvl_hint, sizeof(intrvl_hint), "%ld",
			 prd_set->updt_hint.intrvl_us);
	} else {
		snprintf(intrvl_hint, sizeof(intrvl_hint), "none");
	}
	if (prd_set->updt_hint.offset_us != LDMSD_UPDT_HINT_OFFSET_NONE) {
		snprintf(offset_hint, sizeof(offset_hint), "%ld",
			 prd_set->updt_hint.offset_us);
	} else {
		snprintf(offset_hint, sizeof(offset_hint), "none");
	}
	return ldmsd_append_response_va(reqc, 0,
		"{ "
		"\"inst_name\":\"%s\","
		"\"schema_name\":\"%s\","
		"\"state\":\"%s\","
		"\"origin\":\"%s\","
		"\"producer\":\"%s\","
		"\"hint.sec\":\"%s\","
		"\"hint.usec\":\"%s\","
		"\"timestamp.sec\":\"%d\","
		"\"timestamp.usec\":\"%d\","
		"\"duration.sec\":\"%d\","
		"\"duration.usec\":\"%d\""
		"}",
		prd_set->inst_name, prd_set->schema_name,
		ldmsd_prdcr_set_state_str(prd_set->state),
		producer_name,
		prd_set->prdcr->obj.name,
		intrvl_hint, offset_hint,
		ts.sec, ts.usec,
		dur.sec, dur.usec);
}

/* This function must be called with producer lock held */
int __prdcr_set_status_handler(ldmsd_req_ctxt_t reqc, ldmsd_prdcr_t prdcr,
			int *count, const char *setname, const char *schema)
{
	int rc = 0;
	ldmsd_prdcr_set_t prd_set;

	if (setname) {
		prd_set = ldmsd_prdcr_set_find(prdcr, setname);
		if (!prd_set)
			return 0;
		if (schema && (0 != strcmp(prd_set->schema_name, schema)))
			return 0;
		if (*count) {
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				return rc;
		}
		rc = __prdcr_set_status(reqc, prd_set);
		if (rc)
			return rc;
		(*count)++;
	} else {
		for (prd_set = ldmsd_prdcr_set_first(prdcr); prd_set;
			prd_set = ldmsd_prdcr_set_next(prd_set)) {
			if (schema && (0 != strcmp(prd_set->schema_name, schema)))
				continue;

			if (*count) {
				rc = ldmsd_append_response(reqc, 0, ",", 1);
				if (rc)
					return rc;
			}
			rc = __prdcr_set_status(reqc, prd_set);
			if (rc)
				return rc;
			(*count)++;
		}
	}
	return rc;
}

int prdcr_set_status_handler(ldmsd_req_ctxt_t reqc)
{
	int rc, count = 0;
	json_entity_t spec, value;
	char *prdcr_name, *setname, *schema;
	prdcr_name = setname = schema = NULL;
	ldmsd_prdcr_t prdcr = NULL;

	spec = json_value_find(reqc->json, "spec");
	if (spec) {
		/* producer */
		value = json_value_find(spec, "producer");
		if (value && (JSON_STRING_VALUE != json_entity_type(value))) {
			return ldmsd_send_type_error(reqc,
				"cmd_obj:prdcr_set_status:producer", "a string");
		}
		prdcr_name = json_value_str(value)->str;

		/* set_name */
		value = json_value_find(spec, "set_name");
		if (value && (JSON_STRING_VALUE != json_entity_type(value))) {
			return ldmsd_send_type_error(reqc,
				"cmd_obj:prdcr_set_status:set_name", "a string");
		}
		setname = json_value_str(value)->str;

		/* schema */
		value = json_value_find(spec, "schema");
		if (value && (JSON_STRING_VALUE != json_entity_type(value))) {
			return ldmsd_send_type_error(reqc,
				"cmd_obj:prdcr_set_status:schema", "a string");
		}
		schema = json_value_str(value)->str;
	}

	if (prdcr_name) {
		prdcr = ldmsd_prdcr_find(prdcr_name);
		if (!prdcr) {
			return ldmsd_send_error(reqc, ENOENT,
					"Producer '%s' not found.", prdcr_name);
		}
	}

	rc = ldmsd_append_info_obj_hdr(reqc, "prdcr_set_status");
	if (rc)
		goto out;
	rc = ldmsd_append_response(reqc, 0, "[", 1);
	if (rc)
		goto out;

	if (prdcr) {
		ldmsd_prdcr_lock(prdcr);
		rc = __prdcr_set_status_handler(reqc, prdcr, &count,
						setname, schema);
		ldmsd_prdcr_unlock(prdcr);
		if (rc)
			goto out;
	} else {
		ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
		for (prdcr = ldmsd_prdcr_first(); prdcr;
				prdcr = ldmsd_prdcr_next(prdcr)) {
			ldmsd_prdcr_lock(prdcr);
			rc = __prdcr_set_status_handler(reqc, prdcr, &count,
							setname, schema);
			ldmsd_prdcr_unlock(prdcr);
			if (rc) {
				ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
				goto out;
			}
		}
		ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
	}
	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}", 2);
out:
	if (prdcr) /* ref from find(), first(), or next() */
		ldmsd_prdcr_put(prdcr);
	return rc;
}

static int strgp_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t spec, value, producers, metrics, item;
	char *name, *container, *schema, *s;
	uid_t uid;
	gid_t gid;
	int perm;

	/* instances */
	value = json_value_find(reqc->json, "instances");
	if (value) {
		return ldmsd_send_error(reqc, EINVAL,
				"cfg_obj:strgp not support 'instances'.");
	}

	spec = json_value_find(reqc->json, "spec");

	/* container */
	value = json_value_find(spec, "container");
	if (!value) {
		return ldmsd_send_missing_attr_err(reqc,
				"cfg_obj:strgp:spec", "container");
	}
	if (JSON_STRING_VALUE != json_entity_type(value)) {
		return ldmsd_send_type_error(reqc,
				"cfg_obj:strgp:spec:container", "a string");
	}
	container = json_value_str(value)->str;

	/* schema */
	value = json_value_find(spec, "schema");
	if (!value) {
		return ldmsd_send_missing_attr_err(reqc,
				"cfg_obj:strgp:spec", "schema");
	}
	if (JSON_STRING_VALUE != json_entity_type(value)) {
		return ldmsd_send_type_error(reqc,
				"cfg_obj:strgp:spec:schema", "a string");
	}
	schema = json_value_str(value)->str;

	/* name */
	value = json_value_find(spec, "name");
	if (!value) {
		return ldmsd_send_missing_attr_err(reqc,
				"cfg_obj:strgp:spec", "name");
	}
	if (JSON_STRING_VALUE != json_entity_type(value)) {
		return ldmsd_send_type_error(reqc,
				"cfg_obj:strgp:spec:name", "a string");
	}
	name = json_value_str(value)->str;

	/* prdcr */
	producers = json_value_find(spec, "producers");
	if (producers && (JSON_LIST_VALUE != json_entity_type(producers))) {
		return ldmsd_send_type_error(reqc, "cfg_obj:strgp:spec:producers",
							"a list of strings");
	}

	/* metrics */
	metrics = json_value_find(spec, "metrics");
	if (metrics && (JSON_LIST_VALUE != json_entity_type(metrics))) {
		return ldmsd_send_type_error(reqc, "cfg_obj:strgp:spec:metrics",
								"a list of strings");
	}

	/* perm */
	perm = 0770;
	rc = __find_perm(reqc, spec, &perm);
	if (rc)
		return rc;

	struct ldmsd_sec_ctxt sctxt;
	ldmsd_sec_ctxt_get(&sctxt);
	uid = sctxt.crd.uid;
	gid = sctxt.crd.gid;

	ldmsd_strgp_t strgp = ldmsd_strgp_new_with_auth(name, container,
						schema, uid, gid, perm);
	if (!strgp) {
		rc = errno;
		switch (rc) {
		case EEXIST:
			return ldmsd_send_error(reqc, rc,
				"cfg_obj:strgp '%s' already exists.", name);
		case ENOENT:
			return ldmsd_send_error(reqc, ENOENT, "cfg_obj:strgp '%s' - "
					"the store plugin '%s' not found.",
					name, container);
		case ENOMEM:
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		default:
			return ldmsd_send_error(reqc, rc,
					"Failed to create strgp '%s': %s",
					name, ovis_errno_abbvr(rc));
		}
	}

	/* reuse the received buffer */
	ldmsd_req_buf_reset(reqc->recv_buf);

	/* add producer filter*/
	if (!producers)
		goto add_metrics;

	for (item = json_item_first(producers); item; item = json_item_next(item)) {
		if (JSON_STRING_VALUE != json_entity_type(item)) {
			rc = ldmsd_send_type_error(reqc,
					"cfg_obj:strgp:producers[]", "a string");
			goto err;
		}
		s = json_value_str(item)->str;
		rc = ldmsd_strgp_prdcr_add(name, s, reqc->recv_buf->buf,
						reqc->recv_buf->len, &sctxt);
		switch (rc) {
		case 0:
			break;
		case ENOENT:
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:strgp - The storage policy '%s' "
					"does not exist.", name);
			goto err;
		case EBUSY:
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:strgp '%s' - "
					"Configuration changes cannot be made "
					"while the storage policy is running.",
					name);
			goto err;
		case ENOMEM:
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			rc = ENOMEM;
			goto err;
		case EACCES:
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:strgp '%s' - Permission denied.",
					name);
			goto err;
		default:
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:strgp '%s' - "
					"Failed to add producer regex %s: %s.",
					name, s, ovis_errno_abbvr(rc));
			goto err;
		}
	}

add_metrics:
	/* Add metric filter */
	if (!metrics)
		goto out;
	for (item = json_item_first(metrics); item; item = json_item_next(item)) {
		if (JSON_STRING_VALUE != json_entity_type(item)) {
			rc = ldmsd_send_type_error(reqc,
					"cfg_obj:strgp:metrics[]", "a string");
			goto err;
		}
		s = json_value_str(item)->str;
		rc = ldmsd_strgp_metric_add(name, s, &sctxt);
		switch (rc) {
		case 0:
			break;
		case ENOENT:
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:strgp - The storage policy '%s' "
					"does not exist.", name);
			goto err;
		case EBUSY:
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:strgp '%s' - "
					"Configuration changes cannot be made "
					"while the storage policy is running.",
					name);
			goto err;
		case EEXIST:
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:strgp '%s' - "
					"The metric '%s' is already present.",
					name, s);
			goto err;
		case ENOMEM:
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			rc = ENOMEM;
			goto err;
		case EACCES:
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:strgp '%s' - Permission denied.",
					name);
			goto err;
		default:
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:strgp '%s' - "
					"Failed to add metric '%s': %s.",
					name, s, ovis_errno_abbvr(rc));
			goto err;
		}
	}

out:
	ldmsd_strgp_unlock(strgp);
	return ldmsd_send_error(reqc, 0, NULL);

err:
	if (strgp) {
		ldmsd_strgp_unlock(strgp);
		ldmsd_strgp_put(strgp);
	}
	return rc;
}

static int __smplr_handler(ldmsd_req_ctxt_t reqc, json_entity_t d,
				unsigned long int_v, unsigned long offset_v,
				int perm_value, uid_t uid, gid_t gid)
{
	int rc;
	json_entity_t name, pi_inst;
	char *name_s, *inst_s;
	char obj_name[128];

	name = json_value_find(d, "name");
	if (!name) {
		rc = ldmsd_send_missing_attr_err(reqc, "cfg_obj:smplr", "name");
		return rc;
	}
	if (JSON_STRING_VALUE != json_entity_type(name)) {
		rc = ldmsd_send_type_error(reqc, "smplr:name", "a string");
		return rc;
	}
	name_s = json_value_str(name)->str;
	snprintf(obj_name, 128, "smplr(%s)", name_s);
	pi_inst = json_value_find(d, "plugin_instance");
	if (!pi_inst) {
		rc = ldmsd_send_missing_attr_err(reqc,
				obj_name, "plugin_instance");
		return rc;
	}
	if (JSON_STRING_VALUE != json_entity_type(pi_inst)) {
		rc = ldmsd_send_type_error(reqc, obj_name, "a string");
		return rc;
	}
	inst_s = json_value_str(pi_inst)->str;

	ldmsd_plugin_inst_t pi = ldmsd_plugin_inst_find(inst_s);
	if (!pi) {
		rc = ldmsd_send_error(reqc, ENOENT,
				"%s: Plugin instance `%s` not found\n",
				obj_name, inst_s);
		return rc;
	}
	if (!LDMSD_INST_IS_SAMPLER(pi)) {
		rc = ldmsd_send_error(reqc, EINVAL,
				"%s: The plugin instance `%s` is not a sampler.\n",
				obj_name, inst_s);
		return rc;
	}

	ldmsd_smplr_t smplr = ldmsd_smplr_new_with_auth(name_s, pi,
							int_v, offset_v,
							uid, gid, perm_value);
	if (!smplr) {
		if (errno == EEXIST) {
			rc = ldmsd_send_error(reqc, EEXIST,
					"%s: The smplr %s already exists.",
					obj_name, name_s);
			return rc;
		} else {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		}
	}
	return 0;
}

static int smplr_handler(ldmsd_req_ctxt_t reqc)
{
	unsigned long int_v, offset_v;
	uid_t uid;
	gid_t gid;
	int perm_value;
	int rc;
	json_entity_t spec, instances, interval, offset, item;
	struct ldmsd_sec_ctxt sctxt;

	ldmsd_req_ctxt_sec_get(reqc, &sctxt);
	uid = sctxt.crd.uid;
	gid = sctxt.crd.gid;

	spec = json_value_find(reqc->json, "spec");

	/* sample interval */
	interval = json_value_find(spec, "interval");
	if (!interval) {
		rc = ldmsd_send_missing_attr_err(reqc, "cfg_obj:smplr", "interval");
		return rc;
	}
	if (JSON_STRING_VALUE == json_entity_type(interval)) {
		int_v = ldmsd_time_str2us(json_value_str(interval)->str);
	} else if (JSON_INT_VALUE == json_entity_type(interval)) {
		int_v = json_value_int(interval);
	} else {
		rc = ldmsd_send_type_error(reqc, "smplr:interval",
				"a string or an integer");
		return rc;
	}

	offset = json_value_find(spec, "offset");
	if (offset) {
		if (JSON_STRING_VALUE == json_entity_type(offset)) {
			offset_v = ldmsd_time_str2us(json_value_str(offset)->str);
		} else if (JSON_INT_VALUE == json_entity_type(offset)) {
			offset_v = json_value_int(offset);
		} else {
			rc = ldmsd_send_type_error(reqc, "smplr:offset",
					"a string or an integer");
			return rc;
		}
	} else {
		offset_v = LDMSD_UPDT_HINT_OFFSET_NONE;
	}

	/* Permission */
	perm_value = 0770;
	rc = __find_perm(reqc, spec, &perm_value);
	if (rc)
		return rc;

	instances = json_value_find(reqc->json, "instances");
	if (!instances) {
		rc = __smplr_handler(reqc, spec, int_v, offset_v,
						perm_value, uid, gid);
		if (rc)
			return rc;
	} else {
		for (item = json_item_first(instances); item;
				item = json_item_next(item)) {
			if (JSON_DICT_VALUE != json_entity_type(item)) {
				rc = ldmsd_send_type_error(reqc,
							"smplr:instances[]",
							"a dictionary");
				return rc;
			}
			rc = __smplr_handler(reqc, item, int_v, offset_v,
						perm_value, uid, gid);
			if (rc)
				return rc;
		}
	}

	rc = ldmsd_send_error(reqc, 0, "");
	return rc;
}

static int strgp_action_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t action, names, regex, name;
	char *action_s, *name_s;
	struct ldmsd_sec_ctxt sctxt;

	ldmsd_req_ctxt_sec_get(reqc, &sctxt);

	action = json_value_find(reqc->json, "action");
	action_s = json_value_str(action)->str;
	regex = json_value_find(reqc->json, "regex");
	if (regex) {
		return ldmsd_send_error(reqc, ENOTSUP,
				"act_obj:strgp does not support 'regex'");
	}

	names = json_value_find(reqc->json, "names");
	if (!names) {
		return ldmsd_send_missing_attr_err(reqc, "act_obj:strgp", "names");
	}

	if (0 == strncmp(action_s, "start", 5)) {
		for (name = json_item_first(names); name; name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_strgp_start(name_s, &sctxt, 0);
			switch (rc) {
			case 0:
				break;
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"act_obj:start The strgp '%s' does not exist.", name_s);
			case EBUSY:
				return ldmsd_send_error(reqc, rc,
					"act_obj:start The strgp '%s' is already running.", name_s);
			case EPERM:
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"act_obj:start:updtr '%s' - Permission denied.",
					name_s);
			default:
				return ldmsd_send_error(reqc, rc,
					"Failed to start strgp '%s': %s.",
					name_s, ovis_errno_abbvr(rc));
			}
		}
	} else if (0 == strncmp(action_s, "stop", 4)) {
		for (name = json_item_first(names); name; name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_strgp_stop(name_s, &sctxt);
			switch (rc) {
			case 0:
				break;
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"act_obj:stop - strgp '%s' "
					"does not exist.", name_s);
			case EBUSY:
				return ldmsd_send_error(reqc, rc,
					"act_obj:stop - strgp '%s' "
					"is already stopped.", name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"act_obj:stop:strgp '%s' - "
					"Permission denied.", name_s);
			default:
				return ldmsd_send_error(reqc, rc,
					"Failed to stop strgp '%s': %s.",
					name_s, ovis_errno_abbvr(rc));
			}
		}
	} else {
		for (name = json_item_first(names); name; name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_strgp_del(name_s, &sctxt);
			switch (rc) {
			case 0:
				break;
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"act_obj:delete - "
					"strgp %s does not exist.",
					name_s);
			case EBUSY:
				return ldmsd_send_error(reqc, rc,
					"act_obj:delete - strgp '%s' is in use.",
					name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"act_obj:delete:strgp '%s' - "
					"Permission denied.");
			default:
				return ldmsd_send_error(reqc, rc,
					"Failed to delete strgp '%s': %s.",
					name_s, ovis_errno_abbvr(rc));
			}
		}
	}
	return ldmsd_send_error(reqc, 0, NULL);
}

int __strgp_status_json_obj(ldmsd_req_ctxt_t reqc, ldmsd_strgp_t strgp)
{
	int rc;
	int match_count, metric_count;
	ldmsd_name_match_t match;
	ldmsd_strgp_metric_t metric;
	char *s;

	ldmsd_strgp_lock(strgp);
	rc = ldmsd_append_response_va(reqc, 0,
		       "{\"name\":\"%s\","
		       "\"container\":\"%s\","
		       "\"plugin\":\"%s\","
		       "\"schema\":\"%s\","
		       "\"state\":\"%s\","
		       "\"producers\":[",
		       strgp->obj.name,
		       strgp->inst->inst_name,
		       strgp->inst->plugin_name,
		       strgp->schema,
		       ldmsd_strgp_state_str(strgp->state));
	if (rc)
		goto out;

	match_count = 0;
	for (match = ldmsd_strgp_prdcr_first(strgp); match;
	     match = ldmsd_strgp_prdcr_next(match)) {
		if (match_count) {
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				goto out;
		}
		match_count++;
		rc = ldmsd_append_response_va(reqc, 0, "\"%s\"", match->regex_str);
		if (rc)
			goto out;
	}
	s = "],\"metrics\":[";
	rc = ldmsd_append_response(reqc, 0, s, strlen(s));
	if (rc)
		goto out;

	metric_count = 0;
	for (metric = ldmsd_strgp_metric_first(strgp); metric;
	     metric = ldmsd_strgp_metric_next(metric)) {
		if (metric_count) {
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				goto out;
		}
		metric_count++;
		rc = ldmsd_append_response_va(reqc, 0, "\"%s\"", metric->name);
		if (rc)
			goto out;
	}
	rc = ldmsd_append_response(reqc, 0, "]}", 2);
out:
	ldmsd_strgp_unlock(strgp);
	return rc;
}

static int strgp_status_handler(ldmsd_req_ctxt_t reqc)
{
	int rc = 0;
	json_entity_t spec, name;
	char *name_s;
	ldmsd_strgp_t strgp = NULL;
	int strgp_cnt;

	spec = json_value_find(reqc->json, "spec");
	if (spec) {
		name = json_value_find(spec, "name");
		if (JSON_STRING_VALUE != json_entity_type(name)) {
			return ldmsd_send_type_error(reqc,
					"strgp_status:name", "a string");
		}
		name_s = json_value_str(name)->str;
		strgp = ldmsd_strgp_find(name_s);
		if (!strgp) {
			return ldmsd_send_error(reqc, ENOENT,
				"strgp_status: strgp '%s' does not exist.", name_s);
		}
	}

	rc = ldmsd_append_info_obj_hdr(reqc, "strgp_status");
	if (rc)
		goto out;
	rc = ldmsd_append_response(reqc, 0, "[", 1);
	if (rc)
		goto out;

	if (strgp) {
		rc = __strgp_status_json_obj(reqc, strgp);
		ldmsd_strgp_put(strgp);
		if (rc)
			return rc;
	} else {
		strgp_cnt = 0;
		ldmsd_cfg_lock(LDMSD_CFGOBJ_STRGP);
		for (strgp = ldmsd_strgp_first(); strgp;
			strgp = ldmsd_strgp_next(strgp)) {
			if (strgp_cnt) {
				rc = ldmsd_append_response(reqc, 0, ",", 1);
				if (rc)
					goto unlock_out;
			}
			rc = __strgp_status_json_obj(reqc, strgp);
			if (rc)
				goto unlock_out;
			strgp_cnt++;
		}
		ldmsd_cfg_unlock(LDMSD_CFGOBJ_STRGP);
	}

	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}", 2);

unlock_out:
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_STRGP);
out:
	if (strgp)
		ldmsd_strgp_put(strgp);
	return rc;
}

static int __updtr_handler(ldmsd_req_ctxt_t reqc, json_entity_t spec,
				json_entity_t inst, long interval_us,
				long offset_us, int push_flags,
				int auto_interval, int perm)
{
	int rc;
	json_entity_t prdcr, sets, schema, name, item, list;
	char *name_s, *regex;
	struct ldmsd_sec_ctxt sctxt;
	ldmsd_sec_ctxt_get(&sctxt);
	uid_t uid = sctxt.crd.uid;
	gid_t gid = sctxt.crd.gid;

	/* List of producers */
	prdcr = json_value_find(spec, "producers");
	if (!prdcr && inst)
		prdcr = json_value_find(inst, "producers");
	if (!prdcr) {
		return ldmsd_send_missing_attr_err(reqc,
				"cfg_obj:updtr:spec", "producers");
	}
	if (prdcr && (JSON_LIST_VALUE != json_entity_type(prdcr))) {
		return ldmsd_send_type_error(reqc, "prdcr:spec:producers",
						"a list of strings");
	}

	/* List of the regex of set instance names */
	sets = json_value_find(spec, "set_instances");
	if (!sets && inst)
		sets = json_value_find(inst, "set_instances");
	if (sets && (JSON_LIST_VALUE != json_entity_type(sets))) {
		return ldmsd_send_type_error(reqc, "prdcr:spec:set_instances",
							"a list of strings");
	}

	/* List of the regex of set schema names */
	schema = json_value_find(spec, "set_schema");
	if (!schema && inst)
		schema = json_value_find(inst, "set_schema");
	if (schema && (JSON_LIST_VALUE != json_entity_type(schema))) {
		return ldmsd_send_type_error(reqc, "prdcr:spec:set_schema",
							"a list of strings");
	}

	if (!sets && !schema) {
		return ldmsd_send_error(reqc, EINVAL,
				"cfg_obj:updtr- Either 'set_instances' and/or "
				"'set_schema' must be specified." );
	}

	/* name */
	name = json_value_find(spec, "name");
	if (!name && inst)
		name = json_value_find(inst, "name");
	if (!name) {
		return ldmsd_send_missing_attr_err(reqc,
				"cfg_obj:updtr:spec", "name");
	}
	name_s = json_value_str(name)->str;

	/* Create an updater */
	ldmsd_updtr_t updtr = ldmsd_updtr_new_with_auth(name_s, interval_us,
							offset_us,
							push_flags,
							auto_interval,
							uid, gid, perm);
	if (!updtr) {
		if (errno == EEXIST) {
			return ldmsd_send_error(reqc, errno,
				       "The updtr %s already exists.", name);
		} else if (errno == ENOMEM) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		} else {
			return ldmsd_send_error(reqc, errno,
				       "The updtr could not be created.");
		}
	}
	ldmsd_req_buf_reset(reqc->recv_buf);

	/* Add producers to the updater */
	for (item = json_item_first(prdcr); item; item = json_item_next(item)) {
		if (JSON_STRING_VALUE != json_entity_type(item)) {
			return ldmsd_send_type_error(reqc, "updtr:spec:prdcr",
					"a list of string");
		}
		regex = json_value_str(item)->str;
		rc = ldmsd_updtr_prdcr_add(name_s, regex, reqc->recv_buf->buf,
						reqc->recv_buf->len, &sctxt);
		switch (rc) {
		case 0:
			break;
		case EACCES:
			return ldmsd_send_error(reqc, rc,
					"cfg_obj:updtr '%s': Permission denied.",
					name_s);
		case ENOENT:
			return ldmsd_send_error(reqc, rc,
					"cfg_obj:updtr '%s' does not exist.",
					name_s);
		case EBUSY:
			return ldmsd_send_error(reqc, rc,
					"cfg_obj:updtr '%s' cannot be changed.",
					name_s);
		case ENOMEM:
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		default:
			return ldmsd_send_error(reqc, rc,
				       "Failed to add prdcr '%s' to updtr '%s': %s",
				       regex, name_s, ovis_errno_abbvr(rc));
		}
	}

	/* Add sets */
	const char *match_str = "inst";
	if (sets)
		list = sets;
	else
		list = schema;
add_sets:
	for (item = json_item_first(list); item; item = json_item_next(item)) {
		if (JSON_STRING_VALUE != json_entity_type(item)) {
			return ldmsd_send_type_error(reqc, "updtr:spec:sets/schema",
					"a list of string");
		}
		regex = json_value_str(item)->str;
		rc = ldmsd_updtr_match_add(name_s, regex, match_str,
				reqc->recv_buf->buf, reqc->recv_buf->len, &sctxt);
		switch (rc) {
		case 0:
			break;
		case ENOENT:
			return ldmsd_send_error(reqc, rc,
					"cfg_obj:updtr '%s' does not exist.",
					name_s);
		case EBUSY:
			return ldmsd_send_error(reqc, rc,
					"cfg_obj:updtr '%s' cannot be changed "
					"while the updater is running.", name_s);
			break;
		case ENOMEM:
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		case EACCES:
			return ldmsd_send_error(reqc, rc,
					"cfg_obj:updtr '%s': Permission denied.",
					name_s);
			break;
		default:
			return ldmsd_send_error(reqc, rc,
					"Failed to add set %s '%s' to updtr '%s': %s",
					match_str, regex, name_s,
					ovis_errno_abbvr(rc));
		}
	}
	if (schema && (list != schema)) {
		/* avoid repeating similar code */
		list = schema;
		match_str = "schema";
		goto add_sets;
	}

	return 0;
}

static int updtr_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t spec, value, inst;
	int perm_value;
	char *push_s;
	int push_flags, is_auto_task;
	long interval_us, offset_us;

	spec = json_value_find(reqc->json, "spec");

	/* interval */
	value = json_value_find(spec, "interval");
	if (!value) {
		rc = ldmsd_send_missing_attr_err(reqc, "updtr:spec", "interval");
		return rc;
	}
	if (JSON_STRING_VALUE == json_entity_type(value)) {
		interval_us = ldmsd_time_str2us(json_value_str(value)->str);
	} else if (JSON_INT_VALUE == json_entity_type(value)) {
		interval_us = json_value_int(value);
	} else {
		rc = ldmsd_send_type_error(reqc, "updtr:spec:interval",
				"a string or an integer");
		return rc;
	}

	/* offset */
	value = json_value_find(spec, "offset");
	if (value) {
		if (JSON_STRING_VALUE == json_entity_type(value)) {
			offset_us = ldmsd_time_str2us(json_value_str(value)->str);
		} else if (JSON_INT_VALUE == json_entity_type(value)) {
			offset_us = json_value_int(value);
		} else {
			rc = ldmsd_send_type_error(reqc, "updtr:spec:offset",
					"a string or an integer");
			return rc;
		}
	} else {
		offset_us = LDMSD_UPDT_HINT_OFFSET_NONE;
	}

	/* push */
	push_flags = 0;
	value = json_value_find(spec, "push");
	if (value) {
		if (JSON_BOOL_VALUE == json_entity_type(value)) {
			if (json_value_bool(value)) {
				push_flags = LDMSD_UPDTR_F_PUSH;
			}
		} else if (JSON_STRING_VALUE == json_entity_type(value)) {
			push_s = json_value_str(value)->str;
			if (0 == strcasecmp(push_s, "onchange")) {
				push_flags = LDMSD_UPDTR_F_PUSH | LDMSD_UPDTR_F_PUSH_CHANGE;
			} else if (0 == strcasecmp(push_s, "true") || 0 == strcasecmp(push_s, "yes")) {
				push_flags = LDMSD_UPDTR_F_PUSH;
			} else {
				rc = ldmsd_send_error(reqc, EINVAL,
					       "updtr:spec:push value '%s' is invalid.",
					       push_s);
				return rc;
			}
		} else {
			return ldmsd_send_type_error(reqc,
					"updtr:spec:push", "a boolean or a string");
		}
		is_auto_task = 0;
	}

	/* auto_interval */
	is_auto_task = 0;
	value = json_value_find(spec, "auto_interval");
	if (value) {
		if (JSON_BOOL_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc, "updtr:spec:auto_interval",
					"a boolean");
		}
		if (json_value_bool(value)) {
			if (push_flags) {
				return ldmsd_send_error(reqc, EINVAL,
						"auto_interval and push are "
						"incompatible options");
			}
			is_auto_task = 1;
		}
	}

	/* perm */
	perm_value = 0770;
	rc = __find_perm(reqc, spec, &perm_value);
	if (rc)
		return rc;

	/* instances */
	value = json_value_find(reqc->json, "instances");
	if (!value) {
		rc = __updtr_handler(reqc, spec, NULL, interval_us, offset_us,
				push_flags, is_auto_task, perm_value);
		if (rc)
			return rc;
	} else {
		for (inst = json_item_first(value); inst; inst = json_item_next(inst)) {
			rc = __updtr_handler(reqc, spec, inst, interval_us,
					offset_us, push_flags, is_auto_task,
					perm_value);
			if (rc)
				return rc;
		}
	}
	return ldmsd_send_error(reqc, 0, NULL);
}

static int updtr_action_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t action, names, regex, name;
	char *action_s, *name_s;
	struct ldmsd_sec_ctxt sctxt;

	ldmsd_req_ctxt_sec_get(reqc, &sctxt);

	action = json_value_find(reqc->json, "action");
	action_s = json_value_str(action)->str;
	regex = json_value_find(reqc->json, "regex");

	if (regex) {
		return ldmsd_send_error(reqc, ENOTSUP,
			"act_obj:updtr does not support 'regex'");
	}

	names = json_value_find(reqc->json, "names");
	if (!names)
		return ldmsd_send_missing_attr_err(reqc, "act_obj:smplr", "names");

	if (0 == strncmp(action_s, "start", 5)) {
		for (name = json_item_first(names); name; name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_updtr_start(name_s, NULL, NULL, NULL, &sctxt, 0);
			switch (rc) {
			case 0:
				break;
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"act_obj: The updater '%s' does not exist.", name_s);
			case EBUSY:
				return ldmsd_send_error(reqc, rc,
					"act_obj: The updater '%s' is already running.", name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"act_obj:%s:updtr '%s' - Permission denied.",
					action_s, name_s);
			default:
				return ldmsd_send_error(reqc, rc,
					"Failed to start updtr '%s': %s.",
					name_s, ovis_errno_abbvr(rc));
			}
		}
	} else if (0 == strncmp(action_s, "stop", 4)) {
		for (name = json_item_first(names); name; name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_updtr_stop(name_s, &sctxt);
			switch (rc) {
			case 0:
				break;
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"act_obj:stop - The updater '%s' "
					"does not exist.", name_s);
			case EBUSY:
				return ldmsd_send_error(reqc, rc,
					"act_obj:stop - The updater '%s' "
					"is already stopped.", name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"act_obj:stop:updtr '%s' - "
					"Permission denied.", name_s);
			default:
				return ldmsd_send_error(reqc, rc,
					"Failed to stop updtr '%s': %s.",
					name_s, ovis_errno_abbvr(rc));
			}
		}
	} else {
		for (name = json_item_first(names); name; name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_updtr_del(name_s, &sctxt);
			switch (rc) {
			case 0:
				break;
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"act_obj:delete - "
					"The updater %s does not exist.",
					name_s);
			case EBUSY:
				return ldmsd_send_error(reqc, rc,
					"act_obj:delete - The updater '%s' is in use.",
					name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"act_obj:delete:updtr '%s' - "
					"Permission denied.");
			default:
				return ldmsd_send_error(reqc, rc,
					"Failed to delete updtr '%s': %s.",
					name_s, ovis_errno_abbvr(rc));
			}
		}
	}
	return ldmsd_send_error(reqc, 0, NULL);
}

static const char *update_mode(int push_flags)
{
	if (!push_flags)
		return "Pull";
	if (push_flags & LDMSD_UPDTR_F_PUSH_CHANGE)
		return "Push on Change";
	return "Push on Request";
}

int __updtr_status_json_obj(ldmsd_req_ctxt_t reqc, ldmsd_updtr_t updtr)
{
	int rc;
	ldmsd_prdcr_ref_t ref;
	ldmsd_prdcr_t prdcr;
	int prdcr_count;
	const char *prdcr_state_str(enum ldmsd_prdcr_state state);

	ldmsd_updtr_lock(updtr);
	rc = ldmsd_append_response_va(reqc, 0,
			"{\"name\":\"%s\","
			"\"interval\":\"%ld\","
			"\"offset\":\"%ld\","
			"\"offset_incr\":\"%ld\","
			"\"auto\":\"%s\","
			"\"mode\":\"%s\","
			"\"state\":\"%s\","
			"\"producers\":[",
			updtr->obj.name,
			updtr->sched.intrvl_us,
			updtr->sched.offset_us,
			updtr->sched.offset_skew,
			updtr->is_auto_task ? "true" : "false",
			update_mode(updtr->push_flags),
			ldmsd_updtr_state_str(updtr->state));
	if (rc)
		return rc;

	prdcr_count = 0;
	for (ref = ldmsd_updtr_prdcr_first(updtr); ref;
	     ref = ldmsd_updtr_prdcr_next(ref)) {
		if (prdcr_count) {
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				goto out;
		}
		prdcr_count++;
		prdcr = ref->prdcr;
		rc = ldmsd_append_response_va(reqc, 0,
			       "{\"name\":\"%s\","
			       "\"host\":\"%s\","
			       "\"port\":%hu,"
			       "\"transport\":\"%s\","
			       "\"state\":\"%s\"}",
			       prdcr->obj.name,
			       prdcr->host_name,
			       prdcr->port_no,
			       prdcr->xprt_name,
			       prdcr_state_str(prdcr->conn_state));
		if (rc)
			goto out;
	}
	rc = ldmsd_append_response(reqc, 0, "]}", 2);
out:
	ldmsd_updtr_unlock(updtr);
	return rc;
}

static int updtr_status_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t name, spec;
	char *name_s;
	int updtr_cnt;
	ldmsd_updtr_t updtr = NULL;

	spec = json_value_find(reqc->json, "spec");
	if (spec) {
		name = json_value_find(spec, "name");
		if (JSON_STRING_VALUE != json_entity_type(name)) {
			rc = ldmsd_send_type_error(reqc,
					"prdcr_status:spec:name", "a string");
			return rc;
		}
		name_s = json_value_str(name)->str;
		updtr = ldmsd_updtr_find(name_s);
		if (!updtr) {
			return ldmsd_send_error(reqc, ENOENT,
					"updtr '%s' doesn't exist.", name_s);
		}
	}

	/* Construct the json object of the updater(s) */
	rc = ldmsd_append_info_obj_hdr(reqc, "updtr_status");
	if (rc)
		goto out;
	rc = ldmsd_append_response(reqc, 0, "[", 1);
	if (rc)
		goto out;

	if (updtr) {
		rc = __updtr_status_json_obj(reqc, updtr);
		if (rc)
			goto out;
		ldmsd_updtr_put(updtr);
	} else {
		updtr_cnt = 0;
		ldmsd_cfg_lock(LDMSD_CFGOBJ_UPDTR);
		for (updtr = ldmsd_updtr_first(); updtr;
				updtr = ldmsd_updtr_next(updtr)) {
			if (updtr_cnt) {
				rc = ldmsd_append_response(reqc, 0, ",", 1);
				if (rc)
					goto unlock_out;
			}
			rc = __updtr_status_json_obj(reqc, updtr);
			if (rc)
				goto unlock_out;
			updtr_cnt++;
		}
		ldmsd_cfg_unlock(LDMSD_CFGOBJ_UPDTR);
	}

	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}", 2);

unlock_out:
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_UPDTR);
out:
	if (updtr)
		ldmsd_updtr_put(updtr);
	return rc;
}

static int setgroup_handler(ldmsd_req_ctxt_t reqc)
{
	int rc = 0;
	json_entity_t spec, value, members;
	char *name = NULL;
	char *producer = NULL;
	char *member = NULL;
	ldmsd_setgrp_t grp = NULL;
	long interval_us, offset_us = LDMSD_UPDT_HINT_OFFSET_NONE;
	struct ldmsd_sec_ctxt sctxt;
	int perm;
	int flags = 0;

	spec = json_value_find(reqc->json, "spec");

	/* name */
	value = json_value_find(spec, "name");
	if (!value)
		return ldmsd_send_missing_attr_err(reqc, "cfg_obj:setgroup", "name");
	if (JSON_STRING_VALUE != json_entity_type(value))
		return ldmsd_send_type_error(reqc, "cfg_obj:setgroup:name", "a string");
	name = json_value_str(value)->str;

	/* producer */
	value = json_value_find(spec, "producer");
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
					"cfg_obj:setgroup:producer", "a string");
		}
	}

	/* sample interval */
	value = json_value_find(spec, "interval");
	if (!value) {
		interval_us = 0;
	} else {
		if (JSON_STRING_VALUE == json_entity_type(value)) {
			interval_us = ldmsd_time_str2us(json_value_str(value)->str);
		} else if (JSON_INT_VALUE == json_entity_type(value)) {
			interval_us = json_value_int(value);
		} else {
			return ldmsd_send_type_error(reqc,
						"cfg_obj:setgroup:interval",
						"a string or an integer");
		}
	}

	/* offset */
	value = json_value_find(spec, "offset");
	if (value) {
		if (JSON_STRING_VALUE == json_entity_type(value)) {
			offset_us = ldmsd_time_str2us(json_value_str(value)->str);
		} else if (JSON_INT_VALUE == json_entity_type(value)) {
			offset_us = json_value_int(value);
		} else {
			return ldmsd_send_type_error(reqc,
					"cfg_obj:setgroup:offset",
					"a string or an integer");
		}
	} else {
		offset_us = LDMSD_UPDT_HINT_OFFSET_NONE;
	}

	/* perm */
	perm = 0777;
	rc = __find_perm(reqc, spec, &perm);
	if (rc)
		return rc;

	/* members */
	members = json_value_find(spec, "members");
	if (!members) {
		return ldmsd_send_missing_attr_err(reqc,
				"cfg_obj:setgroup", "members");
	}
	if (JSON_LIST_VALUE != json_entity_type(members)) {
		return ldmsd_send_type_error(reqc,
				"cfg_obj:setgroup:members", "a list of strings");
	}

	ldmsd_req_ctxt_sec_get(reqc, &sctxt);

	grp = ldmsd_setgrp_new_with_auth(name, producer, interval_us, offset_us,
					sctxt.crd.uid, sctxt.crd.gid, perm, flags);
	if (!grp) {
		rc = errno;
		if (errno == EEXIST) {
			return ldmsd_send_error(reqc, rc,
				"cfg_obj:setgroup '%s': '%s' already exists.", name);
		} else {
			return ldmsd_send_error(reqc, rc,
				"cfg_obj:setgroup: Failed to create '%s': %s.",
				name, ovis_errno_abbvr(rc));
		}
	}

	/* Add members */
	for (value = json_item_first(members); value; value = json_item_next(value)) {
		if (JSON_STRING_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
					"cfg_obj:setgroup:members[]", "a string");
		}
		member = json_value_str(value)->str;
		rc = ldmsd_setgrp_ins(name, member);
		if (rc == ENOENT) {
			/* Impossible to happen. do nothing */
		} else if (rc != 0){
			rc = ldmsd_send_error(reqc, rc,
					"cfg_obj:setgroup: Failed to add "
					"member '%s' to setgroup '%s'",
					member, name);
			goto err;
		}
	}

	return ldmsd_send_error(reqc, 0, NULL);
err:
	ldmsd_setgrp_unlock(grp);
	ldmsd_setgrp_put(grp);
	return rc;
}

static int setgroup_action_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t action, list, value;
	char *action_s, *name_s;
	struct ldmsd_sec_ctxt sctxt;

	ldmsd_req_ctxt_sec_get(reqc, &sctxt);

	/* regex */
	value = json_value_find(reqc->json, "regex");
	if (value) {
		return ldmsd_send_error(reqc, ENOTSUP,
				"act_obj:setgroup not support 'regex'.");
	}

	action = json_value_find(reqc->json, "action");
	action_s = json_value_str(action)->str;
	list = json_value_find(reqc->json, "names");

	if (0 == strncmp(action_s, "start", 5)) {
		for (value = json_item_first(list); value;
				value = json_item_next(value)) {
			if (JSON_STRING_VALUE != json_entity_type(value)) {
				return ldmsd_send_type_error(reqc,
						"cfg_obj:setgroup:names[]", "a string");
			}
			name_s = json_value_str(value)->str;
			rc = ldmsd_setgrp_start(name_s, &sctxt);
			if (!rc)
				continue;
			switch (rc) {
			case ENOMEM:
				ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
				return ENOMEM;
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"act_obj:setgroup:start: '%s' not found.",
					name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"act_obj:setgroup:start: '%s' "
					"permission denied");
			default:
				return ldmsd_send_error(reqc, rc,
					"act_obj:setgroup:start: failed to start '%s': %s",
					name_s, ovis_errno_abbvr(rc));
			}
		}
	} else if (0 == strncmp(action_s, "stop", 4)) {
		return ldmsd_send_error(reqc, ENOTSUP, "act_obj:setgroup - "
				"Action 'stop' not supported for "
				"'setgroup' configuration objects.");
	} else {
		for (value = json_item_first(list); value;
				value = json_item_next(value)) {
			if (JSON_STRING_VALUE != json_entity_type(value)) {
				return ldmsd_send_type_error(reqc,
						"cfg_obj:setgroup:names[]", "a string");
			}
			name_s = json_value_str(value)->str;
			rc = ldmsd_setgrp_del(name_s, &sctxt);
			if (!rc)
				continue;
			switch (rc) {
			case ENOMEM:
				ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
				return ENOMEM;
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					"act_obj:setgroup:delete: '%s' not found.",
					name_s);
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"act_obj:setgroup:delete: '%s' "
					"permission denied");
			default:
				return ldmsd_send_error(reqc, rc,
					"act_obj:setgroup: failed to delete '%s': %s",
					name_s, ovis_errno_abbvr(rc));
			}
		}
	}
	return ldmsd_send_error(reqc, 0, NULL);
}

static int smplr_action_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t action, names, name, regex;
	char *name_s, *action_s;
	struct ldmsd_sec_ctxt sctxt;

	ldmsd_req_ctxt_sec_get(reqc, &sctxt);

	action = json_value_find(reqc->json, "action");
	action_s = json_value_str(action)->str;
	names = json_value_find(reqc->json, "names");
	regex = json_value_find(reqc->json, "regex");

	if (0 == strncmp(action_s, "start", 5)) {
		for (name = json_item_first(names); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_smplr_start(name_s, NULL, NULL, 0, 0, &sctxt);
			if (rc == ENOENT) {
				rc = ldmsd_send_error(reqc, rc,
					"smplr '%s' does not exist.", name_s);
			} else if (rc == EBUSY) {
				rc = ldmsd_send_error(reqc, rc,
					"smplr '%s' already started", name_s);
			} else if (rc > 0) {
				rc = ldmsd_send_error(reqc, rc,
					"Failed to start smplr '%s'", name_s);
			}
		}
	} else if (0 == strncmp(action_s, "stop", 4)) {
		for (name = json_item_first(names); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_smplr_stop(name_s, &sctxt);
			if (rc == ENOENT) {
				rc = ldmsd_send_error(reqc, rc,
					"smplr '%s' does not exist.", name_s);
			} else if (rc == EBUSY) {
				rc = ldmsd_send_error(reqc, rc,
					"smplr '%s' not running", name_s);
			} else if (rc > 0) {
				rc = ldmsd_send_error(reqc, rc,
					"Failed to stop smplr '%s'", name_s);
			}
		}
	} else {
		for (name = json_item_first(names); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_smplr_del(name_s, &sctxt);
			if (rc == ENOENT) {
				rc = ldmsd_send_error(reqc, rc,
					"smplr '%s' does not exist.", name_s);
			} else if (rc == EBUSY) {
				rc = ldmsd_send_error(reqc, rc,
					"smplr '%s' is in use", name_s);
			} else if (rc > 0) {
				rc = ldmsd_send_error(reqc, rc,
					"Failed to delete smplr '%s'", name_s);
			}
		}
	}

	if (regex) {
		rc = ldmsd_send_error(reqc, ENOTSUP,
				"act_obj:smplr: 'regex' not supported. "
				"All the name in 'names' were processed.");
	} else {
		rc = ldmsd_send_error(reqc, 0, "");
	}
	return rc;
}

int __smplr_status_json_obj(ldmsd_req_ctxt_t reqc, ldmsd_smplr_t smplr)
{
	extern const char *smplr_state_str(enum ldmsd_smplr_state state);
	int rc;
	ldmsd_set_entry_t sent;
	ldmsd_sampler_type_t samp;
	int first = 1;

	ldmsd_smplr_lock(smplr);
	rc = ldmsd_append_response_va(reqc, 0,
		       "{ \"name\":\"%s\","
		       "\"plugin\":\"%s\","
		       "\"instance\":\"%s\","
		       "\"interval_us\":\"%ld\","
		       "\"offset_us\":\"%ld\","
		       "\"synchronous\":\"%d\","
		       "\"state\":\"%s\","
		       "\"sets\": [ ",
		       smplr->obj.name,
		       smplr->pi->plugin_name,
		       smplr->pi->inst_name,
		       smplr->interval_us,
		       smplr->offset_us,
		       smplr->synchronous,
		       smplr_state_str(smplr->state)
		       );
	if (rc)
		goto out;
	samp = LDMSD_SAMPLER(smplr->pi);
	first = 1;
	LIST_FOREACH(sent, &samp->set_list, entry) {
		const char *name = ldms_set_instance_name_get(sent->set);
		rc = ldmsd_append_response_va(reqc, 0, "%s\"%s\"", first?"":",", name);
		if (rc)
			goto out;
		first = 0;
	}
	rc = ldmsd_append_response_va(reqc, 0, "]}");
 out:
	ldmsd_smplr_unlock(smplr);
	return rc;
}

static int smplr_status_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	ldmsd_smplr_t smplr = NULL;
	char *name_s;
	int count;
	json_entity_t spec, name;

	spec = json_value_find(reqc->json, "spec");
	if (spec) {
		name = json_value_find(spec, "name");
		if (JSON_STRING_VALUE != json_entity_type(name)) {
			rc = ldmsd_send_type_error(reqc, "smplr_status:name", "a string");
			return rc;
		}
		name_s = json_value_str(name)->str;
		smplr = ldmsd_smplr_find(name_s);
		if (!smplr) {
			/* Do not report any status */
			rc = ldmsd_send_error(reqc, ENOENT,
					"smplr '%s' doesn't exist.", name_s);
			return rc;
		}
	}
	rc = ldmsd_append_info_obj_hdr(reqc, "smplr_status");
	if (rc)
		goto out;
	rc = ldmsd_append_response(reqc, 0, "[", 1);
	if (rc)
		goto out;
	if (smplr) {
		rc = __smplr_status_json_obj(reqc, smplr);
		if (rc)
			goto out;
		ldmsd_smplr_put(smplr);
	} else {
		ldmsd_cfg_lock(LDMSD_CFGOBJ_SMPLR);
		/* Determine the attribute value length */
		count = 0;
		for (smplr = ldmsd_smplr_first(); smplr;
				smplr = ldmsd_smplr_next(smplr)) {
			if (count) {
				rc = ldmsd_append_response(reqc, 0, ",", 1);
				if (rc)
					goto unlock_out;
			}
			rc = __smplr_status_json_obj(reqc, smplr);
			if (rc)
				goto unlock_out;
			count++;
		}
		ldmsd_cfg_unlock(LDMSD_CFGOBJ_SMPLR);
	}


	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}", 2);

unlock_out:
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_SMPLR);
out:
	if (smplr)
		ldmsd_smplr_put(smplr);
	return rc;
}

static int plugin_status_handler(ldmsd_req_ctxt_t reqc)
{
	int rc, count;
	ldmsd_plugin_inst_t inst;
	json_entity_t qr = NULL;
	json_entity_t val;
	jbuf_t jb = NULL;

	rc = ldmsd_append_response(reqc, LDMSD_REC_SOM_F, "[", 1);
	if (rc)
		goto out;
	count = 0;
	LDMSD_PLUGIN_INST_FOREACH(inst) {
		if (count) {
			rc = ldmsd_append_response(reqc, 0, ",\n", 1);
			if (rc)
				goto out;
		}
		count++;
		qr = inst->base->query(inst, "status");
		if (!qr) {
			rc = ENOMEM;
			goto out;
		}
		val = json_value_find(qr, "rc");
		if (!val) {
			ldmsd_log(LDMSD_LERROR, "The status JSON dict of "
					"plugin instance '%s' does not contain "
					"the attribute 'rc'.\n", inst->inst_name);
			rc = ldmsd_send_error(reqc, EINTR,
					"Failed to get the status of '%s'",
					inst->inst_name);
			goto out;
		}
		rc = json_value_int(val);
		if (rc) {
			rc = ldmsd_send_error(reqc, rc,
					"Failed to get the status of '%s'",
					inst->inst_name);
			goto out;
		}

		jb = json_entity_dump(NULL, json_value_find(qr, "status"));
		if (!jb) {
			rc = ENOMEM;
			goto out;
		}
		rc = ldmsd_append_response_va(reqc, 0, "%s", jb->buf);
		if (rc)
			goto out;
	}
	rc = ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]", 1);
out:
	if (qr)
		json_entity_free(qr);
	if (jb)
		jbuf_free(jb);
	return rc;
}

static int __query_inst(ldmsd_req_ctxt_t reqc, const char *query,
			ldmsd_plugin_inst_t inst)
{
	int rc = 0;
	json_entity_t qr = NULL;
	json_entity_t ra;
	jbuf_t jb = NULL;
	qr = inst->base->query(inst, query);
	if (!qr) {
		rc = ldmsd_send_error(reqc, ENOMEM, "cmd_obj:plugin_query - "
				"Failed to query '%s' from plugin instance '%s'.",
				query, inst->inst_name);
		return (rc)?rc:ENOMEM;
	}
	rc = json_value_int(json_attr_value(json_attr_find(qr, "rc")));
	if (rc) {
		rc = ldmsd_send_error(reqc, rc, "cmd_obj:plugin_query - "
				"Failed to query '%s' from plugin instance '%s'.",
				query, inst->inst_name);
	}
	ra = json_attr_find(qr, (char *)query);
	if (ra) {
		jb = json_entity_dump(NULL, json_attr_value(ra));
		if (!jb) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		}
		rc = ldmsd_append_response_va(reqc, 0, "%s", jb->buf);
	} else {
		rc = ldmsd_append_response_va(reqc, 0,
				"{\"plugin\":\"%s\",\"name\":\"%s\"}",
				inst->plugin_name, inst->inst_name);
	}

	if (rc)
		return rc;

	if (qr)
		json_entity_free(qr);
	if (jb)
		jbuf_free(jb);
	return rc;
}

static int plugin_query_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t spec, value;
	char *query;
	char *name = NULL;
	int count;
	ldmsd_plugin_inst_t inst;

	spec = json_value_find(reqc->json, "spec");
	if (!spec) {
		return ldmsd_send_missing_attr_err(reqc, "cmd_obj:plugin_query", "spec");
	}

	/* query */
	value = json_value_find(spec, "query");
	if (!value) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:plugin_query:spec", "query");
	}
	if (JSON_STRING_VALUE != json_entity_type(value)) {
		return ldmsd_send_type_error(reqc,
				"cmd_obj:plugin_query:spec:query", "a string");
	}
	query = json_value_str(value)->str;

	/* name */
	value = json_value_find(spec, "name");
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
					"cmd_obj:plugin_sets:spec:plugin", "a string");
		}
		name = json_value_str(value)->str;
	}

	rc = ldmsd_append_info_obj_hdr(reqc, "plugin_query");
	if (rc)
		return rc;

	rc = ldmsd_append_response(reqc, 0, "[", 1);
	if (rc)
		return rc;

	count = 0;
	if (name) {
		inst = ldmsd_plugin_inst_find(name);
		if (!inst) {
			return ldmsd_send_error(reqc, ENOENT, "cmd_obj:plugin_sets - "
				"Plugin instance '%s' not found", name);
		}
		rc = __query_inst(reqc, query, inst);
		ldmsd_plugin_inst_put(inst);
		if (rc)
			return rc;
	} else {
		/*  query all instances */
		LDMSD_PLUGIN_INST_FOREACH(inst) {
			if (count) {
				rc = ldmsd_append_response(reqc, 0, ",", 1);
				if (rc) {
					ldmsd_plugin_inst_put(inst);
					return rc;
				}
			}
			count++;
			rc = __query_inst(reqc, query, inst);
			if (rc) {
				ldmsd_plugin_inst_put(inst);
				return rc;
			}
		}
	}

	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}", 2);
}

static int plugin_instance_handler(ldmsd_req_ctxt_t reqc)
{
	int rc = 0;
	json_entity_t spec, plugin, name, config, item;
	ldmsd_plugin_inst_t pi;
	char *plugin_s, *name_s;

	spec = json_value_find(reqc->json, "spec");
	name = json_value_find(spec, "name");
	if (!name) {
		rc = ldmsd_send_missing_attr_err(reqc, "cfg_obj:plugin_instance", "name");
		return rc;
	}
	if (JSON_STRING_VALUE != json_entity_type(name)) {
		rc = ldmsd_send_type_error(reqc, "plugin_instance:name", "a string");
		return rc;
	}
	name_s = json_value_str(name)->str;

	plugin = json_value_find(spec, "plugin");
	if (!plugin) {
		plugin_s = name_s;
	} else if (JSON_STRING_VALUE != json_entity_type(plugin)) {
		rc = ldmsd_send_type_error(reqc, "plugin_instance:plugin", "a string");
		return rc;
	} else {
		plugin_s = json_value_str(plugin)->str;
	}

	config = json_value_find(spec, "config");
	if (!config) {
		rc = ldmsd_send_missing_attr_err(reqc,
				"cfg_obj:plugin_instance", "config");
		return rc;
	}
	if ((JSON_DICT_VALUE != json_entity_type(config)) &&
			(JSON_LIST_VALUE != json_entity_type(config))) {
		/*
		 * The JSON_LIST_VALUE is supported because some plugins
		 * need to be configured multiple times in some setup, e.g.,
		 * test_sampler. These plugins MUST be redesigned on how they
		 * should be configured.
		 */
		rc = ldmsd_send_type_error(reqc, "plugin_instance:config",
				"a dictionary or a list of dictionaries");
		return rc;
	}
	/* reuse the receive buffer for an underlying error message */
	ldmsd_req_buf_reset(reqc->recv_buf);

	pi = ldmsd_plugin_inst_load(name_s, plugin_s,
			reqc->recv_buf->buf, reqc->recv_buf->len);
	if (!pi) {
		rc = ldmsd_send_error(reqc, errno, "%s", reqc->recv_buf->buf);
		return rc;
	}

	/*
	 * TODO: process 'perm' attribute
	 */


	/* Add the config dictionary */
	/*
	 * TODO: resolve this.
	 *
	 * Is it possible that a plugin instance will need multiple config object?
	 * I hope not. Any plugin that is required to be configured with multiple
	 * config lines should be re-designed so that all information will be
	 * contained in the plugin instance cfgobj.
	 *
	 * Before this change, the config json object of a plugin instance
	 * is designed to be a list of dictionary. Each dictionary represents
	 * a config line.
	 *
	 * TODO: change the list of dictionaries design to be just a dictionary.
	 *
	 */
	if (JSON_LIST_VALUE == json_entity_type(config)) {
		for (item = json_item_first(config); item; item = json_item_next(item)) {
			if (JSON_DICT_VALUE != json_entity_type(item)) {
				return ldmsd_send_type_error(reqc,
					"cfg_obj:plugin_instance:config",
					"a dictionary or a list of dictionaries.");
			}
		}
	}

	/*
	 * TODO: I hate this. I need to defer configuring plugin instances
	 * because sampler plugins create LDMS sets at config time.
	 *
	 * I strongly believe that the sampler plugin interface should
	 * include 'start()' which will create LDMS sets as configured and schedule
	 * the next sampling.
	 *
	 * The 'start()' will be called when the corresponding sampler policy
	 * is started.
	 */
	if (!ldmsd_is_initialized()) {
		ldmsd_deferred_pi_config_t cfg;
		cfg = ldmsd_deferred_pi_config_new(name_s, config,
							reqc->rem_key.msg_no,
							reqc->xprt->file.filename);
		if (!cfg) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		}
	} else {
		if (JSON_LIST_VALUE == json_entity_type(config)) {
			for (item = json_item_first(config); item;
					item = json_item_next(item)) {
				rc = ldmsd_plugin_inst_config(pi, item,
							reqc->recv_buf->buf,
							reqc->recv_buf->len);
				/*
				 * Send the config error to the requester.
				 */
				if (rc) {
					rc = ldmsd_send_error(reqc, rc, "%s",
							reqc->recv_buf->buf);
					return rc;
				}
			}
		} else {
			rc = ldmsd_plugin_inst_config(pi, config,
					reqc->recv_buf->buf, reqc->recv_buf->len);
			if (rc) {
				rc = ldmsd_send_error(reqc, rc, "%s",
						reqc->recv_buf->buf);
				return rc;
			}
		}

	}
	rc = ldmsd_send_error(reqc, 0, "");
	return rc;
}

extern int ldmsd_term_plugin(const char *name);
static int plugin_instance_action_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t action, names, name, regex;
	char *name_s, *action_s;

	action = json_value_find(reqc->json, "action");
	action_s = json_value_str(action)->str;
	names = json_value_find(reqc->json, "names");
	regex = json_value_find(reqc->json, "regex");

	if (regex) {
		return ldmsd_send_error(reqc, ENOTSUP,
			"act_obj:updtr does not support 'regex'");
	}

	if (0 == strncmp(action_s, "delete", 6)) {
		for (name = json_item_first(names); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_term_plugin(name_s);
			if (rc == ENOENT) {
				rc = ldmsd_send_error(reqc, rc,
					"plugin '%s' not found.", name_s);
			} else if (rc != 0){
				rc = ldmsd_send_error(reqc, rc,
					"Failed to terminate the plugin '%s'.",
						name_s);
			}
		}
	} else {
		rc = ldmsd_send_error(reqc, ENOTSUP, "plugin instance: action "
				"'%s' not supported.", action_s);
	}
	if (rc)
		return rc;
	return ldmsd_send_error(reqc, 0, NULL);
}

//static int plugn_config_handler(ldmsd_req_ctxt_t reqc)
//{
//	char *name, *config_attr, *attr_name;
//	name = config_attr = NULL;
//	ldmsd_plugin_inst_t inst = NULL;
//	char *token, *next_token, *ptr;
//	json_entity_t d, a;
//	int rc = 0;
//
//	reqc->errcode = 0;
//	d = NULL;
//
//	attr_name = "name";
//	name = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_NAME);
//	if (!name)
//		goto einval;
//	config_attr = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_STRING);
//
//	d = json_entity_new(JSON_DICT_VALUE);
//	if (!d)
//		goto enomem;
//
//	if (config_attr) {
//		for (token = strtok_r(config_attr, " \t\n", &ptr); token;) {
//			char *value = strchr(token, '=');
//			if (value) {
//				value[0] = '\0';
//				value++;
//			} else {
//				value = "";
//			}
//			a = json_entity_new(JSON_ATTR_VALUE,
//					json_entity_new(JSON_STRING_VALUE, token),
//					json_entity_new(JSON_STRING_VALUE, value));
//			if (!a)
//				goto enomem;
//			json_attr_add(d, a);
//			next_token = strtok_r(NULL, " \t\n", &ptr);
//			token = next_token;
//		}
//	}
//
//	inst = ldmsd_plugin_inst_find(name);
//	if (!inst) {
//		linebuf_printf(reqc, "Instance not found: %s", name);
//		reqc->errcode = ENOENT;
//		goto send_reply;
//	}
//
//	if (reqc->flags & LDMSD_REQ_DEFER_FLAG) {
//		ldmsd_deferred_pi_config_t cfg;
//		cfg = ldmsd_deferred_pi_config_new(name, d,
//							reqc->key.msg_no,
//							reqc->xprt->file.filename);
//		if (!cfg) {
//			ldmsd_log(LDMSD_LERROR, "Memory allocation failure\n");
//			goto enomem;
//		}
//	} else {
//		reqc->errcode = ldmsd_plugin_inst_config(inst, d,
//							 reqc->recv_buf,
//							 reqc->recv_len);
//		/* if there is an error, the plugin should already fill line_buf */
//	}
//	goto send_reply;
//
//einval:
//	reqc->errcode = EINVAL;
//	(void)snprintf(reqc->recv_buf, reqc->recv_len,
//		 "The attribute '%s' is required by config.", attr_name);
//	goto send_reply;
//enomem:
//	rc = reqc->errcode = ENOMEM;
//	(void)snprintf(reqc->recv_buf, reqc->recv_len, "Out of memory");
//send_reply:
//	ldmsd_send_req_response(reqc, reqc->recv_buf);
//	if (name)
//		free(name);
//	if (config_attr)
//		free(config_attr);
//	if (d)
//		json_entity_free(d);
//	if (inst)
//		ldmsd_plugin_inst_put(inst); /* put ref from find */
//	return rc;
//}
//
extern struct plugin_list plugin_list;
int __plugn_list_string(ldmsd_req_ctxt_t reqc)
{
	json_entity_t spec, plugin;
	char *plugin_s = NULL;;
	const char *desc;
	int rc, count = 0;
	ldmsd_plugin_inst_t inst;
	rc = 0;

	spec = json_value_find(reqc->json, "spec");
	if (spec) {
		plugin = json_value_find(spec, "plugin");
		if (plugin) {
			if (JSON_STRING_VALUE != json_entity_type(plugin)) {
				return ldmsd_send_type_error(reqc,
					"cfg_obj:plugin_list:plugin", "a string");
			}
			plugin_s = json_value_str(plugin)->str;
		}
	}

	LDMSD_PLUGIN_INST_FOREACH(inst) {
		if (plugin_s && strcmp(plugin_s, inst->type_name)
			   && strcmp(plugin_s, inst->plugin_name)) {
			/* does not match type nor plugin name */
			continue;
		}
		if (count) {
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				return rc;
		}
		desc = ldmsd_plugin_inst_desc(inst);
		if (!desc)
			desc = inst->plugin_name;
		rc = ldmsd_append_response_va(reqc, 0,
				"{\"%s\": \"%s\"}", inst->inst_name, desc);
		if (rc)
			goto out;
		count++;
	}

	if (!count) {
		if (plugin) {
			return ldmsd_send_error(reqc, ENOENT,
					"cmd_obj:plugin_list - "
					"Plugin instance '%s' not found.",
					plugin_s);
		}
	}
out:
	return rc;
}

static int plugin_list_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;

	rc = ldmsd_append_info_obj_hdr(reqc, "plugin_list");
	if (rc)
		return rc;
	rc = ldmsd_append_response(reqc, 0, "[", 1);
	if (rc)
		return rc;

	rc = __plugn_list_string(reqc);
	if (rc)
		return rc;
	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}", 2);
}

static int plugin_usage_handler(ldmsd_req_ctxt_t reqc)
{
	int rc = 0;
	json_entity_t spec, value;
	char *name = NULL;
	char *type = NULL;
	const char *usage = NULL;
	char *str;
	ldmsd_plugin_inst_t inst = NULL;

	spec = json_value_find(reqc->json, "spec");
	if (!spec) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:plugin_usage", "spec");
	}

	/* name */
	value = json_value_find(spec, "name");
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
				"cmd_obj:plugin_usage:spec:name", "a string");
		}
		name = json_value_str(value)->str;
	}

	/* type */
	value = json_value_find(spec, "type");
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
				"cmd_obj:plugin_usage:spec:type", "a string");
		}
		type = json_value_str(value)->str;
	}

	if (!name && !type) {
		return ldmsd_send_error(reqc, EINVAL, "cmd_obj:plugin_query:spec - "
				"Either 'name' or 'type' must be given.");
	}

	rc = ldmsd_append_info_obj_hdr(reqc, "plugin_usage");
	if (rc)
		return rc;

	rc = ldmsd_append_response(reqc, 0, "{", 1);
	if (rc)
		return rc;

	if (name) {
		inst = ldmsd_plugin_inst_find(name);
		if (!inst) {
			return ldmsd_send_error(reqc, ENOENT,
					"cmd_obj:plugin_usage - Plugin instance "
					"'%s' not found.", name);
		}
		usage = ldmsd_plugin_inst_help(inst);
		if (!usage) {
			rc = ldmsd_send_error(reqc, ENOSYS, "cmd_obj:plugin_usage - "
						"'%s' has no usage.", name);
			goto out;
		}
		rc = ldmsd_append_response_va(reqc, 0, "\"%s\":\"%s\"", name, usage);
		if (rc)
			goto out;
	}

	if (type) {
		if (name) {
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				goto out;
		}
		if (0 == strcmp(type, "true")) {
			if (!name) {
				rc = ldmsd_send_error(reqc, EINVAL,
					"cmd_obj:plugin_query - "
					"The 'name' must be given "
					"if 'type' is true.");
				goto out;
			}
			type = (char *)inst->type_name;
		}
		if (0 == strcmp(type, "sampler")) {
			usage = ldmsd_sampler_help();
			str = "\"Common attributes of sampler plugin instances\":";
			rc = ldmsd_append_response(reqc, 0, str, strlen(str));
			if (rc)
				goto out;
			rc = ldmsd_append_response_va(reqc, 0, "%s", usage);
			if (rc)
				goto out;
		} else if (0 == strcmp(type, "store")) {
			usage = ldmsd_store_help();
			str = "\"Common attributes of store plugin instance\":";
			rc = ldmsd_append_response(reqc, 0, str, strlen(str));
			if (rc)
				goto out;
			rc = ldmsd_append_response_va(reqc, 0, "%s", usage);
			if (rc)
				goto out;
		} else if (0 == strcmp(type, "all")) {
			usage = ldmsd_sampler_help();
			str = "\"Common attributes of sampler plugin instances\":";
			rc = ldmsd_append_response(reqc, 0, str, strlen(str));
			if (rc)
				goto out;
			rc = ldmsd_append_response_va(reqc, 0, "%s", usage);
			if (rc)
				goto out;

			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				goto out;

			usage = ldmsd_store_help();
			str = "\"Common attributes of store plugin instance\":";
			rc = ldmsd_append_response(reqc, 0, str, strlen(str));
			if (rc)
				goto out;
			rc = ldmsd_append_response_va(reqc, 0, "%s", usage);
			if (rc)
				goto out;
		} else {
			rc = ldmsd_send_error(reqc, EINVAL,
					"cmd_obj:plugin_usage - "
					"Unrecognized type '%s'", type);
			if (rc)
				goto out;
		}
	}

	rc = ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "}}", 2);

out:
	if (inst)
		ldmsd_plugin_inst_put(inst); /* put ref from find */
	return rc;
}

/* Caller must hold the set tree lock. */
int __plugn_sets_json_obj(ldmsd_req_ctxt_t reqc,
				ldmsd_plugin_inst_t inst)
{
	ldmsd_set_entry_t ent;
	ldmsd_sampler_type_t samp = (void*)inst->base;
	int rc, set_count;
	rc = ldmsd_append_response_va(reqc, 0,
			"{"
			"\"plugin\":\"%s\","
			"\"sets\":[",
			inst->inst_name);
	if (rc)
		return rc;
	set_count = 0;
	LIST_FOREACH(ent, &samp->set_list, entry) {
		if (set_count) {
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				return rc;
		}
		rc = ldmsd_append_response_va(reqc, 0, "\"%s\"",
			       ldms_set_instance_name_get(ent->set));
		if (rc)
			return rc;
		set_count++;
	}
	return ldmsd_append_response(reqc, 0, "]}", 2);
}

static int plugin_sets_handler(ldmsd_req_ctxt_t reqc)
{
	int rc = 0;
	json_entity_t spec, plugin;
	char *name = NULL;
	int plugn_count;
	ldmsd_plugin_inst_t inst;

	rc = ldmsd_append_info_obj_hdr(reqc, "plugn_sets");
	if (rc)
		return rc;

	rc = ldmsd_append_response(reqc, 0, "[", 1);
	if (rc)
		return rc;
	spec = json_value_find(reqc->json, "spec");
	if (spec) {
		plugin = json_value_find(spec, "plugin");
		if (plugin) {
			if (JSON_STRING_VALUE != json_entity_type(plugin))
				return ldmsd_send_type_error(reqc,
					"cmd_obj:plugin_sets:spec:plugin", "a string");
			name = json_value_str(plugin)->str;
		}
	}
	if (name) {
		inst = ldmsd_plugin_inst_find(name);
		if (!inst) {
			return ldmsd_send_error(reqc, ENOENT, "cmd_obj:plugin_sets - "
					"Plugin instance '%s' not found", name);
		}
		rc = __plugn_sets_json_obj(reqc, inst);
		ldmsd_plugin_inst_put(inst);
		if (rc)
			return rc;
	} else {
		plugn_count = 0;
		LDMSD_PLUGIN_INST_FOREACH(inst) {
			if (strcmp(inst->type_name, "sampler"))
				continue; /* skip non-sampler instance */
			if (plugn_count) {
				rc = ldmsd_append_response(reqc, 0, ",", 1);
				if (rc) {
					ldmsd_plugin_inst_put(inst);
					return rc;
				}
			}
			rc = __plugn_sets_json_obj(reqc, inst);
			if (rc) {
				ldmsd_plugin_inst_put(inst);
				return rc;
			}

			plugn_count += 1;
		}
	}
	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}", 2);
}

extern int ldmsd_set_udata(const char *set_name, const char *metric_name,
			   int64_t udata, ldmsd_sec_ctxt_t sctxt);
extern int ldmsd_set_udata_regex(char *set_name, char *regex_str,
		int64_t base, int64_t inc, char *er_str, size_t errsz,
		ldmsd_sec_ctxt_t sctxt);
static int set_udata_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t spec, value, udata;
	char *set_name, *metric_name, *regex;
	set_name = metric_name = regex = NULL;
	int64_t v, base, incr;
	struct ldmsd_sec_ctxt sctxt;

	spec = json_value_find(reqc->json, "spec");
	if (!spec) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:set_udata", "spec");
	}

	/* set instance */
	value = json_value_find(spec, "set_instance");
	if (!value) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:set_udata:spec", "set_instance");
	}
	if (JSON_STRING_VALUE != json_entity_type(value)) {
		return ldmsd_send_type_error(reqc,
				"cmd_obj:set_udata:spec:set_instance", "a string");
	}
	set_name = json_value_str(value)->str;

	/* metric */
	value = json_value_find(spec, "metric");
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
					"cmd_obj:set_udata:spec:metric", "a string");
		}
		metric_name = json_value_str(value)->str;
	}

	/* regex */
	value = json_value_find(spec, "regex");
	if (value) {
		if (JSON_STRING_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
					"cmd_obj:set_udata:spec:regex", "a string");
		}
		regex = json_value_str(value)->str;
	}

	if (!metric_name && !regex) {
		return ldmsd_send_error(reqc, EINVAL, "cmd_obj:set_udata - "
				"Either 'metric' or 'regex' must be given.");
	}

	/* udata */
	udata = json_value_find(spec, "udata");
	if (!udata) {
		if (!udata) {
			return ldmsd_send_missing_attr_err(reqc,
					"cmd_obj:set_udata:spec", "udata");
		}
		if (JSON_DICT_VALUE != json_entity_type(udata)) {
			return ldmsd_send_type_error(reqc,
					"cmd_obj:set_udata:spec:udata", "a dictionary");
		}
	}

	/* udata value */
	value = json_value_find(udata, "value");
	if (!value && metric_name) {
		return ldmsd_send_error(reqc, EINVAL,
			"cmd_obj:set_udata - If 'metric' is given, "
			"the 'udata:value' must be given.");
	}
	if (value) {
		if (JSON_INT_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
				"cmd_obj:set_udata:spec:udata:value", "an integer");
		}
		v = json_value_int(value);
	}

	/* base */
	value = json_value_find(udata, "base");
	if (!value && regex) {
		return ldmsd_send_error(reqc, EINVAL,
			"cmd_obj:set_udata - If 'regex' is given, "
			"the 'udata:base' must be given.");
	}
	if (value) {
		if (JSON_INT_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
				"cmd_obj:set_udata:spec:udata:base", "an integer");
		}
		base = json_value_int(value);
	}

	/* increment */
	value = json_value_find(udata, "increment");
	if (value && metric_name) {
		return ldmsd_send_error(reqc, EINVAL,
			"cmd_obj:set_udata - The 'increment' is ignored because "
			"'metric' is given.");
	}
	if (value) {
		if (JSON_INT_VALUE != json_entity_type(value)) {
			return ldmsd_send_type_error(reqc,
				"cmd_obj:set_udata:spec:udata:base", "an integer");
		}
		incr = json_value_int(value);
	} else {
		incr = 0;
	}

	ldmsd_req_ctxt_sec_get(reqc, &sctxt);

	if (metric_name) {
		rc = ldmsd_set_udata(set_name, metric_name, v, &sctxt);
		switch (rc) {
		case 0:
			break;
		case EACCES:
			return ldmsd_send_error(reqc, rc, "cmd_obj:set_udata - "
					"Metric '%s' in set '%s': "
					"Permission denied.",
					metric_name, set_name);
		case ENOENT:
			return ldmsd_send_error(reqc, rc, "cmd_obj:set_udata - "
					"Set '%s' not found.", set_name);
		case -ENOENT:
			return ldmsd_send_error(reqc, rc, "cmd_obj:set_udata - "
					"Metric '%s' not found in Set '%s'.",
					metric_name, set_name);
		case EINVAL:
			return ldmsd_send_error(reqc, rc, "cmd_obj:set_udata - "
					"User data '%s' is invalid.", v);
		default:
			return ldmsd_send_error(reqc, rc, "cmd_obj:set_udata - "
					"Failed to set udata '%s' of metric '%s' "
					"in set '%s': %s", v, metric_name, set_name,
					ovis_errno_abbvr(rc));
		}
	} else {
		/* regex */
		ldmsd_req_buf_reset(reqc->recv_buf);
		rc = ldmsd_set_udata_regex(set_name, regex, base, incr,
							reqc->recv_buf->buf,
							reqc->recv_buf->len,
							&sctxt);
		if (rc) {
			return ldmsd_send_error(reqc, rc, "cmd_obj:set_udata - "
					"%s", reqc->recv_buf->buf);
		}
	}
	return ldmsd_send_error(reqc, 0, NULL);
}

static int verbosity_change_handler(ldmsd_req_ctxt_t reqc)
{
	char *level_s;
	json_entity_t spec, level, test;

	spec = json_value_find(reqc->json, "spec");
	if (!spec) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:verbosity_change", "spec:level");
	}
	level = json_value_find(spec, "level");
	if (!level) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:verbosity_change:spec", "level");
	}
	if (JSON_STRING_VALUE != json_entity_type(level)) {
		return ldmsd_send_type_error(reqc,
			"cmd_obj:verbosity_change:spec:level", "a string");
	}
	level_s = json_value_str(level)->str;
	int rc = ldmsd_loglevel_set(level_s);
	if (rc < 0) {
		return ldmsd_send_error(reqc, EINVAL,
				"Invalid verbosity level '%s', expecting DEBUG, "
				"INFO, ERROR, CRITICAL and QUIET",
				level_s);
	}
	test = json_value_find(spec, "test");
	if (test) {
		ldmsd_log(LDMSD_LDEBUG, "TEST DEBUG\n");
		ldmsd_log(LDMSD_LINFO, "TEST INFO\n");
		ldmsd_log(LDMSD_LWARNING, "TEST WARNING\n");
		ldmsd_log(LDMSD_LERROR, "TEST ERROR\n");
		ldmsd_log(LDMSD_LCRITICAL, "TEST CRITICAL\n");
		ldmsd_log(LDMSD_LALL, "TEST ALWAYS\n");
	}

	return ldmsd_send_error(reqc, 0, NULL);
}

static int daemon_status_handler(ldmsd_req_ctxt_t reqc)
{
	int rc, i;
	extern pthread_t *ev_thread;
	extern int *ev_count;

	rc = ldmsd_append_info_obj_hdr(reqc, "daemon_status");
	if (rc)
		return rc;

	rc = ldmsd_append_response(reqc, 0, "[", 1);
	if (rc)
		return rc;
	for (i = 0; i < ldmsd_ev_thread_count_get(); i++) {
		if (i) {
			rc = ldmsd_append_response(reqc, 0, ",", 1);
			if (rc)
				return rc;
		}

		rc = ldmsd_append_response_va(reqc, 0,
				"{ \"thread\":\"%p\","
				"\"task_count\":\"%d\"}",
				(void *)ev_thread[i], ev_count[i]);
		if (rc)
			return rc;
	}
	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}", 2);
}

static int version_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	struct ldms_version ldms_version;
	struct ldmsd_version ldmsd_version;

	ldms_version_get(&ldms_version);

	rc = ldmsd_append_info_obj_hdr(reqc, "version");
	if (rc)
		return rc;
	rc = ldmsd_append_response_va(reqc, 0,
			"{\"LDMS Version\": \"%hhu.%hhu.%hhu.%hhu\",",
			ldms_version.major, ldms_version.minor,
			ldms_version.patch, ldms_version.flags);
	if (rc)
		return rc;

	ldmsd_version_get(&ldmsd_version);
	rc = ldmsd_append_response_va(reqc, 0,
			"\"LDMSD Version\": \"%hhu.%hhu.%hhu.%hhu\"}",
			ldmsd_version.major, ldmsd_version.minor,
			ldmsd_version.patch, ldmsd_version.flags);
	return ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "}", 1);
}

/*
 * The tree contains environment variables given in
 * configuration files or via ldmsd_controller/ldmsctl.
 */
int env_cmp(void *a, const void *b)
{
	return strcmp(a, b);
}
struct rbt env_tree = RBT_INITIALIZER(env_cmp);
pthread_mutex_t env_tree_lock  = PTHREAD_MUTEX_INITIALIZER;

struct env_node {
	char *name;
	struct rbn rbn;
};

static void env_node_del(struct env_node *node)
{
	free(node->name);
	free(node);
}

static int env_node_new(const char *name, struct rbt *tree, pthread_mutex_t *lock)
{
	struct env_node *env_node;
	struct rbn *rbn;

	rbn = rbt_find(tree, name);
	if (rbn) {
		/* The environment variable is already recorded.
		 * Its value will be retrieved by calling getenv().
		 */
		return EEXIST;
	}

	env_node = malloc(sizeof(*env_node));
	if (!env_node)
		return ENOMEM;
	env_node->name = strdup(name);
	if (!env_node->name)
		return ENOMEM;
	rbn_init(&env_node->rbn, env_node->name);
	if (lock) {
		pthread_mutex_lock(lock);
		rbt_ins(tree, &env_node->rbn);
		pthread_mutex_unlock(lock);
	} else {
		rbt_ins(tree, &env_node->rbn);
	}

	return 0;
}

/*
 * { "obj" : "env",
 *   "spec": { "name": <env name string>,
 *             "value": <env value string>
 *           }
 * }
 */

static int env_handler(ldmsd_req_ctxt_t reqc)
{
	int rc = 0;
	json_entity_t spec;
	json_entity_t name;
	json_entity_t value;
	char *name_s, *val_s;
	char *exp_val = NULL;

	spec = json_value_find(reqc->json, "spec");
	name = json_value_find(spec, "name");
	if (!name) {
		rc = ldmsd_send_missing_attr_err(reqc, "env", "name");
		return rc;
	}
	if (JSON_STRING_VALUE != json_entity_type(name)) {
		rc = ldmsd_send_type_error(reqc, "env:spec:name", "a string");
		return rc;
	}
	name_s = json_value_str(name)->str;

	value = json_value_find(spec, "value");
	if (!value) {
		rc = ldmsd_send_missing_attr_err(reqc, "env", "value");
		return rc;
	}
	if (JSON_STRING_VALUE != json_entity_type(value)) {
		rc = ldmsd_send_type_error(reqc, "env:spec:value", "a string");
		return rc;
	}
	val_s = json_value_str(value)->str;

	if (reqc->xprt->trust) {
		/*
		 * We trust to expand the commands.
		 */
		exp_val = str_repl_cmd(val_s);
		if (!exp_val) {
			rc = errno;
			rc = ldmsd_send_error(reqc, rc,
				"Failed to expand the string '%s'", val_s);
			return rc;
		}
		val_s = exp_val;
	}

	rc = setenv(name_s, val_s, 1);
	if (rc) {
		rc = errno;
		rc = ldmsd_send_error(reqc, rc,
			"Failed to set the env: %s=%s", name_s, exp_val);
		free(exp_val);
		return rc;
	}

	/*
	 * Record the set environment variable for exporting purpose
	 */
	rc = env_node_new(name_s, &env_tree, &env_tree_lock);
	if (rc == ENOMEM) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory.\n");
		return ENOMEM;
	} else if (rc == EEXIST) {
		ldmsd_log(LDMSD_LINFO, "Reset the env: %s=%s\n",
				name_s, val_s);
		rc = 0;
	} else if (rc == 0) {
		ldmsd_log(LDMSD_LINFO, "Set the env: %s=%s",
				name_s, val_s);
	}

	rc = ldmsd_send_error(reqc, 0, NULL);
	if (exp_val)
		free(exp_val);
	return 0;
}

static int include_handler(ldmsd_req_ctxt_t reqc)
{
	json_entity_t path, spec;
	char *path_s = NULL;
	int rc = 0;

	spec = json_value_find(reqc->json, "spec");
	path = json_value_find(spec, "path");
	if (!path)
		return ldmsd_send_missing_attr_err(reqc, "cmd:include", "path");
	if (JSON_STRING_VALUE != json_entity_type(path))
		return ldmsd_send_type_error(reqc, "cmd:include:path", "a string");
	path_s = json_value_str(path)->str;
	rc = process_config_file(path_s, reqc->xprt->trust);
	if (rc) {
		/*
		 * The actual error is always reported to the log file
		 * with the line number of the config file path.
		 */
		if (LDMSD_CFG_XPRT_CONFIG_FILE != reqc->xprt->type) {
			rc = ldmsd_send_error(reqc, rc,
				"An error occurs when including the config file %s. "
				"Please see the detail in the log file.",
				path_s);
		} else {
			rc = ldmsd_send_error(reqc, rc, "Failed to include "
						"the config file '%s'", path_s);
		}
	}

	return rc;
}
//
//static int oneshot_handler(ldmsd_req_ctxt_t reqc)
//{
//	char *name, *time_s, *attr_name;
//	name = time_s = NULL;
//	int rc = 0;
//	struct ldmsd_sec_ctxt sctxt;
//
//	name = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_NAME);
//	if (!name) {
//		attr_name = "name";
//		goto einval;
//	}
//	time_s = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_TIME);
//	if (!time_s) {
//		attr_name = "time";
//		goto einval;
//	}
//
//	ldmsd_req_ctxt_sec_get(reqc, &sctxt);
//	reqc->errcode = ldmsd_smplr_oneshot(name, time_s, &sctxt);
//	if (reqc->errcode) {
//		if ((int)reqc->errcode < 0) {
//			reqc->errcode = -reqc->errcode;
//			linebuf_printf(reqc, "Failed to get the current time. "
//					"Error %d", reqc->errcode);
//		} else if (reqc->errcode == EINVAL) {
//			linebuf_printf(reqc, "The given timestamp is in the past.");
//		} else if (reqc->errcode == ENOENT) {
//			linebuf_printf(reqc, "The sampler policy '%s' not found",
//									name);
//		} else {
//			linebuf_printf(reqc, "Failed to do the oneshot. Error %d",
//								reqc->errcode);
//		}
//	}
//	goto send_reply;
//
//einval:
//	reqc->errcode = EINVAL;
//	Snprintf(&reqc->recv_buf, &reqc->recv_len,
//			"The attribute '%s' is required by oneshot.",
//			attr_name);
//
//send_reply:
//	ldmsd_send_req_response(reqc, reqc->recv_buf);
//	if (name)
//		free(name);
//	if (time_s)
//		free(time_s);
//	return rc;
//}
//
//extern int ldmsd_logrotate();
//static int logrotate_handler(ldmsd_req_ctxt_t reqc)
//{
//	reqc->errcode = ldmsd_logrotate();
//	if (reqc->errcode) {
//		Snprintf(&reqc->recv_buf, &reqc->recv_len,
//				"Failed to rotate the log file. %s",
//				strerror(reqc->errcode));
//	}
//	ldmsd_send_req_response(reqc, reqc->recv_buf);
//	return 0;
//}

static int daemon_exit_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	cleanup_requested = 1;
	ldmsd_log(LDMSD_LINFO, "User requested exit.\n");
	rc = ldmsd_append_info_obj_hdr(reqc, "exit_daemon");
	if (rc)
		return rc;
	return ldmsd_append_response_va(reqc, LDMSD_REC_EOM_F, "%s",
					"\"exit daemon request received.\"}");
}

extern size_t __get_remaining(size_t max_msg, ldmsd_req_buf_t buf);
static int
__greeting_rsp_multi_rec_handler(ldmsd_req_ctxt_t reqc, json_entity_t num_rec)
{
	int rc = 0;
	int curr = 0;
	uint64_t tot_num_rec;
	char *str;

	tot_num_rec = json_value_int(num_rec);
	if ((rc = ldmsd_append_info_obj_hdr(reqc, "greeting")))
		goto out;
	str = "{\"mode\":\"rsp_multi_rec\",\"records\":[";
	if ((rc = ldmsd_append_response(reqc, 0, str, strlen(str))))
		goto out;
	if ((rc = ldmsd_append_response_va(reqc, 0, "\"rec_%d", curr)))
		goto out;
	do {
		if (curr + 1 == tot_num_rec) {
			/* Pad to the end of the record */
			rc = ldmsd_append_response_va(reqc, LDMSD_REC_EOM_F, "%*s\"]}}",
				__get_remaining(reqc->xprt->max_msg, reqc->send_buf) - 4, "");
			goto out;
		}
		rc = ldmsd_append_response_va(reqc, 0, "%*s\"",
				__get_remaining(reqc->xprt->max_msg, reqc->send_buf) - 1, "");
		if (rc)
			goto out;
		curr++;
		/* \c *curr is assumed to be incremented by 1 */
		if ((rc = ldmsd_append_response_va(reqc, 0, ",\"rec_%d", curr)))
			goto out;
	} while (curr < tot_num_rec);

out:
	return rc;
}

static int
__greeting_echo_handler(ldmsd_req_ctxt_t reqc, json_entity_t v)
{
	int rc;
	int cnt = 0;
	char *str;
	json_entity_t item;
	jbuf_t jb = NULL;

	if ((rc = ldmsd_append_info_obj_hdr(reqc, "greeting")))
		goto out;
	str = "{\"mode\":\"echo\",\"list\":[";
	if ((rc = ldmsd_append_response(reqc, 0, str, strlen(str))))
		goto out;
	if (JSON_LIST_VALUE != json_entity_type(v)) {
		ldmsd_log(LDMSD_LINFO, "greeting handler: Received a wrong JSON type\n");
		rc = ldmsd_send_error(reqc, EINVAL, "The JSON object of "
				"the 'value' attribute has type '%s' "
				"instead of '%s'\n", json_type_str(json_entity_type(v)),
				json_type_str(JSON_LIST_VALUE));
		if (rc)
			goto out;
	}
	for (item = json_item_first(v); item; item = json_item_next(item)) {
		if (cnt) {
			if ((rc = ldmsd_append_response(reqc, 0, ",", 1)))
				goto out;
		}
		jb = json_entity_dump(jb, item);
		if (!jb) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			goto out;
		}
		if ((rc = ldmsd_append_response_va(reqc, 0, "%s", jb->buf)))
			goto out;
		jbuf_free(jb);
		jb = NULL;
		cnt++;
	}
	rc = ldmsd_append_response(reqc, LDMSD_REC_EOM_F, "]}}", 3);
out:
	return rc;
}

typedef struct greeting_ping_pong_ctxt *greeting_ping_pong_ctxt_t;
struct greeting_ping_pong_ctxt {
	ldmsd_req_ctxt_t org_req;
	ldmsd_req_ctxt_t req;
	json_str_t s;
	int round;
	greeting_ping_pong_ctxt_t prev;
};

static void __cleanup_greeting_ping_pong_ctxt(greeting_ping_pong_ctxt_t ctxt)
{
	greeting_ping_pong_ctxt_t prev;
	ldmsd_req_ctxt_ref_put(ctxt->org_req, "greeting_ctxt:ref");
	do {
		prev = ctxt->prev;
		free(ctxt);
		ctxt = prev;
	} while (ctxt);
}

static int __greeting_ping_pong_response(ldmsd_req_ctxt_t reqc);
static int __greeting_ping_pong_request(greeting_ping_pong_ctxt_t ctxt)
{
	ldmsd_req_ctxt_t reqc;
	struct ldmsd_msg_key key;
	char *s;
	int rc;

	ldmsd_msg_key_get(ctxt->org_req->xprt->ldms.ldms, &key);
	reqc = ldmsd_req_ctxt_alloc(&key, ctxt->org_req->xprt);
	if (!reqc){
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return ENOMEM;
	}
	ctxt->req = reqc;
	reqc->ctxt = ctxt;
	reqc->resp_handler = __greeting_ping_pong_response;

	s = "{\"type\":\"cmd_obj\",\"cmd\":\"ping_pong\",\"spec\":";
	rc = ldmsd_append_request_va(reqc, LDMSD_REC_SOM_F, "%s", s);
	if (rc)
		goto err;
	s = "{\"round\":";
	rc = ldmsd_append_request_va(reqc, 0, "%s", s);
	if (rc)
		goto err;
	rc = ldmsd_append_request_va(reqc, LDMSD_REC_EOM_F, "%d}}", ctxt->round);
	if (rc)
		goto err;
	return 0;
err:
	ldmsd_req_ctxt_free(reqc);
	return rc;
}

static int
__greeting_ping_pong_words(ldmsd_req_ctxt_t reqc, json_str_t last_word)
{
	int rc;
	char *hdr;
	greeting_ping_pong_ctxt_t ctxt;
	ldmsd_req_ctxt_t org_req;

	ctxt = (greeting_ping_pong_ctxt_t)reqc->ctxt;

	hdr = "{\"mode\":\"ping_pong\",";

	if (!ctxt) {
		org_req = reqc;
		/*
		 * Client doesn't request LDSMD to ask for words.
		 */
		rc = ldmsd_append_info_obj_hdr(org_req, "greeting");
		if (rc)
			goto out;
		rc = ldmsd_append_response_va(org_req, 0, "%s", hdr);
		if (rc)
			goto out;
		rc = ldmsd_append_response_va(org_req, LDMSD_REC_EOM_F,
					"\"round\":1,\"words\":[\"%s\"]}}",
							last_word->str);
	} else {
		/* Get the original request */
		org_req = ctxt->org_req;
		rc = ldmsd_append_info_obj_hdr(org_req, "greeting");
		if (rc)
			goto out;
		rc = ldmsd_append_response_va(org_req, 0, "%s", hdr);
		if (rc)
			goto out;
		rc = ldmsd_append_response_va(org_req, 0,
				"\"round\":%d,\"words\":[\"%s\"",
				ctxt->round, last_word->str);
		if (rc)
			goto out;
		do {
			rc = ldmsd_append_response_va(org_req, 0, ",\"\%s\"",
								ctxt->s->str);
			if (rc)
				goto out;

			ctxt = ctxt->prev;
		} while (ctxt);
		rc = ldmsd_append_response(org_req, LDMSD_REC_EOM_F, "]}}", 3);
	}

out:
	return rc;
}

static int __greeting_ping_pong_response(ldmsd_req_ctxt_t reqc)
{
	json_entity_t spec, mode, value;
	json_str_t mode_s;
	int rc = 0;
	json_entity_t word;
	json_entity_t more;
	greeting_ping_pong_ctxt_t ctxt, prev;

	ctxt = prev = (greeting_ping_pong_ctxt_t)reqc->ctxt;

	spec = json_value_find(reqc->json, "spec");
	mode = json_value_find(spec, "mode");
	if (!mode) {
		rc = ldmsd_send_missing_attr_err(reqc, "greeting", "mode");
		goto out;
	}
	mode_s = json_value_str(mode);

	if (0 != strncmp(mode_s->str, "ping_pong", mode_s->str_len)) {
		rc = ldmsd_send_error(reqc, ENOTSUP, "Received unexpected mode %s",
								mode_s->str);
		goto out;
	}

	value = json_value_find(spec, "value");
	if (!value) {
		rc = ldmsd_send_missing_attr_err(reqc, "greeting/ping_pong", "value");
		goto out;
	}

	if (JSON_DICT_VALUE != json_entity_type(value)) {
		ldmsd_log(LDMSD_LINFO, "greeting handler: Received a wrong JSON type\n");
		rc = ldmsd_send_error(reqc, EINVAL, "The JSON object of "
				"the 'value' attribute has type '%s' "
				"instead of '%s'\n",
				json_type_str(json_entity_type(value)),
				json_type_str(JSON_DICT_VALUE));
		goto out;
	}

	more = json_value_find(value, "more");
	word = json_value_find(value, "word");

	if (!word) {
		rc = ldmsd_send_missing_attr_err(reqc, "greeting:ping_pong", "word");
		goto out;
	}

	if (more && (json_value_bool(more))) {
		ctxt = malloc(sizeof(*ctxt));
		if (!ctxt) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			rc = ENOMEM;
			goto out;
		}
		ctxt->org_req = prev->org_req;
		ctxt->s = json_value_str(word);
		ctxt->prev = prev;
		ctxt->round = prev->round + 1;


		/* Create a request to send back to the requester for the second word.  */
		rc = __greeting_ping_pong_request(ctxt);
	} else {
		rc = __greeting_ping_pong_words(reqc, json_value_str(word));
	}
	if (prev->req) {
		/*
		 * We got the response of this request already,
		 * so the context should not be used anymore.
		 */
		ldmsd_req_ctxt_free(prev->req);
	}

out:
	if (!more || rc) {
		if (ctxt)
			__cleanup_greeting_ping_pong_ctxt(ctxt);
	}
	return rc;
}


static int
__greeting_ping_pong_handler(ldmsd_req_ctxt_t reqc, json_entity_t v)
{
	int rc;
	json_entity_t word;
	json_entity_t more;
	greeting_ping_pong_ctxt_t ctxt = NULL;

	if (JSON_DICT_VALUE != json_entity_type(v)) {
		ldmsd_log(LDMSD_LINFO, "greeting handler: Received a wrong JSON type\n");
		rc = ldmsd_send_error(reqc, EINVAL, "The JSON object of "
				"the 'value' attribute has type '%s' "
				"instead of '%s'\n", json_type_str(json_entity_type(v)),
				json_type_str(JSON_DICT_VALUE));
		goto out;
	}

	more = json_value_find(v, "more");
	word = json_value_find(v, "word");

	if (!word) {
		rc = ldmsd_send_missing_attr_err(reqc, "greeting:ping_pong", "word");
		goto out;
	}

	if (more && (json_value_bool(more))) {
		ctxt = malloc(sizeof(*ctxt));
		if (!ctxt) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			rc = ENOMEM;
			goto out;
		}
		ldmsd_req_ctxt_ref_get(reqc, "greeting_ctxt:ref");
		ctxt->org_req = reqc;
		ctxt->s = json_value_str(word);
		ctxt->prev = NULL;
		ctxt->round = 1;

		/* Create a request to send back to the requester for the second word.  */
		rc = __greeting_ping_pong_request(ctxt);
		if (rc)
			__cleanup_greeting_ping_pong_ctxt(ctxt);
	} else {
		rc = __greeting_ping_pong_words(reqc, json_value_str(word));
	}
out:
	return rc;
}

static int
__greeting_long_rsp_handler(ldmsd_req_ctxt_t reqc, json_entity_t v)
{
	int rc;
	json_entity_t len_s;
	int len;

	if (JSON_DICT_VALUE != json_entity_type(v)) {
		ldmsd_log(LDMSD_LINFO, "greeting handler: Received a wrong JSON type\n");
		rc = ldmsd_send_error(reqc, EINVAL, "The JSON object of "
				"the 'value' attribute has type '%s' "
				"instead of '%s'\n", json_type_str(json_entity_type(v)),
				json_type_str(JSON_DICT_VALUE));
		goto out;
	}

	len_s = json_value_find(v, "length");
	if (!len_s) {
		rc = ldmsd_send_missing_attr_err(reqc, "greeting:long_rsp", "length");
		goto out;
	}
	if (JSON_INT_VALUE != json_entity_type(len_s)) {
		ldmsd_log(LDMSD_LINFO, "greeting/long_rsp: \"length\" must be an integer\n");
		rc = ldmsd_send_error(reqc, EINVAL, "'length' must be an integer.");
		goto out;
	}
	len = (int)json_value_int(len_s);

	rc = ldmsd_append_info_obj_hdr(reqc, "greeting");
	if (rc)
		return rc;

	rc = ldmsd_append_response_va(reqc, LDMSD_REC_EOM_F,
				"{\"mode\":\"long_rsp\","
				"\"msg\":\"%0*d\"}}", len, len);
out:
	return rc;
}

#define GREETING_STREAM "This is a raw stream."
struct greeting_raw_stream {
	size_t len;
	char s[OVIS_FLEX];
};

struct greeting_stream_ctxt {
	enum {
		RAW = 1,
		STRING = 2,
		JSON = 3,
	} type;
};

static int __greeting_stream_cb(ldmsd_stream_client_t c, void *ctxt,
				       ldmsd_stream_type_t stream_type,
				       const char *data, size_t data_len,
				       json_entity_t entity)
{
	json_entity_t len, s;
	struct greeting_stream_ctxt *gctxt = ctxt;
	struct greeting_raw_stream *strm;
	const char *type;

	if (gctxt->type == RAW) {
		type = "raw";
		if (stream_type != LDMSD_STREAM_STRING) {
			ldmsd_log(LDMSD_LERROR, "Greeting-stream: expecting "
					"stream type '%s', but received '%s'.\n",
					ldmsd_stream_type_enum2str(LDMSD_STREAM_STRING),
					ldmsd_stream_type_enum2str(stream_type));
			return 0;
		}
		strm = (struct greeting_raw_stream *)data;
		strm->len = ntohl(strm->len);
		if ((strm->len != strlen(GREETING_STREAM)) ||
				(0 != strcmp(strm->s, GREETING_STREAM))) {
			ldmsd_log(LDMSD_LERROR, "Greeting-stream: Received "
					"an unexpected data: '%lu %s'\n",
					strm->len, strm->s);
			return 0;
		}
	} else if (gctxt->type == STRING) {
		type = "string";
		if (stream_type != LDMSD_STREAM_STRING) {
			ldmsd_log(LDMSD_LERROR, "Greeting-stream: expecting "
					"stream type '%s', but received '%s'.\n",
					ldmsd_stream_type_enum2str(LDMSD_STREAM_STRING),
					ldmsd_stream_type_enum2str(stream_type));
			return 0;
		}
		if (0 != strncmp(data, GREETING_STREAM, data_len)) {
			ldmsd_log(LDMSD_LERROR, "Greeting-stream: Received "
					"an unexpected data: '%s'\n", data);
			return 0;
		}
	} else {
		type = "json";
		if (stream_type != LDMSD_STREAM_JSON) {
			ldmsd_log(LDMSD_LERROR, "Greeting-stream: expecting "
					"stream type '%s', but received '%s'.\n",
					ldmsd_stream_type_enum2str(LDMSD_STREAM_JSON),
					ldmsd_stream_type_enum2str(stream_type));
			return 0;
		}
		len = json_value_find(entity, "len");
		s = json_value_find(entity, "str");
		if ((0 != strcmp(json_value_str(s)->str, GREETING_STREAM)) ||
			(strlen(GREETING_STREAM) != json_value_int(len))) {
			ldmsd_log(LDMSD_LERROR, "Greeting-stream: Received "
					"an unexpected data: '%s'\n", data);
			return 0;
		}
	}
	ldmsd_log(LDMSD_LERROR, "greeting-stream: %s: received correct data.\n", type);
	free(ctxt);
	return 0;
}

static int __greeting_stream_subscribe_handler(ldmsd_req_ctxt_t reqc, json_entity_t v)
{
	json_entity_t type;
	char *type_s;
	struct greeting_stream_ctxt *ctxt;

	if (JSON_DICT_VALUE != json_entity_type(v)) {
		return ldmsd_send_type_error(reqc,
			"cmd_obj:greeting:stream_subscribe:value", "a dictionary");
	}
	type = json_value_find(v, "type");
	if (!type) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:greeting:stream_subscribe:value", "type");
	}
	type_s = json_value_str(type)->str;

	ctxt = malloc(sizeof(*ctxt));
	if (!ctxt) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return ENOMEM;
	}

	if (0 == strcmp(type_s, "raw"))
		ctxt->type = RAW;
	else if (0 == strcmp(type_s, "string"))
		ctxt->type = STRING;
	else if (0 == strcmp(type_s, "json"))
		ctxt->type = JSON;
	else {
		free(ctxt);
		return ldmsd_send_error(reqc, EINVAL, "type '%s' is invalid.", type_s);
	}

	ldmsd_stream_subscribe(type_s, __greeting_stream_cb, ctxt);
	return ldmsd_send_error(reqc, 0, NULL);
}

static int __greeting_stream_publish_handler(ldmsd_req_ctxt_t reqc, json_entity_t v)
{
	int rc;
	json_entity_t prdcr_name, type;
	char *type_s;
	ldmsd_prdcr_t prdcr;
	struct greeting_raw_stream *strm;
	size_t len = strlen(GREETING_STREAM);
	ldmsd_req_buf_t buf;

	buf = ldmsd_req_buf_alloc(256);
	if (!buf) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return ENOMEM;
	}

	prdcr_name = json_value_find(v, "producer");
	if (!prdcr_name) {
		return ldmsd_send_missing_attr_err(reqc, "greeting:stream_publish", "producer");
	}

	type = json_value_find(v, "type");
	if (!type) {
		return ldmsd_send_missing_attr_err(reqc, "greeting:stream_publish", "type");
	}
	type_s = json_value_str(type)->str;

	prdcr = ldmsd_prdcr_find(json_value_str(prdcr_name)->str);
	if (!prdcr) {
		return ldmsd_send_error(reqc, ENOENT, "Producer %s not found.",
				json_value_str(prdcr_name)->str);
	}
	ldmsd_prdcr_get(prdcr);

	if (0 == strcmp("raw", type_s)) {
		strm = (struct greeting_raw_stream *)buf->buf;
		strm->len = htonl(len);
		memcpy(strm->s, GREETING_STREAM, len + 1);

		rc = ldmsd_stream_publish(prdcr->xprt, type_s,
				LDMSD_STREAM_STRING,
				(const char *)strm, sizeof(*strm) + len + 1);
	} else if (0 == strcmp("string", type_s)) {
		buf->off = snprintf(buf->buf, len + 1, "%s", GREETING_STREAM);
		rc = ldmsd_stream_publish(prdcr->xprt, type_s,
				LDMSD_STREAM_STRING, buf->buf, buf->off);
	} else {
		buf->off = snprintf(buf->buf, buf->len, "{\"len\":%lu,\"str\":\"%s\"}",
						len, GREETING_STREAM);
		rc = ldmsd_stream_publish(prdcr->xprt, type_s,
				LDMSD_STREAM_JSON, buf->buf, buf->off);
	}


	if (rc) {
		rc = ldmsd_send_error(reqc, rc, "Failed to publish.");
	} else {
		rc = ldmsd_send_error(reqc, 0, NULL);
	}
	ldmsd_prdcr_put(prdcr);
	return rc;
}

static int greeting_handler(ldmsd_req_ctxt_t reqc)
{
	json_entity_t spec, mode, value;
	json_str_t mode_s;
	int rc = 0;

	spec = json_value_find(reqc->json, "spec");
	mode = json_value_find(spec, "mode");
	if (!mode) {
		rc = ldmsd_send_missing_attr_err(reqc, "greeting", "mode");
		return rc;
	}
	mode_s = json_value_str(mode);

	value = json_value_find(spec, "value");
	if (0 == strncmp(mode_s->str, "echo", mode_s->str_len)) {
		if (!value) {
			rc = ldmsd_send_missing_attr_err(reqc, "greeting/echo", "value");
			goto out;
		}
		rc = __greeting_echo_handler(reqc, value);
	} else if (0 == strncmp(mode_s->str, "rsp_multi_rec", mode_s->str_len)) {
		if (!value) {
			rc = ldmsd_send_missing_attr_err(reqc, "greeting/rsp_multi_rec", "value");
			goto out;
		}
		rc = __greeting_rsp_multi_rec_handler(reqc, value);
	} else if (0 == strncmp(mode_s->str, "ping_pong", mode_s->str_len)) {
		if (!value) {
			rc = ldmsd_send_missing_attr_err(reqc, "greeting/ping_pong", "value");
			goto out;
		}
		rc = __greeting_ping_pong_handler(reqc, value);
	} else if (0 == strncmp(mode_s->str, "long_rsp", mode_s->str_len)) {
		if (!value) {
			rc = ldmsd_send_missing_attr_err(reqc, "greeting/long_rsp", "value");
			goto out;
		}
		rc = __greeting_long_rsp_handler(reqc, value);
	} else if (0 == strncmp(mode_s->str, "publish_stream", mode_s->str_len)) {
		if (!value) {
			rc = ldmsd_send_missing_attr_err(reqc, "greeting/publish_stream", "value");
			goto out;
		}
		rc = __greeting_stream_publish_handler(reqc, value);
	} else if (0 == strncmp(mode_s->str, "subscribe_stream", mode_s->str_len)) {
		if (!value) {
			rc = ldmsd_send_missing_attr_err(reqc, "greeting/subscribe_stream", "value");
			goto out;
		}
		rc = __greeting_stream_subscribe_handler(reqc, value);
	} else {
		rc = ldmsd_send_error(reqc, ENOTSUP, "Not supported mode %s",
								mode_s->str);
	}
out:
	return rc;
}

//
//static int unimplemented_handler(ldmsd_req_ctxt_t reqc)
//{
//	reqc->errcode = ENOSYS;
//
//	Snprintf(&reqc->recv_buf, &reqc->recv_len,
//			"The request is not implemented");
//	ldmsd_send_req_response(reqc, reqc->recv_buf);
//	return 0;
//}
//
//static int eperm_handler(ldmsd_req_ctxt_t reqc)
//{
//	reqc->errcode = EPERM;
//	Snprintf(&reqc->recv_buf, &reqc->recv_len,
//			"Operation not permitted.");
//	ldmsd_send_req_response(reqc, reqc->recv_buf);
//	return 0;
//}
//
//static int ebusy_handler(ldmsd_req_ctxt_t reqc)
//{
//	reqc->errcode = EBUSY;
//	Snprintf(&reqc->recv_buf, &reqc->recv_len,
//			"Daemon busy.");
//	ldmsd_send_req_response(reqc, reqc->recv_buf);
//	return 0;
//}

int ldmsd_set_route_request(ldmsd_prdcr_t prdcr,
			ldmsd_req_ctxt_t org_reqc, char *inst_name,
			ldmsd_req_resp_fn resp_handler, void *ctxt)
{
	size_t inst_name_len;
	ldmsd_req_ctxt_t reqc;
	ldmsd_cfg_xprt_t xprt;
	struct ldmsd_msg_key key;
	char *str;
	int rc;

	xprt = ldmsd_cfg_xprt_ldms_new(prdcr->xprt);

	ldmsd_msg_key_get(xprt, &key);

	inst_name_len = strlen(inst_name);
	reqc = ldmsd_req_ctxt_alloc(&key, xprt);
	if (!reqc)
		return ENOMEM;
	reqc->resp_handler = resp_handler;
	reqc->ctxt = ctxt;

	str = 	"{\"cmd\": \"set_route\","
		" \"spec\": {\"instance\":\"";

	rc = ldmsd_append_request(reqc, LDMSD_REC_SOM_F, str, strlen(str));
	if (rc)
		goto out;

	/* append the instance name to the JSON string */
	rc = ldmsd_append_request(reqc, 0, inst_name, inst_name_len);
	if (rc)
		goto out;

	str =	"\","
		" \"type\":\"internal\"}"
		"}";
	rc = ldmsd_append_request(reqc, 0, str, strlen(str));
out:
	if (rc) {
		/* rc is not zero only if sending fails (a transport error) so
		 * no need to keep the request command context around */
		ldmsd_req_ctxt_free(reqc);
	}

	return rc;
}

ldmsd_req_buf_t __set_route_json_get(int is_internal, ldmsd_set_info_t info)
{
	ldmsd_req_buf_t buf;
	size_t cnt;

	buf = calloc(1, sizeof(*buf) + 1024);
	if (!buf)
		return NULL;
	buf->len = 1024;
	buf->buf[0] = '\0';

	if (info->origin_type == LDMSD_SET_ORIGIN_SMPLR) {
	smplr:
		cnt = snprintf(&buf->buf[buf->off], buf->len - buf->off,
				"{"
				"\"host\":\"%s\","
				"\"type\":\"%s\","
				"\"detail\":"
					"{"
					"\"name\":\"%s\","
					"\"interval_us\":\"%lu\","
					"\"offset_us\":\"%ld\","
					"\"sync\":\"%s\","
					"\"trans_start_sec\":\"%ld\","
					"\"trans_start_usec\":\"%ld\","
					"\"trans_end_sec\":\"%ld\","
					"\"trans_end_usec\":\"%ld\""
					"}"
				"}",
				ldmsd_myhostname_get(),
				ldmsd_set_info_origin_enum2str(info->origin_type),
				info->origin_name,
				info->interval_us,
				info->offset_us,
				((info->sync)?"true":"false"),
				info->start.tv_sec,
				info->start.tv_usec,
				info->end.tv_sec,
				info->end.tv_usec);
		if (cnt > buf->len - buf->off) {
			buf = ldmsd_req_buf_realloc(buf, buf->len * 2);
			if (!buf)
				return NULL;
			goto smplr;
		}
		buf->off += cnt;
		if (!is_internal) {
			cnt = snprintf(&buf->buf[buf->off], buf->len - buf->off, "]}");
			if (cnt > buf->len - buf->off) {
				buf = ldmsd_req_buf_realloc(buf, buf->len * 2);
				if (!buf)
					return NULL;
				cnt = snprintf(&buf->buf[buf->off], buf->len - buf->off, "]}");
			}
			buf->off += cnt;
		}
	} else {
	prdcr_set:
		cnt = snprintf(&buf->buf[buf->off], buf->len - buf->off,
				"{"
				"\"host\":\"%s\","
				"\"type\":\"%s\","
				"\"detail\":"
					"{"
					"\"name\":\"%s\","
					"\"host\":\"%s\","
					"\"update_int\":\"%ld\","
					"\"update_off\":\"%ld\","
					"\"update_sync\":\"%s\","
					"\"last_start_sec\":\"%ld\","
					"\"last_start_usec\":\"%ld\","
					"\"last_end_sec\":\"%ld\","
					"\"last_end_usec\":\"%ld\""
					"}"
				"}",
				ldmsd_myhostname_get(),
				ldmsd_set_info_origin_enum2str(info->origin_type),
				info->origin_name,
				info->prd_set->prdcr->host_name,
				info->interval_us,
				info->offset_us,
				((info->sync)?"true":"false"),
				info->start.tv_sec,
				info->start.tv_usec,
				info->end.tv_sec,
				info->end.tv_usec);
		if (cnt > buf->len - buf->off) {
			buf = ldmsd_req_buf_realloc(buf, buf->len * 2);
			if (!buf)
				return NULL;
			goto prdcr_set;
		}
		buf->off += cnt;
	}
	return buf;
}

typedef struct set_route_ctxt {
	ldmsd_req_buf_t my_set;
	int is_internal;
	const char *inst_name;
	const char *schema_name;
	/*
	 * The request context of the command
	 * that LDMSD has received from a client.
	 */
	ldmsd_req_ctxt_t cmd_reqc;
} *set_route_ctxt_t;

static void free_set_route_ctxt(set_route_ctxt_t ctxt)
{
	if (ctxt->inst_name)
		free((char *)ctxt->inst_name);
	if (ctxt->schema_name)
		free((char *)ctxt->schema_name);
	if (ctxt->my_set)
		ldmsd_req_buf_free(ctxt->my_set);
	if (ctxt->cmd_reqc)
		ldmsd_req_ctxt_ref_put(ctxt->cmd_reqc, "set_route_ctxt:ref");
	free(ctxt);
}

static int __send_set_route_rsp_obj(ldmsd_req_ctxt_t reqc,
		const char *inst_name, const char *schema_name,
		ldmsd_req_buf_t my_set, json_entity_t downstream)
{
	int rc = 0;
	char *str;
	json_entity_t hop;
	jbuf_t jb;

	str = "{\"type\":\"info\",\"name\":\"set_route\",\"info\":{\")";
	if ((rc = ldmsd_append_response(reqc, LDMSD_REC_SOM_F, str, strlen(str))))
		goto out;

	str = "{\"instance\":\"";
	if ((rc = ldmsd_append_response(reqc, 0, str, strlen(str))))
		goto out;
	if ((rc = ldmsd_append_response(reqc, 0, inst_name, strlen(inst_name))))
		goto out;
	str = "\",\"schema\":\"";
	if ((rc = ldmsd_append_response(reqc, 0, str, strlen(str))))
		goto out;
	if ((rc = ldmsd_append_response(reqc, 0, schema_name, strlen(schema_name))))
		goto out;
	str = "\",\"route\":[";
	if ((rc = ldmsd_append_response(reqc, 0, str, strlen(str))))
		goto out;
	if ((rc = ldmsd_append_response(reqc, 0, my_set->buf, my_set->off)))
		goto out;
	if (!downstream)
		goto end;
	for (hop = json_item_first(downstream); hop; hop = json_item_next(hop)) {
		if ((rc = ldmsd_append_response(reqc, 0, ",", 1)))
			goto out;
		jb = json_entity_dump(jb, hop);
		if (!jb) {
			rc = ENOMEM;
			ldmsd_log(LDMSD_LCRITICAL, "ldmsd: out of memory\n");
			rc = ldmsd_send_error(reqc, ENOMEM, "ldmsd: out of memory");
			if (rc)
				goto out;
		}
		if ((rc = ldmsd_append_response(reqc, 0, jb->buf, strlen(jb->buf))))
			goto out;
		jbuf_free(jb);
	}
end:
	str = "]}}";
	rc = ldmsd_append_response(reqc, LDMSD_REC_EOM_F, str, strlen(str));
out:
	return rc;
}

static int set_route_resp_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t route, spec;
	set_route_ctxt_t ctxt = (set_route_ctxt_t)(reqc->ctxt);
	ldmsd_req_ctxt_t recv_reqc = ctxt->cmd_reqc;

	spec = json_value_find(reqc->json, "spec");
	route = json_value_find(spec, "route");
	if (!route) {
		if ((rc = ldmsd_send_error(recv_reqc, EINTR,
				"ldmsd: internal error when "
				"handling the set_route request")))
			goto out;
	}

	rc = __send_set_route_rsp_obj(recv_reqc, ctxt->inst_name,
			ctxt->schema_name, ctxt->my_set, route);
out:
	free_set_route_ctxt(ctxt);
	ldmsd_req_ctxt_free(reqc);
	return 0;
}

static int set_route_handler(ldmsd_req_ctxt_t reqc)
{
	char *inst_name;
	struct set_route_ctxt *ctxt = NULL;
	int is_internal = 0;
	int rc = 0;
	ldmsd_set_info_t info = NULL;
	ldmsd_req_buf_t buf;
	json_entity_t spec, name, type;

	spec = json_value_find(reqc->json, "spec");
	name = json_value_find(spec, "instance");
	if (!name) {
		return ldmsd_send_missing_attr_err(reqc,
					"set_route", "instance");
	}
	inst_name = json_value_str(name)->str;

	type = json_value_find(spec, "type");
	if (!type) {
		return ldmsd_send_missing_attr_err(reqc, "set_route", "type");
	}
	is_internal = json_value_bool(type);

	info = ldmsd_set_info_get(inst_name);
	if (!info) {
		rc = ldmsd_send_error(reqc, ENOENT,
				"%s: Set '%s' not exist.",
				ldmsd_myhostname_get(), inst_name);
		goto out;
	}

	buf = __set_route_json_get(is_internal, info);
	if (info->origin_type == LDMSD_SET_ORIGIN_PRDCR) {
		ctxt = calloc(1, sizeof(*ctxt));
		if (!ctxt)
			goto oom;
		ldmsd_req_ctxt_ref_get(reqc, "set_route_ctxt:ref"); /* put back when the ctxt is freed */
		ctxt->cmd_reqc = reqc;
		ctxt->inst_name = strdup(ldms_set_instance_name_get(info->set));
		if (!ctxt->inst_name)
			goto oom;
		ctxt->schema_name = strdup(ldms_set_schema_name_get(info->set));
		if (!ctxt->schema_name)
			goto oom;
		ctxt->is_internal = is_internal;
		ctxt->my_set = buf;
		rc = ldmsd_set_route_request(info->prd_set->prdcr,
				reqc, inst_name, set_route_resp_handler, ctxt);
		if (rc) {
			rc = ldmsd_send_error(reqc, rc,
					"%s: error forwarding set_route_request to "
					"prdcr '%s'", ldmsd_myhostname_get(),
					info->origin_name);
			goto err;
		}
	} else {
		rc = __send_set_route_rsp_obj(reqc, inst_name,
				ldms_set_schema_name_get(info->set),
				buf, NULL);
	}

	goto out;
oom:
	ldmsd_log(LDMSD_LCRITICAL, "ldmsd: out of memory\n");
err:
	if (ctxt)
		free_set_route_ctxt(ctxt);
out:
	if (info)
		ldmsd_set_info_delete(info);
	return rc;
}

static int stream_republish_cb(ldmsd_stream_client_t c, void *ctxt,
			       ldmsd_stream_type_t stream_type,
			       const char *data, size_t data_len,
			       json_entity_t entity)
{
	int rc;
	char *s;
	size_t s_len;
	jbuf_t jb = NULL;
	const char *stream = ldmsd_stream_client_name(c);

	if (data) {
		s = (char *)data;
		s_len = data_len;
	} else {
		jb = json_entity_dump(NULL, entity);
		if (!jb) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		}
		s = jb->buf;
		s_len = jb->buf_len;
	}
	rc = ldmsd_stream_publish((ldms_t)ctxt, stream, stream_type, s, s_len);
	if (jb)
		jbuf_free(jb);
	return rc;
}

static int stream_subscribe_handler(ldmsd_req_ctxt_t reqc)

{
	json_entity_t spec, names, n;
	char *stream_name;

	spec = json_value_find(reqc->json, "spec");
	if (!spec) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:stream_subscribe", "spec");
	}

	/* list of stream names */
	names = json_value_find(spec, "names");
	if (!names) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:stream_subscribe:spec", "names");
	}
	if (JSON_LIST_VALUE != json_entity_type(names)) {
		return ldmsd_send_type_error(reqc,
				"cmd_obj:stream_subscribe:spec:names", "a list of strings");
	}

	for (n = json_item_first(names); n; n = json_item_next(n)) {
		if (JSON_STRING_VALUE != json_entity_type(n)) {
			return ldmsd_send_type_error(reqc,
					"cmd_obj:stream_subscribe:spec:names[]",
					"a string");
		}
		stream_name = json_value_str(n)->str;
		ldmsd_stream_subscribe(stream_name, stream_republish_cb, reqc->xprt->ldms.ldms);
	}

	return ldmsd_send_error(reqc, 0, NULL);
}

static int listen_handler(ldmsd_req_ctxt_t reqc)
{
	ldmsd_listen_t listen;
	int rc = 0;
	json_entity_t xprt, port, host, auth, spec;
	char *xprt_s, *host_s, *port_s, *auth_s;
	unsigned short port_no = -1;
	xprt_s = port_s = host_s = auth_s = NULL;

	if (ldmsd_is_initialized()) {
		/*
		 * Adding a new listening endpoint is prohibited
		 * after LDMSD is initialized.
		 */
		rc = ldmsd_send_error(reqc, EPERM, "LDMSD has been started. "
				"Adding a listening endpoint is prohibited.");
		goto out;
	}

	spec = json_value_find(reqc->json, "spec");

	xprt = json_value_find(spec, "xprt");
	if (!xprt) {
		rc = ldmsd_send_missing_attr_err(reqc, "listen", "xprt");
		goto out;
	}
	if (JSON_STRING_VALUE != json_entity_type(xprt)) {
		rc = ldmsd_send_error(reqc, EINVAL, "listen:xprt must be a string.");
		goto out;
	}
	xprt_s = json_value_str(xprt)->str;

	port = json_value_find(spec, "port");
	if (port) {
		if (JSON_STRING_VALUE == json_entity_type(port)) {
			port_s = json_value_str(port)->str;
			port_no = atoi(port_s);
		} else if (JSON_INT_VALUE == json_entity_type(port)) {
			port_no = (int)json_value_int(port);
		} else {
			rc = ldmsd_send_error(reqc, EINVAL,
				"listen:port must be either 'init' or 'string'.");
			goto out;
		}

		if (port_no < 1 || port_no > USHRT_MAX) {
			rc = ldmsd_send_error(reqc, EINVAL,
					"'%s' transport with invalid port '%s'",
					xprt, port);
			goto out;
		}
	}
	host = json_value_find(spec, "host");
	if (host) {
		if (JSON_STRING_VALUE != json_entity_type(host)) {
			rc = ldmsd_send_error(reqc, EINVAL, "listen:host must be a string.");
			goto out;
		}
		host_s = json_value_str(host)->str;
	}
	auth = json_value_find(spec, "auth");
	if (auth) {
		if (JSON_STRING_VALUE != json_entity_type(auth)) {
			rc = ldmsd_send_error(reqc, EINVAL, "listen:auth must be a string.");
			goto out;
		}
		auth_s = json_value_str(auth)->str;
	}

	listen = ldmsd_listen_new(xprt_s, port_no, host_s, auth_s);
	if (!listen) {
		if (errno == EEXIST) {
			rc = ldmsd_send_error(reqc, EEXIST,
					"The listening endpoint %s:%s is already exists",
					xprt_s, port_s);
		} else if (errno == ENOENT) {
			rc = ldmsd_send_error(reqc, ENOENT,
					"The given 'auth' (%s) "
					"does not exist.", auth_s);
		} else {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		}
	}
out:
	return rc;
}

struct export_context {
	ldmsd_req_ctxt_t reqc;
	FILE *f;
	size_t cnt; /* number of character in the file */
	int is_print_comma;
	int error;
};

void __print_cfg_obj_hdr(struct export_context *export, const char *cfg_obj)
{
	if (export->is_print_comma)
		export->cnt += fprintf(export->f, ",");
	else
		export->is_print_comma = 1;
	export->cnt += fprintf(export->f, "\n{\"type\":\"cfg_obj\","
					  "\"cfg_obj\":\"%s\",\"spec\":{", cfg_obj);
}

void __print_cfg_obj_tail(struct export_context *export)
{
	/* } for spec and the other for the whole JSon dict */
	export->cnt += fprintf(export->f, "}}");
}

struct envvar_name {
	const char *env;
	uint8_t is_exported;
};

static void __print_env(struct export_context *export, const char *env, struct rbt *exported_env_tree)
{
	int rc;
	char *v = getenv(env);

	if (v) {
		rc = env_node_new(env, exported_env_tree, NULL);
		if (rc == EEXIST) {
			/* The environment variable was already exported */
			return;
		}
		__print_cfg_obj_hdr(export, "env");
		export->cnt += fprintf(export->f, "\"name\":\"%s\",\"value\":\"%s\"}}", env, v);
	}
}

struct env_trav_ctxt {
	struct export_context *export;
	struct rbt *exported_env_tree;;
};

static int __export_env(struct rbn *rbn, void *ctxt, int i)
{
	struct env_trav_ctxt *_ctxt = (struct env_trav_ctxt *)ctxt;
	__print_env(_ctxt->export, (const char *)rbn->key, _ctxt->exported_env_tree);
	return i;
}

static int __export_envs(struct export_context *export)
{
	int rc = 0;
	ldmsd_req_ctxt_t reqc = export->reqc;
	struct rbt exported_env_tree = RBT_INITIALIZER(env_cmp);

	char *ldmsd_envvar_tbl[] = {
		"LDMS_AUTH_FILE",
		"LDMSD_MEM_SIZE_ENV",
		"LDMSD_PIDFILE",
		"LDMSD_PLUGIN_LIBPATH",
		"LDMSD_UPDTR_OFFSET_INCR",
		"MMALLOC_DISABLE_MM_FREE",
		"OVIS_EVENT_HEAP_SIZE",
		"OVIS_NOTIFICATION_RETRY",
		"ZAP_EVENT_WORKERS",
		"ZAP_EVENT_QDEPTH",
		"ZAP_LIBPATH",
		NULL,
	};

	struct env_trav_ctxt ctxt = { export, &exported_env_tree };

	/*
	 *  Export all environment variables set with the env command.
	 */
	pthread_mutex_lock(&env_tree_lock);
	rbt_traverse(&env_tree, __export_env, (void *)&ctxt);
	pthread_mutex_unlock(&env_tree_lock);


	/*
	 * Export environment variables used in the LDMSD process that are
	 * set directly in bash.
	 */
	/*
	 * Environment variables used by core LDMSD (not plugins).
	 */
	int i;
	for (i = 0; ldmsd_envvar_tbl[i]; i++) {
		__print_env(export, ldmsd_envvar_tbl[i], &exported_env_tree);
	}

	/*
	 * Environment variables used by zap transports
	 */
	char **zap_envs = ldms_xprt_zap_envvar_get();
	if (!zap_envs) {
		if (errno) {
			export->error = rc;
			rc = ldmsd_send_error(reqc, rc, "cmd_obj:export_config - "
					"Failed to get zap environment variables. Error %d");
			goto cleanup;
		} else {
			/*
			 * nothing to do
			 * no env var used by the loaded zap transports
			 */
		}
	} else {
		int i;
		for (i = 0; zap_envs[i]; i++) {
			__print_env(export, zap_envs[i], &exported_env_tree);
			free(zap_envs[i]);
		}
		free(zap_envs);
	}

	/*
	 * Environment variables used by loaded plugins.
	 */
	ldmsd_plugin_inst_t inst;
	json_entity_t qr, env_attr, envs, item;
	char *name;
	LDMSD_PLUGIN_INST_FOREACH(inst) {
		qr = inst->base->query(inst, "env"); /* Query env used by the pi instance */
		if (!qr)
			continue;
		env_attr = json_attr_find(qr, "env");
		if (!env_attr)
			goto next;
		envs = json_attr_value(env_attr);
		if (JSON_LIST_VALUE != json_entity_type(envs)) {
			rc = ldmsd_send_error(reqc, EINTR,
					"Cannot get the environment "
					"variable list from plugin instance '%s'",
					inst->inst_name);
			export->error = EINTR;
			goto cleanup;
		}
		for (item = json_item_first(envs); item; item = json_item_next(item)) {
			name = json_value_str(item)->str;
			__print_env(export, name, &exported_env_tree);
		}
	next:
		json_entity_free(qr);
	}

	/*
	 * Clean up the exported_env_tree;
	 */
	struct env_node *env_node;
	struct rbn *rbn;
cleanup:
	rbn = rbt_min(&exported_env_tree);
	while (rbn) {
		rbt_del(&exported_env_tree, rbn);
		env_node = container_of(rbn, struct env_node, rbn);
		env_node_del(env_node);
		rbn = rbt_min(&exported_env_tree);
	}
	return rc;
}

static int __export_auth_cfg_objs(struct export_context *export)
{
	ldmsd_auth_t auth;
	char *name, *value;
	int i;
	ldmsd_cfg_lock(LDMSD_CFGOBJ_AUTH);
	for (auth = (ldmsd_auth_t)ldmsd_cfgobj_first(LDMSD_CFGOBJ_AUTH); auth;
			auth = (ldmsd_auth_t)ldmsd_cfgobj_next(&auth->obj)) {
		if (0 == strcmp(DEFAULT_AUTH, auth->obj.name)) {
			/* Skip the default authentication */
			continue;
		}

		__print_cfg_obj_hdr(export, "auth");
		export->cnt += fprintf(export->f,
					"\"name\":\"%s\",\"plugin\":\"%s\"",
					auth->obj.name, auth->plugin);
		for (i = 0, name = av_name(auth->attrs, i); name; i++) {
			value = av_value_at_idx(auth->attrs, i);
			if (i == 0)
				export->cnt += fprintf(export->f, ",\"opts\":{");
			export->cnt += fprintf(export->f, "\"%s\":\"%s\"", name, value);
		}
		if (i > 0) {
			/* At least one option exists. */
			export->cnt += fprintf(export->f, "}");
		}
		__print_cfg_obj_tail(export);

	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_AUTH);
	return 0;
}

extern struct ldmsd_cmd_line_args cmd_line_args;
static void __export_daemon_cfg_obj(struct export_context *export)
{
	int i = 0;
	ldmsd_auth_t default_auth;
	char *name, *value;

	__print_cfg_obj_hdr(export, "daemon");

	/* log */
	if (cmd_line_args.log_path) {

		export->cnt += fprintf(export->f, "\"log\":{"
				"\"dst\":\"%s\",\"level\":\"%s\"}",
				cmd_line_args.log_path,
				ldmsd_loglevel_to_str(cmd_line_args.verbosity));
	} else {
		export->cnt += fprintf(export->f, "\"log\":{\"level\":\"%s\"}",
				ldmsd_loglevel_to_str(cmd_line_args.verbosity));
	}

	/* default authentication */
	default_auth = ldmsd_auth_default_get();
	export->cnt += fprintf(export->f, ",\"default-auth\":{\"plugin\":\"%s\"",
							default_auth->plugin);
	if (default_auth->attrs) {
		export->cnt += fprintf(export->f, ",\"args\": {");
		for (i = 0, name = av_name(default_auth->attrs, i); name; i++) {
			if (0 == i)
				export->cnt += fprintf(export->f, ",");
			value = av_value_at_idx(default_auth->attrs, i);
			export->cnt += fprintf(export->f, "\"%s\":\"%s\"",
							name, value);
		}
		export->cnt += fprintf(export->f, "}");
	}
	export->cnt += fprintf(export->f, "}");

	/* memory for all LDMSD set instances */
	export->cnt += fprintf(export->f, ",\"mem\":\"%s\"",
					cmd_line_args.mem_sz_str);

	/* Number of workers */
	if (cmd_line_args.ev_thread_count > 1) {
		export->cnt += fprintf(export->f, ",\"workers\":%d",
					cmd_line_args.ev_thread_count);
	}

	/* hostname */
	export->cnt += fprintf(export->f, ",\"hostname\":\"%s\"",
					cmd_line_args.myhostname);

	/* daemon name */
	export->cnt += fprintf(export->f, ",\"daemon-name\":\"%s\"",
						cmd_line_args.daemon_name);

	/* banner */
	if (!cmd_line_args.banner)
		export->cnt += fprintf(export->f, ",\"banner\":false");

	/* pidfile */
	if (cmd_line_args.pidfile) {
		export->cnt += fprintf(export->f, ",\"pidfile\":\"%s\"",
							cmd_line_args.pidfile);
	}

	/* kernel options */
	if (cmd_line_args.do_kernel) {
		export->cnt += fprintf(export->f, ",\"kernel\":{\"publish\":true,"
				"\"path\":\"%s\"}", cmd_line_args.kernel_setfile);
	}

	__print_cfg_obj_tail(export);
}

static void __export_listen_cfg_objs(struct export_context *export)
{
	ldmsd_listen_t listen;
	for (listen = (ldmsd_listen_t)ldmsd_cfgobj_first(LDMSD_CFGOBJ_LISTEN);
			listen ; listen = (ldmsd_listen_t)ldmsd_cfgobj_next(&listen->obj)) {
		__print_cfg_obj_hdr(export, "listen");
		export->cnt += fprintf(export->f, "\"xprt\":\"%s\",\"port\":%d",
				listen->xprt, listen->port_no);
		if (listen->host) {
			export->cnt += fprintf(export->f, ",\"host\":\"%s\"",
								listen->host);
		}
		if (listen->auth_name) {
			export->cnt += fprintf(export->f, ",\"auth\":\"%s\"",
							listen->auth_name);
		}
		__print_cfg_obj_tail(export);
	}
}

static int __export_smplrs(struct export_context *export)
{
	ldmsd_smplr_t smplr;
	char *intrvl, *offset;
	ldmsd_cfg_lock(LDMSD_CFGOBJ_SMPLR);
	for (smplr = ldmsd_smplr_first(); smplr; smplr = ldmsd_smplr_next(smplr)) {
		offset = NULL;
		ldmsd_smplr_lock(smplr);

		offset = NULL;

		intrvl = ldmsd_time_us2str(smplr->interval_us);
		if (!intrvl) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			export->error = ENOMEM;
			return ENOMEM;
		}
		if (smplr->offset_us != LDMSD_UPDT_HINT_OFFSET_NONE) {
			offset = ldmsd_time_us2str(smplr->offset_us);
			if (!offset) {
				ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
				export->error = ENOMEM;
				return ENOMEM;
			}
		}

		__print_cfg_obj_hdr(export, "smplr");
		export->cnt += fprintf(export->f, "\"name\":\"%s\","
						  "\"plugin_instance\":\"%s\","
						  "\"interval\":\"%s\"",
						  smplr->obj.name,
						  smplr->pi->inst_name,
						  intrvl);

		if (offset) {
			export->cnt += fprintf(export->f,
					",\"offset\":\"%s\"", offset);
		}

		if (smplr->obj.perm != 0770) {
			export->cnt += fprintf(export->f, ",\"perm\":\"%o\"",
					smplr->obj.perm);
		}
		ldmsd_smplr_unlock(smplr);
		free(intrvl);
		if (offset)
			free(offset);
		__print_cfg_obj_tail(export);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_SMPLR);
	return 0;
}

static int __export_prdcrs(struct export_context *export)
{
	int rc = 0;
	char *interval;
	ldmsd_prdcr_t prdcr;
	ldmsd_prdcr_stream_t stream;
	ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
	for (prdcr = ldmsd_prdcr_first(); prdcr; prdcr = ldmsd_prdcr_next(prdcr)) {

		interval = ldmsd_time_us2str(prdcr->conn_intrvl_us);
		if (!interval) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			export->error = ENOMEM;
			ldmsd_prdcr_unlock(prdcr);
			ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
			return ENOMEM;
		}

		__print_cfg_obj_hdr(export, "prdcr");

		ldmsd_prdcr_lock(prdcr);

		/* mandatory attributes, except 'type' */
		export->cnt += fprintf(export->f,
					"\"name\":\"%s\","
					"\"type\":\"%s\","
					"\"host\":\"%s\","
					"\"port\":%hu,"
					"\"xprt\":\"%s\","
					"\"interval\":\"%s\"",
					prdcr->obj.name,
					ldmsd_prdcr_type2str(prdcr->type),
					prdcr->host_name,
					prdcr->port_no,
					prdcr->xprt_name,
					interval);

		/* authentication */
		if (prdcr->conn_auth) {
			export->cnt += fprintf(export->f,
					",\"auth\":\"%s\"", prdcr->conn_auth);
		}
		/* streams */
		if (!LIST_EMPTY(&prdcr->stream_list)) {
			export->cnt += fprintf(export->f,
					",\"streams\":[");
			export->is_print_comma = 0;
			LIST_FOREACH(stream, &prdcr->stream_list, entry) {
				if (export->is_print_comma)
					export->cnt += fprintf(export->f, ",");
				else
					export->is_print_comma = 1;
				export->cnt += fprintf(export->f, "\"%s\"",
								stream->name);
			}
			export->cnt += fprintf(export->f, "]");
		}

		/* perm */
		/*
		 * TODO: This is bad. We need the given permission by user
		 * before the object permission got manipulated.
		 */
		int perm = prdcr->obj.perm & ~LDMSD_PERM_DSTART;
		if (perm != 0770) {
			export->cnt += fprintf(export->f, ",\"perm\":\"%o\"",
					perm);
		}
		ldmsd_prdcr_unlock(prdcr);
		free(interval);
		__print_cfg_obj_tail(export);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
	return rc;
}

static int __export_updtrs(struct export_context *export)
{
	ldmsd_updtr_t updtr;
	ldmsd_name_match_t match;
	char *interval, *offset;
	ldmsd_prdcr_ref_t prd;
	struct rbn *rbn;
	json_entity_t sets, schemas, s;
	jbuf_t jb;

	ldmsd_cfg_lock(LDMSD_CFGOBJ_UPDTR);
	for (updtr = ldmsd_updtr_first(); updtr; updtr = ldmsd_updtr_next(updtr)) {
		sets = json_entity_new(JSON_LIST_VALUE);
		if (!sets)
			goto oom;
		schemas = json_entity_new(JSON_LIST_VALUE);
		if (!schemas)
			goto oom;

		offset = NULL;

		ldmsd_updtr_lock(updtr);
		interval = ldmsd_time_us2str(updtr->sched.intrvl_us);
		if (!interval)
			goto oom;
		if (updtr->sched.offset_us != LDMSD_UPDT_HINT_OFFSET_NONE) {
			offset = ldmsd_time_us2str(updtr->sched.offset_us);
			if (!offset)
				goto oom;
		}

		__print_cfg_obj_hdr(export, "updtr");

		/* mandatory attributes */
		export->cnt += fprintf(export->f, "\"name\":\"%s\""
						",\"interval\":\"%s\"",
						updtr->obj.name, interval);

		/* offset */
		if (updtr->sched.offset_us != LDMSD_UPDT_HINT_OFFSET_NONE) {
			export->cnt += fprintf(export->f,
					",\"offset\":\"%s\"", offset);
		}
		/* auto interval */
		if (updtr->is_auto_task) {
			export->cnt += fprintf(export->f,
					",\"auth_interval\":true");
		}
		/* push */
		if (updtr->push_flags) {
			if (updtr->push_flags && LDMSD_UPDTR_F_PUSH_CHANGE) {
				export->cnt += fprintf(export->f, ",\"push\":\"onchange\"");
			} else {
				export->cnt += fprintf(export->f, ",\"push\":\"yes\"");
			}
		}

		/* perm */
		int perm = updtr->obj.perm & LDMSD_PERM_DSTART;
		if (perm != 0770) {
			export->cnt += fprintf(export->f,
						",\"perm\":\"%o\"", perm);
		}

		/* producers */
		export->cnt += fprintf(export->f, ",\"producers\":[");
		export->is_print_comma = 0;
		for (rbn = rbt_min(&updtr->prdcr_tree); rbn; rbn = rbn_succ(rbn)) {
			prd = container_of(rbn, struct ldmsd_prdcr_ref, rbn);
			if (export->is_print_comma)
				export->cnt += fprintf(export->f, ",");
			else
				export->is_print_comma = 1;
			export->cnt += fprintf(export->f, "\"%s\"",
						prd->prdcr->obj.name);
		}
		export->cnt += fprintf(export->f, "]");

		/* matched sets by set names or schema names */
		for (match = ldmsd_updtr_match_first(updtr); match;
				match = ldmsd_updtr_match_next(match)) {
			s = json_entity_new(JSON_STRING_VALUE, match->regex_str);
			if (!s)
				goto oom;
			if (match->selector == LDMSD_NAME_MATCH_INST_NAME)
				json_item_add(sets, s);
			else
				json_item_add(schemas, s);
		}
		/* matched by set names */
		if (0 != json_list_len(sets)) {
			jb = json_entity_dump(NULL, sets);
			if (!jb)
				goto oom;
			export->cnt += fprintf(export->f,
					",\"set_instances\":%s", jb->buf);
			jbuf_free(jb);
		}

		/* matched by schema names */
		if (0 != json_list_len(schemas)) {
			jb = json_entity_dump(NULL, schemas);
			if (!jb)
				goto oom;
			export->cnt += fprintf(export->f,
					",\"set_instances\":%s", jb->buf);
			jbuf_free(jb);
		}
		ldmsd_updtr_unlock(updtr);

		__print_cfg_obj_tail(export);

		free(interval);
		if (offset)
			free(offset);
		json_entity_free(sets);
		json_entity_free(schemas);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_UPDTR);
	return 0;
oom:
	ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
	export->error = ENOMEM;
	ldmsd_updtr_unlock(updtr);
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_UPDTR);
	return ENOMEM;
}

static int __export_strgps(struct export_context *export)
{
	ldmsd_strgp_t strgp;
	ldmsd_name_match_t match;
	ldmsd_strgp_metric_t metric;

	ldmsd_cfg_lock(LDMSD_CFGOBJ_STRGP);
	for (strgp = ldmsd_strgp_first(); strgp; strgp = ldmsd_strgp_next(strgp)) {
		ldmsd_strgp_lock(strgp);

		__print_cfg_obj_hdr(export, "strgp");

		/* mandatory attributes */
		export->cnt += fprintf(export->f,
				"\"name\":\"%s\",\"container\":\"%s\",\"schema\":\"%s\"",
				strgp->obj.name,
				strgp->inst->inst_name,
				strgp->schema);

		/* permission */
		int perm = strgp->obj.perm & ~LDMSD_PERM_DSTART;
		if (perm != 0770) {
			export->cnt += fprintf(export->f, ",\"perm\":\"%o\"", perm);
		}

		/* Producers */
		if (LIST_EMPTY(&strgp->prdcr_list))
			goto metrics;
		export->is_print_comma = 0;
		export->cnt += fprintf(export->f, ",\"producers\":[");
		LIST_FOREACH(match, &strgp->prdcr_list, entry) {
			if (export->is_print_comma)
				export->cnt += fprintf(export->f, ",");
			else
				export->is_print_comma = 1;
			export->cnt += fprintf(export->f, "\"%s\"", match->regex_str);
		}
		export->cnt += fprintf(export->f, "]");

	metrics:
		/* strgp_metric_add */
		if (TAILQ_EMPTY(&strgp->metric_list))
			goto next;
		export->is_print_comma = 0;
		export->cnt += fprintf(export->f, ",\"metrics\":[");
		TAILQ_FOREACH(metric, &strgp->metric_list, entry) {
			if (export->is_print_comma)
				export->cnt += fprintf(export->f, ",");
			else
				export->is_print_comma = 1;
			export->cnt += fprintf(export->f, "\"%s\"", metric->name);
		}
		export->cnt += fprintf(export->f, "]");

	next:
		ldmsd_strgp_unlock(strgp);
		__print_cfg_obj_tail(export);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_STRGP);
	return 0;
}

//struct setgroup_ctxt {
//	FILE *f;
//	ldmsd_setgrp_t setgrp;
//	int cnt;
//};
//
//static int __export_setgroup_member(ldms_set_t set, const char *name, void *arg)
//{
//	struct setgroup_ctxt *ctxt = (struct setgroup_ctxt *)arg;
//	if (ctxt->cnt == 0) {
//		fprintf(ctxt->f, "setgroup_ins name=%s instance=%s",
//					ctxt->setgrp->obj.name, name);
//	} else {
//		fprintf(ctxt->f, ",%s", name);
//	}
//	ctxt->cnt++;
//	return 0;
//}
//
//static int __decimal_to_octal(int decimal)
//{
//	int octal = 0;
//	int i = 1;
//	while (0 != decimal) {
//		octal += (decimal % 8) * i;
//		decimal /= 8;
//		i *= 10;
//	}
//	return octal;
//}
//
//static int __export_setgroups_config(FILE *f)
//{
//	int rc = 0;
//	ldmsd_setgrp_t setgrp;
//	struct setgroup_ctxt ctxt = {f, 0, 0};
//	fprintf(f, "# ----- Setgroups -----\n");
//	ldmsd_cfg_lock(LDMSD_CFGOBJ_SETGRP);
//	for (setgrp = ldmsd_setgrp_first(); setgrp;
//			setgrp = ldmsd_setgrp_next(setgrp)) {
//		ldmsd_setgrp_lock(setgrp);
//		/* setrgroup_add */
//		fprintf(f, "setgroup_add name=%s producer=%s perm=0%d",
//					setgrp->obj.name, setgrp->producer,
//					__decimal_to_octal(setgrp->obj.perm));
//		if (setgrp->interval_us) {
//			fprintf(f, " interval=%ld", setgrp->interval_us);
//			if (setgrp->offset_us != LDMSD_UPDT_HINT_OFFSET_NONE)
//				fprintf(f, " offset=%ld", setgrp->offset_us);
//		}
//		fprintf(f, "\n");
//
//		/* setgroup_ins */
//		ctxt.setgrp = setgrp;
//		rc = ldmsd_group_iter(setgrp->set, __export_setgroup_member, &ctxt);
//		if (rc) {
//			ldmsd_setgrp_unlock(setgrp);
//			goto out;
//		}
//		fprintf(f, "\n"); /* newline of the setgroup_ins line */
//		ldmsd_setgrp_unlock(setgrp);
//	}
//out:
//	ldmsd_cfg_unlock(LDMSD_CFGOBJ_SETGRP);
//	return rc;
//}

static int __export_plugin_instances(struct export_context *export)
{
	ldmsd_plugin_inst_t inst;
	json_entity_t json, config;
	jbuf_t jb;
	json = NULL;
	int rc;

	LDMSD_PLUGIN_INST_FOREACH(inst) {
		json = inst->base->query(inst, "config");
		if (!json || !(config = json_value_find(json, "config"))) {
			ldmsd_log(LDMSD_LERROR, "Failed to export the config of "
					"plugin instance '%s'. "
					"The config record cannot be founded.\n",
					inst->inst_name);
			goto err;
		}
		/*
		 * Assume that \c json is a JSON dict that contains
		 * the plugin instance configuration attributes.
		 */
		if (JSON_LIST_VALUE != json_entity_type(config)) {
			ldmsd_log(LDMSD_LERROR, "Failed to export the config of "
					"plugin instance '%s'. "
					"LDMSD cannot interpret the query result.\n",
					inst->inst_name);
			export->error = EINVAL;
			goto err;
		}

		jb = json_entity_dump(NULL, config);
		if (!jb) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			export->error = ENOMEM;
			goto err;
		}

		__print_cfg_obj_hdr(export, "plugin_instance");
		export->cnt += fprintf(export->f, "\"name\":\"%s\","
						"\"plugin\":\"%s\","
						"\"config\":%s",
						inst->inst_name,
						inst->plugin_name,
						jb->buf);
		__print_cfg_obj_tail(export);
		jbuf_free(jb);
		json_entity_free(json);
		continue;

err:
		json_entity_free(json);
		rc = ldmsd_send_error(export->reqc, export->error, "Failed to export "
			"the config of plugin instance '%s'.", inst->inst_name);
		return rc;

	}
	return 0;
}

//int failover_config_export(FILE *f);
static int export_config_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t spec, file, list, item;
	struct export_context export = {0};
	int mode = 0; /* 0x1 -- env, 0x10 -- cmdline, 0x100 -- cfgcmd */
	char *path, *s;

	spec = json_value_find(reqc->json, "spec");
	if (!spec) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:export_config", "spec");
	}

	/* file */
	file = json_value_find(spec, "file");
	if (!file) {
		return ldmsd_send_missing_attr_err(reqc,
				"cmd_obj:export_config:spec", "file");
	}
	if (JSON_STRING_VALUE != json_entity_type(file)) {
		return ldmsd_send_type_error(reqc,
				"cmd_obj:export_config:spec:file", "a string");
	}
	path = json_value_str(file)->str;

	export.f = fopen(path, "w");
	if (!export.f) {
		rc = errno;
		return ldmsd_send_error(reqc, rc, "cmd_obj:export_config - "
				"Failed to open the file '%s', errno %d.",
				path, rc);
	}
	export.cnt = 0;
	export.is_print_comma = 0;
	export.reqc = reqc;

	/* export env */
	list = json_value_find(spec, "export");
	if (list) {
		if (JSON_LIST_VALUE != json_entity_type(list)) {
			rc = ldmsd_send_type_error(reqc,
				"cmd_obj:export_config:spec:export", "a list");
			goto out;
		}
		for (item = json_item_first(list); item; item = json_item_next(item)) {
			if (JSON_STRING_VALUE != json_entity_type(item)) {
				rc = ldmsd_send_type_error(reqc,
					"cmd_obj:export_config:spec:export",
							"a list of string");
				goto out;
			}
			s = json_value_str(item)->str;

			if (0 == strncmp(s, "env", 3))
				mode |= 0x1;
			else if (0 == strncmp(s, "daemon", 6))
				mode |= 0x10;
			else if (0 == strncmp(s, "cfg_obj", 7))
				mode |= 0x100;
			else {
				rc = ldmsd_send_error(reqc, EINVAL,
						"cmd_obj:export_config - "
						"unrecognized export option '%s'.",
						s);
				goto out;
			}
		}
	}

	export.cnt = fprintf(export.f, "[");

	if (!mode || (mode & 0x1)) {
		/* export environment variables */
		rc = __export_envs(&export);
		if (rc || (export.error))
			goto out;

	}
	if (!mode || (mode & 0x10)) {
		/* Export authentication methods */
		rc = __export_auth_cfg_objs(&export);
		if (rc || export.error)
			goto out;
		/* export command-line options */
		__export_daemon_cfg_obj(&export);
		if (export.error)
			goto out;
		/* Export listening endpoint configuration */
		__export_listen_cfg_objs(&export);
		if (export.error)
			goto out;
	}
	if (!mode || (mode & 0x100)) {
		rc = __export_plugin_instances(&export);
		if (rc || export.error)
			goto out;
		rc = __export_smplrs(&export);
		if (rc || export.error) {
			rc = ldmsd_send_error(reqc, rc, "cmd_obj:export_config - "
					"Failed to export the 'smplr' "
					"configuration objects");
			goto out;
		}
		rc = __export_prdcrs(&export);
		if (rc || export.error) {
			rc = ldmsd_send_error(reqc, rc, "cmd_obj:export_config - "
					"Failed to export the Producer-related "
					"config commands");
			goto out;
		}
		rc = __export_updtrs(&export);
		if (rc) {
			rc = ldmsd_send_error(reqc, rc, "cmd_obj:export_config - "
					"Failed to export the Updater-related "
					"config commands");
			goto out;
		}
		rc = __export_strgps(&export);
		if (rc) {
			rc = ldmsd_send_error(reqc, rc, "cmd_obj:export_config - "
					"Failed to export the Storage "
					"policy-related config commands");
			goto out;
		}
//		rc = __export_setgroups_config(f);
//		if (rc) {
//			rc = ldmsd_send_error(reqc, rc, "cmd_obj:export_config - "
//					"Failed to export the setgroup-related "
//					"config commands");
//			goto out;
//		}
//		rc = failover_config_export(f);
//		if (rc) {
//			reqc->errcode = rc;
//			(void)snprintf(reqc->recv_buf, reqc->recv_len,
//					"Failed to export the failover-related "
//					"config commands");
//			goto send_reply;
//		}
	}
	export.cnt += fprintf(export.f, "]");
	rc = ldmsd_send_error(reqc, 0, NULL);
out:
	if (export.f)
		fclose(export.f);
	return rc;
}

static int auth_handler(ldmsd_req_ctxt_t reqc)
{
	int rc = 0;
	struct attr_value_list *avl = NULL;
	json_entity_t name, plugin, args, arg, spec;
	json_str_t an, av;
	char *name_s = NULL, *plugin_s = NULL;
	ldmsd_auth_t auth_dom = NULL;

	spec = json_value_find(reqc->json, "spec");
	name = json_value_find(spec, "name");
	if (!name) {
		rc = ldmsd_send_missing_attr_err(reqc, "auth", "name");
		goto out;
	}
	if (JSON_STRING_VALUE != json_entity_type(name)) {
		rc = ldmsd_send_error(reqc, EINVAL, "auth:name must be a string.");
		goto out;
	}
	name_s = json_value_str(name)->str;

	plugin = json_value_find(spec, "plugin");
	if (!plugin) {
		rc = ldmsd_send_missing_attr_err(reqc, "auth", "plugin");
		goto out;
	}
	if (JSON_STRING_VALUE != json_entity_type(plugin)) {
		rc = ldmsd_send_error(reqc, EINVAL, "auth:plugin must be a string.");
		goto out;
	}
	plugin_s = json_value_str(plugin)->str;

	args = json_value_find(spec, "args");
	if (!args)
		goto new_auth;

	avl = av_new(json_attr_count(args));
	if (!avl) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		rc = ENOMEM;
		goto out;
	}
	for (arg = json_attr_first(args); arg; arg = json_attr_next(arg)) {
		an = json_attr_name(arg);
		av = json_value_str(json_attr_value(arg));
		rc = av_add(avl, an->str, av->str);
		if (rc)
			goto out;
	}

new_auth:
	auth_dom = ldmsd_auth_new_with_auth(name_s, plugin_s, avl,
					    geteuid(), getegid(), 0600);
	if (!auth_dom) {
		rc = ldmsd_send_error(reqc, errno,
				"Authentication domain creation failed, "
				"errno: %d", errno);
	}

out:
	return rc;
}

static int auth_action_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	json_entity_t action, names, name, regex;
	char *name_s, *action_s;
	struct ldmsd_sec_ctxt sctxt;

	action = json_value_find(reqc->json, "action");
	action_s = json_value_str(action)->str;
	names = json_value_find(reqc->json, "names");
	regex = json_value_find(reqc->json, "regex");

	if (regex) {
		return ldmsd_send_error(reqc, ENOTSUP,
				"act_obj:smplr: 'regex' not supported. "
				"All the name in 'names' were processed.");
	}

	ldmsd_req_ctxt_sec_get(reqc, &sctxt);

	if (0 == strncmp(action_s, "delete", 6)) {
		for (name = json_item_first(names); name;
				name = json_item_next(name)) {
			name_s = json_value_str(name)->str;
			rc = ldmsd_auth_del(name_s, &sctxt);
			switch (rc) {
			case 0:
				break;
			case EACCES:
				return ldmsd_send_error(reqc, rc,
					"auth '%s': action '%s': Permission denied",
					name_s, action_s);
			case ENOENT:
				return ldmsd_send_error(reqc, rc,
					 "authentication domain '%s' not found "
					 "- action '%s'", name_s, action_s);
			default:
				return ldmsd_send_error(reqc, rc,
					 "Failed to delete authentication domain '%s'",
					 name_s);
			}
		}
	} else {
		return ldmsd_send_error(reqc, ENOTSUP, "Authentication action "
						"'%s' not supported.", action_s);
	}
	return ldmsd_send_error(reqc, 0, NULL);
}
