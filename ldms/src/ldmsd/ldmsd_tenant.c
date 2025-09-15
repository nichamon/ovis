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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif /* _GNU_SOURCE */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ovis_util/util.h"
#include "ovis_ref/ref.h"

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_request.h"
#include "ldmsd_tenant.h"
#include "ldmsd_jobmgr_query.h"

extern ovis_log_t config_log;

#define LDMSD_TENANT_SCHEMA_PREFIX "TENANT"
#define LDMSD_TENANT_DEFAULT_MAX_CNT 64
#define JOBMGR_EVENT_NAME_LEN 256

int ldmsd_tenant_max_cnt = LDMSD_TENANT_DEFAULT_MAX_CNT; /* The default number of tenants */

enum ldmsd_tenant_metric_e {
	TENANT_LAST_EVENT = 0,
	TENANT_LIST,
};

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) (sizeof(a) / sizeof(a[0]))
#endif /* ARRAY_LEN */

/* The table contains the metrics in the tenant schema besides the tenant record definition and the list of tenants */
static struct ldms_metric_template_s tenant_metrics[] = {
	{ "last_event"                 , 0,  LDMS_V_CHAR_ARRAY   , ""       , JOBMGR_EVENT_NAME_LEN }, /* This collects the events delivered by jobmgr */
	{0},
};

static int __jobmgr_event_cb(ldmsd_jobmgr_query_t q, ldmsd_jobmgr_query_event_t e, void *arg)
{
	int rc = 0;
	ldmsd_tenant_def_t tdef = (ldmsd_tenant_def_t)arg;
	ldms_mval_t last_e;

	switch (e->event_type) {
	case LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE:
		/* Set heap is too small. */
		/* TODO: */
		ovis_log(tenant_log, OVIS_LERROR,
			"Current tenant count '%d' is larger than Max Tenant count '%d'. "
			"Tenants more than '%d' will be dropped. To address this, "
			"please restart ldmsd with higher maximum tenant count "
			"and larger pre-allocated memory with -m.\n.",
			e->no_space.number_of_records, ldmsd_tenant_max_cnt, ldmsd_tenant_max_cnt);
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE:
		if (tdef->deleted) {
			/* Users have sent the tenant delete command. */
			ldmsd_cfgobj_put(&tdef->obj, "init");
		} else {
			/* An error has occured. */
			/* TODO: report an error. Check with jobmgr if this can happen. */
			ldmsd_cfgobj_put(&tdef->obj, "jobmgr");
			ldmsd_cfgobj_rm(&tdef->obj);
			ldmsd_cfgobj_put(&tdef->obj, "init");
		}
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_START:
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_END:
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_START:
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_END:
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_START:
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_END:
		ldms_transaction_begin(tdef->set);
		last_e = ldms_metric_get(tdef->set, tdef->mids[TENANT_LAST_EVENT]);
		// snprintf(last_e->a_char, "%s", ldmsd_jobmgr_event_to_str(e->event_type));
		snprintf(last_e->a_char, JOBMGR_EVENT_NAME_LEN, "%d", e->event_type);
		ldms_transaction_end(tdef->set);
		break;
	default:
		/*TODO: Handle this case */
		break;
	}
	return rc;
}

void ldmsd_tenant___del(ldmsd_cfgobj_t obj)
{
	ldmsd_tenant_def_t tdef = (ldmsd_tenant_def_t)obj;
	free((char*)tdef->schema_name);
	free((char*)tdef->set_name);
	free(tdef->mids);
	if (tdef->schema)
		ldms_schema_delete(tdef->schema);
	if (tdef->set)
		ldms_set_delete(tdef->set);
	ldmsd_cfgobj___del(obj);
}

