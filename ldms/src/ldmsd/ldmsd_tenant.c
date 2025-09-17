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

int __na_tenant_metric_init(const char *attr_value, struct ldmsd_tenant_metric_s *tmet);
void __na_tenant_metric_cleanup(void *src_data);
int __na_tenant_values_get(struct ldmsd_tenant_data_s *tdata,
			   struct ldmsd_tenant_row_table_s *vtbl);
struct ldmsd_tenant_source_s tenant_na_source = {
	.type = LDMSD_TENANT_SRC_NONE,
	.name = "tenant_src_na",
	.can_provide = NULL,
	.init_tenant_metric = __na_tenant_metric_init,
	.cleanup = __na_tenant_metric_cleanup,
	.get_tenant_values = __na_tenant_values_get,
};

static struct ldmsd_tenant_source_s *tenant_source_tbl[] = {
	&tenant_na_source,
	&tenant_job_scheduler_source,
	/* Add new sources here */
	NULL
};

/* Not applicable tenant metric */

int __na_tenant_metric_init(const char *attr_value, struct ldmsd_tenant_metric_s *tmet)
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

void __na_tenant_metric_cleanup(void *src_data)
{
	/* TODO: complete this */
	assert(0 == ENOSYS);
}

int __na_tenant_values_get(struct ldmsd_tenant_data_s *tdata,
			   struct ldmsd_tenant_row_table_s *vtbl)
{
	int i;

	if (0 < vtbl->active_rows) {
		/*
		 * There is always a single row for metrics without source.
		 * After it has been populated with empty strings, nothing else to do.
		 */
		return 0;
	}
	for (i = 0; i < vtbl->num_cols; i++) {
		LDMSD_TENANT_ROWTBL_CELL_PTR(vtbl, 0, i)->v_char = '\0';
	}
	vtbl->active_rows = 1;
	return 0;
}

/* Tenant definition creation */

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

struct ldmsd_tenant_metric_s *__process_tenant_attr(const char *value)
{
	int rc;
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
		rc = __na_tenant_metric_init(value, tmet);
		if (rc) {
			goto err;
		}
		tmet->mtempl.flags = LDMS_MDESC_F_DATA;
	} else {
		rc = src->init_tenant_metric(value, tmet);
		if (rc) {
			ovis_log(config_log, OVIS_LINFO,
				"Failed to process tenant attribute '%s' with code %d.\n",
				value, rc);
			rc = __na_tenant_metric_init(value, tmet);
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
		tsrc = &tdef->sources[src_type];
		while ((tmet = TAILQ_FIRST(&tsrc->mlist))) {
			TAILQ_REMOVE(&tsrc->mlist, tmet, ent);
			__tenant_metric_destroy(tmet);
		}
	}
	free(tdef->name);
}

pthread_mutex_t tenant_def_list_lock = PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(ldmsd_tenant_def_list, ldmsd_tenant_def_s) tenant_def_list;

/* Assume that tdata->mlist has been populated. */
static int __tenant_data_init(struct ldmsd_tenant_def_s *tdef, struct ldmsd_tenant_data_s *tdata)
{
	int i, rc;
	size_t offset;
	struct ldmsd_tenant_metric_s *tmet;

	tdata->gn = 0;

	/* TODO: start row table */
	struct ldmsd_tenant_row_table_s *tbl = &(tdata->vtbl);


	tbl->num_cols = tdata->mcount;

	tbl->col_offsets = malloc(tbl->num_cols * sizeof(size_t));
	tbl->col_sizes = malloc(tbl->num_cols * sizeof(size_t));
	tbl->rows = malloc(sizeof(void *));
	if (!tbl->col_offsets || !tbl->col_sizes || !tbl->rows) {
		goto enomem;
	}

	offset = 0;
	i = 0;
	tmet = TAILQ_FIRST(&tdata->mlist);
	while (tmet) {
		tmet->__rent_id = ldms_record_metric_add(tdef->rec_def,
							 tmet->mtempl.name,
							 tmet->mtempl.unit,
							 tmet->mtempl.type,
							 tmet->mtempl.len);
		if (tmet->__rent_id < 0) {
			rc = -tmet->__rent_id;
			ovis_log(config_log, OVIS_LERROR,
				"Cannot create tenant definition '%s' because " \
				"ldmsd failed to create the record definition " \
				"with error %d.\n", tdef->name, rc);
			return rc;
		}

		tbl->col_sizes[i] = ldms_metric_value_size_get(tmet->mtempl.type, tmet->mtempl.len);
		tbl->col_offsets[i] = offset;
		offset += tbl->col_sizes[i];
		i++;
		tmet = TAILQ_NEXT(tmet, ent);
	}
	assert((i == tdata->mcount) && !tmet); /* If i and tmet must be aligned. */

	tbl->row_size = offset;

	/* Allocate memory of the first row */
	tbl->rows[0] = calloc(1, tbl->row_size);
	if (!tbl->rows[0]) {
		free(tbl->rows);
		free(tbl->col_sizes);
		free(tbl->col_offsets);
		goto enomem;
	}
	tbl->allocated_rows = 1;
	tbl->active_rows = 0;

	/* TODO: END row table */

	struct ldmsd_tenant_row_s *row;
	struct ldmsd_tenant_row_list_s *rlist = &(tdata->row_list);
	TAILQ_INIT(&rlist->rows);
	rlist->num_cols = tdata->mcount;
	rlist->col_offsets = malloc(rlist->num_cols * sizeof(size_t));
	rlist->col_sizes = malloc(rlist->num_cols * sizeof(size_t));
	if (!rlist->col_offsets || !rlist->col_sizes) {
		goto enomem;
	}

	offset = 0;
	i = 0;
	tmet = TAILQ_FIRST(&tdata->mlist);
	while (tmet) {
		tmet->__rent_id = ldms_record_metric_add(tdef->rec_def,
							 tmet->mtempl.name,
							 tmet->mtempl.unit,
							 tmet->mtempl.type,
							 tmet->mtempl.len);
		if (tmet->__rent_id < 0) {
			rc = -tmet->__rent_id;
			ovis_log(config_log, OVIS_LERROR,
				"Cannot create tenant definition '%s' because " \
				"ldmsd failed to create the record definition " \
				"with error %d.\n", tdef->name, rc);
			return rc;
		}

		rlist->col_sizes[i] = ldms_metric_value_size_get(tmet->mtempl.type, tmet->mtempl.len);
		rlist->col_offsets[i] = offset;
		offset += rlist->col_sizes[i];
		i++;
		tmet = TAILQ_NEXT(tmet, ent);
	}
	assert((i == tdata->mcount) && !tmet); /* If i and tmet must be aligned. */

	rlist->row_size = offset;

	/* Allocate memory of the first row */
	row = calloc(1, rlist->row_size);
	if (!row) {
		free(rlist->col_sizes);
		free(rlist->col_offsets);
		goto enomem;
	}
	rlist->allocated_rows = 1;
	rlist->active_rows = 0;

	return 0;
 enomem:
	ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
	return ENOMEM;
}

