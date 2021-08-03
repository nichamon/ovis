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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <coll/rbt.h>
#include <ovis_util/util.h>
#include <netdb.h>
#include "ldms.h"
#include "ldmsd.h"
#include "ldms_xprt.h"
#include "ldmsd_request.h"
#include "ldmsd_failover.h"
#include "ldmsd_event.h"
#include "ldmsd_cfgobj.h"
#include "config.h"

static void prdcr_task_cb(ldmsd_task_t task, void *arg);

int prdcr_resolve(const char *hostname, unsigned short port_no,
		  struct sockaddr_storage *ss, socklen_t *ss_len)
{
	struct hostent *h;

	h = gethostbyname(hostname);
	if (!h)
		return -1;

	if (h->h_addrtype != AF_INET)
		return -1;

	memset(ss, 0, sizeof *ss);
	struct sockaddr_in *sin = (struct sockaddr_in *)ss;
	sin->sin_addr.s_addr = *(unsigned int *)(h->h_addr_list[0]);
	sin->sin_family = h->h_addrtype;
	sin->sin_port = htons(port_no);
	*ss_len = sizeof(*sin);
	return 0;
}

void ldmsd_prdcr___del(ldmsd_cfgobj_t obj)
{
	ldmsd_prdcr_t prdcr = (ldmsd_prdcr_t)obj;
	if (prdcr->host_name)
		free(prdcr->host_name);
	if (prdcr->xprt_name)
		free(prdcr->xprt_name);
	if (prdcr->conn_auth)
		free(prdcr->conn_auth);
	if (prdcr->conn_auth_args)
		av_free(prdcr->conn_auth_args);
	ldmsd_cfgobj___del(obj);
}

static ldmsd_prdcr_set_t prdcr_set_new(const char *inst_name, const char *schema_name)
{
	ldmsd_prdcr_set_t set = calloc(1, sizeof *set);
	if (!set)
		goto err_0;
	set->state = LDMSD_PRDCR_SET_STATE_START;
	set->inst_name = strdup(inst_name);
	if (!set->inst_name)
		goto err_1;
	set->schema_name = strdup(schema_name);
	if (!set->schema_name)
		goto err_2;
	pthread_mutex_init(&set->lock, NULL);
	rbn_init(&set->rbn, set->inst_name);

	set->ref_count = 1;
	set->worker = assign_prdset_worker();
	return set;
 err_2:
	free(set->inst_name);
 err_1:
	free(set);
 err_0:
	return NULL;
}

void __prdcr_set_del(ldmsd_prdcr_set_t set)
{
	ldmsd_log(LDMSD_LINFO, "Deleting producer set %s from producer %s\n",
				set->inst_name, set->prdcr->obj.name);
	if (set->schema_name)
		free(set->schema_name);

	if (set->set) {
		ldms_set_ref_put(set->set, "prdcr_set");
		ldms_set_unpublish(set->set);
		ldms_set_delete(set->set);
	}

	ldmsd_strgp_ref_t strgp_ref = LIST_FIRST(&set->strgp_list);
	while (strgp_ref) {
		LIST_REMOVE(strgp_ref, entry);
		ldmsd_strgp_put(strgp_ref->strgp);
		free(strgp_ref);
		strgp_ref = LIST_FIRST(&set->strgp_list);
	}

	if (set->updt_hint_entry.le_prev)
		LIST_REMOVE(set, updt_hint_entry);

	free(set->inst_name);
	free(set);
}

void ldmsd_prdcr_set_ref_get(ldmsd_prdcr_set_t set)
{
	assert(set->ref_count);
	(void)__sync_fetch_and_add(&set->ref_count, 1);
}

void ldmsd_prdcr_set_ref_put(ldmsd_prdcr_set_t set)
{
	assert(set->ref_count);
	if (0 == __sync_sub_and_fetch(&set->ref_count, 1))
		__prdcr_set_del(set);
}

static void prdcr_set_del(ldmsd_prdcr_set_t set)
{
	set->state = LDMSD_PRDCR_SET_STATE_START;
	ldmsd_prdcr_set_ref_put(set);
}

static int __prdset_state_start(ldmsd_prdcr_set_t prdset, struct prdset_data *data)
{
	ev_t updtr_ev, strgp_ev;
	struct prdset_data *updtr_ev_data, *strgp_ev_data;
	updtr_ev_data = strgp_ev_data = NULL;

	updtr_ev = ev_new(prdset_state_type);
	if (!updtr_ev) {
		LDMSD_LOG_ENOMEM();
		goto enomem;
	}
	strgp_ev = ev_new(prdset_state_type);
	if (!strgp_ev) {
		LDMSD_LOG_ENOMEM();
		goto enomem;
	}

	updtr_ev_data = EV_DATA(updtr_ev, struct prdset_data);
	strgp_ev_data = EV_DATA(strgp_ev, struct prdset_data);
	memset(updtr_ev_data, 0, sizeof(struct prdset_data));
	memset(strgp_ev_data, 0, sizeof(struct prdset_data));

	if (prdset_data_copy(data, updtr_ev_data))
		goto enomem;
	if (prdset_data_copy(data, strgp_ev_data)) {
		prdset_data_cleanup(updtr_ev_data);
		goto enomem;
	}


	ldmsd_prdcr_set_ref_get(prdset);
	updtr_ev_data->prdset = prdset;
	ldmsd_prdcr_set_ref_get(prdset);
	strgp_ev_data->prdset = prdset;
	ev_post(prdset->worker, updtr_tree_w, updtr_ev, 0);
	ev_post(prdset->worker, strgp_tree_w, strgp_ev, 0);
	return 0;
enomem:
	if (updtr_ev) {
		ev_put(updtr_ev);
	}
	if (strgp_ev) {
		ev_put(strgp_ev);
	}
	return ENOMEM;
}

int prdset_state_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e)
{
	if (EV_OK != status)
		return 0;

	int rc;
	struct prdset_data *prdset_data = EV_DATA(e, struct prdset_data);
	switch (prdset_data->state) {
	case LDMSD_PRDCR_SET_STATE_START:
		rc = __prdset_state_start(prdset_data->prdset, prdset_data);
		break;
	default:
		break;
	}

	prdset_data_cleanup(prdset_data);
	ev_put(e);
	return rc;
}

static int
__post_prdset_state_ev(ldmsd_prdcr_set_t prdset, ev_worker_t src, ev_worker_t dst)
{
	struct prdset_data *prdset_data;
	ev_t prdset_ev = ev_new(prdset_state_type);
	if (!prdset_ev) {
		LDMSD_LOG_ENOMEM();
		goto enomem;
	}
	prdset_data = EV_DATA(prdset_ev, struct prdset_data);
	ldmsd_prdcr_set_ref_get(prdset);
	memset(prdset_data, 0, sizeof(*prdset_data));
	prdset_data->prdset = prdset;
	prdset_data->state = prdset->state;
	prdset_data->prdcr_name = strdup(prdset->prdcr->obj.name);
	if (!prdset_data->prdcr_name) {
		LDMSD_LOG_ENOMEM();
		ldmsd_prdcr_set_ref_put(prdset);
		goto enomem;
	}

	return ev_post(src, dst, prdset_ev, 0);
enomem:
	if (prdset_ev)
		ev_put(prdset_ev);
	return ENOMEM;
}

extern int __setgrp_members_lookup(ldmsd_prdcr_set_t setgrp);
int prdset_lookup_complete_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e)
{
	if (EV_OK != status)
		return 0;

	int flags;
	int rc = 0;
	ldms_set_t set = EV_DATA(e, struct lookup_data)->set;
	ldmsd_prdcr_set_t prdset = EV_DATA(e, struct lookup_data)->prdset;
	if (LDMS_LOOKUP_OK != EV_DATA(e, struct lookup_data)->status) {
		prdset->state = LDMSD_PRDCR_SET_STATE_START;
		goto out;
	}

	if (!prdset->set) {
		/* This is the first lookup of the set. */
		ldms_set_ref_get(set, "prdcr_set");
		prdset->set = set;
	} else {
		assert(0 == "multiple lookup on the same prdcr_set");
	}
	flags = ldmsd_group_check(prdset->set);
	if (flags & LDMSD_GROUP_IS_GROUP) {
		/*
		 * Lookup the member sets
		 */
		if (__setgrp_members_lookup(prdset))
			goto out;
	}
	prdset->state = LDMSD_PRDCR_SET_STATE_READY;
	ldmsd_log(LDMSD_LINFO, "Set %s is ready.\n", prdset->inst_name);

	rc = __post_prdset_state_ev(prdset, prdset->worker, updtr_tree_w);
	/* TODO: REMOVE This. ldmsd_strgp_update(prdset); */

out:
	ldmsd_prdcr_set_ref_put(prdset); /* Put back the reference taken before calling ldms_xprt_lookup() */
	ev_put(e);
	return rc;
}

void __ldmsd_prdset_lookup_cb(ldms_t xprt, enum ldms_lookup_status status,
					int more, ldms_set_t set, void *arg)
{
	ev_t ev;
	ldmsd_prdcr_set_t prd_set = arg;

	if (status != LDMS_LOOKUP_OK) {
		assert(NULL == set);
		status = (status < 0 ? -status : status);
		if (status == ENOMEM) {
			ldmsd_log(LDMSD_LERROR,
				"prdcr %s: Set memory allocation failure in lookup of "
				"set '%s'. Consider changing the -m parameter on the "
				"command line to a larger value. The current value is %s\n",
				prd_set->prdcr->obj.name,
				prd_set->inst_name,
				ldmsd_get_max_mem_sz_str());
		} else if (status == EEXIST) {
			ldmsd_log(LDMSD_LERROR,
					"prdcr %s: The set '%s' already exists. "
					"It is likely that there are multiple "
					"producers providing a set with the same instance name.\n",
					prd_set->prdcr->obj.name, prd_set->inst_name);
		} else {
			ldmsd_log(LDMSD_LERROR,
				  	"prdcr %s: Error %d in lookup callback of set '%s'\n",
					prd_set->prdcr->obj.name,
					status, prd_set->inst_name);
		}
	}

	ev = ev_new(lookup_complete_type);
	EV_DATA(ev, struct lookup_data)->prdset = prd_set;
	EV_DATA(ev, struct lookup_data)->more = more;
	EV_DATA(ev, struct lookup_data)->set = set;
	EV_DATA(ev, struct lookup_data)->status = status;
	ev_post(NULL, prd_set->worker, ev, 0);
}

static int __prdset_updtr_running(ldmsd_prdcr_set_t prdset)
{
	int rc;
	switch (prdset->state) {
	case LDMSD_PRDCR_SET_STATE_START:
		prdset->state = LDMSD_PRDCR_SET_STATE_LOOKUP;
		assert(prdset->set == NULL);
		rc = ldms_xprt_lookup(prdset->prdcr->xprt, prdset->inst_name,
				      LDMS_LOOKUP_BY_INSTANCE,
				      __ldmsd_prdset_lookup_cb, prdset);
		if (rc) {
			/* If the error is EEXIST, the set is already in the set tree. */
			if (rc == EEXIST) {
				ldmsd_log(LDMSD_LERROR, "Prdcr '%s': "
					"lookup failed synchronously. "
					"The set '%s' already exists. "
					"It is likely that there are more "
					"than one producers pointing to "
					"the set.\n",
					prdset->prdcr->obj.name,
					prdset->inst_name);
			} else {
				ldmsd_log(LDMSD_LINFO, "Synchronous error "
						"%d from ldms_lookup\n", rc);
			}
			prdset->state = LDMSD_PRDCR_SET_STATE_START;
		}
		break;
	case LDMSD_PRDCR_SET_STATE_READY:
		break;
	default:
		break;
	}
	return rc;
}

int prdset_updtr_state_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e)
{
	if (EV_OK != status)
		return 0;

	int rc;
	enum ldmsd_updtr_state updtr_state = EV_DATA(e, struct updtr_state_data)->state;
	ldmsd_prdcr_set_t prdset = EV_DATA(e, struct updtr_state_data)->obj;

	ldmsd_log(LDMSD_LINFO, "prdset '%s' received updtr_state event with state '%d'\n",
							prdset->inst_name, updtr_state);

	switch (updtr_state) {
	case LDMSD_UPDTR_STATE_RUNNING:
		rc = __prdset_updtr_running(prdset);
		break;
	default:
		break;
	}
	return rc;
}

