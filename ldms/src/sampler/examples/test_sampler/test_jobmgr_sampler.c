/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2025 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2025,2025 Open Grid Computing, Inc. All rights reserved.
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
#define _GNU_SOURCE

#include "ovis_ref/ref.h"

#include "ldmsd.h"
#include "ldmsd_jobmgr.h"
#include "ldmsd_jobmgr_query.h"
#include "ldmsd_plug_api.h"

typedef struct test_jobmgr_samp_s {
	struct ref_s ref;
	FILE *f;
	ldmsd_jobmgr_query_t q;
	int subscribe;
	size_t heap_sz;
} *test_jobmgr_samp_t;

const char *_usage = "\
This is a test sampler for jobmgr. It subscribes for jobmgr events and print\n\
them to the given file.\n\
\n\
config name=INST file=PATH\n\
\n\
";

static const char *usage(ldmsd_plug_handle_t handle)
{
	return "config name=INST file=PATH";
}

int qrec_print(ldmsd_jobmgr_query_event_t qev, char *buf, size_t bufsz)
{
	off_t off = 0;
	int i, rc;
	size_t len;
	ldms_mval_t mv, qrec;
	const char *sep = "";
	int n_metrics;
	enum ldms_value_type mv_type;
	const char *mv_name;

	qrec = qev->start_end.qrec;
	n_metrics = ldms_record_card(qev->start_end.qrec);
	rc = 0;
	i = 0;
	off += snprintf(buf+off, bufsz-off, "{");
 loop:
	if (i >= n_metrics)
		goto done;
	mv_name = ldms_record_metric_name_get(qrec, i);
	mv_type = ldms_record_metric_type_get(qrec, i, &len);
	mv = ldms_record_metric_get(qrec, i);
	switch (mv_type) {
	case LDMS_V_S8:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%hhd", sep, mv_name,mv->v_s8);
		break;
	case LDMS_V_U8:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%hhu", sep, mv_name,mv->v_u8);
		break;
	case LDMS_V_S16:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%hd", sep, mv_name,mv->v_s16);
		break;
	case LDMS_V_U16:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%hu", sep, mv_name,mv->v_u16);
		break;
	case LDMS_V_S32:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%d", sep, mv_name,mv->v_s32);
		break;
	case LDMS_V_U32:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%u", sep, mv_name,mv->v_u32);
		break;
	case LDMS_V_S64:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%ld", sep, mv_name,mv->v_s64);
		break;
	case LDMS_V_U64:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%lu", sep, mv_name,mv->v_u64);
		break;
	case LDMS_V_F32:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%f", sep, mv_name,mv->v_f);
		break;
	case LDMS_V_D64:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%lf", sep, mv_name,mv->v_d);
		break;
	case LDMS_V_CHAR_ARRAY:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":\"%s\"", sep, mv_name,mv->a_char);
		break;
	case LDMS_V_TIMESTAMP:
		off += snprintf(buf+off, bufsz-off, "%s\"%s\":%d.%06d", sep, mv_name,mv->v_ts.sec, mv->v_ts.usec);
		break;
	default:
		goto next;
	}
	sep = ",";
	if (off >= bufsz) {
		rc = ENOBUFS;
		goto out;
	}
 next:
	i += 1;
	goto loop;
 done:
	off += snprintf(buf+off, bufsz-off, "}");
	if (off >= bufsz)
		rc = ENOBUFS;
 out:
	return rc;
}

int jobmgr_cb(ldmsd_jobmgr_query_t q, ldmsd_jobmgr_query_event_t qev, void *cb_arg)
{
	test_jobmgr_samp_t j = cb_arg;
	char buf[4096];

	switch (qev->event_type) {
	case LDMSD_JOBMGR_QUERY_EVENT_NO_SPACE:
		assert(0);
		fprintf(j->f, "no_space\n");
		return 0;
	case LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE:
		fprintf(j->f, "client_close\n");
		return 0;
	default:
		break;
	}

	qrec_print(qev, buf, sizeof(buf));

	switch (qev->event_type) {
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_START:
		fprintf(j->f, "job_start: %s\n", buf);
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_START:
		fprintf(j->f, "task_start: %s\n", buf);
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_TASK_END:
		fprintf(j->f, "task_end: %s\n", buf);
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_JOB_END:
		fprintf(j->f, "job_end: %s\n", buf);
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_CLIENT_CLOSE:
		fprintf(j->f, "client_close\n");
		ref_put(&j->ref, "jobmgr_cb");
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_START:
		fprintf(j->f, "step_start: %s\n", buf);
		break;
	case LDMSD_JOBMGR_QUERY_EVENT_STEP_END:
		fprintf(j->f, "step_end: %s\n", buf);
		break;
	default:
		break;
	}

	return 0;
}

