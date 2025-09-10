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

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ovis_util/util.h"
#include "ovis_ref/ref.h"

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_tenant.h"

extern ovis_log_t config_log;

extern struct ldmsd_tenant_source_s tenant_job_scheduler_source; /* TODO: Implement this */

static struct ldmsd_tenant_source_s *tenant_source_tbl[] = {
	&tenant_job_scheduler_source,
	/* Add new sources here */
	NULL
};

static struct ldmsd_tenant_source_s *__get_source(const char *attr_value)
{
	int i;
	for (i = 0; tenant_source_tbl[i]; i++) {
		if (tenant_source_tbl[i]->can_provide &&
		    tenant_source_tbl[i]->can_provide(attr_value)) {
			goto out;
		}
	}
	return NULL;
 out:
	ref_get(&(tenant_source_tbl[i]->ref), "__get_source");
	return tenant_source_tbl[i];
}

void __tenant_metric_destroy(struct ldmsd_tenant_metric_s *tmet)
{
	/* TODO: Implement this, or let the source cleanup
	because source is the one who initializes this */
}

int __init_empty_tenant_metric(const char *attr_value, struct ldmsd_tenant_metric_s *tmet)
{
	ldms_metric_template_t mtempl = &(tmet->mtempl);

	tmet->src_data = NULL;
	tmet->__src_type = LDMSD_TENANT_SRC_NONE;
	mtempl->name = strdup(attr_value);
	if (!mtempl->name) {
		ovis_log(config_log, OVIS_LCRIT, "Memory allocation failure.\n");
		return ENOMEM;
	}
	mtempl->flags = LDMS_MDESC_F_META;
	mtempl->type = LDMS_V_CHAR;
	mtempl->unit = "";
	mtempl->len = 1;
	return 0;
}

struct ldmsd_tenant_metric_s *__process_tenant_attr(const char *value)
{
	int i, rc;
	struct ldmsd_tenant_source_s *src;
	struct ldmsd_tenant_metric_s *tmet;

	errno = 0;

	tmet = calloc(1, sizeof(*tmet));
	if (!tmet) {
		ovis_log(config_log, OVIS_LCRIT, "Memory allocation failure\n");
		errno = ENOMEM;
		return NULL;
	}

	src = __get_source(value);
	if (!src) {
		ovis_log(config_log, OVIS_LINFO, "Tenant attribute '%s' is unavailable.\n", value);
		rc = __init_empty_tenant_metric(value, tmet);
		if (rc) {
			goto err;
		}
		tmet->mtempl.flags = LDMS_MDESC_F_DATA;
	} else {
		rc = src->init_tenant_metric(value, tmet);
		if (!rc) {
			ovis_log(config_log, OVIS_LINFO,
				"Failed to process tenant attribute '%s' with code %d.\n",
				value, rc);
			rc = __init_empty_tenant_metric(value, tmet);
			if (rc)
				goto err;
		}
		tmet->__src_type = src->type;
	}

	return tmet;
 err:
	__tenant_metric_destroy(tmet);
	errno = rc;
	return NULL;
}

void __tenant_def_destroy(void *arg)
{
	struct ldmsd_tenant_def_s *tdef = (struct ldmsd_tenant_def_s *)arg;
	struct ldmsd_tenant_data_s *tsrc;
	struct ldmsd_tenant_metric_s *tmet;
	int src_type;

	for (src_type = 0; src_type < LDMSD_TENANT_SRC_COUNT; src_type++) {
		tsrc = tdef->sources[src_type];
		while ((tmet = TAILQ_FIRST(&tsrc->mlist))) {
			TAILQ_REMOVE(&tsrc->mlist, tmet, ent);
			__tenant_metric_destroy(tmet);
		}
	}
	free(tdef->name);
}

pthread_mutex_t tenant_def_list_lock = PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(ldmsd_tenant_def_list, ldmsd_tenant_def_s) tenant_def_list;

/*
 * Failure in parsing a tenant attribute to an LDMS metric results in a metric of CHAR with an empty string as its value
 * Failure to create the record definition retults in an error of tenant definition creation.
 */