#define UPDT_HINT_TREE_ADD 1
#define UPDT_HINT_TREE_REMOVE 2
void prdcr_hint_tree_update(ldmsd_prdcr_t prdcr, ldmsd_prdcr_set_t prd_set,
				struct ldmsd_updtr_schedule *hint, int op)
{
	struct rbn *rbn;
	struct ldmsd_updt_hint_set_list *list;
	struct ldmsd_updtr_schedule *hint_key;
	if (0 == hint->intrvl_us)
		return;
	rbn = rbt_find(&prdcr->hint_set_tree, hint);
	if (op == UPDT_HINT_TREE_REMOVE) {
		if (!rbn)
			return;
		list = container_of(rbn, struct ldmsd_updt_hint_set_list, rbn);
		assert(prd_set->ref_count);
		assert(prd_set->updt_hint_entry.le_prev);
		LIST_REMOVE(prd_set, updt_hint_entry);
		prd_set->updt_hint_entry.le_next = NULL;
		prd_set->updt_hint_entry.le_prev = NULL;
		ldmsd_prdcr_set_ref_put(prd_set);

		if (LIST_EMPTY(&list->list)) {
			rbt_del(&prdcr->hint_set_tree, &list->rbn);
			free(list->rbn.key);
			free(list);
		}
	} else if (op == UPDT_HINT_TREE_ADD) {
		if (!rbn) {
			list = malloc(sizeof(*list));
			hint_key = malloc(sizeof(*hint_key));
			*hint_key = *hint;
			rbn_init(&list->rbn, hint_key);
			rbt_ins(&prdcr->hint_set_tree, &list->rbn);
			LIST_INIT(&list->list);
		} else {
			list = container_of(rbn,
					struct ldmsd_updt_hint_set_list, rbn);
		}
		ldmsd_prdcr_set_ref_get(prd_set);
		LIST_INSERT_HEAD(&list->list, prd_set, updt_hint_entry);
	}
}

static void prdcr_reset_set(ldmsd_prdcr_t prdcr, ldmsd_prdcr_set_t prd_set)
{
	prdcr_hint_tree_update(prdcr, prd_set,
			       &prd_set->updt_hint, UPDT_HINT_TREE_REMOVE);
	rbt_del(&prdcr->set_tree, &prd_set->rbn);
	ldmsd_prdcr_set_ref_put(prd_set);	/* set_tree reference */
	prdcr_set_del(prd_set);
}

/**
 * Destroy all sets for the producer
 */
static void prdcr_reset_sets(ldmsd_prdcr_t prdcr)
{
	ldmsd_prdcr_set_t prd_set;
	struct rbn *rbn;
	while ((rbn = rbt_min(&prdcr->set_tree))) {
		prd_set = container_of(rbn, struct ldmsd_prdcr_set, rbn);
		prdcr_reset_set(prdcr, prd_set); /* TODO: event-driven */
	}
}

/**
 * Find the prdcr_set with the matching instance name
 *
 * Must be called with the prdcr->lock held.
 */
static ldmsd_prdcr_set_t _find_set(ldmsd_prdcr_t prdcr, const char *inst_name)
{
	struct rbn *rbn = rbt_find(&prdcr->set_tree, inst_name);
	if (rbn)
		return container_of(rbn, struct ldmsd_prdcr_set, rbn);
	return NULL;
}

void ldmsd_prd_set_updtr_task_update(ldmsd_prdcr_set_t prd_set)
{
	ldmsd_updtr_t updtr;
	ldmsd_name_match_t match;
	char *str;
	int rc;
	ldmsd_cfg_lock(LDMSD_CFGOBJ_UPDTR);
	for (updtr = ldmsd_updtr_first(); updtr; updtr = ldmsd_updtr_next(updtr)) {
		ldmsd_updtr_lock(updtr);
		if (updtr->state != LDMSD_UPDTR_STATE_RUNNING) {
			ldmsd_updtr_unlock(updtr);
			continue;
		}

		/* Updaters for push don't schedule any updates. */
		if (0 != updtr->push_flags) {
			ldmsd_updtr_unlock(updtr);
			continue;
		}
		if (!ldmsd_updtr_prdcr_find(updtr, prd_set->prdcr->obj.name)) {
			ldmsd_updtr_unlock(updtr);
			continue;
		}
		if (!TAILQ_EMPTY(&updtr->match_list)) {
			TAILQ_FOREACH(match, &updtr->match_list, entry) {
				if (match->selector == LDMSD_NAME_MATCH_INST_NAME)
					str = prd_set->inst_name;
				else
					str = prd_set->schema_name;
				rc = regexec(&match->regex, str, 0, NULL, 0);
				if (!rc)
					goto update_tasks;
			}
			goto nxt_updtr;
		}
	update_tasks:
		pthread_mutex_lock(&prd_set->lock);
		ldmsd_updtr_tasks_update(updtr, prd_set);
		pthread_mutex_unlock(&prd_set->lock);
	nxt_updtr:
		ldmsd_updtr_unlock(updtr);
	}


	ldmsd_cfg_unlock(LDMSD_CFGOBJ_UPDTR);
}

static void __update_set_info(ldmsd_prdcr_set_t set, ldms_dir_set_t dset)
{
	long intrvl_us;
	long offset_us;
	char *hint = ldms_dir_set_info_get(dset, LDMSD_SET_INFO_UPDATE_HINT_KEY);
	if (hint) {
		char *endptr;
		char *s = strdup(hint);
		char *tok;
		if (!s) {
			ldmsd_lerror("%s:%d Memory allocation failure.\n",
				     __func__, __LINE__);
			return;
		}
		tok = strtok_r(s, ":", &endptr);
		offset_us = LDMSD_UPDT_HINT_OFFSET_NONE;
		intrvl_us = strtol(tok, NULL, 0);
		tok = strtok_r(NULL, ":", &endptr);
		if (tok)
			offset_us = strtol(tok, NULL, 0);

		/* Sanity check the hints */
		if (offset_us >= intrvl_us) {
			ldmsd_lerror("set %s: Invalid hint '%s', ignoring hint\n",
					set->inst_name, hint);
		} else {
			if (offset_us != LDMSD_UPDT_HINT_OFFSET_NONE)
				set->updt_hint.offset_us = offset_us;
			set->updt_hint.intrvl_us = intrvl_us;
		}
		free(s);
	}
}

extern void __ldmsd_prdset_lookup_cb(ldms_t xprt, enum ldms_lookup_status status,
				     int more, ldms_set_t set, void *arg);
static void _add_cb(ldms_t xprt, ldmsd_prdcr_t prdcr, ldms_dir_set_t dset)
{
	ldmsd_prdcr_set_t set;

	ldmsd_log(LDMSD_LINFO, "Adding the metric set '%s'\n", dset->inst_name);

	/* Check to see if it's already there */
	set = _find_set(prdcr, dset->inst_name);
	if (!set) {
		/* See if the ldms set is already there */
		ldms_set_t xs = ldms_xprt_set_by_name(xprt, dset->inst_name);
		if (xs) {
			ldmsd_log(LDMSD_LCRITICAL, "Received dir_add, prdset is missing, but set %s is present...ignoring",
					dset->inst_name);
			return;
		}
		set = prdcr_set_new(dset->inst_name, dset->schema_name);
		if (!set) {
			ldmsd_log(LDMSD_LERROR, "Memory allocation failure in %s "
				 "for set_name %s\n",
				 __FUNCTION__, dset->inst_name);
			return;
		}
		set->prdcr = prdcr;
		ldmsd_prdcr_set_ref_get(set); 	/* set_tree reference */
		rbt_ins(&prdcr->set_tree, &set->rbn);
	} else {
		ldmsd_log(LDMSD_LCRITICAL, "Received a dir_add update for "
			  "'%s', prdcr_set still present with refcount %d, and set "
			  "%p.\n", dset->inst_name, set->ref_count, set->set);
		return;
	}

	__update_set_info(set, dset);
	if (0 != set->updt_hint.intrvl_us) {
		ldmsd_log(LDMSD_LDEBUG, "producer '%s' add set '%s' to hint tree\n",
						prdcr->obj.name, set->inst_name);
		prdcr_hint_tree_update(prdcr, set,
				&set->updt_hint, UPDT_HINT_TREE_ADD);
 	}

	ev_t prdset_ev = ev_new(prdset_state_type);
	if (!prdset_ev) {
		LDMSD_LOG_ENOMEM();
		return;
	}
	(void) __post_prdset_state_ev(set, prdcr->worker, set->worker);
}

/*
 * Process the directory list and add or restore specified sets.
 */
static void prdcr_dir_cb_add(ldms_t xprt, ldms_dir_t dir, ldmsd_prdcr_t prdcr)
{
	int i;
	for (i = 0; i < dir->set_count; i++)
		_add_cb(xprt, prdcr, &dir->set_data[i]);
}

static void prdcr_dir_cb_list(ldms_t xprt, ldms_dir_t dir, ldmsd_prdcr_t prdcr)
{
	return prdcr_dir_cb_add(xprt, dir, prdcr);
}

/*
 * Process the deleted set. This will only be received from downstream
 * peers that are older than 4.3.4
 */
static void prdcr_dir_cb_del(ldms_t xprt, ldms_dir_t dir, ldmsd_prdcr_t prdcr)
{
	ldmsd_prdcr_set_t set;
	int i;

	for (i = 0; i < dir->set_count; i++) {
		struct rbn *rbn = rbt_find(&prdcr->set_tree, dir->set_data[i].inst_name);
		if (!rbn)
			continue;
		set = container_of(rbn, struct ldmsd_prdcr_set, rbn);
		assert(set->ref_count);
		prdcr_reset_set(prdcr, set);
	}
}

static void prdcr_dir_cb_upd(ldms_t xprt, ldms_dir_t dir, ldmsd_prdcr_t prdcr)
{
	/* TODO: event-driven */
	ldmsd_prdcr_set_t set;
	int i;
	struct ldmsd_updtr_schedule prev_hint;

	for (i = 0; i < dir->set_count; i++) {
		set = ldmsd_prdcr_set_find(prdcr, dir->set_data[i].inst_name);
		if (!set) {
			/* Received an update, but the set is gone. */
			ldmsd_log(LDMSD_LERROR,
				  "Ignoring 'dir update' for the set, '%s', which "
				  "is not present in the prdcr_set tree.\n",
				  dir->set_data[i].inst_name);
			continue;
		}
		pthread_mutex_lock(&set->lock);
		prdcr_hint_tree_update(prdcr, set, &set->updt_hint, UPDT_HINT_TREE_REMOVE);
		prev_hint = set->updt_hint;
		__update_set_info(set, &dir->set_data[i]);
		prdcr_hint_tree_update(prdcr, set, &set->updt_hint, UPDT_HINT_TREE_ADD);
		pthread_mutex_unlock(&set->lock);
		if (0 != ldmsd_updtr_schedule_cmp(&prev_hint, &set->updt_hint)) {
			/*
			 * Update Updater tasks only when
			 * there are any changes to
			 * avoid unnecessary iterations.
			 */
			ldmsd_prdcr_unlock(prdcr);
			ldmsd_prd_set_updtr_task_update(set);
			ldmsd_prdcr_lock(prdcr);
		}
	}
}

int
prdcr_dir_complete_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e)
{
	ldmsd_prdcr_t prdcr = EV_DATA(e, struct dir_data)->prdcr;
	ldms_dir_t dir = EV_DATA(e, struct dir_data)->dir;

	switch (dir->type) {
	case LDMS_DIR_LIST:
		prdcr_dir_cb_list(prdcr->xprt, dir, prdcr);
		break;
	case LDMS_DIR_ADD:
		prdcr_dir_cb_add(prdcr->xprt, dir, prdcr);
		break;
	case LDMS_DIR_DEL:
		prdcr_dir_cb_del(prdcr->xprt, dir, prdcr);
		break;
	case LDMS_DIR_UPD:
		prdcr_dir_cb_upd(prdcr->xprt, dir, prdcr);
		break;
	}
	ev_put(e);
	ldmsd_prdcr_put(prdcr); /* Put back the ref taken when the ev's posted */
	return 0;
}

/*
 * The ldms_dir has completed. Decode the directory type and call the
 * appropriate handler function.
 */
static void prdcr_dir_cb(ldms_t xprt, int status, ldms_dir_t dir, void *arg)
{
	ldmsd_prdcr_t prdcr = arg;
	if (status) {
		ldmsd_log(LDMSD_LINFO, "Error %d in dir on producer %s host %s.\n",
			 status, prdcr->obj.name, prdcr->host_name);
		return;
	}
	assert(xprt == prdcr->xprt);
	ev_t dir_ev = ev_new(dir_complete_type);
	if (!dir_ev)
		goto enomem;
	EV_DATA(dir_ev, struct dir_data)->dir = dir;
	EV_DATA(dir_ev, struct dir_data)->prdcr = ldmsd_prdcr_get(prdcr);
	ev_post(NULL, prdcr->worker, dir_ev, 0);
	return ;

enomem:
	ldmsd_log(LDMSD_LCRITICAL, "Out of memory");
	ldms_xprt_dir_free(xprt, dir);
	return;
}

