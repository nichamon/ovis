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

enum ldmsd_tenant_src_type {
	LDMSD_TENANT_SRC_JOB_SCHEDULER = 1, /* Job scheduler source */
};

struct tenant_metric_info {
	const char *name;
	const char *unit;
	enum ldms_value_type vtype;
};

/**
 * \brief Generic source interface
 *
 * Each source type implements this interface to provide tenant attributes.
 * Source are reference counted to allow sharing between multiple tenant metrics.
 */
struct ldmsd_tenant_source {
	ref_t ref;
	enum ldmsd_tenant_src_type type;        /**< Source type identifier */
	const char *name;                       /**< Human-readable source name */

	/* Interface methods */
	int (*can_provide)(const char *attr_name);
	/**< Determine the metric info, e.g., value type and unit */
	int (*get_metric_info)(struct attr_value *av, struct tenant_metric_info *minfo);
	int (*retrieve_value)(struct ldmsd_tenant_metric *tmet, ldms_mval_t mval);    /**< Runtime value retrieval method */
	void (*cleanup)(void *src_data);        /**< Cleanup source-specific data */
};

/**
 * \brief Tenant metric structure
 *
 * Represents a single tenant attribute.
 * The actual metric metadata (name, type, unit) is stored in
 * the record definition and accessed via \c rent_id.
 */
struct ldmsd_tenant_metric {
	struct ldmsd_tenant_source *src;        /**< Source that provides this metric (reference counted) */
	void *src_data;                         /**< Source-specific runtime data */
	struct ldms_metric_template_s mtempl;   /**< Metric template */
	int rent_id;                            /**< Reference to metric in LDMS record definition */
	TAILQ_ENTRY(tenant_metric) ent;         /**< List linkage */
};

/**
 * \brief List type for tenant metrics
 */
TAILQ_HEAD(ldmsd_tenant_metric_list, ldmsd_tenant_metric);

/**
 * \brief Tenant definition structure
 *
 * Reprensents the definition of
 */
struct ldmsd_tenant_def {
	struct ref_s ref;
	char *name;                              /**< Name of the tenant type, this is a key to reuse tenant definition. */
	struct ldmsd_tenant_metric_list mlist;   /**< List of metrics */
};

/**
 * \brief Create tenant definition from attribute requests
 *
 * This is the main entry point for creating tenant definitions.
 * It processes a list of attribute requests, creates appropriate metrics for each,
 * and build the record definition.
 *
 * \param attrs   User-specified attribute list for a tenant definition
 *
 * \return a handle of tenant metric list. errno is set on failure.
 */
struct ldmsd_tenant_def *ldmsd_tenant_def_create(struct attr_value_list attr_list);

#endif