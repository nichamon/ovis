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

extern struct ldmsd_tenant_source_s tenant_job_scheduler_source;
extern struct ldmsd_tenant_source_s tenant_na_source;

struct tenant_source_entry {
	int source_type;
	struct ldmsd_tenant_source_s *src;
};

static struct tenant_source_entry tenant_source_tbl[] = {
	{ LDMSD_TENANT_SRC_JOB_SCHEDULER, &tenant_job_scheduler_source },
	{ LDMSD_TENANT_SRC_NA, &tenant_na_source },
	{ -1, NULL }
};

static struct ldmsd_tenant_source_s *tenant_find_source(enum ldmsd_tenant_src_type_e type)
{
	int i;
	for (i = 0; tenant_source_tbl[i].src; i++) {
		if (tenant_source_tbl[i].source_type == type)
			return tenant_source_tbl[i].src;
	}
	return NULL;
}

/* ----------- Handlers of attributes that don't have any providers --------- */

static int __na_tenant_metric_init(const char *attr_value, struct ldmsd_tenant_metric_s *tmet)
{
	ldms_metric_template_t mtempl = &(tmet->mtempl);

	tmet->src_data = NULL;
	tmet->__src_type = LDMSD_TENANT_SRC_NA;
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

static void __na_tenant_metric_cleanup(struct ldmsd_tenant_metric_s *tmet)
{
	free((char*)tmet->mtempl.name);
}

static int __na_tenant_src_data_init(struct ldmsd_tenant_data_s *tdata)
{
	tdata->init_num_rows = 1;
	return 0;
}

static void __na_tenant_src_data_cleanup(void *src_ctxt)
{
	/* Nothing to do */
}

static int __na_tenant_values_get(struct ldmsd_tenant_data_s *tdata,
			   struct ldmsd_tenant_row_list_s *rlist,
			   int *is_empty)
{
	int i;
	struct ldmsd_tenant_row_s *row;
	*is_empty = 1;
	if (0 < rlist->active_rows) {
		/*
		 * There is always a single row for metrics without source.
		 * After it has been populated with empty strings, nothing else to do.
		 */
		return 0;
	}
	row = TAILQ_FIRST(&rlist->rows);
	for (i = 0; i < rlist->meta.num_cols; i++) {
		LDMSD_TENANT_ROW_CELL_PTR(rlist, row, i)->v_char = LDMSD_TENANT_MISSING_VALUE_CHAR;
	}
	rlist->active_rows = 1;
	return 0;
}

struct ldmsd_tenant_source_s tenant_na_source = {
	.type = LDMSD_TENANT_SRC_NA,
	.name = "tenant_src_na",
	.can_provide = NULL,
	.init_tenant_metric = __na_tenant_metric_init,
	.cleanup_tenant_metric = __na_tenant_metric_cleanup,
	.init_source_ctxt = __na_tenant_src_data_init,
	.cleanup_source_ctxt = __na_tenant_src_data_cleanup,
	.get_tenant_values = __na_tenant_values_get,
};
/* ------END:  Handlers of attributes that don't have any providers --------- */


/* Tenant definition creation */

static struct ldmsd_tenant_source_s *__get_source(const char *attr_value)
{
	int i;
	for (i = 0; tenant_source_tbl[i].src; i++) {
		if (tenant_source_tbl[i].src->can_provide &&
		    tenant_source_tbl[i].src->can_provide(attr_value)) {
			goto out;
		}
	}
	return NULL;
 out:
	return tenant_source_tbl[i].src;
}

void __tenant_metric_destroy(struct ldmsd_tenant_metric_s *tmet)
{
	struct ldmsd_tenant_source_s *src = tenant_find_source(tmet->__src_type);
	src->cleanup_tenant_metric(tmet);
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
	struct ldmsd_tenant_data_s *tdata;
	struct ldmsd_tenant_metric_s *tmet;
	int i;

	for (i = 0; tenant_source_tbl[i].src; i++) {
		tdata = &tdef->sources[i];
		while ((tmet = TAILQ_FIRST(&tdata->mlist))) {
			TAILQ_REMOVE(&tdata->mlist, tmet, ent);
			__tenant_metric_destroy(tmet);
		}
		tdata->src->cleanup_source_ctxt(tdata->src_ctxt);
	}
	free(tdef->name);
	free(tdef->rec_def_tmpl);
	// ldms_record_delete(tdef->rec_def);
	free(tdef);
}

pthread_mutex_t tenant_def_list_lock = PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(ldmsd_tenant_def_list, ldmsd_tenant_def_s) tenant_def_list;

/* Assume that tdata->mlist has been populated. */
static int __tenant_data_init(struct ldmsd_tenant_def_s *tdef, struct ldmsd_tenant_data_s *tdata)
{
	int i, rc;
	size_t offset;
	struct ldmsd_tenant_metric_s *tmet;
	ldmsd_tenant_row_list_meta_t rlist_meta = &tdata->rlist_meta;

	// tdata->gn = 0;
	if (0 == tdata->mcount) {
		/* No metric from this source */
		return 0;
	}

	rlist_meta->num_cols = tdata->mcount;
	rlist_meta->col_offsets = malloc(rlist_meta->num_cols * sizeof(size_t));
	rlist_meta->col_sizes = malloc(rlist_meta->num_cols * sizeof(size_t));
	if (!rlist_meta->col_offsets || !rlist_meta->col_sizes) {
		goto enomem;
	}

	offset = 0;
	i = 0;
	tmet = TAILQ_FIRST(&tdata->mlist);
	while (tmet) {
		tdef->rec_def_tmpl[i] = tmet->mtempl;
		tmet->__rent_id = i;

		// tmet->__rent_id = ldms_record_metric_add(tdef->rec_def,
		// 					 tmet->mtempl.name,
		// 					 tmet->mtempl.unit,
		// 					 tmet->mtempl.type,
		// 					 tmet->mtempl.len);
		// if (tmet->__rent_id < 0) {
		// 	rc = -tmet->__rent_id;
		// 	ovis_log(config_log, OVIS_LERROR,
		// 		"Cannot create tenant definition '%s' because "
		// 		"ldmsd failed to create the record definition "
		// 		"with error %d.\n", tdef->name, rc);
		// 	return rc;
		// }

		rlist_meta->col_sizes[i] = ldms_metric_value_size_get(tmet->mtempl.type, tmet->mtempl.len);
		rlist_meta->col_offsets[i] = offset;
		offset += rlist_meta->col_sizes[i];
		i++;
		tmet = TAILQ_NEXT(tmet, ent);
	}
	assert((i == tdata->mcount) && !tmet); /* If i and tmet must be aligned. */

	rlist_meta->row_size = offset;

	rc = tdata->src->init_source_ctxt(tdata);
	return rc;
 enomem:
	ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
	return ENOMEM;
}

/*
 * Failure in parsing a tenant attribute to an LDMS metric results in a metric of CHAR with an empty string as its value
 * Failure to create the record definition retults in an error of tenant definition creation.
 */
struct ldmsd_tenant_def_s *ldmsd_tenant_def_create(const char *name, struct ldmsd_str_list *str_list)
{
	int i, rc;
	struct ldmsd_tenant_def_s *tdef;
	struct ldmsd_tenant_metric_s *tmet;
	struct ldmsd_str_ent *str;

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
	for (i = 0; tenant_source_tbl[i].src; i++) {
		tdef->sources[i].src = tenant_source_tbl[i].src;
		TAILQ_INIT(&tdef->sources[i].mlist);
	}

	// tdef->rec_def = ldms_record_create(LDMSD_TENANT_REC_DEF_NAME);
	// if (!tdef->rec_def) {
	// 	ref_put(&tdef->ref, "create");
	// 	goto enomem;
	// }

	/* Initialize the data of each source */
	TAILQ_FOREACH(str, str_list, entry) {
		tmet = __process_tenant_attr(str->str);
		if (!tmet) {
			goto err;
		}
		tdef->sources[tmet->__src_type].mcount++;
		tdef->sources[tmet->__src_type].total_mem += ldms_metric_value_size_get(tmet->mtempl.type, tmet->mtempl.len);
		TAILQ_INSERT_TAIL(&tdef->sources[tmet->__src_type].mlist, tmet, ent);
		tdef->num_attrs++;
	}

	tdef->rec_def_tmpl = calloc(1, sizeof(struct ldms_metric_template_s) * (tdef->num_attrs + 1)); /* Create one extra element as the termining element of the array */
	if (!tdef->rec_def_tmpl) {
		goto enomem;
	}

	for (i = 0; tenant_source_tbl[i].src; i++) {
		rc = __tenant_data_init(tdef, &tdef->sources[i]);
		if (rc) {
			ref_put(&tdef->ref, "create");
			return NULL;
		}
	}
	// tdef->rec_def_heap_sz = ldms_record_heap_size_get(tdef->rec_def);

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
	int rc = 0;
	size_t heap_sz;
	int rec_def_idx, list_idx;
	ldms_record_t rec_def;
	int mids[tdef->num_attrs];

	rec_def = ldms_record_from_template(LDMSD_TENANT_REC_DEF_NAME, tdef->rec_def_tmpl, mids);
	if (!rec_def) {
		ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
		return ENOMEM;
	}
	heap_sz = num_tenants * ldms_record_heap_size_get(rec_def);

	rec_def_idx = ldms_schema_record_add(schema, rec_def);
	if (rec_def_idx < 0) {
		rc = -rec_def_idx;
		goto out;
	}
	list_idx = ldms_schema_metric_list_add(schema, LDMSD_TENANT_LIST_NAME, NULL, heap_sz);
	if (list_idx < 0) {
		rc = -list_idx;
		goto out;
	}
	if (_tenant_rec_def_idx)
		*_tenant_rec_def_idx = rec_def_idx;
	if (_tenants_idx)
		*_tenants_idx = list_idx;
 out:
	ldms_record_delete(rec_def);
	return rc;

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

ldmsd_tenant_row_t ldmsd_tenant_row_add(ldmsd_tenant_row_list_t rlist)
{
	ldmsd_tenant_row_t row;
	ldmsd_tenant_row_t *new_array;
	row = calloc(1, sizeof(*row) + rlist->meta.row_size);
	new_array = realloc(rlist->row_array, (rlist->allocated_rows+1) * sizeof(ldmsd_tenant_row_t));
	if (!row || !new_array) {
		free(row);
		free(new_array);
		return NULL;
	}
	TAILQ_INIT(&rlist->rows);
	rlist->row_array = new_array;
	rlist->row_array[rlist->allocated_rows] = row;
	TAILQ_INSERT_TAIL(&rlist->rows, row, ent);
	rlist->allocated_rows++;
	return row;
}

void ldmsd_tenant_mval_missing_val(ldms_mval_t dst, enum ldms_value_type type, size_t len)
{
	memset(dst, 0, ldms_metric_value_size_get(type, len));
	switch (type) {
	case LDMS_V_CHAR:
		dst->v_char = LDMSD_TENANT_MISSING_VALUE_CHAR;
		break;
	case LDMS_V_CHAR_ARRAY:
		dst->a_char[0] = LDMSD_TENANT_MISSING_VALUE_CHAR;
		break;
	case LDMS_V_RECORD_ARRAY:
	case LDMS_V_RECORD_INST:
	case LDMS_V_RECORD_TYPE:
		ovis_log(NULL, OVIS_LWARN, "Unsupported value type '%s' for missing values\n",
						ldms_metric_type_to_str(type));
		assert(0 == ENOTSUP);
	default:
		dst->v_u8 = LDMSD_TENANT_MISSING_VALUE_INT;
		break;
	}
}

void ldmsd_tenant_col_missing_val(ldms_mval_t dst, ldmsd_tenant_col_map_t col_map)
{
	enum ldms_value_type type;
	if (col_map->type == LDMS_V_LIST)
		type = col_map->ele_type;
	else
		type = col_map->type;
	ldmsd_tenant_mval_missing_val(dst, type, col_map->len);
}

ldmsd_tenant_row_list_t ldmsd_tenant_row_list_create(ldmsd_tenant_row_list_meta_t meta, int num_rows)
{
	ldmsd_tenant_row_list_t rlist = NULL;
	ldmsd_tenant_row_t row = NULL;
	int i;

	if (!meta) {
		errno = EINVAL;
		return NULL;
	}

	rlist = calloc(1, sizeof(*rlist));
	if (!rlist) {
		goto enomem;
	}
	memcpy(&rlist->meta, meta, sizeof(*meta));

	rlist->row_array = malloc(num_rows * sizeof(ldmsd_tenant_row_t));
	if (!rlist->row_array) {
		goto enomem;
	}
	for (i = 0; i < num_rows; i++) {
		row = calloc(1, sizeof(*row) + meta->row_size);
		if (!row) {
			goto enomem;
		}
		rlist->row_array[i] = row;
		TAILQ_INIT(&rlist->rows);
		TAILQ_INSERT_TAIL(&rlist->rows, row, ent);
		rlist->allocated_rows++;
	}
	return rlist;
 enomem:
	ovis_log(NULL, OVIS_LCRIT, "Memory allocation failure.\n");
	errno = ENOMEM;
	while ((row = TAILQ_FIRST(&rlist->rows))) {
		TAILQ_REMOVE(&rlist->rows, row, ent);
		free(row);
	}
	free(rlist->row_array);
	free(rlist);
	return NULL;
}

/*
 * **************** !!!!!!!! tdata->mcount MUST be determined before calling this function
 */
int ldmsd_tenant_values_sample(struct ldmsd_tenant_def_s *tdef, ldms_set_t set, int tenant_rec_mid, int tenants_mid)
{
	int i, j, type, rc;
	ldms_mval_t tenants, tenant, src, dst;
	struct ldmsd_tenant_data_s *tdata;
	struct ldmsd_tenant_metric_s *tmet;
	int tmp_idx;
	int is_src_empty, is_empty;
	ldmsd_tenant_row_list_t rlists[LDMSD_TENANT_SRC_COUNT];
	const char *set_name = ldms_set_instance_name_get(set);

	tenants = ldms_metric_get(set, tenants_mid);
	if (!tenants) {
		ovis_log(NULL, OVIS_LWARN,
			"Failed to retrieve the current information " \
			"of tenants for set %s.\n", set_name);
		return ENOENT;
	}

	ldms_list_purge(set, tenants);

	/*
	 * Get all the tenant metric values from all sources and calculate the number of tenants
	 */
	int total_tenant_cnt = 1;
	is_empty = 1;   /* Check if all sources have no values */
	for (i = 0; i < LDMSD_TENANT_SRC_COUNT; i++) {
		tdata = &tdef->sources[i];
		if (0 == tdata->mcount) {
			/* No tenant attributes are from this source, skip */
			continue;
		}

		rlists[i] = ldmsd_tenant_row_list_create(&tdata->rlist_meta, tdata->init_num_rows);
		if (!rlists[i]) {
			rc = errno;
			return rc;
		}
		rc = tdata->src->get_tenant_values(tdata, rlists[i], &is_src_empty);
		if (rc) {
			/* TODO: complete this */
		}
		total_tenant_cnt *= rlists[i]->active_rows;
		is_empty *= is_src_empty;
	}

	if (is_empty)
		total_tenant_cnt = 0;

	for (i = 0; i < total_tenant_cnt; i++) {
		int src_idx[LDMSD_TENANT_SRC_COUNT] = {0};
		tmp_idx = i;

		tenant = ldms_record_alloc(set, tenant_rec_mid);
		if (!tenant) {
			return ENOMEM; /* The caller should resize the set with a bigger heap */
		}

		/* Calculate which index to pick from each source */
		for (type = LDMSD_TENANT_SRC_COUNT - 1; type >= 0; type--) {
			tdata = &tdef->sources[type];
			if (0 == tdata->mcount) {
				/* No metrics from this source, skip */
				continue;
			}
			src_idx[type] = tmp_idx % rlists[type]->active_rows;
			tmp_idx = tmp_idx / rlists[type]->active_rows;
		}

		/* Add a tenant to the tenant list */
		for (type = 0; type < LDMSD_TENANT_SRC_COUNT; type++) {
			tdata = &tdef->sources[type];
			if (0 == tdata->mcount) {
				/* No metrics from this source, skip */
				continue;
			}
			/* The number of metrics in tsrc->mlist must be equal to  */
			for (j = 0, tmet = TAILQ_FIRST(&tdata->mlist); tmet;
					j++, tmet = TAILQ_NEXT(tmet, ent)) {
				dst = ldms_record_metric_get(tenant, tmet->__rent_id);
				src = LDMSD_TENANT_ROWLIST_CELL_PTR(rlists[type], src_idx[type], j);

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

// struct iter {
// 	int n_providers;
// 	struct list_head *heads; /* array of heads */
// 	struct row *rows; /* current row of each provider (head) */
// };

// struct row *iter_first(struct iter *itr)
// {
// 	struct row *row;
// 	for (i = 0; i < itr->n_providers; i++) {
// 		rows[i] = first(itr->heads[i]);
// 	}
// 	/* make joined row */
// 	row = join(itr->rows);
// 	return row;
// }

// struct row *iter_next(struct iter *itr)
// {
// 	for (i = itr->n_providers - 1; i>=0; i--) {
// 		rows[i] = next(itr->heads[i]);
// 		if (rows[i])
// 			break;
// 		rows[i] = first(itr->heads[i]);
// 	}
// 	if (i == -1)
// 		return NULL;
// 	row = join(iter->rows);
// 	return row;
// }