static int __on_subs_resp(ldmsd_req_cmd_t rcmd)
{
	ldmsd_req_cmd_free(rcmd);
	return 0;
}

/* Send subscribe request to peer */
static int __prdcr_subscribe(ldmsd_prdcr_t prdcr)
{
	ldmsd_req_cmd_t rcmd;
	int rc;
	ldmsd_prdcr_stream_t s;
	LIST_FOREACH(s, &prdcr->stream_list, entry) {
		rcmd = ldmsd_req_cmd_new(prdcr->xprt, LDMSD_STREAM_SUBSCRIBE_REQ,
					 NULL, __on_subs_resp, prdcr);
		rc = errno;
		if (!rcmd)
			goto err_0;
		rc = ldmsd_req_cmd_attr_append_str(rcmd, LDMSD_ATTR_NAME, s->name);
		if (rc)
			goto err_1;
		rc = ldmsd_req_cmd_attr_term(rcmd);
		if (rc)
			goto err_1;
	}
	return 0;
 err_1:
	ldmsd_req_cmd_free(rcmd);
 err_0:
	return rc;
}

static void __prdcr_remote_set_delete(ldmsd_prdcr_t prdcr, ldms_set_t set)
{
	const char *state_str = "bad_state";
	ldmsd_prdcr_set_t prdcr_set;
	if (!set)
		return;
	prdcr_set = ldmsd_prdcr_set_find(prdcr, ldms_set_instance_name_get(set));
	pthread_mutex_lock(&prdcr_set->lock);
	assert(prdcr_set->ref_count);
	switch (prdcr_set->state) {
	case LDMSD_PRDCR_SET_STATE_START:
		state_str = "START";
		break;
	case LDMSD_PRDCR_SET_STATE_LOOKUP:
		state_str = "LOOKUP";
		break;
	case LDMSD_PRDCR_SET_STATE_READY:
		state_str = "READY";
		break;
	case LDMSD_PRDCR_SET_STATE_UPDATING:
		state_str = "UPDATING";
		break;
	case LDMSD_PRDCR_SET_STATE_DELETED:
		state_str = "DELETING";
		break;
	}
	ldmsd_log(LDMSD_LINFO,
			"Deleting %s in the %s state\n",
			prdcr_set->inst_name, state_str);
	pthread_mutex_unlock(&prdcr_set->lock);
	prdcr_reset_set(prdcr, prdcr_set);
}

const char *_conn_state_str[] = {
	[LDMSD_PRDCR_STATE_STOPPED]       =  "LDMSD_PRDCR_STATE_STOPPED",
	[LDMSD_PRDCR_STATE_DISCONNECTED]  =  "LDMSD_PRDCR_STATE_DISCONNECTED",
	[LDMSD_PRDCR_STATE_CONNECTING]    =  "LDMSD_PRDCR_STATE_CONNECTING",
	[LDMSD_PRDCR_STATE_CONNECTED]     =  "LDMSD_PRDCR_STATE_CONNECTED",
	[LDMSD_PRDCR_STATE_STOPPING]      =  "LDMSD_PRDCR_STATE_STOPPING",
};

static const char *conn_state_str(int state)
{
	if (LDMSD_PRDCR_STATE_STOPPED<=state && state<=LDMSD_PRDCR_STATE_STOPPING)
		return _conn_state_str[state];
	return "UNKNOWN_STATE";
}


static int prdcr_connect_ev(ldmsd_prdcr_t prdcr, unsigned long interval_us)
{
	struct timespec to;
	ev_t conn_ev = ev_new(prdcr_connect_type);
	if (!conn_ev)
		return ENOMEM;

	EV_DATA(conn_ev, struct cfgobj_data)->obj = &ldmsd_prdcr_get(prdcr)->obj;
	EV_DATA(conn_ev, struct cfgobj_data)->ctxt = NULL;

	clock_gettime(CLOCK_REALTIME, &to);
	to.tv_sec += interval_us / 1000000;
	to.tv_nsec += (interval_us % 1000000) * 1000;

	ev_post(prdcr->worker, prdcr->worker, conn_ev, &to);
	return 0;
}

int
prdcr_xprt_event_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev)
{
	ldmsd_prdcr_t prdcr = (ldmsd_prdcr_t)EV_DATA(ev, struct cfgobj_data)->obj;
	ldms_xprt_event_t xprt_ev = (ldms_xprt_event_t)EV_DATA(ev, struct cfgobj_data)->ctxt;

	ldmsd_log(LDMSD_LINFO, "%s:%d Producer %s (%s %s:%d) conn_state: %d %s\n",
				__func__, __LINE__,
				prdcr->obj.name, prdcr->xprt_name,
				prdcr->host_name, (int)prdcr->port_no,
				prdcr->conn_state,
				conn_state_str(prdcr->conn_state));
	switch(xprt_ev->type) {
	case LDMS_XPRT_EVENT_DISCONNECTED:
		prdcr->xprt->disconnected = 1;
		break;
	default:
		assert(prdcr->xprt->disconnected == 0);
		break;
	}
	switch (xprt_ev->type) {
	case LDMS_XPRT_EVENT_CONNECTED:
		ldmsd_log(LDMSD_LINFO, "Producer %s is connected (%s %s:%d)\n",
				prdcr->obj.name, prdcr->xprt_name,
				prdcr->host_name, (int)prdcr->port_no);
		prdcr->conn_state = LDMSD_PRDCR_STATE_CONNECTED;
		if (__prdcr_subscribe(prdcr)) { /* TODO: look into this */
			ldmsd_log(LDMSD_LERROR,
				  "Could not subscribe to stream data on producer %s\n",
				  prdcr->obj.name);
		}
		if (ldms_xprt_dir(prdcr->xprt, prdcr_dir_cb, prdcr,
				  LDMS_DIR_F_NOTIFY))
			ldms_xprt_close(prdcr->xprt);
		break;
	case LDMS_XPRT_EVENT_RECV:
		ldmsd_recv_msg(prdcr->xprt, xprt_ev->data, xprt_ev->data_len);
		break;
	case LDMS_XPRT_EVENT_SET_DELETE:
		__prdcr_remote_set_delete(prdcr, xprt_ev->set_delete.set);
		break;
	case LDMS_XPRT_EVENT_REJECTED:
		ldmsd_log(LDMSD_LERROR, "Producer %s rejected the "
				"connection (%s %s:%d)\n", prdcr->obj.name,
				prdcr->xprt_name, prdcr->host_name,
				(int)prdcr->port_no);
		goto reset_prdcr;
	case LDMS_XPRT_EVENT_DISCONNECTED:
		ldmsd_log(LDMSD_LINFO, "Producer %s is disconnected (%s %s:%d)\n",
				prdcr->obj.name, prdcr->xprt_name,
				prdcr->host_name, (int)prdcr->port_no);
		goto reset_prdcr;
	case LDMS_XPRT_EVENT_ERROR:
		ldmsd_log(LDMSD_LINFO, "Producer %s: connection error to %s %s:%d\n",
				prdcr->obj.name, prdcr->xprt_name,
				prdcr->host_name, (int)prdcr->port_no);
		goto reset_prdcr;
	case LDMS_XPRT_EVENT_SEND_COMPLETE:
		/*
		 * TODO: see what LDMSD should do with this.
		 * Can it just ignore the event for now?
		 */
		break;
	default:
		assert(0);
	}
	ev_put(ev);
	ldmsd_xprt_event_free(xprt_ev);
	ldmsd_prdcr_put(prdcr);
	return 0;

reset_prdcr:
	/* TODO: event-driven */
	prdcr_reset_sets(prdcr);
	switch (prdcr->conn_state) {
	case LDMSD_PRDCR_STATE_STOPPING:
		prdcr->conn_state = LDMSD_PRDCR_STATE_STOPPED;
		break;
	case LDMSD_PRDCR_STATE_DISCONNECTED:
	case LDMSD_PRDCR_STATE_CONNECTING:
	case LDMSD_PRDCR_STATE_CONNECTED:
		prdcr->conn_state = LDMSD_PRDCR_STATE_DISCONNECTED;
		if (prdcr_connect_ev(prdcr, prdcr->conn_intrvl_us))
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory.\n");
		break;
	case LDMSD_PRDCR_STATE_STOPPED:
		assert(0 == "STOPPED shouldn't have xprt event");
		break;
	default:
		assert(0 == "BAD STATE");
	}
	if (prdcr->xprt) {
		ldmsd_xprt_term(prdcr->xprt);
		ldms_xprt_put(prdcr->xprt);
		prdcr->xprt = NULL;
	}
	ldmsd_log(LDMSD_LINFO, "%s:%d Producer (after reset) %s (%s %s:%d)"
				" conn_state: %d %s\n",
				__func__, __LINE__,
				prdcr->obj.name, prdcr->xprt_name,
				prdcr->host_name, (int)prdcr->port_no,
				prdcr->conn_state,
				conn_state_str(prdcr->conn_state));
	ev_put(ev);
	free(xprt_ev);
	ldmsd_prdcr_put(prdcr);
	return 0;
}

static void prdcr_connect_cb(ldms_t x, ldms_xprt_event_t e, void *cb_arg)
{
	ldms_xprt_event_t xprt_ev;
	ldmsd_prdcr_t prdcr = (ldmsd_prdcr_t)cb_arg;
	assert(x == prdcr->xprt);

	xprt_ev = ldmsd_xprt_event_get(e);
	if (!xprt_ev)
		goto enomem;

	ev_t ev = ev_new(prdcr_xprt_type);
	if (!ev)
		goto enomem;
	EV_DATA(ev, struct cfgobj_data)->obj = &ldmsd_prdcr_get(prdcr)->obj;
	EV_DATA(ev, struct cfgobj_data)->ctxt = xprt_ev;

	ev_post(NULL, prdcr->worker, ev, 0);
	return;

enomem:
	LDMSD_LOG_ENOMEM();
	return;
}

extern const char *auth_name;
extern struct attr_value_list *auth_opt;

static void prdcr_connect(ldmsd_prdcr_t prdcr)
{
	int ret;

	assert(prdcr->xprt == NULL);
	switch (prdcr->type) {
	case LDMSD_PRDCR_TYPE_ACTIVE:
		prdcr->conn_state = LDMSD_PRDCR_STATE_CONNECTING;
		prdcr->xprt = ldms_xprt_new_with_auth(prdcr->xprt_name,
					ldmsd_linfo, prdcr->conn_auth,
					prdcr->conn_auth_args);
		if (prdcr->xprt) {
			ret  = ldms_xprt_connect(prdcr->xprt,
						 (struct sockaddr *)&prdcr->ss,
						 prdcr->ss_len,
						 prdcr_connect_cb, prdcr);
			if (ret) {
				ldms_xprt_put(prdcr->xprt);
				prdcr->xprt = NULL;
				prdcr->conn_state = LDMSD_PRDCR_STATE_DISCONNECTED;
			}
		} else {
			ldmsd_log(LDMSD_LERROR, "%s Error %d: creating endpoint on transport '%s'.\n",
				 __func__, errno, prdcr->xprt_name);
			prdcr->conn_state = LDMSD_PRDCR_STATE_DISCONNECTED;
		}
		break;
	case LDMSD_PRDCR_TYPE_PASSIVE:
		prdcr->xprt = ldms_xprt_by_remote_sin((struct sockaddr_in *)&prdcr->ss);
		/* Call connect callback to advance state and update timers*/
		if (prdcr->xprt) {
			struct ldms_xprt_event conn_ev = {.type = LDMS_XPRT_EVENT_CONNECTED};
			prdcr_connect_cb(prdcr->xprt, &conn_ev, prdcr);
		}
		break;
	case LDMSD_PRDCR_TYPE_LOCAL:
		assert(0);
	}
}

int
prdcr_connect_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e)
{
	if (EV_OK != status)
		return 0;

	ldmsd_prdcr_t prdcr = (ldmsd_prdcr_t)EV_DATA(e, struct cfgobj_data)->obj;

	switch (prdcr->conn_state) {
	case LDMSD_PRDCR_STATE_DISCONNECTED:
		prdcr_connect(prdcr);
		break;
	case LDMSD_PRDCR_STATE_STOPPED:
	case LDMSD_PRDCR_STATE_STOPPING:
	case LDMSD_PRDCR_STATE_CONNECTING:
	case LDMSD_PRDCR_STATE_CONNECTED:
		/* do nothing */
		break;
	}
	ev_put(e);
	ldmsd_prdcr_put(prdcr); /* put back the reference taken when post \c e */
	return 0;
}

