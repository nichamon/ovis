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
#include <stdlib.h>
#include <string.h>

#include "ovis_util/util.h"
#include "ovis_ref/ref.h"

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_tenant.h"

ovis_log_t config_log;

extern struct ldmsd_tenant_source_s tenant_job_scheduler_source; /* TODO: Implement this */

static struct ldmsd_tenant_source_s *tenant_source_tbl[] = {
	&tenant_job_scheduler_source,
	/* Add new sources here */
	NULL
};

static struct ldmsd_tenant_source_s *__get_source(struct attr_value *attr)
{
	int i;
	for (i = i; tenant_source_tbl[i]; i++) {
		if (tenant_source_tbl[i]->can_provide &&
		    tenant_source_tbl[i]->can_provide(attr->value)) {
			goto out;
		}
	}
	return NULL;
 out:
	ref_get(&tenant_source_tbl[i]->ref, "__get_source");
	return &tenant_source_tbl[i];
}

void __tenant_metric_destroy(struct ldmsd_tenant_metric_s *tmet)
{
	/* TODO: Implement this, or let the source cleanup
	because source is the one who initializes this */
	struct ldmsd_tenant_source_s *src;
	src = tmet->__src;
	ldmsd_tenant_source_put(src);
}

int __init_empty_tenant_metric(struct attr_value *av, struct ldmsd_tenant_metric_s *tmet)
{
	ldms_metric_template_t mtempl = &(tmet->mtempl);

	tmet->__src = tmet->src_data = NULL;
	mtempl->name = strdup(av->value);
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

struct ldmsd_tenant_metric_s *__process_tenant_attr(struct attr_value *av)
{
	int i, rc;
	struct attr_value *av;
	struct ldmsd_tenant_source_s *src;
	struct ldmsd_tenant_metric_s *tmet;

	errno = 0;

	tmet = calloc(1, sizeof(*tmet));
	if (!tmet) {
		ovis_log(config_log, OVIS_LCRIT, "Memory allocation failure\n");
		errno = ENOMEM;
		return NULL;
	}

	src = __get_source(av);
	if (!src) {
		ovis_log(config_log, OVIS_LINFO, "Tenant attribute '%s' is unavailable.\n", av->value);
		rc = __init_empty_tenant_metric(av, tmet);
		if (rc) {
			goto err;
		}
		tmet->mtempl.flags = LDMS_MDESC_F_DATA;
	} else {
		rc = src->init_tenant_metric(av, tmet);
		if (!rc) {
			ovis_log(config_log, OVIS_LINFO,
				"Failed to process tenant attribute '%s:%s' with code %d.\n",
				av->name, av->value, rc);
			rc = __init_empty_tenant_metric(av, tmet);
			if (rc)
				goto err;
		}
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
	struct ldmsd_tenant_metric_s *tmet;

	while ((tmet = TAILQ_FIRST(&tdef->mlist))) {
		TAILQ_REMOVE(&tdef->mlist, tmet, ent);
		__tenant_metric_destroy(tmet);
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
	struct attr_value *av;
	struct ldmsd_tenant_def_s *tdef;
	struct ldmsd_tenant_metric_s *tmet;

	tdef = malloc(sizeof(*tdef));
	if (!tdef) {
		goto enomem;
	}

	ref_init(&tdef->ref, "creation", __tenant_def_destroy, tdef);

	tdef->name = strdup(name);
	if (!tdef->name) {
		ref_put(&tdef->ref, "creation");
		goto enomem;
	}
	TAILQ_INIT(&tdef->mlist);

	for (i = 0; i < av_list->count; i++) {
		av = &av_list[i];
		tmet = __process_tenant_attr(av);
		if (!tmet) {
			goto err;
		}

		rc = ldms_record_metric_add_template(tdef->rec_def, &tmet->mtempl, &tmet->__rent_id);
		if (rc) {
			ovis_log(config_log, OVIS_LERROR,
				"Cannot create tenant definition '%s' because " \
				"ldmsd failed to create the record definition " \
				"with error %d.\n", name, rc);
			goto err;
		}
	}

	ptherad_mutex_lock(&tenant_def_list_lock);
	LIST_INSERT_HEAD(&tenant_def_list, tdef, ent);
	ptherad_mutex_unlock(&tenant_def_list_lock);
	return tdef;

 enomem:
	ovis_log(config_log, OVIS_LCRIT, "Memory failure allocation\n");
	errno = ENOMEM;
	return NULL;
 err:
	ref_put(&tdef->ref, "creation");
	return NULL;
}

struct ldmsd_tenant_def_s *ldmsd_tenant_def_find(const char *name)
{
	struct ldmsd_tenant_def_s *tdef;

	pthread_mutex_lock(&tenant_def_list_lock);
	LIST_FOREACH(tdef, &tenant_def_list, ent) {
		if (0 == strcmp(tdef->name, name)) {
			pthread_mutex_unlock(&tenant_def_list_lock);
			return tdef;
		}
	}
	pthread_mutex_unlock(&tenant_def_list_lock);
	return 	NULL;
}
