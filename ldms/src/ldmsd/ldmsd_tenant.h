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
#ifndef __LDMSD_TENANT_H__
#define __LDMSD_TENANT_H__

#include "ovis_ref/ref.h"
#include "ovis_log/ovis_log.h"

#include "ldmsd.h"
#include "sampler_base.h"

struct ldmsd_tenant_source_s;
enum ldmsd_tenant_src_type {
	LDMSD_TENANT_SRC_NONE = 0,
	LDMSD_TENANT_SRC_JOB_SCHEDULER = 1, /* Job scheduler source */
};

#define LDMSD_TENANT_SRC_COUNT (LDMSD_TENANT_SRC_JOB_SCHEDULER+1)

/**
 * \brief Tenant metric structure
 *
 * Represents a single tenant attribute.
 * The actual metric metadata (name, type, unit) is stored in
 * the record definition and accessed via \c rent_id.
 */
struct ldmsd_tenant_metric_s {
	/* Fields for tenant source to assign values */
	void *src_data;                         /**< Source-specific runtime data */
	struct ldms_metric_template_s mtempl;   /**< Metric template */

	/* Fields internal to tenant core logic */
	int __rent_id;                            /**< Reference to metric in LDMS record definition */
	enum ldmsd_tenant_src_type __src_type;    /**< Source type providing the value */
	// struct ldmsd_tenant_source_s *__src;      /**< Source that provides this metric (reference counted) */
	TAILQ_ENTRY(ldmsd_tenant_metric_s) ent;         /**< List linkage */
};

/**
 * \brief List type for tenant metrics
 */
TAILQ_HEAD(ldmsd_tenant_metric_list, ldmsd_tenant_metric_s);

/* TODO: Remove this */
struct ldmsd_tenant_row_table_s {
	void **rows;     /**< Array of row pointers to ldms_mval_t */
	size_t row_size;        /**< Size of each row in bytes */
	size_t *col_offsets;    /**< Offset of each column within a row */
	size_t *col_sizes;      /**< Size of each column's ldms_mval_t */
	int num_cols;           /**< Number of columns */
	int active_rows;        /**< Currently valid/used rows */
	int allocated_rows;     /**< Total allocated rows (high water mark) */
};

#define LDMSD_TENANT_ROWTBL_CELL_PTR(_t_, _rid_, _cid_) \
    (((_rid_) < (_t_)->allocated_rows && (_cid_) < (_t_)->num_cols) ? \
     ((ldms_mval_t)((_t_)->rows[_rid_] + (_t_)->col_offsets[_cid_])) : NULL)

#define LDMSD_TENANT_ROWTBL_CELL_VAL(_t_, _rid_, _cid_) \
    (*(ldms_mval_t *)LDMSD_TENANT_ROWTBL_CELL_PTR(_t_, _rid_, _cid_))

/* TODO: END -- Remove this */

struct ldmsd_tenant_col_cfg_s {
	int mid;            /* LDMS metric ID in the source set */
	int rec_mid;        /* Record member ID (-1 if N/A) in the source set */
};

enum ldmsd_tenant_iter_type_e {
	LDMSD_TENANT_ITER_T_SCALAR,
	LDMSD_TENANT_ITER_T_LIST,
	LDMSD_TENANT_ITER_T_REC,
	LDMSD_TENANT_ITER_T_REC_ARRAY,
	LDMSD_TENANT_ITER_T_MISSING,
};

struct ldmsd_tenant_col_iter_s {
	enum ldmsd_tenant_iter_type_e type;
	struct ldmsd_tenant_col_cfg_s *cfg;
	ldms_set_t set;
	uint8_t exhausted;   /* 1 when no more data available */
	union {
		struct { /* single scalar value, no state needed */ } scalar;
		struct {
			ldms_mval_t curr; /* Current element*/
			enum ldms_value_type type; /* element type */
			size_t len; /* element length */
		} list;
		struct {
			ldms_mval_t rec;
			ldms_mval_t curr;
		} rec_inst;
		struct {
			int curr_idx;
			int max_len;
			ldms_mval_t array;
			ldms_mval_t curr_rec;
		} rec_array;
	} state;
};

struct ldmsd_tenant_row_s {
	void *data;        /* Raw row data (array of *ldms_mval_t )*/
	TAILQ_ENTRY(ldmsd_tenant_row_s) ent;
};

struct ldmsd_tenant_row_list_s {
	int num_cols;
	size_t row_size;
	size_t *col_offsets;
	size_t *col_sizes;
	int active_rows;        /* Number of rows containing valid values */
	int allocated_rows;     /* Number of allocated rows, which >= num_active */
	TAILQ_HEAD(, ldmsd_tenant_row_s) rows;
};

struct ldmsd_tenant_data_s {
	struct ldmsd_tenant_source_s *src;
	size_t total_mem;	/* Summation of the memory of each metric ldms_mval_t */
	int mcount;		/* Number of metrics */
	struct ldmsd_tenant_metric_list mlist;
	uint64_t gn;    /* This number is to check if the source needs to update the mval table or not. The source is responsible for genarating this number. */
	struct ldmsd_tenant_row_table_s vtbl; /**< Metric values table */ /* TODO: We might want to remove this so that we don't need to lock it. */
	struct ldmsd_tenant_row_list_s row_list; /**< List of rows */
	void *src_ctxt;        /* Sources-specific context */
};