static int set_cmp(void *a, const void *b)
{
	return strcmp(a, b);
}

int ldmsd_prdcr_str2type(const char *type)
{
	enum ldmsd_prdcr_type prdcr_type;
	if (0 == strcasecmp(type, "active"))
		prdcr_type = LDMSD_PRDCR_TYPE_ACTIVE;
	else if (0 == strcasecmp(type, "passive"))
		prdcr_type = LDMSD_PRDCR_TYPE_PASSIVE;
	else if (0 == strcasecmp(type, "local"))
		prdcr_type = LDMSD_PRDCR_TYPE_LOCAL;
	else
		return -EINVAL;
	return prdcr_type;
}

const char *ldmsd_prdcr_type2str(enum ldmsd_prdcr_type type)
{
	if (LDMSD_PRDCR_TYPE_ACTIVE == type)
		return "active";
	else if (LDMSD_PRDCR_TYPE_PASSIVE == type)
		return "passive";
	else if (LDMSD_PRDCR_TYPE_LOCAL == type)
		return "local";
	else
		return NULL;
}

const char *ldmsd_prdcr_state_str(enum ldmsd_prdcr_state state)
{
	switch (state) {
	case LDMSD_PRDCR_STATE_STOPPED:
		return "STOPPED";
	case LDMSD_PRDCR_STATE_DISCONNECTED:
		return "DISCONNECTED";
	case LDMSD_PRDCR_STATE_CONNECTING:
		return "CONNECTING";
	case LDMSD_PRDCR_STATE_CONNECTED:
		return "CONNECTED";
	case LDMSD_PRDCR_STATE_STOPPING:
		return "STOPPING";
	}
	return "BAD STATE";
}

ldmsd_prdcr_t
ldmsd_prdcr_new_with_auth(const char *name, const char *xprt_name,
		const char *host_name, const unsigned short port_no,
		enum ldmsd_prdcr_type type, int conn_intrvl_us,
		const char *auth, uid_t uid, gid_t gid, int perm)
{
	extern struct rbt *cfgobj_trees[];
	struct ldmsd_prdcr *prdcr;
	ldmsd_auth_t auth_dom = NULL;

	ldmsd_log(LDMSD_LDEBUG, "ldmsd_prdcr_new(name %s, xprt %s, host %s, port %u, type %u, intv %d\n",
		name, xprt_name, host_name,(unsigned) port_no, (unsigned)type, conn_intrvl_us);
	prdcr = (struct ldmsd_prdcr *)
		ldmsd_cfgobj_new_with_auth(name, LDMSD_CFGOBJ_PRDCR,
				sizeof *prdcr, ldmsd_prdcr___del,
				uid, gid, perm);
	if (!prdcr)
		return NULL;

	prdcr->type = type;
	prdcr->conn_intrvl_us = conn_intrvl_us;
	prdcr->port_no = port_no;
	prdcr->conn_state = LDMSD_PRDCR_STATE_STOPPED;
	rbt_init(&prdcr->set_tree, set_cmp);
	rbt_init(&prdcr->hint_set_tree, ldmsd_updtr_schedule_cmp);
	prdcr->host_name = strdup(host_name);
	if (!prdcr->host_name)
		goto out;
	prdcr->xprt_name = strdup(xprt_name);
	if (!prdcr->port_no)
		goto out;

	if (prdcr_resolve(host_name, port_no, &prdcr->ss, &prdcr->ss_len)) {
		errno = EAFNOSUPPORT;
		ldmsd_log(LDMSD_LERROR, "ldmsd_prdcr_new: %s:%u not resolved.\n",
			host_name,(unsigned) port_no);
		goto out;
	}

	if (!auth)
		auth = DEFAULT_AUTH;
	auth_dom = ldmsd_auth_find(auth);
	if (!auth_dom) {
		errno = ENOENT;
		goto out;
	}
	prdcr->conn_auth = strdup(auth_dom->plugin);
	if (!prdcr->conn_auth)
		goto out;
	if (auth_dom->attrs) {
		prdcr->conn_auth_args = av_copy(auth_dom->attrs);
		if (!prdcr->conn_auth_args)
			goto out;
	}

	ldmsd_task_init(&prdcr->task);
	prdcr->worker = assign_prdcr_worker();

	ldmsd_cfgobj_unlock(&prdcr->obj); /* TODO: remove this when ready */
	return prdcr;
out:
	rbt_del(cfgobj_trees[LDMSD_CFGOBJ_PRDCR], &prdcr->obj.rbn);
	ldmsd_cfgobj_unlock(&prdcr->obj);
	ldmsd_cfgobj_put(&prdcr->obj);
	return NULL;
}

ldmsd_prdcr_t
ldmsd_prdcr_new(const char *name, const char *xprt_name,
		const char *host_name, const unsigned short port_no,
		enum ldmsd_prdcr_type type,
		int conn_intrvl_us)
{
	return ldmsd_prdcr_new_with_auth(name, xprt_name, host_name,
			port_no, type, conn_intrvl_us,
			DEFAULT_AUTH, getuid(), getgid(), 0777);
}

extern struct rbt *cfgobj_trees[];
extern pthread_mutex_t *cfgobj_locks[];
ldmsd_cfgobj_t __cfgobj_find(const char *name, ldmsd_cfgobj_type_t type);

int ldmsd_prdcr_del(const char *prdcr_name, ldmsd_sec_ctxt_t ctxt)
{
	int rc = 0;
	ldmsd_prdcr_t prdcr;
	pthread_mutex_lock(cfgobj_locks[LDMSD_CFGOBJ_PRDCR]);
	prdcr = (ldmsd_prdcr_t) __cfgobj_find(prdcr_name, LDMSD_CFGOBJ_PRDCR);
	if (!prdcr) {
		rc = ENOENT;
		goto out_0;
	}

	ldmsd_prdcr_lock(prdcr);
	rc = ldmsd_cfgobj_access_check(&prdcr->obj, 0222, ctxt);
	if (rc)
		goto out_1;
	if (prdcr->conn_state != LDMSD_PRDCR_STATE_STOPPED) {
		rc = EBUSY;
		goto out_1;
	}
	if (ldmsd_cfgobj_refcount(&prdcr->obj) > 2) {
		rc = EBUSY;
		goto out_1;
	}

	/* removing from the tree */
	rbt_del(cfgobj_trees[LDMSD_CFGOBJ_PRDCR], &prdcr->obj.rbn);
	ldmsd_prdcr_put(prdcr); /* putting down reference from the tree */

	rc = 0;
	/* let-through */
out_1:
	ldmsd_prdcr_unlock(prdcr);
out_0:
	pthread_mutex_unlock(cfgobj_locks[LDMSD_CFGOBJ_PRDCR]);
	if (prdcr)
		ldmsd_prdcr_put(prdcr); /* `find` reference */
	return rc;
}

ldmsd_prdcr_t ldmsd_prdcr_first()
{
	return (ldmsd_prdcr_t)ldmsd_cfgobj_first(LDMSD_CFGOBJ_PRDCR);
}

ldmsd_prdcr_t ldmsd_prdcr_next(struct ldmsd_prdcr *prdcr)
{
	return (ldmsd_prdcr_t)ldmsd_cfgobj_next(&prdcr->obj);
}

static void prdcr_task_cb(ldmsd_task_t task, void *arg)
{
	ldmsd_prdcr_t prdcr = arg;
	ldmsd_prdcr_lock(prdcr);
	switch (prdcr->conn_state) {
	case LDMSD_PRDCR_STATE_STOPPED:
	case LDMSD_PRDCR_STATE_STOPPING:
		ldmsd_task_stop(&prdcr->task);
		break;
	case LDMSD_PRDCR_STATE_DISCONNECTED:
		prdcr_connect_ev(prdcr, prdcr->conn_intrvl_us);
		break;
	case LDMSD_PRDCR_STATE_CONNECTING:
	case LDMSD_PRDCR_STATE_CONNECTED:
		ldmsd_task_stop(&prdcr->task);
		break;
	}
	ldmsd_prdcr_unlock(prdcr);
}

int __ldmsd_prdcr_start(ldmsd_prdcr_t prdcr, ldmsd_sec_ctxt_t ctxt)
{
	int rc;
	ldmsd_prdcr_lock(prdcr);
	rc = ldmsd_cfgobj_access_check(&prdcr->obj, 0222, ctxt);
	if (rc)
		goto out;
	if (prdcr->conn_state != LDMSD_PRDCR_STATE_STOPPED) {
		rc = EBUSY;
		goto out;
	}

	prdcr->conn_state = LDMSD_PRDCR_STATE_DISCONNECTED;

	prdcr->obj.perm |= LDMSD_PERM_DSTART;

	/*
	 * TODO: fix this
	 */

	ldmsd_task_start(&prdcr->task, prdcr_task_cb, prdcr,
			 LDMSD_TASK_F_IMMEDIATE,
			 prdcr->conn_intrvl_us, 0);
out:
	ldmsd_prdcr_unlock(prdcr);
	return rc;
}

int ldmsd_prdcr_start(const char *name, const char *interval_str,
		      ldmsd_sec_ctxt_t ctxt)
{
	int rc = 0;

	ldmsd_prdcr_t prdcr = ldmsd_prdcr_find(name);
	if (!prdcr)
		return ENOENT;
	if (interval_str)
		prdcr->conn_intrvl_us = strtol(interval_str, NULL, 0);
	rc = __ldmsd_prdcr_start(prdcr, ctxt);
	ldmsd_prdcr_put(prdcr);
	return rc;
}

int __ldmsd_prdcr_stop(ldmsd_prdcr_t prdcr, ldmsd_sec_ctxt_t ctxt)
{
	int rc;
	ldmsd_prdcr_lock(prdcr);
	rc = ldmsd_cfgobj_access_check(&prdcr->obj, 0222, ctxt);
	if (rc)
		goto out;
	if (prdcr->conn_state == LDMSD_PRDCR_STATE_STOPPED) {
		rc = 0; /* already stopped,
			 * return 0 so that caller knows
			 * stop succeeds. */
		goto out;
	}
	if (prdcr->conn_state == LDMSD_PRDCR_STATE_STOPPING) {
		rc = EBUSY;
		goto out;
	}
	if (prdcr->type == LDMSD_PRDCR_TYPE_LOCAL)
		prdcr_reset_sets(prdcr);
	ldmsd_task_stop(&prdcr->task);
	prdcr->obj.perm &= ~LDMSD_PERM_DSTART;
	prdcr->conn_state = LDMSD_PRDCR_STATE_STOPPING;
	if (prdcr->xprt)
		ldms_xprt_close(prdcr->xprt);
	ldmsd_prdcr_unlock(prdcr);
	ldmsd_task_join(&prdcr->task);
	ldmsd_prdcr_lock(prdcr);
	if (!prdcr->xprt)
		prdcr->conn_state = LDMSD_PRDCR_STATE_STOPPED;
out:
	ldmsd_prdcr_unlock(prdcr);
	return rc;
}

int ldmsd_prdcr_stop(const char *name, ldmsd_sec_ctxt_t ctxt)
{
	int rc = 0;
	ldmsd_prdcr_t prdcr = ldmsd_prdcr_find(name);
	if (!prdcr)
		return ENOENT;
	rc = __ldmsd_prdcr_stop(prdcr, ctxt);
	ldmsd_prdcr_put(prdcr);
	return rc;
}

int ldmsd_prdcr_subscribe(ldmsd_prdcr_t prdcr, const char *stream)
{
	int rc;
	ldmsd_req_cmd_t rcmd;
	ldmsd_prdcr_stream_t s = NULL;
	ldmsd_prdcr_lock(prdcr);
	LIST_FOREACH(s, &prdcr->stream_list, entry) {
		if (0 == strcmp(s->name, stream)) {
			rc = EEXIST;
			goto err_0;
		}
	}
	rc = ENOMEM;
	s = calloc(1, sizeof *s);
	if (!s)
		goto err_0;
	s->name = strdup(stream);
	if (!s->name)
		goto err_1;
	LIST_INSERT_HEAD(&prdcr->stream_list, s, entry);
	if (prdcr->conn_state == LDMSD_PRDCR_STATE_CONNECTED) {
		/* issue stream subscribe request right away if connected */
		rcmd = ldmsd_req_cmd_new(prdcr->xprt, LDMSD_STREAM_SUBSCRIBE_REQ,
					 NULL, __on_subs_resp, prdcr);
		rc = errno;
		if (!rcmd)
			goto rcmd_err;
		rc = ldmsd_req_cmd_attr_append_str(rcmd, LDMSD_ATTR_NAME, s->name);
		if (rc)
			goto rcmd_err;
		rc = ldmsd_req_cmd_attr_term(rcmd);
		if (rc)
			goto rcmd_err;
	}
	ldmsd_prdcr_unlock(prdcr);
	return 0;
 err_1:
	if (s)
		free(s);
 err_0:
	ldmsd_prdcr_unlock(prdcr);
	return rc;

 rcmd_err:
	ldmsd_prdcr_unlock(prdcr);
	if (rcmd)
		ldmsd_req_cmd_free(rcmd);
	/* intentionally leave `s` in the list */
	return rc;
}