ldmsd_tenant_def_t ldmsd_tenant_def_new(const char *name, const char *dameon_name,
					struct ldmsd_str_list *attr_list,
					uid_t uid, gid_t gid, int perm)
{
	int rc;
	char *errstr = NULL;
	const char *schema_name = name;
	char *set_name = NULL;
	size_t heap_sz;
	size_t template_len = ARRAY_LEN(tenant_metrics) - 1;

	ldmsd_tenant_def_t tdef;

	tdef = (ldmsd_tenant_def_t)ldmsd_cfgobj_new_with_auth(name,
							      LDMSD_CFGOBJ_TENANT,
							      sizeof(*tdef),
							      ldmsd_tenant___del,
							      uid, gid, perm);
	if (!tdef) {
		ovis_log(tenant_log, OVIS_LCRIT, "Memory allocation failure.\n");
		errno = ENOMEM;
		return NULL;
	}

	tdef->mids = malloc(sizeof(int) * template_len);
	if (!tdef->mids) {
		goto enomem;
	}
	memset(tdef->mids, -1, template_len);

	rc = asprintf(&set_name, "%s/%s", dameon_name, schema_name);
	if (rc < 0) {
		goto enomem;
	}

	ldmsd_cfgobj_get(&tdef->obj, "jobmgr");
	tdef->jquery = ldmsd_jobmgr_query_new(attr_list, __jobmgr_event_cb, tdef, &errstr);
	if (!tdef->jquery) {
		ovis_log(tenant_log, OVIS_LERROR,
				     "Fail to query job information. Error %s\n", errstr);
		rc = EINTR;
		goto put_jobmgr;
	}

	tdef->schema = ldms_schema_from_template(schema_name, tenant_metrics, tdef->mids);
	if (!tdef->schema) {
		ldmsd_cfgobj_put(&tdef->obj, "jobmgr");
		ovis_log(tenant_log, OVIS_LERROR,
				    "Failed to create a schema of tenant '%s'\n", name);

		rc = EINTR;
		goto put_jobmgr;
	}

	tdef->tenant_recdef_mid = ldms_schema_record_add(tdef->schema, tdef->jquery->recdef);
	if (0 > tdef->tenant_recdef_mid) {
		rc = -tdef->tenant_recdef_mid;
		ovis_log(tenant_log, OVIS_LERROR,
				     "Failed to add the tenant record definition "
				     "to the schemea of tenant '%s'. Error %d.\n", name, rc);
		goto put_jobmgr;
	}

	heap_sz = ldms_record_heap_size_get(tdef->jquery->recdef) * ldmsd_tenant_max_cnt;
	tdef->tenant_list_mid = ldms_schema_metric_list_add(tdef->schema,
							    LDMSD_TENANT_LIST_NAME,
							    "", heap_sz);
	if (0 > tdef->tenant_list_mid) {
		rc = -tdef->tenant_list_mid;
		ovis_log(tenant_log, OVIS_LERROR,
				     "Failed to add the tenant list to "
				     "the schemea of tenant '%s'. Error %d.\n", name, rc);
		goto put_jobmgr;
	}

	tdef->set = ldms_set_new(set_name, tdef->schema);
	if (!tdef->set) {
		ovis_log(tenant_log, OVIS_LERROR,
				     "Failed to create the set of tenant '%s'\n", name);
		rc = EINTR;
		goto put_jobmgr;
	}
	tdef->heap_sz = ldms_set_heap_size_get(tdef->set);

	rc = ldmsd_jobmgr_query_execute(tdef->jquery, tdef->set, tdef->tenant_recdef_mid, tdef->tenant_list_mid);
	if (rc) {
		ovis_log(tenant_log, OVIS_LERROR,
				     "Failed to query the job information "
				     "for tenant '%s'. Error %d\n", name, rc);
		goto put_jobmgr;
	}
	ldms_set_publish(tdef->set);
	free(set_name);
	return tdef;
 enomem:
	ovis_log(tenant_log, OVIS_LCRIT, "Memory allocation failure.\n");
	rc = ENOMEM;
	goto cleanup;
 put_jobmgr:
	ldmsd_cfgobj_put(&tdef->obj, "jobmgr");
 cleanup:
	ldmsd_cfgobj_del(&tdef->obj);
	free(set_name);
	errno = rc;
	return NULL;
}

int ldmsd_tenant_def_del(ldmsd_tenant_def_t tdef)
{
	tdef->deleted = 1;
	ldmsd_jobmgr_query_close(tdef->jquery);

	ldmsd_cfgobj_rm(&tdef->obj);
	ldmsd_cfgobj_find_put(&tdef->obj);
	return 0;
}

int ldmsd_tenant_def_str(ldmsd_tenant_def_t tdef, int summary, ldmsd_req_ctxt_t reqc)
{
	int rc;
	int cnt;
	ldmsd_tenant_metric_t m;

	rc = ldmsd_linebuf_printf(reqc, "{\"name\" : \"%s\","
				        "\"attributes\" : ",
					tdef->obj.name);
	if (rc)
		goto enomem;

	cnt = 0;
	TAILQ_FOREACH(m, &tdef->mlist, ent) {
		if (cnt) {
			rc = ldmsd_linebuf_printf(reqc, ",");
			if (rc)
				goto enomem;
		}
		rc = ldmsd_linebuf_printf(reqc, "\"%s\"", m->mtempl.name);
		if (rc)
			goto enomem;
		cnt++;
	}
	rc = ldmsd_linebuf_printf(reqc, "}");
	if (rc)
		goto enomem;

	return 0;
enomem:
	return ENOMEM;
}