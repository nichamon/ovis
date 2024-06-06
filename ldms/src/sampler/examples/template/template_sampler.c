/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2024 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2024 Open Grid Computing, Inc. All rights reserved.
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
/**
 * \file meminfo.c
 * \brief /proc/meminfo data provider
 */
#define _GNU_SOURCE

#include <inttypes.h>
#include <unistd.h>
#include <sys/errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>

/* Must include these three headers */
#include "ldms.h"
#include "ldmsd.h"
#include "sampler_base.h"
/* -------------------------------- */


static ldms_set_t set = NULL;
static ldmsd_msg_log_f msglog;
static int metric_offset;
static base_data_t base;

#define SAMP "example"
#define MAX_LIST_LEN 10
#define ARRAY_LEN    5

static struct ldms_metric_template_s my_record_template[] = {
	{       "rec_ent_u64",    0, LDMS_V_U64,       "unit", 1},
	{ "rec_ent_array_u64",    0, LDMS_V_U64_ARRAY, "unit", 3},
	{0}
	/*
	 * Only scalars and arrays can be record entries.
	 */
};
static int record_ent_ids;

static struct ldms_metric_template_s my_metric_template[] = {
	{            "u64",       0, LDMS_V_U64,        "unit",    1 }, /* 1 because this is a scalar */
	{      "array_u64",       0, LDMS_V_U64_ARRAY,  "unit",    ARRAY_LEN }, /* 5 is the array length */
	{     "list_o_u64",       0, LDMS_V_LIST,           "",    /* set heap_sz later */ },
	{         "record",       0, LDMS_V_RECORD_TYPE,    "",    /* set rec_def later */ },
	{ "list_o_records",       0, LDMS_V_LIST,           "",    /* set heap_sz later */ },
	{0},
};
static int my_metric_ids[5]; /* 5 is the number of metrics in my_metric_template */

static int create_metric_set(base_data_t base)
{
	ldms_schema_t schema;
	int rc, i;
	uint64_t metric_value;
	char *s;
	int rc;

	schema = base_schema_new(base); /* Create schema and populate common metrics */
	if (!schema) {
		rc = errno;
		msglog(LDMSD_LERROR,
		       SAMP ": The schema '%s' could not be created, errno=%d.\n",
		       base->schema_name, rc);
		goto err;
	}

	/* ------- Fill the missing fields in my_metric_template ---------------*/

	/* Calculate the list_o_u64 heap size */
	size_t list_o_u64_heap_sz = ldms_list_heap_size_get(LDMS_V_U64, MAX_LIST_LEN, 1);

	/* Create a record definition */
	my_metric_template[3].rec_def = ldms_record_from_template(my_metric_template[3].name,
								my_record_template,
								record_ent_ids);

	/* Calculate list_o_records heap size */
	my_metric_template[4].len = MAX_LIST_LEN * ldms_record_heap_size_get(rec_def);

	/* The template is completely filled and ready. */
	/*---------------------------------------------------------------------*/

	rc = ldms_scheam_metric_add_template(schema, my_metric_template, my_metric_ids);
	if (rc) {
		msglog(LDMSD_LERROR,
		       SAMP ": Error %d: Failed to add scalars and arrays.\n", rc);
		goto err;
	}


	set = base_set_new(base); /* Create an LDMS set */
	if (!set) {
		rc = errno;
		goto err;
	}

	return 0;

 err:
	return rc;
}

static const char *usage(struct ldmsd_plugin *self)
{
	/*
	 * TODO: Extend the string to explain the plugin-specific attributes
	 *
	 * See procnetdev2 as an example
	 */
	return  "config name=" SAMP " " BASE_CONFIG_USAGE;
}

static int config(struct ldmsd_plugin *self, struct attr_value_list *kwl, struct attr_value_list *avl)
{
	int rc;
	char *value;

	if (set) {
		msglog(LDMSD_LERROR, SAMP ": Set already created.\n");
		return EINVAL;
	}

	base = base_config(avl, SAMP, SAMP, msglog); /* sampler_base object created */
	if (!base) {
		rc = errno;
		goto err;
	}

	/*
	 * TODO: process plugin-specific attributes if any
	 *
	 * For example, char *attr_value = av_value(avl, "attr_name")
	 */

	rc = create_metric_set(base);
	if (rc) {
		msglog(LDMSD_LERROR, SAMP ": failed to create a metric set.\n");
		goto err;
	}
	return 0;
 err:
	base_del(base);
	return rc;
}

static int sample(struct ldmsd_sampler *self)
{
	static uint64_t mvalue = 1;
	int rc;
	int i;

	base_sample_begin(base); /* Map current job information */

	/* Set the value of metric u64 */
	ldms_metric_set_u64(base->set, my_metric_ids[i], mvalue);

	/* Set the values of array */
	for (i = 0; i < ARRAY_LEN; i++) {
		ldms_metric_array_set_u64(base->set, my_metric_ids[1], i, mvalue);
	}

	ldms_list_purge(base->set, ldms_metric_get(base->set, my_metric_ids[2]));
	...



	base_sample_end(base);  /* stamp a timestamp */

	mvalue++;
	return 0;
}

static void term(struct ldmsd_plugin *self)
{
	/*
	 * The plugin will be unloaded after this call.
	 *
	 * TODO: Clean up any resources the plugin has allocated.
	 *  - Free the memory
	 *  - Close the file descriptors
	 *  - etc
	 */
	if (base)
		base_del(base);
	if (set)
		ldms_set_delete(set);
	set = NULL;
}

static ldms_set_t get_set(struct ldmsd_sampler *self)
{
	return set;
}

static struct ldmsd_sampler example_plugin = { /* TODO: Rename the variable name */
	.base = {
		.name = SAMP, /* Sampler plugin name */
		.type = LDMSD_PLUGIN_SAMPLER, /* Always LDMSD_PLUGIN_SAMPLER */
		.usage = usage, /* Return a brief description and attributes to be set */
		.config = config, /* Receive configuration attributes, define the set schema, and create a set */
		.term = term, /* Called before unload the plugin: close any opened files and free memory */
	},
	.get_set = get_set, /* Obsolete -- there is no caller. */
	.sample = sample, /* Sample the metric values from the source */
};

struct ldmsd_plugin *get_plugin(ldmsd_msg_log_f log_fn)
{
	msglog = log_fn;
	set = NULL;
	return &example_plugin.base; /* TODO: Change the variable name */
}