int ldmsd_prdcr_unsubscribe(ldmsd_prdcr_t prdcr, const char *stream)
{
	int rc;
	ldmsd_req_cmd_t rcmd;
	ldmsd_prdcr_stream_t s = NULL;
	ldmsd_prdcr_lock(prdcr);
	LIST_FOREACH(s, &prdcr->stream_list, entry) {
		if (0 == strcmp(s->name, stream))
			break; /* found */
	}
	if (!s) {
		rc = ENOENT;
		goto out;
	}
	LIST_REMOVE(s, entry);
	free((void*)s->name);
	free(s);
	if (prdcr->conn_state == LDMSD_PRDCR_STATE_CONNECTED) {
		/* issue stream unsubscribe request right away if connected */
		rcmd = ldmsd_req_cmd_new(prdcr->xprt, LDMSD_STREAM_UNSUBSCRIBE_REQ,
					 NULL, __on_subs_resp, prdcr);
		rc = errno;
		if (!rcmd)
			goto rcmd_err;
		rc = ldmsd_req_cmd_attr_append_str(rcmd, LDMSD_ATTR_NAME, stream);
		if (rc)
			goto rcmd_err;
		rc = ldmsd_req_cmd_attr_term(rcmd);
		if (rc)
			goto rcmd_err;
	}
	rc = 0;
	/* let-through */
 out:
	ldmsd_prdcr_unlock(prdcr);
	return rc;

 rcmd_err:
	ldmsd_prdcr_unlock(prdcr);
	if (rcmd)
		ldmsd_req_cmd_free(rcmd);
	return rc;
}