#define LDMSD_TENANT_REC_DEF_NAME "tenant_def"
#define LDMSD_TENANT_LIST_NAME    "tenants"

/**
 * \brief Tenant definition structure
 *
 * Reprensents the definition of
 */
struct ldmsd_tenant_def_s {
	struct ref_s ref;
	char *name;                              /**< Name of the tenant type, this is a key to reuse tenant definition. */
	struct ldmsd_tenant_data_s sources[LDMSD_TENANT_SRC_COUNT];   /**< List of metrics by sources, for easy querying */
	ldms_record_t rec_def;                   /**< Definition of the record of the tenant metrics */
	size_t rec_def_heap_sz;                  /**< Heap size of a record instance */
	LIST_ENTRY(ldmsd_tenant_def_s) ent;      /**< Entry in the definition list */
};

/**
 * \brief Generic source interface
 *
 * Each source type implements this interface to provide tenant attributes.
 * Source are reference counted to allow sharing between multiple tenant metrics.
 */
struct ldmsd_tenant_source_s {
	struct ref_s ref;
	enum ldmsd_tenant_src_type type;        /**< Source type identifier */
	const char *name;                       /**< Human-readable source name */

	/* Interface methods */
	int (*can_provide)(const char *attr_name);
	/**< Assign values to src_data and metric_template, the flag in metric_template can be ignored */
	int (*init_tenant_metric)(const char *attr_value, struct ldmsd_tenant_metric_s *tmet);
	/**< Runtime value retrieval method, assuming that \c mval was allocated with enough memory */
	int (*get_tenant_values)(struct ldmsd_tenant_data_s *tdata, struct ldmsd_tenant_row_table_s *vtbl);
	void (*cleanup)(void *src_data);        /**< Cleanup source-specific data */
};

/**
 * \brief Create tenant definition from attribute requests
 *
 * This is the main entry point for creating tenant definitions.
 * It processes a list of attribute requests, creates appropriate metrics for each,
 * and build the record definition.
 *
 * A tenant is defined as a unique vector of attributes in the \c av_list.
 * Two tenants are considered different if they differ in any attribute value.
 *
 * Each tenant is represented as a vector where:
 *  - Attribute from the same source group together
 *  - Attribute position follows the order in \c av_list among attributes from the same source
 *
 * \param name    A tenant definition name
 * \param attrs   User-specified attribute list for a tenant definition
 *
 * \return a handle of tenant definition. errno is set on failure.
 */
struct ldmsd_tenant_def_s *ldmsd_tenant_def_create(const char *name, struct attr_value_list *av_list);

/**
 * \brief Call when the tenant definition should not be used anymore
 *
 * \param tdef   A tenant definition handle
 */
void ldmsd_tenant_def_free(struct ldmsd_tenant_def_s *tdef);

/**
 * \brief Find a tenant definition
 *
 * \param name   A tenant definition name
 *
 * \return a handle of a tenant definition. NULL is returned if the name doesn't exist.
 */
struct ldmsd_tenant_def_s *ldmsd_tenant_def_find(const char *name);

/**
 * \brief Return a reference from \c ldmsd_tenant_def_find
 *
 * \param tdef   A tenant definition handle
 */
void ldmsd_tenant_def_put(struct ldmsd_tenant_def_s *tdef);

/**
 * \brief Add an LDMS list metric to \c schema
 *
 * \param tdef   A tenant definition handle
 * \param schema An LDMS schema handle
 * \param num_tenants           Estimated number of tenants
 * \param _tenant_rec_def_idx   Metric index of the tenant record definition
 * \param _tenant_list_idx      Metric index of the tenant list
 *
 * \return 0 on success; otherwise, a non-zero error code is returned
 */
int ldmsd_tenant_schema_list_add(struct ldmsd_tenant_def_s *tdef, ldms_schema_t schema,
				 int num_tenants,
				 int *_tenant_rec_def_idx, int *_tenants_idx);

/**
 * \brief Resize the row tables
 *
 * If \c num_rows is less than the number of allocated rows, the function does nothing.
 *
 * \param rtbl       A handle to the row table to be resized (more rows)
 * \param num_rows   A new number of rows
 */
int ldmsd_tenant_row_table_resize(struct ldmsd_tenant_row_table_s *rtbl, int num_rows);

/**
 * \brief Update the tenant list of an LDMS set
 *
 * This function updates the list of tenants to contains the information of all current tenants
 *
 * \param tdef             Tenant definition to retrieve values for
 * \param set              An LDMS set to be updated
 * \param tenant_rec_mid   Metric ID of the record definition metric
 * \param tenants_mid      Metric ID of the tenant list
 *
 * \return 0 on success, negative error code on failure (partial updates possible)
 */
int ldmsd_tenant_values_sample(struct ldmsd_tenant_def_s *tdef, ldms_set_t set, int tenant_rec_mid, int tenants_mid);

#endif