#define LOG_ERROR(handle, fmt, ...)  \
		ovis_log(ldmsd_plug_log_get(handle), \
			 OVIS_LERROR, fmt, ## __VA_ARGS__ )

static int config(ldmsd_plug_handle_t handle,
		  struct attr_value_list *kwl, struct attr_value_list *avl)
{
	char *val;
	char *err_str;
	char *comp_id;
	char *inst;
	char *t, *save;
	ldms_schema_t sch;
	test_jobmgr_samp_t j = ldmsd_plug_ctxt_get(handle);
	int recdef_midx;
	int list_midx;
	ldms_set_t set;
	ldms_mval_t mv;
	struct ldmsd_str_list slist, *metrics;
	struct ldmsd_str_ent *str_ent;

	if (j->q)
		return EEXIST;

	val = av_value(avl, "metrics");
	if (!val) {
		metrics = NULL;
	} else {
		TAILQ_INIT(&slist);
		metrics = &slist;
		t = strtok_r(val, ",", &save);
		while (t) {
			str_ent = malloc (sizeof(*str_ent));
			assert(str_ent);
			str_ent->str = t;
			TAILQ_INSERT_TAIL(metrics, str_ent, entry);
			t = strtok_r(NULL, ",", &save);
		}
	}

	j->q = ldmsd_jobmgr_query_new(metrics, jobmgr_cb, j, &err_str);
	assert(j->q);

	val = av_value(avl, "heap_sz");
	if (!val) {
		val = "65536";
	}
	j->heap_sz = strtol(val, NULL, 0);

	val = av_value(avl, "file");
	if (!val) {
		LOG_ERROR(handle, "'file' attribute is required.\n");
		return EINVAL;
	}
	j->f = fopen(val, "w");
	if (!j->f) {
		LOG_ERROR(handle, "cannot open file: %s\n", val);
		goto err;
	}
	setbuf(j->f, NULL);

	sch = ldms_schema_new("test_jobmgr");
	assert(sch);
	ldms_schema_metric_add(sch, "component_id", LDMS_V_U64);
	recdef_midx = ldms_schema_record_add(sch, j->q->recdef);
	assert(recdef_midx >= 0);
	list_midx = ldms_schema_metric_list_add(sch, "job_rec", NULL, j->heap_sz);
	assert(list_midx >= 0);

	inst = av_value(avl, "instance");
	if (!inst) {
		val = "test_jobmgr";
	}

	comp_id = av_value(avl, "component_id");
	if (!comp_id) {
		comp_id = "0";
	}

	set = ldms_set_new_with_auth(inst, sch, getuid(), getgid(), 0440);
	assert(set);

	mv = ldms_metric_get(set, 0); /* component_id */
	mv->v_u64 = strtoul(comp_id, NULL, 0);

	ldmsd_jobmgr_query_execute(j->q, set, recdef_midx, list_midx);

	ldms_set_publish(set);

	return 0;

 err:
	free(val);
	if (j->f) {
		fclose(j->f);
		j->f = NULL;
	}
	return errno;
}

void __del(void *arg)
{
	test_jobmgr_samp_t j = arg;
	if (j->f)
		fclose(j->f);
	assert(j->q == NULL);
	free(j);
}

static int constructor(ldmsd_plug_handle_t handle)
{
	test_jobmgr_samp_t j = calloc(1, sizeof(*j));
	if (!j)
		return ENOMEM;
	ref_init(&j->ref, "constructor", __del, j);
	ldmsd_plug_ctxt_set(handle, j);
	return 0;
}

static void destructor(ldmsd_plug_handle_t handle)
{
	test_jobmgr_samp_t j = ldmsd_plug_ctxt_get(handle);
	if (!j)
		return;
	ref_put(&j->ref, "constructor");
}

static int sample(ldmsd_plug_handle_t handle)
{
	/* no op */
	return 0;
}

struct ldmsd_sampler ldmsd_plugin_interface = {
	.base = {
		.type = LDMSD_PLUGIN_SAMPLER,
		.flags = LDMSD_PLUGIN_MULTI_INSTANCE,
		.config = config,
		.usage = usage,
		.constructor = constructor,
		.destructor = destructor,
	},
	.sample = sample,
};