int ldmsd_prdcr_start_regex(const char *prdcr_regex, const char *interval_str,
			    char *rep_buf, size_t rep_len,
			    ldmsd_sec_ctxt_t ctxt)
{
	regex_t regex;
	ldmsd_prdcr_t prdcr;
	int rc;

	rc = ldmsd_compile_regex(&regex, prdcr_regex, rep_buf, rep_len);
	if (rc)
		return rc;

	ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
	for (prdcr = ldmsd_prdcr_first(); prdcr; prdcr = ldmsd_prdcr_next(prdcr)) {
		rc = regexec(&regex, prdcr->obj.name, 0, NULL, 0);
		if (rc)
			continue;
		if (interval_str)
			prdcr->conn_intrvl_us = strtol(interval_str, NULL, 0);
		__ldmsd_prdcr_start(prdcr, ctxt);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
	regfree(&regex);
	return 0;
}

int ldmsd_prdcr_stop_regex(const char *prdcr_regex, char *rep_buf,
			   size_t rep_len, ldmsd_sec_ctxt_t ctxt)
{
	regex_t regex;
	ldmsd_prdcr_t prdcr;
	int rc;

	rc = ldmsd_compile_regex(&regex, prdcr_regex, rep_buf, rep_len);
	if (rc)
		return rc;
	ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
	for (prdcr = ldmsd_prdcr_first(); prdcr; prdcr = ldmsd_prdcr_next(prdcr)) {
		rc = regexec(&regex, prdcr->obj.name, 0, NULL, 0);
		if (rc)
			continue;
		__ldmsd_prdcr_stop(prdcr, ctxt);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
	regfree(&regex);
	return 0;
}

/**
 * Get the first producer set
 *
 * This function must be called with the ldmsd_cfgobj_lock held.
 */
ldmsd_prdcr_set_t ldmsd_prdcr_set_first(ldmsd_prdcr_t prdcr)
{
	struct rbn *rbn = rbt_min(&prdcr->set_tree);
	if (rbn)
		return container_of(rbn, struct ldmsd_prdcr_set, rbn);
	return NULL;
}

/**
 * Get the next producer set
 *
 * This function must be called with the ldmsd_cfgobj_lock held.
 */
ldmsd_prdcr_set_t ldmsd_prdcr_set_next(ldmsd_prdcr_set_t prd_set)
{
	struct rbn *rbn = rbn_succ(&prd_set->rbn);
	if (rbn)
		return container_of(rbn, struct ldmsd_prdcr_set, rbn);
	return NULL;
}

/**
 * Get the producer set with the given name \c setname
 *
 * This function must be called with the producer lock held.
 */
ldmsd_prdcr_set_t ldmsd_prdcr_set_find(ldmsd_prdcr_t prdcr, const char *setname)
{
	struct rbn *rbn = rbt_find(&prdcr->set_tree, setname);
	if (rbn)
		return container_of(rbn, struct ldmsd_prdcr_set, rbn);
	return NULL;
}

/**
 * Get the first set with the given update hint \c intrvl and \c offset.
 *
 * Caller must hold the producer lock.
 */
ldmsd_prdcr_set_t ldmsd_prdcr_set_first_by_hint(ldmsd_prdcr_t prdcr,
					struct ldmsd_updtr_schedule *hint)
{
	struct rbn *rbn;
	ldmsd_updt_hint_set_list_t list;
	rbn = rbt_find(&prdcr->hint_set_tree, hint);
	if (!rbn)
		return NULL;
	list = container_of(rbn, struct ldmsd_updt_hint_set_list, rbn);
	if (LIST_EMPTY(&list->list))
		assert(0);
	return LIST_FIRST(&list->list);
}

/**
 * Get the next set with the given update hint \c intrvl and \c offset.
 *
 * Caller must hold the producer lock.
 */
ldmsd_prdcr_set_t ldmsd_prdcr_set_next_by_hint(ldmsd_prdcr_set_t prd_set)
{
	return LIST_NEXT(prd_set, updt_hint_entry);
}

/* Must be called with strgp lock held. */
void ldmsd_prdcr_update(ldmsd_strgp_t strgp)
{
	/*
	 * For each producer with a name that matches the storage
	 * policy's regex list, add a reference to this storage policy
	 * to each producer set matching the schema name
	 */
	ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
	ldmsd_prdcr_t prdcr;
	for (prdcr = ldmsd_prdcr_first(); prdcr; prdcr = ldmsd_prdcr_next(prdcr)) {
		int rc;
		ldmsd_name_match_t match = ldmsd_strgp_prdcr_first(strgp);
		for (rc = 0; match; match = ldmsd_strgp_prdcr_next(match)) {
			rc = regexec(&match->regex, prdcr->obj.name, 0, NULL, 0);
			if (!rc)
				break;
		}
		if (rc)
			continue;

		ldmsd_prdcr_lock(prdcr);
		/*
		 * For each producer set matching our schema, add our policy
		 */
		ldmsd_prdcr_set_t prd_set;
		for (prd_set = ldmsd_prdcr_set_first(prdcr);
		     prd_set; prd_set = ldmsd_prdcr_set_next(prd_set)) {
			ldmsd_strgp_unlock(strgp);
			pthread_mutex_lock(&prd_set->lock);
			if (prd_set->state < LDMSD_PRDCR_SET_STATE_READY) {
				ldmsd_strgp_lock(strgp);
				pthread_mutex_unlock(&prd_set->lock);
				continue;
			}

			ldmsd_strgp_lock(strgp);
			ldmsd_strgp_update_prdcr_set(strgp, prd_set);
			pthread_mutex_unlock(&prd_set->lock);
		}
		ldmsd_prdcr_unlock(prdcr);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
}

int ldmsd_prdcr_subscribe_regex(const char *prdcr_regex, char *stream_name,
				char *rep_buf, size_t rep_len,
				ldmsd_sec_ctxt_t ctxt)
{
	regex_t regex;
	ldmsd_prdcr_t prdcr;
	int rc;

	rc = ldmsd_compile_regex(&regex, prdcr_regex, rep_buf, rep_len);
	if (rc)
		return rc;
	ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
	for (prdcr = ldmsd_prdcr_first(); prdcr; prdcr = ldmsd_prdcr_next(prdcr)) {
		rc = regexec(&regex, prdcr->obj.name, 0, NULL, 0);
		if (rc)
			continue;
		ldmsd_prdcr_subscribe(prdcr, stream_name);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
	regfree(&regex);
	return 0;
}

int ldmsd_prdcr_unsubscribe_regex(const char *prdcr_regex, char *stream_name,
				char *rep_buf, size_t rep_len,
				ldmsd_sec_ctxt_t ctxt)
{
	regex_t regex;
	ldmsd_prdcr_t prdcr;
	int rc;

	rc = ldmsd_compile_regex(&regex, prdcr_regex, rep_buf, rep_len);
	if (rc)
		return rc;
	ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
	for (prdcr = ldmsd_prdcr_first(); prdcr; prdcr = ldmsd_prdcr_next(prdcr)) {
		rc = regexec(&regex, prdcr->obj.name, 0, NULL, 0);
		if (rc)
			continue;
		ldmsd_prdcr_unsubscribe(prdcr, stream_name);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
	regfree(&regex);
	return 0;
}















extern void ldmsd_req_ctxt_sec_get(ldmsd_req_ctxt_t rctxt, ldmsd_sec_ctxt_t sctxt);

struct prdcr_start_ctxt {
	struct ldmsd_cfgobj_cfg_ctxt base;
	ldmsd_interval interval;
	uint8_t is_deferred;
};

struct prdcr_peercfg_ctxt {
	struct ldmsd_cfgobj_cfg_ctxt base;
	void *ctxt;
};

static struct ldmsd_cfgobj_cfg_rsp *
prdcr_stop_handler(ldmsd_prdcr_t prdcr, void *cfg_ctxt)
{
	struct ldmsd_cfgobj_cfg_ctxt *ctxt = (struct ldmsd_cfgobj_cfg_ctxt *)cfg_ctxt;
	struct ldmsd_cfgobj_cfg_rsp *rsp;
	int rc;

	rsp = calloc(1, sizeof(*rsp));
	if (!rsp)
		goto enomem;

	rc = ldmsd_cfgobj_access_check(&prdcr->obj, 0222, &ctxt->sctxt);
	if (rc) {
		rsp->errcode = EPERM;
		rc = asprintf(&rsp->errmsg, "prdcr '%s' permission denied.",
							prdcr->obj.name);
		if (rc < 0)
			goto enomem;
		else
			goto out;
	}
	if (prdcr->conn_state == LDMSD_PRDCR_STATE_STOPPED)
		goto out;

	if (prdcr->conn_state == LDMSD_PRDCR_STATE_STOPPING) {
		rsp->errcode = EBUSY;
		rc = asprintf(&rsp->errmsg, "prdcr '%s' already stopped.",
							prdcr->obj.name);
		if (rc < 0)
			goto enomem;
		else
			goto out;
	}

	if (prdcr->type == LDMSD_PRDCR_TYPE_LOCAL)
		prdcr_reset_sets(prdcr);

	prdcr->obj.perm &= ~LDMSD_PERM_DSTART;
	prdcr->conn_state = LDMSD_PRDCR_STATE_STOPPING;
	if (prdcr->xprt)
		ldms_xprt_close(prdcr->xprt);
	if (!prdcr->xprt)
		prdcr->conn_state = LDMSD_PRDCR_STATE_STOPPED;
out:
	return rsp;
enomem:
	LDMSD_LOG_ENOMEM();
	free(rsp);
	return NULL;
}

static struct ldmsd_cfgobj_cfg_rsp *
prdcr_start_handler(ldmsd_prdcr_t prdcr, void *cfg_ctxt)
{
	struct prdcr_start_ctxt *ctxt = (struct prdcr_start_ctxt *)cfg_ctxt;
	struct ldmsd_cfgobj_cfg_rsp *rsp;
	int rc;

	rsp = calloc(1, sizeof(*rsp));
	if (!rsp)
		goto enomem;

	rc = ldmsd_cfgobj_access_check(&prdcr->obj, 0222, &ctxt->base.sctxt);
	if (rc) {
		rsp->errcode = EPERM;
		rc = asprintf(&rsp->errmsg, "prdcr '%s' permission denied.",
							prdcr->obj.name);
		if (rc < 0)
			goto enomem;
		else
			goto out;
	}

	if (prdcr->conn_state != LDMSD_PRDCR_STATE_STOPPED) {
		rsp->errcode = EBUSY;
		rc = asprintf(&rsp->errmsg, "prdcr '%s' is already running.",
							prdcr->obj.name);
		if (rc < 0)
			goto enomem;
		else
			goto out;
	}

	if (ctxt->interval)
		prdcr->conn_intrvl_us = ctxt->interval;
	prdcr->obj.perm |= LDMSD_PERM_DSTART;

	if (!ctxt->is_deferred) {
		prdcr->conn_state = LDMSD_PRDCR_STATE_DISCONNECTED;
		if (prdcr_connect_ev(prdcr, 0))
			goto enomem;
	}

out:
	return rsp;
enomem:
	LDMSD_LOG_ENOMEM();
	free(rsp);
	return NULL;
}

static struct ldmsd_msg_buf *
__prdcr_status_json_obj(ldmsd_prdcr_t prdcr)
{
	ldmsd_prdcr_set_t prv_set;
	int set_count = 0;
	int cnt;
	struct ldmsd_msg_buf *buf = ldmsd_msg_buf_new(1024);
	if (!buf)
		goto enomem;

	cnt = ldmsd_msg_buf_append(buf,
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
			ldmsd_prdcr_state_str(prdcr->conn_state));
	if (cnt < 0)
		goto enomem;

	set_count = 0;
	for (prv_set = ldmsd_prdcr_set_first(prdcr); prv_set;
	     prv_set = ldmsd_prdcr_set_next(prv_set)) {
		if (set_count) {
			cnt = ldmsd_msg_buf_append(buf, ",\n");
			if (cnt < 0)
				goto enomem;
		}

		cnt = ldmsd_msg_buf_append(buf,
			"{ \"inst_name\":\"%s\","
			"\"schema_name\":\"%s\","
			"\"state\":\"%s\"}",
			prv_set->inst_name,
			(prv_set->schema_name ? prv_set->schema_name : ""),
			ldmsd_prdcr_set_state_str(prv_set->state));
		if (cnt < 0)
			goto enomem;

		set_count++;
	}
	cnt = ldmsd_msg_buf_append(buf, "]}");
	if (cnt < 0)
		goto enomem;

	return buf;

enomem:
	LDMSD_LOG_ENOMEM();
	ldmsd_msg_buf_free(buf);
	return NULL;
}

static struct ldmsd_cfgobj_cfg_rsp *
prdcr_status_handler(ldmsd_prdcr_t prdcr, void *cfg_ctxt)
{
	struct ldmsd_cfgobj_cfg_rsp *rsp;

	rsp = calloc(1, sizeof(*rsp));
	if (!rsp) {
		LDMSD_LOG_ENOMEM();
		return NULL;
	}
	rsp->ctxt = __prdcr_status_json_obj(prdcr);
	if (!rsp->ctxt) {
		rsp->errcode = ENOMEM;
		free(rsp);
		return NULL;
	}
	return rsp;
}

static struct ldmsd_cfgobj_cfg_rsp *
prdcr_del_handler(ldmsd_prdcr_t prdcr, void *cfg_ctxt)
{
	int rc;
	struct ldmsd_cfgobj_cfg_ctxt *ctxt = (struct ldmsd_cfgobj_cfg_ctxt *)cfg_ctxt;
	struct ldmsd_cfgobj_cfg_rsp *rsp = calloc(1, sizeof(*rsp));
	if (!rsp)
		goto enomem;
	rsp->errcode = ldmsd_cfgobj_access_check(&prdcr->obj, 0222, &ctxt->sctxt);
	if (rsp->errcode) {
		rc = asprintf(&rsp->errmsg, "prdcr '%s' permission denied.",
							  prdcr->obj.name);
		if (rc < 0)
			goto enomem;
		else
			goto out;
	}

	if (prdcr->conn_state != LDMSD_PRDCR_STATE_STOPPED) {
		rsp->errcode = EBUSY;
		rc = asprintf(&rsp->errmsg, "prdcr '%s' is in use.", prdcr->obj.name);
		if (rc < 0)
			goto enomem;
		else
			goto out;
	}

	if (ldmsd_cfgobj_refcount(&prdcr->obj) > 2) {
		rsp->errcode = EBUSY;
		rc = asprintf(&rsp->errmsg, "prdcr '%s' is in use.", prdcr->obj.name);
		if (rc < 0)
			goto enomem;
		else
			goto out;
	}
	rsp->ctxt = ldmsd_prdcr_get(prdcr);

out:
	return rsp;

enomem:
	LDMSD_LOG_ENOMEM();
	return NULL;
}

#ifdef LDMSD_FAILOVER
extern int __failover_send_prdcr(ldmsd_failover_t f, ldms_t x, ldmsd_prdcr_t p);
static struct ldmsd_cfgobj_cfg_rsp *
prdcr_failover_peercfg_handler(ldmsd_prdcr_t prdcr, void *cfg_ctxt)
{
	int rc;
	struct ldmsd_cfgobj_cfg_rsp *rsp;
	struct prdcr_cfg_ctxt *ctxt = (struct prdcr_cfg_ctxt *)cfg_ctxt;
	ldmsd_req_ctxt_t reqc = ctxt->reqc;
	struct fo_peercfg_ctxt *fo_ctxt = (struct fo_peercfg_ctxt *)reqc->ctxt;

	rsp = malloc(sizeof(*rsp));
	if (!rsp)
		return NULL;
	rc = __failover_send_prdcr(NULL, fo_ctxt->x, prdcr);
	rsp->errcode = rc;
	return rsp;
}
#endif /* LDMSD_FAILOVER */

int prdcr_cfg_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e)
{
	if (EV_OK != status)
		return 0;

	int rc;
	struct ldmsd_cfgobj_cfg_rsp *rsp;
	ev_t rsp_ev;

	ldmsd_prdcr_t prdcr = (ldmsd_prdcr_t)EV_DATA(e, struct cfgobj_data)->obj;
	struct ldmsd_cfgobj_cfg_ctxt *cfg_ctxt = EV_DATA(e, struct cfgobj_data)->ctxt;
	ldmsd_req_ctxt_t reqc = cfg_ctxt->reqc;

	switch (reqc->req_id) {
	case LDMSD_PRDCR_DEL_REQ:
		rsp = prdcr_del_handler(prdcr, cfg_ctxt);
		break;
	case LDMSD_PRDCR_START_REQ:
	case LDMSD_PRDCR_START_REGEX_REQ:
	case LDMSD_PRDCR_DEFER_START_REQ:
	case LDMSD_PRDCR_DEFER_START_REGEX_REQ:
		rsp = prdcr_start_handler(prdcr, cfg_ctxt);
		break;
	case LDMSD_PRDCR_STOP_REQ:
	case LDMSD_PRDCR_STOP_REGEX_REQ:
		rsp = prdcr_stop_handler(prdcr, cfg_ctxt);
		break;
	case LDMSD_PRDCR_STATUS_REQ:
		rsp = prdcr_status_handler(prdcr, cfg_ctxt);
		break;
#ifdef LDMSD_FAILOVER
	case LDMSD_FAILOVER_PEERCFG_REQ:
		rsp = prdcr_failover_peercfg_handler(prdcr, cfg_ctxt);
		break;
#endif /* LDMSD_FAILOVER */
	default:
		ldmsd_log(LDMSD_LERROR, "%s received an unsupported request ID %d.\n",
							__func__, reqc->req_id);
		assert(0);
		rc = EINTR;
		goto err;
	}

	if (!rsp)
		goto enomem;

	rsp_ev = ev_new(cfgobj_rsp_type);
	if (!rsp_ev)
		goto enomem;

	EV_DATA(rsp_ev, struct cfgobj_rsp_data)->ctxt = cfg_ctxt;
	EV_DATA(rsp_ev, struct cfgobj_rsp_data)->rsp = rsp;

	ev_post(prdcr->worker, prdcr_tree_w, rsp_ev, 0);
	ldmsd_prdcr_put(prdcr); /* Take in prdcr_cfg_actor() */
	ev_put(e);
	return 0;
enomem:
	LDMSD_LOG_ENOMEM();
	rc = ENOMEM;
err:
	ev_put(e);
	free(rsp);
	return rc;
}

typedef int (*prdset_actor_fn)(ldmsd_prdcr_t prdcr, ldmsd_prdcr_set_t prdset, void *args);
int __prdset_filter(ldmsd_prdcr_t prdcr, struct ldmsd_match_queue *filter,
					prdset_actor_fn actor, void *args)
{
	int rc;
	ldmsd_prdcr_set_t prdset;
	struct ldmsd_name_match *m;
	char *s;

	if (TAILQ_EMPTY(filter)) {
		for (prdset = ldmsd_prdcr_set_first(prdcr); prdset;
				prdset = ldmsd_prdcr_set_next(prdset)) {
			(void) actor(prdcr, prdset, args);
		}
	} else {
		for (prdset = ldmsd_prdcr_set_first(prdcr); prdset;
				prdset = ldmsd_prdcr_set_next(prdset)) {
			TAILQ_FOREACH(m, filter, entry) {
				if (LDMSD_NAME_MATCH_INST_NAME == m->selector)
					s = prdset->inst_name;
				else
					s = prdset->schema_name;
				rc = regexec(&m->regex, s, 0, NULL, m->regex_flags);
				if (0 == rc) {
					/* matched */
					(void) actor(prdcr, prdset, args);
					break;
				}
			}
		}
	}
	return 0;
}

static int
__prdcr_post_updtr_state(ldmsd_prdcr_t prdcr, ldmsd_prdcr_set_t prdset, void *args)
{
	int rc;
	ev_t e = args;
	struct updtr_info *info = EV_DATA(e, struct updtr_state_data)->updtr_info;

	ev_t ev = ev_new(updtr_state_type);
	if (!ev) {
		LDMSD_LOG_ENOMEM();
		return ENOMEM;
	}
	ref_get(&info->ref, "prdcr2prdset");
	ldmsd_prdcr_set_ref_get(prdset);
	EV_DATA(ev, struct updtr_state_data)->state = EV_DATA(e, struct updtr_state_data)->state;
	EV_DATA(ev, struct updtr_state_data)->updtr_info = info;
	EV_DATA(ev, struct updtr_state_data)->obj = prdset;
	rc = ev_post(prdcr->worker, prdset->worker, ev, 0);
	if (rc) {
		ldmsd_log(LDMSD_LINFO, "%s: prdcr '%s' failed to post "
				"an event to prdset '%s', error %d\n",
				__func__, prdcr->obj.name, prdset->inst_name, rc);
		ref_put(&info->ref, "prdcr2prdset");
		ldmsd_prdcr_set_ref_put(prdset);
		ev_put(ev);
	}
	return rc;
}


int prdcr_updtr_state_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e)
{
	if (EV_OK != status)
		return 0;

	struct updtr_info *uinfo;
	enum ldmsd_updtr_state updtr_state;
	ldmsd_prdcr_t prdcr;
	int rc;

	uinfo = EV_DATA(e, struct updtr_state_data)->updtr_info;
	updtr_state = EV_DATA(e, struct updtr_state_data)->state;
	prdcr = EV_DATA(e, struct updtr_state_data)->obj;

	switch (updtr_state) {
	case LDMSD_UPDTR_STATE_RUNNING:
		rc = __prdset_filter(prdcr, &uinfo->match_list, __prdcr_post_updtr_state, e);
		break;
	default:
		break;
	}

	ldmsd_prdcr_put(prdcr);
	ev_put(e);
	return rc;
}

/* Producer tree worker event handlers */

/*
 * Aggregate the status of each producer.
 *
 * Without an error of a producer, the response to the client looks like this
 *    [ {<producer status} , {}, ... ].
 * In case an producer responding with an error, the response will be
 * <prdcr A's errmsg>, <prdcr B's errmsg>. \c reqc->errcode is the same
 * as the first error occurrence.
 */
static int
tree_status_rsp_handler(ldmsd_req_ctxt_t reqc, struct ldmsd_cfgobj_cfg_rsp *rsp,
						struct ldmsd_cfgobj_cfg_ctxt *ctxt)
{
	struct ldmsd_msg_buf *buf = (struct ldmsd_msg_buf *)rsp->ctxt;

	if (reqc->errcode) {
		if (!rsp->errcode) {
			/* Ignore the status */
			goto out;
		}
		(void) linebuf_printf(reqc, ", %s", rsp->errmsg);
		goto out;
	}

	if (rsp->errcode) {
		reqc->errcode = rsp->errcode;
		(void) snprintf(reqc->line_buf, reqc->line_len, "%s", rsp->errmsg);
		goto out;
	}

	if (1 < ctxt->num_recv)
		(void) linebuf_printf(reqc, ",");
	(void) linebuf_printf(reqc, "%s", buf->buf);

	if (ldmsd_cfgtree_done(ctxt)) {
		(void) linebuf_printf(reqc, "]");
		ldmsd_send_req_response(reqc, reqc->line_buf);
		free(ctxt);
	}

out:
	ldmsd_msg_buf_free(buf);
	free(rsp->errmsg);
	free(rsp);
	return 0;
}

#ifdef LDMSD_FAILOVER
static int tree_failover_peercfg_rsp_handler(ldmsd_req_ctxt_t reqc,
					     struct ldmsd_cfgobj_cfg_rsp *rsp,
					     struct prdcr_cfg_ctxt *ctxt)
{
	ev_t ev;
	struct fo_peercfg_ctxt *fo_ctxt = reqc->ctxt;
	if (rsp->errcode)
		fo_ctxt->is_prdcr_error = EINTR;
	free(rsp);
	free(rsp->errmsg);

	if (!__prdcr_tree_done(ctxt))
		return 0;

	fo_ctxt->is_prdcr_done = 1;

	ev = ev_new(cfgobj_rsp_type);
	if (!ev)
		return ENOMEM;
	EV_DATA(ev, struct cfgobj_rsp_data)->rsp = NULL;
	EV_DATA(ev, struct cfgobj_rsp_data)->ctxt = reqc;
	return ev_post(prdcr_tree_w, failover_w, ev, 0);
}
#endif /* LDMSD_FAILOVER */

static int
tree_cfg_rsp_handler(ldmsd_req_ctxt_t reqc, struct ldmsd_cfgobj_cfg_rsp *rsp,
					  struct ldmsd_cfgobj_cfg_ctxt *ctxt)
{
	if (rsp->errcode) {
		if (!reqc->errcode)
			reqc->errcode = rsp->errcode;
		if (ctxt->num_recv > 1)
			(void)linebuf_printf(reqc, ", ");
		(void)linebuf_printf(reqc, "%s", rsp->errmsg);
	}

	free(rsp->errmsg);
	free(rsp->ctxt);
	free(rsp);

	if (ldmsd_cfgtree_done(ctxt)) {
		/* All producers have sent back the responses */
		ldmsd_send_req_response(reqc, reqc->line_buf);
		free(ctxt);
	}
	return 0;
}

extern int ldmsd_cfgobj_tree_del_rsp_handler(ldmsd_req_ctxt_t reqc,
				struct ldmsd_cfgobj_cfg_rsp *rsp,
				struct ldmsd_cfgobj_cfg_ctxt *ctxt,
				enum ldmsd_cfgobj_type type);
int prdcr_tree_cfg_rsp_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t e)
{
	if (EV_OK != status)
		return 0;

	void *rsp = EV_DATA(e, struct cfgobj_rsp_data)->rsp;
	struct ldmsd_cfgobj_cfg_ctxt *ctxt = EV_DATA(e, struct cfgobj_rsp_data)->ctxt;
	struct ldmsd_req_ctxt *reqc = ctxt->reqc;
	int rc = 0;

	ctxt->num_recv++;
	if (!rsp)
		goto out;

	switch (reqc->req_id) {
	case LDMSD_PRDCR_ADD_REQ:
		assert(0 == "Impossible case");
		break;
	case LDMSD_PRDCR_DEL_REQ:
		rc = ldmsd_cfgobj_tree_del_rsp_handler(reqc, rsp, ctxt,
						   LDMSD_CFGOBJ_PRDCR);
		break;
	case LDMSD_PRDCR_DEFER_START_REQ:
	case LDMSD_PRDCR_START_REQ:
	case LDMSD_PRDCR_DEFER_START_REGEX_REQ:
	case LDMSD_PRDCR_START_REGEX_REQ:
	case LDMSD_PRDCR_STOP_REQ:
	case LDMSD_PRDCR_STOP_REGEX_REQ:
		rc = tree_cfg_rsp_handler(reqc, rsp, ctxt);
		break;
	case LDMSD_PRDCR_STATUS_REQ:
		rc = tree_status_rsp_handler(reqc, rsp, ctxt);
		break;
#ifdef LDMSD_FAILOVER
	case LDMSD_FAILOVER_PEERCFG_REQ:
		rc = tree_failover_peercfg_rsp_handler(reqc, rsp, ctxt);
		break;
#endif /* LDMSD_FAILOVER */
	default:
		assert(0 == "impossible case");
		rc = EINTR;
		goto out;
	}

	if (rc) {
		reqc->errcode = EINTR;
		(void) snprintf(reqc->line_buf, reqc->line_len,
				"LDMSD: failed to construct the response.");
		rc = 0;
	}

out:
	ev_put(e);
	return rc;
}


static int tree_prdcr_stop_regex_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	char *regex_str = NULL;
	regex_t regex;
	ldmsd_prdcr_t prdcr, nxt_prdcr;
	struct ldmsd_cfgobj_cfg_ctxt *ctxt = NULL;

	ctxt = calloc(1, sizeof(*ctxt));
	if (!ctxt)
		goto enomem;

	reqc->errcode = 0;

	regex_str = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_REGEX);
	if (!regex_str) {
		reqc->errcode = EINVAL;
		rc = linebuf_printf(reqc,
				"The attribute 'regex' is required by prdcr_start_regex.");
		goto send_reply;
	}

	reqc->errcode = ldmsd_compile_regex(&regex, regex_str, reqc->line_buf,
								reqc->line_len);
	if (reqc->errcode)
		goto send_reply;

	ldmsd_req_ctxt_sec_get(reqc, &ctxt->sctxt);

	prdcr = ldmsd_prdcr_first();
	while (prdcr) {
		nxt_prdcr = ldmsd_prdcr_next(prdcr);
		rc = regexec(&regex, prdcr->obj.name, 0, NULL, 0);
		if (rc)
			goto next;
		if (!nxt_prdcr)
			ctxt->is_all = 1;
		rc = ldmsd_cfgtree_post2cfgobj(&prdcr->obj, prdcr_tree_w,
						prdcr->worker, reqc, ctxt);
		if (rc) {
			if (ENOMEM == rc)
				goto enomem;
			reqc->errcode = EINTR;
			rc = linebuf_printf(reqc,
					"Failed to handle the prdcr_start command.");
			goto send_reply;
		}
	next:
		prdcr = nxt_prdcr;
	}
	goto out;
enomem:
	reqc->errcode = ENOMEM;
	rc = linebuf_printf(reqc, "LDMSD: out of memory.");
	LDMSD_LOG_ENOMEM();
	free(ctxt);
send_reply:
	ldmsd_send_req_response(reqc, reqc->line_buf);
out:
	free(regex_str);
	regfree(&regex);
	return 0;
}