struct ldmsd_tenant_def_s *ldmsd_tenant_def_create(const char *name, struct attr_value_list *av_list)
{
	int i, rc;
	const char *attr_value;
	struct ldmsd_tenant_def_s *tdef;
	struct ldmsd_tenant_metric_s *tmet;
	enum ldmsd_tenant_src_type src_type;

	tdef = malloc(sizeof(*tdef));
	if (!tdef) {
		goto enomem;
	}

	ref_init(&tdef->ref, "create", __tenant_def_destroy, tdef);

	tdef->name = strdup(name);
	if (!tdef->name) {
		ref_put(&tdef->ref, "create");
		goto enomem;
	}

	for (src_type = 0; src_type < LDMSD_TENANT_SRC_COUNT; src_type++) {
		TAILQ_INIT(&tdef->sources[src_type]->mlist);
	}

	tdef->rec_def = ldms_record_create(LDMSD_TENANT_REC_DEF_NAME);
	if (!tdef->rec_def) {
		ref_put(&tdef->ref, "create");
		goto enomem;
	}

	/* TODO: this is for testing; remove this */
	char *attr_values[] = { "job_id", "user", "job_name",
			       "job_uid", "job_gid" };

	for (i = 0; i < 5; i++) {
		tmet = __process_tenant_attr(attr_values[i]);
		if (!tmet) {
			goto err;
		}
		TAILQ_INSERT_TAIL(&tdef->sources[tmet->__src_type]->mlist, tmet, ent);
	}

	// for (i = 0; i < av_list->count; i++) {
	// 	attr_value = av_value_at_idx(av_list, i);
	// 	tmet = __process_tenant_attr(attr_value);
	// 	if (!tmet) {
	// 		goto err;
	// 	}
	// 	TAILQ_INSERT_TAIL(&tdef->sources[tmet->__src->type].mlist, tmet, ent);
	// }

	for (src_type = 0; src_type < LDMSD_TENANT_SRC_COUNT; src_type++) {
		tdef->sources[src_type] = tenant_source_tbl[src_type];
		TAILQ_FOREACH(tmet, &tdef->sources[src_type]->mlist, ent) {
			tmet->__rent_id = (tdef->rec_def, tmet->mtempl.name,
					   tmet->mtempl.unit, tmet->mtempl.type,
					   tmet->mtempl.len);
			if (tmet->__rent_id < 0) {
				ovis_log(config_log, OVIS_LERROR,
					"Cannot create tenant definition '%s' because " \
					"ldmsd failed to create the record definition " \
					"with error %d.\n", name, rc);
				goto err;
			}
		}
	}
	tdef->rec_def_heap_sz = ldms_record_heap_size_get(tdef->rec_def);

	pthread_mutex_lock(&tenant_def_list_lock);
	LIST_INSERT_HEAD(&tenant_def_list, tdef, ent);
	pthread_mutex_unlock(&tenant_def_list_lock);
	return tdef;

 enomem:
	ovis_log(config_log, OVIS_LCRIT, "Memory failure allocation\n");
	errno = ENOMEM;
	return NULL;
 err:
	ref_put(&tdef->ref, "creation");
	return NULL;
}

void ldmsd_tenant_def_free(struct ldmsd_tenant_def_s *tdef)
{
	ref_put(&tdef->ref, "create");
}

struct ldmsd_tenant_def_s *ldmsd_tenant_def_find(const char *name)
{
	struct ldmsd_tenant_def_s *tdef;

	pthread_mutex_lock(&tenant_def_list_lock);
	LIST_FOREACH(tdef, &tenant_def_list, ent) {
		if (0 == strcmp(tdef->name, name)) {
			ref_get(&tdef->ref, "find");
			pthread_mutex_unlock(&tenant_def_list_lock);
			return tdef;
		}
	}
	pthread_mutex_unlock(&tenant_def_list_lock);
	return NULL;

}

void ldmsd_tenant_def_put(struct ldmsd_tenant_def_s *tdef)
{
	ref_put(&tdef->ref, "find");
}