int ldmsd_tenant_row_table_resize(struct ldmsd_tenant_row_table_s *rtbl, int num_rows)
{
	int i;
	void **new_rows;

	new_rows = realloc(rtbl->rows, num_rows * sizeof(void *));
	if (!new_rows) {
		ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
		return ENOMEM;
	}
	rtbl->rows = new_rows;
	/* TODO: are we leaking the previous rtbl->rows? */

	/* Allocate new rows */
	for (i = rtbl->allocated_rows; i < num_rows; i++) {
		rtbl->rows[i] = calloc(1, rtbl->row_size);
		if (!rtbl->rows[i]) {
			ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
			return ENOMEM;
		}
	}
	rtbl->allocated_rows = num_rows;
	return 0;
}

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

	tdef = ldmsd_tenant_def_find(name);
	if (tdef) {
		ldmsd_tenant_def_put(tdef);
		errno = EEXIST;
		return NULL;
	}

	tdef = calloc(1, sizeof(*tdef));
	if (!tdef) {
		goto enomem;
	}

	ref_init(&tdef->ref, "create", __tenant_def_destroy, tdef);

	tdef->name = strdup(name);
	if (!tdef->name) {
		ref_put(&tdef->ref, "create");
		goto enomem;
	}

	/* Init each source */
	for (src_type = 0; src_type < LDMSD_TENANT_SRC_COUNT; src_type++) {
		tdef->sources[src_type].src = tenant_source_tbl[src_type];
		TAILQ_INIT(&tdef->sources[src_type].mlist);
	}

	tdef->rec_def = ldms_record_create(LDMSD_TENANT_REC_DEF_NAME);
	if (!tdef->rec_def) {
		ref_put(&tdef->ref, "create");
		goto enomem;
	}



	/* Initialize the data of each source */

	/* TODO: this is for testing; remove this */
	char *attr_values[] = { "job_id", "user", "job_name",
			       "job_uid", "job_gid" };

	for (i = 0; i < 5; i++) {
		attr_value = attr_values[i];
		tmet = __process_tenant_attr(attr_value);
		if (!tmet) {
			goto err;
		}
		tdef->sources[tmet->__src_type].mcount++;
		TAILQ_INSERT_TAIL(&tdef->sources[tmet->__src_type].mlist, tmet, ent);
	}

	// for (i = 0; i < av_list->count; i++) {
	// 	attr_value = av_value_at_idx(av_list, i);
	// 	tmet = __process_tenant_attr(attr_value);
	// 	if (!tmet) {
	// 		goto err;
	// 	}
	//	tdef->sources[tmet->__src_type].mcount++;
	//	tdef->sources[tmet->__src_type].total_mem += ldms_metric_value_size_get(tmet->mtempl.type, tmet->mtempl.len);
	// 	TAILQ_INSERT_TAIL(&tdef->sources[tmet->__src->type].mlist, tmet, ent);
	// }

	for (src_type = 0; src_type < LDMSD_TENANT_SRC_COUNT; src_type++) {
		rc = __tenant_data_init(tdef, &tdef->sources[src_type]);
		if (rc) {
			ref_put(&tdef->ref, "create");
			return NULL;
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

int ldmsd_tenant_schema_list_add(struct ldmsd_tenant_def_s *tdef, ldms_schema_t schema,
				 int num_tenants,
				 int *_tenant_rec_def_idx, int *_tenants_idx)
{
	int rc;
	size_t heap_sz;
	int rec_def_idx, list_idx;

	heap_sz = num_tenants * tdef->rec_def_heap_sz;
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
	if (_tenant_rec_def_idx)
		*_tenant_rec_def_idx = rec_def_idx;
	if (_tenants_idx)
		*_tenants_idx = list_idx;
	return 0;
}



static void __mval_copy(ldms_mval_t src, ldms_mval_t dst, enum ldms_value_type t, int len)
{
	size_t sz;

	if (!ldms_type_is_array(t))
		len = 1;

	switch (t) {
	case LDMS_V_CHAR:
	case LDMS_V_U8:
	case LDMS_V_S8:
	case LDMS_V_CHAR_ARRAY:
	case LDMS_V_U8_ARRAY:
	case LDMS_V_S8_ARRAY:
		sz = sizeof(uint8_t) * len;
		break;
	case LDMS_V_U16:
	case LDMS_V_S16:
	case LDMS_V_U16_ARRAY:
	case LDMS_V_S16_ARRAY:
		sz = sizeof(uint16_t) * len;
		break;
	case LDMS_V_U32:
	case LDMS_V_S32:
	case LDMS_V_U32_ARRAY:
	case LDMS_V_S32_ARRAY:
		sz = sizeof(uint32_t) * len;
		break;
	case LDMS_V_U64:
	case LDMS_V_S64:
	case LDMS_V_U64_ARRAY:
	case LDMS_V_S64_ARRAY:
		sz = sizeof(uint64_t) * len;
		break;
	case LDMS_V_F32:
	case LDMS_V_F32_ARRAY:
		sz = sizeof(float) * len;
		break;
	case LDMS_V_D64:
	case LDMS_V_D64_ARRAY:
		sz = sizeof(double) * len;
		break;
	default:
		assert(0 == "Unrecognized LDMS value array type");
	}

	memcpy(dst, src, sz);
}


int ldmsd_tenant_values_sample(struct ldmsd_tenant_def_s *tdef, ldms_set_t set, int tenant_rec_mid, int tenants_mid)
{
	int i, j, rc;
	ldms_mval_t tenants;
	ldms_mval_t tenant;
	int src_type;
	struct ldmsd_tenant_data_s *tdata;
	struct ldmsd_tenant_metric_s *tmet;
	int tmp_idx;
	struct ldmsd_tenant_row_table_s *vtbl;
	struct ldmsd_tenant_row_list_s *rlist;
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
		tdata = &tdef->sources[src_type];
		if (0 == tdata->mcount) {
			/* No tenant attributes are from this source, skip */
			continue;
		}
		/* TODO: Lock the tdata because multiple threads (multiple sets) can access the table at the same time. */
		// vtbl = &tdata->vtbl;
		// rc = tdata->src->get_tenant_values(tdata, vtbl);

		rlist = &tdata->row_list;
		rc = tdata->src->get_tenant_values(tdata, rlist);

		if (rc) {
			/* TODO: complete this */
		}
		total_tenant_cnt *= vtbl->active_rows;
	}

	for (i = 0; i < total_tenant_cnt; i++) {
		int src_idx[LDMSD_TENANT_SRC_COUNT] = {0};
		tmp_idx = i;

		tenant = ldms_record_alloc(set, tenant_rec_mid);
		if (!tenant) {
			/* TODO: resize */
			assert(0 == ENOMEM);
		}

		/* Calculate which index to pick from each source */
		for (src_type = LDMSD_TENANT_SRC_COUNT - 1; src_type >= 0; src_type--) {
			tdata = &tdef->sources[src_type];
			if (0 == tdata->mcount) {
				/* No metrics from this source, skip */
				continue;
			}
			vtbl = &tdata->vtbl;
			src_idx[src_type] = tmp_idx % vtbl->active_rows;
			tmp_idx = tmp_idx / vtbl->active_rows;
		}

		/* Add a tenant to the tenant list */
		for (src_type = 0; src_type < LDMSD_TENANT_SRC_COUNT; src_type++) {
			tdata = &tdef->sources[src_type];
			if (0 == tdata->mcount) {
				/* No metrics from this source, skip */
				continue;
			}
			vtbl = &tdata->vtbl;
			/* The number of metrics in tsrc->mlist must be equal to  */
			for (j = 0, tmet = TAILQ_FIRST(&tdata->mlist); tmet;
					j++, tmet = TAILQ_NEXT(tmet, ent)) {
				ldms_mval_t dst = ldms_record_metric_get(tenant, tmet->__rent_id);
				ldms_mval_t src = LDMSD_TENANT_ROWTBL_CELL_PTR(vtbl, src_idx[src_type], j);

				__mval_copy(src, dst, tmet->mtempl.type, tmet->mtempl.len);
			}
		}
		rc = ldms_list_append_record(set, tenants, tenant);
		if (rc) {
			/* TODO: complete this */
		}
	}

	return 0;
}