static int __tree_forward2prdcr(ldmsd_req_ctxt_t reqc)
{
	int rc;
	char *name = NULL;
	ldmsd_prdcr_t prdcr;
	struct ldmsd_cfgobj_cfg_ctxt *ctxt = NULL;

	ctxt = calloc(1, sizeof(*ctxt));
	if (!ctxt)
		goto enomem;

	reqc->errcode = 0;

	name = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_NAME);
	if (!name) {
		reqc->errcode = EINVAL;
		rc = linebuf_printf(reqc,
				"The attribute 'name' is required.");
		goto send_reply;
	}

	ldmsd_req_ctxt_sec_get(reqc, &ctxt->sctxt);

	prdcr = ldmsd_prdcr_find(name);
	if (!prdcr) {
		reqc->errcode = ENOENT;
		rc = linebuf_printf(reqc, "prdcr '%s' does not exist.", name);
		goto send_reply;
	}

	ctxt->is_all = 1;
	rc = ldmsd_cfgtree_post2cfgobj(&prdcr->obj, prdcr_tree_w,
					prdcr->worker, reqc, ctxt);
	ldmsd_prdcr_put(prdcr); /* Put back ldmsd_prdcr_find()'s reference */
	if (rc) {
		if (ENOMEM == rc)
			goto enomem;
		reqc->errcode = EINTR;
		rc = linebuf_printf(reqc,
				"Failed to handle the prdcr_start command.");
		goto send_reply;
	}

	goto out;

enomem:
	reqc->errcode = ENOMEM;
	(void) linebuf_printf(reqc, "LDMSD: out of memory.");
	free(ctxt);
send_reply:
	ldmsd_send_req_response(reqc, reqc->line_buf);
out:
	free(name);
	return 0;
}

static inline int tree_prdcr_del_handler(ldmsd_req_ctxt_t reqc)
{
	return __tree_forward2prdcr(reqc);
}

static inline int tree_prdcr_stop_handler(ldmsd_req_ctxt_t reqc)
{
	return __tree_forward2prdcr(reqc);
}

static int tree_prdcr_start_regex_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	char *regex_str, *interval_str;
	regex_str = interval_str = NULL;
	regex_t regex;
	ldmsd_prdcr_t prdcr, nxt_prdcr;
	struct prdcr_start_ctxt *ctxt = NULL;

	ctxt = calloc(1, sizeof(*ctxt));
	if (!ctxt)
		goto enomem;

	if (LDMSD_PRDCR_DEFER_START_REGEX_REQ == reqc->req_id)
		ctxt->is_deferred = 1;

	reqc->errcode = 0;

	regex_str = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_REGEX);
	if (!regex_str) {
		reqc->errcode = EINVAL;
		rc = linebuf_printf(reqc,
				"The attribute 'regex' is required by prdcr_start_regex.");
		goto send_reply;
	}

	reqc->errcode = ldmsd_compile_regex(&regex, regex_str, reqc->line_buf,
								reqc->line_len);
	if (reqc->errcode)
		goto send_reply;

	interval_str = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_INTERVAL);
	if (interval_str) {
		reqc->errcode = ldmsd_time_dur_str2us(interval_str, &ctxt->interval);
		if (reqc->errcode) {
			rc = linebuf_printf(reqc,
					"The interval '%s' is invalid.", interval_str);
			goto send_reply;
		}
	}

	ldmsd_req_ctxt_sec_get(reqc, &ctxt->base.sctxt);

	prdcr = ldmsd_prdcr_first();
	while (prdcr) {
		nxt_prdcr = ldmsd_prdcr_next(prdcr);
		rc = regexec(&regex, prdcr->obj.name, 0, NULL, 0);
		if (rc)
			goto next;
		if (!nxt_prdcr)
			ctxt->base.is_all = 1;
		rc = ldmsd_cfgtree_post2cfgobj(&prdcr->obj, prdcr_tree_w,
						prdcr->worker, reqc, &ctxt->base);
		if (rc) {
			if (ENOMEM == rc)
				goto enomem;
			reqc->errcode = EINTR;
			rc = linebuf_printf(reqc,
					"Failed to handle the %s command.",
					ldmsd_req_id2str(reqc->req_id));
			goto send_reply;
		}
	next:
		prdcr = nxt_prdcr;
	}
	goto out;
enomem:
	reqc->errcode = ENOMEM;
	rc = linebuf_printf(reqc, "LDMSD: out of memory.");
	LDMSD_LOG_ENOMEM();
	free(ctxt);
send_reply:
	ldmsd_send_req_response(reqc, reqc->line_buf);
out:
	free(regex_str);
	free(interval_str);
	regfree(&regex);
	return 0;
}

static int tree_prdcr_start_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	char *name, *interval_str;
	name = interval_str = NULL;
	ldmsd_prdcr_t prdcr;
	struct prdcr_start_ctxt *ctxt = NULL;

	ctxt = calloc(1, sizeof(*ctxt));
	if (!ctxt)
		goto enomem;

	if (LDMSD_PRDCR_DEFER_START_REQ == reqc->req_id)
		ctxt->is_deferred = 1;

	reqc->errcode = 0;

	name = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_NAME);
	if (!name) {
		reqc->errcode = EINVAL;
		rc = linebuf_printf(reqc,
				"The attribute 'name' is required by prdcr_start.");
		goto send_reply;
	}

	interval_str = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_INTERVAL);
	if (interval_str) {
		reqc->errcode = ldmsd_time_dur_str2us(interval_str, &ctxt->interval);
		if (reqc->errcode) {
			rc = linebuf_printf(reqc,
					"The interval '%s' is invalid.", interval_str);
			goto send_reply;
		}
	}

	ldmsd_req_ctxt_sec_get(reqc, &ctxt->base.sctxt);

	prdcr = ldmsd_prdcr_find(name);
	if (!prdcr) {
		reqc->errcode = ENOENT;
		(void) linebuf_printf(reqc, "prdcr '%s' does not exist.", name);
		goto send_reply;
	}

	ctxt->base.is_all = 1;
	rc = ldmsd_cfgtree_post2cfgobj(&prdcr->obj, prdcr_tree_w,
					prdcr->worker, reqc, &ctxt->base);
	ldmsd_prdcr_put(prdcr); /* Put back ldmsd_prdcr_find()'s reference */
	if (rc) {
		if (ENOMEM == rc)
			goto enomem;
		reqc->errcode = EINTR;
		rc = linebuf_printf(reqc,
				"Failed to handle the prdcr_start command.");
		goto send_reply;
	}

	goto out;