#define NUM_TENANTS 1
int ldmsd_tenant_schema_list_add(struct ldmsd_tenant_def_s *tdef, ldms_schema_t schema,
				 int *_tenant_rec_def_idx, int *_tenants_idx,
				 size_t *_heap_sz)
{
	int rc;
	size_t heap_sz;
	int rec_def_idx, list_idx;

	heap_sz = NUM_TENANTS * tdef->rec_def_heap_sz;
	rec_def_idx = ldms_schema_record_add(schema, tdef->rec_def);
	if (rec_def_idx < 0) {
		rc = -rec_def_idx;
		return rc;
	}
	list_idx = ldms_schema_metric_list_add(schema, LDMSD_TENANT_LIST_NAME, NULL, heap_sz);
	if (list_idx < 0) {
		rc = -list_idx;
		return rc;
	}

	*_tenant_rec_def_idx = rec_def_idx;
	*_tenants_idx = list_idx;
	*_heap_sz = heap_sz;

	return 0;
}


int ldmsd_tenant_values_sample(struct ldmsd_tenant_def_s *tdef, ldms_set_t set, int tenants_mid)
{
	int i, j, rc;
	ldms_mval_t tenants;
	ldms_mval_t tenant;
	enum ldmsd_tenant_src_type src_type;
	struct ldmsd_tenant_data_s *tdata;
	struct ldmsd_tenant_metric_s *tmet;
	ldms_mval_t *src_mval_list[LDMSD_TENANT_SRC_COUNT];
	int src_count[LDMSD_TENANT_SRC_COUNT];
	int tmp_idx;
	int rec_idx;
	const char *set_name = ldms_set_instance_name_get(set);

	tenants = ldms_metric_get(set, tenants_mid);
	if (!tenants) {
		ovis_log(NULL, OVIS_LINFO,
			"Failed to retrieve the current information " \
			"of tenants for set %s.\n", set_name);
		return ENOENT;
	}

	ldms_list_purge(set, tenants);

	/* TODO: complete this */
	/*
	 * Get all the tenant metric values from all sources and calculate the number of tenants
	 */
	int total_tenant_cnt = 1;
	for (src_type = 0; src_type < LDMSD_TENANT_SRC_COUNT; src_type++) {
		tdata = tdef->sources[src_type];
		if (0 == tdata->mcount) {
			/* No tenant attributes are from this source, skip */
			continue;
		}
		src_mval_list[src_type] = malloc(sizeof(ldms_mval_t) * tdata->mcount);
		if (!src_mval_list[src_type]) {
			/* TODO: complete this */
		}
		rc = tdata->src->get_tenant_values(tdata, &src_mval_list[src_type], &src_count[src_type]);
		if (rc) {
			/* TODO: complete this */
		}
		total_tenant_cnt *= src_count[src_type];
	}

	for (i = 0; i < total_tenant_cnt; i++) {
		int src_idx[LDMSD_TENANT_SRC_COUNT] = {0};
		tmp_idx = i;

		tenant = ldms_record_alloc(set, tenants_mid);
		if (!tenant) {
			/* TODO: resize */
		}

		/* Calculate which index to pick from each source */
		for (src_type = LDMSD_TENANT_SRC_COUNT - 1; src_type >= 0; src_type--) {
			if (0 == tdef->sources[src_type]->mcount) {
				/* No metrics from this source, skip */
				continue;
			}
			src_idx[src_type] = tmp_idx % src_count[src_type];
			tmp_idx = tmp_idx / src_count[src_type];
		}

		/* Add a tenant to the tenant list */
		rec_idx = 0;
		ldms_mval_t *src_rec;
		for (src_type = 0; src_type < LDMSD_TENANT_SRC_COUNT; src_type++) {
			tdata = &tdef->sources[src_type];
			if (0 == tdata->mcount) {
				/* No metrics from this source, skip */
				continue;
			}
			src_rec = &src_mval_list[src_type][src_idx[src_type]];
			tmet = TAILQ_FIRST(&tdata->mlist);
			j = 0;
			/* The number of metrics in tsrc->mlist must be equal to  */
			while (tmet) {
				ldms_record_metric_set(tenant, tmet->__rent_id, src_rec[j]);
				tmet = TAILQ_NEXT(tmet, ent);
				j++;
			}
		}
		rc = ldms_list_append_record(set, tenants, tenant);
		if (rc) {
			/* TODO: complete this */
		}
	}

	return 0;
}