enomem:
	reqc->errcode = ENOMEM;
	LDMSD_LOG_ENOMEM();
	(void) linebuf_printf(reqc, "LDMSD: out of memory.");
	free(ctxt);
send_reply:
	ldmsd_send_req_response(reqc, reqc->line_buf);
out:
	free(name);
	free(interval_str);
	return 0;
}

static int tree_prdcr_status_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	char *name;
	ldmsd_prdcr_t prdcr, nxt_prdcr;
	struct ldmsd_cfgobj_cfg_ctxt *ctxt = calloc(1, sizeof(*ctxt));
	if (!ctxt)
		goto enomem;

	ldmsd_req_ctxt_sec_get(reqc, &ctxt->sctxt);

	name = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_NAME);

	rc = linebuf_printf(reqc, "[");

	if (name) {
		prdcr = ldmsd_prdcr_find(name);
		if (!prdcr) {
			reqc->errcode = ENOENT;
			(void) linebuf_printf(reqc, "prdcr '%s' not found.", name);
		} else {
			rc = ldmsd_cfgtree_post2cfgobj(&prdcr->obj, prdcr_tree_w,
							prdcr->worker, reqc, ctxt);
			ldmsd_prdcr_put(prdcr);
			if (rc) {
				if (ENOMEM == rc)
					goto enomem;
				reqc->errcode = EINTR;
				rc = linebuf_printf(reqc,
						"Failed to handle the prdcr_start command.");
				goto send_reply;
			}
			ctxt->is_all = 1;
		}
	} else {
		prdcr = ldmsd_prdcr_first();
		while (prdcr) {
			nxt_prdcr = ldmsd_prdcr_next(prdcr);
			if (!nxt_prdcr)
				ctxt->is_all = 1;
			rc = ldmsd_cfgtree_post2cfgobj(&prdcr->obj, prdcr_tree_w,
							prdcr->worker, reqc, ctxt);
			if (rc) {
				if (ENOMEM == rc)
					goto enomem;
				reqc->errcode = EINTR;
				rc = linebuf_printf(reqc,
						"Failed to handle the prdcr_start command.");
				goto send_reply;
			}
			prdcr = nxt_prdcr;
		}
	}

	goto out;
enomem:
	reqc->errcode = ENOMEM;
	LDMSD_LOG_ENOMEM();
	(void) linebuf_printf(reqc, "LDMSD: out of memory.");
send_reply:
	ldmsd_send_req_response(reqc, reqc->line_buf);
out:
	free(name);
	return rc;

}

static int tree_prdcr_add_handler(ldmsd_req_ctxt_t reqc)
{
	ldmsd_prdcr_t prdcr;
	char *name, *host, *xprt, *attr_name, *type_s, *port_s, *interval_s;
	char *auth;
	enum ldmsd_prdcr_type type = -1;
	unsigned short port_no = 0;
	int interval_us = -1;
	size_t rc;
	uid_t uid;
	gid_t gid;
	int perm;
	char *perm_s = NULL;

	reqc->errcode = 0;
	name = host = xprt = type_s = port_s = interval_s = auth = NULL;

	attr_name = "name";
	name = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_NAME);
	if (!name)
		goto einval;

	attr_name = "type";
	type_s = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_TYPE);
	if (!type_s) {
		goto einval;
	} else {
		type = ldmsd_prdcr_str2type(type_s);
		if ((int)type < 0) {
			rc = linebuf_printf(reqc,
					"The attribute type '%s' is invalid.",
					type_s);
			if (rc < 0)
				goto enomem;
			reqc->errcode = EINVAL;
			goto send_reply;
		}
		if (type == LDMSD_PRDCR_TYPE_LOCAL) {
			rc = linebuf_printf(reqc,
					"Producer with type 'local' is "
					"not supported.");
			if (rc < 0)
				goto enomem;
			reqc->errcode = EINVAL;
			goto send_reply;
		}
	}

	attr_name = "xprt";
	xprt = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_XPRT);
	if (!xprt)
		goto einval;

	attr_name = "host";
	host = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_HOST);
	if (!host)
		goto einval;

	attr_name = "port";
	port_s = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_PORT);
	if (!port_s) {
		goto einval;
	} else {
		long ptmp = 0;
		ptmp = strtol(port_s, NULL, 0);
		if (ptmp < 1 || ptmp > USHRT_MAX) {
			goto einval;
		}
		port_no = (unsigned)ptmp;
	}

	attr_name = "interval";
	interval_s = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_INTERVAL);
	if (!interval_s) {
		goto einval;
	} else {
		 interval_us = strtol(interval_s, NULL, 0);
	}

	auth = ldmsd_req_attr_str_value_get_by_id(reqc, LDMSD_ATTR_AUTH);

	struct ldmsd_sec_ctxt sctxt;
	ldmsd_req_ctxt_sec_get(reqc, &sctxt);
	uid = sctxt.crd.uid;
	gid = sctxt.crd.gid;

	perm = 0770;
	perm_s = ldmsd_req_attr_str_value_get_by_name(reqc, "perm");
	if (perm_s)
		perm = strtol(perm_s, NULL, 0);

	prdcr = ldmsd_prdcr_new_with_auth(name, xprt, host, port_no, type,
					  interval_us, auth, uid, gid, perm);
	if (!prdcr) {
		if (errno == EEXIST)
			goto eexist;
		else if (errno == EAFNOSUPPORT)
			goto eafnosupport;
		else if (errno == ENOENT)
			goto ebadauth;
		else
			goto enomem;
	}

	goto send_reply;
ebadauth:
	reqc->errcode = ENOENT;
	rc = linebuf_printf(reqc,
			"Authentication name not found, check the auth_add configuration.");
	goto send_reply;
enomem:
	reqc->errcode = ENOMEM;
	(void) linebuf_printf(reqc, "Memory allocation failed.");
	goto send_reply;
eexist:
	reqc->errcode = EEXIST;
	rc = linebuf_printf(reqc, "The prdcr %s already exists.", name);
	goto send_reply;
eafnosupport:
	reqc->errcode = EAFNOSUPPORT;
	rc = linebuf_printf(reqc, "Error resolving hostname '%s'\n", host);
	goto send_reply;
einval:
	reqc->errcode = EINVAL;
	rc = linebuf_printf(reqc, "The attribute '%s' is required.", attr_name);
send_reply:
	ldmsd_send_req_response(reqc, reqc->line_buf);
	free(name);
	free(type_s);
	free(port_s);
	free(interval_s);
	free(host);
	free(xprt);
	free(perm_s);
	free(auth);
	return 0;
}

#ifdef LDMSD_FAILOVER
int tree_failover_peercfg_handler(ldmsd_req_ctxt_t reqc)
{
	int rc;
	ldmsd_prdcr_t prdcr;
	struct prdcr_cfg_ctxt *ctxt = calloc(1, sizeof(*ctxt));
	if (!ctxt) {
		LDMSD_LOG_ENOMEM();
		return ENOMEM;
	}

	for (prdcr = ldmsd_prdcr_first(); prdcr; prdcr = ldmsd_prdcr_next(prdcr)) {
		if (!ldmsd_cfgobj_is_failover(&prdcr->obj))
			continue;
		rc = __tree_cfg_ev_post(prdcr, reqc, ctxt);
		if (rc) {
			/*
			 * TODO: handle this
			 */
		}
	}

	return 0;
}
#endif /* LDMSD_FAILOVER */

int prdcr_tree_cfg_actor(ev_worker_t src, ev_worker_t dst,
					ev_status_t status, ev_t e)
{
	if (EV_OK != status)
		return 0;

	struct ldmsd_req_ctxt *reqc = EV_DATA(e, struct cfg_data)->reqc;
	int rc;

	switch (reqc->req_id) {
	case LDMSD_PRDCR_ADD_REQ:
		rc = tree_prdcr_add_handler(reqc);
		break;
	case LDMSD_PRDCR_DEL_REQ:
		rc = tree_prdcr_del_handler(reqc);
		break;
	case LDMSD_PRDCR_DEFER_START_REQ:
	case LDMSD_PRDCR_START_REQ:
		rc = tree_prdcr_start_handler(reqc);
		break;
	case LDMSD_PRDCR_DEFER_START_REGEX_REQ:
	case LDMSD_PRDCR_START_REGEX_REQ:
		rc = tree_prdcr_start_regex_handler(reqc);
		break;
	case LDMSD_PRDCR_STOP_REQ:
		rc = tree_prdcr_stop_handler(reqc);
		break;
	case LDMSD_PRDCR_STOP_REGEX_REQ:
		rc = tree_prdcr_stop_regex_handler(reqc);
		break;
	case LDMSD_PRDCR_STATUS_REQ:
		rc = tree_prdcr_status_handler(reqc);
		break;
#ifdef LDMSD_FAILOVER
	case LDMSD_FAILOVER_PEERCFG_REQ:
		rc = tree_failover_peercfg_handler(reqc);
		break;
#endif /* LDMSD_FAILOVER */
	default:
		ldmsd_log(LDMSD_LERROR, "%s doesn't handle req_id %d\n",
						__func__, reqc->req_id);
		/* TODO: put back reqc's 'create' ref */
		goto out;
	}
out:
	if (rc) {
		/* TODO: handle this */
	}
	ev_put(e);
	return 0;
}

int __tree_post_updtr_state(ldmsd_prdcr_t prdcr, void *args)
{
	int rc;
	ev_t e = args;
	struct updtr_info *info = EV_DATA(e, struct updtr_state_data)->updtr_info;
	enum ldmsd_updtr_state state = EV_DATA(e, struct updtr_state_data)->state;

	ev_t ev = ev_new(updtr_state_type);
	if (!ev) {
		LDMSD_LOG_ENOMEM();
		return ENOMEM;
	}
	ref_get(&info->ref, "prdcr_tree2prdcr");
	EV_DATA(ev, struct updtr_state_data)->state = state;
	EV_DATA(ev, struct updtr_state_data)->updtr_info = info;
	EV_DATA(ev, struct updtr_state_data)->obj = ldmsd_prdcr_get(prdcr);
	rc = ev_post(prdcr_tree_w, prdcr->worker, ev, 0);
	if (rc) {
		ldmsd_log(LDMSD_LINFO, "%s: prdcr_tree failed to post "
				"an event to prdcr '%s' error %d\n",
				__func__, prdcr->obj.name, rc);
		ref_put(&info->ref, "prdcr_tree2prdcr");
		ldmsd_prdcr_put(prdcr);
		ev_put(ev);
	}
	return rc;
}

typedef int (*actor_fn)(ldmsd_prdcr_t prdcr, void *args);
int __prdcr_filter(struct ldmsd_filter *filter, actor_fn actor, void *args)
{
	ldmsd_prdcr_t prdcr;
	struct ldmsd_filter_ent *fent;
	int rc;

	for (prdcr = ldmsd_prdcr_first(); prdcr; prdcr = ldmsd_prdcr_next(prdcr)) {
		TAILQ_FOREACH(fent, filter, entry) {
			if (fent->is_regex) {
				rc = regexec(&fent->regex, prdcr->obj.name, 0, NULL, 0);
			} else {
				rc = strcmp(prdcr->obj.name, fent->str);
			}
			if (0 == rc) {
				/* matched */
				(void) actor(prdcr, args);
				break;
			}
		}
	}
	return 0;
}

int prdcr_tree_updtr_state_actor(ev_worker_t src, ev_worker_t dst,
					ev_status_t status, ev_t e)
{
	if (EV_OK != status)
		return 0;

	int rc;
	struct updtr_info *updtr_info = EV_DATA(e, struct updtr_state_data)->updtr_info;
	enum ldmsd_updtr_state state = EV_DATA(e, struct updtr_state_data)->state;

	switch (state) {
	case LDMSD_UPDTR_STATE_RUNNING:
		rc = __prdcr_filter(&updtr_info->prdcr_list,
					__tree_post_updtr_state, e);
		break;
	default:
		break;
	}

	ev_put(e);
	return rc;
}
