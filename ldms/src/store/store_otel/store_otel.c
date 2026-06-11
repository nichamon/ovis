/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2026 Open Grid Computing, Inc. All rights reserved.
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
 *      Neither the name of Open Grid Computing nor the names of any
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
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

/*
 * store_otel - LDMSD storage plugin bridging LDMS decomposed rows to
 * OpenTelemetry via OTLP/HTTP+JSON.
 *
 * Architecture:
 *   ldmsd aggregator
 *     └── store_otel (this plugin)
 *           └── OTLP/HTTP+JSON POST to OTel Collector (localhost:4318)
 *                 └── backend (Prometheus, Grafana, etc.)
 *
 * Only implements commit() — requires a decomposition config on the strgp.
 * open()/close() are unused in the decomposed path.
 *
 * Per-strgp state is allocated lazily on the first commit() call and stored
 * in strgp->store_handle.
 *
 * Configuration:
 *   config name=<NAME> plugin=store_otel \
 *          [host=<host>]           # default: localhost
 *          [port=<port>]           # default: 4318
 *          [buffer_limit=<N>]      # data points before auto-flush; 0 = flush every commit (default)
 *          [timeout=<secs>]        # HTTP POST timeout in seconds; default: 5
 *
 * OTLP mapping:
 *   ResourceMetrics
 *     Resource.attributes:
 *       ldms.producer    <- ldms_set_producer_name_get(set)
 *     ScopeMetrics
 *       Scope.name = "store_otel"
 *       Metrics[]  -- one Gauge per numeric/scalar column in the row
 *         DataPoint.attributes:
 *           ldms.component_id  <- ldms_set_component_id_get(set)
 *           ldms.job_id        <- ldms_set_job_id_get(set)
 *           ldms.app_id        <- ldms_set_app_id_get(set)
 *           ldms.uid           <- ldms_set_uid_get(set)
 *           ldms.gid           <- ldms_set_gid_get(set)
 *         time_unix_nano <- ldms_set_timestamp_get(set)
 *         as_double      <- column value (scalar types only; arrays skipped)
 *
 * Dependencies: libcurl
 * Build: cc -shared -fPIC -o store_otel.so store_otel.c -lcurl
 */

#define _GNU_SOURCE
#include <sys/queue.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <curl/curl.h>

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"

/* --------------------------------------------------------------------------
 * Logging convenience
 * -------------------------------------------------------------------------- */
#define LOG_(inst, level, fmt, ...) \
	ovis_log((inst)->log, level, fmt, ##__VA_ARGS__)

/* --------------------------------------------------------------------------
 * Plugin-instance state (one per ldmsd plugin instance, lives in ctxt)
 * -------------------------------------------------------------------------- */
typedef struct store_otel_s {
	ovis_log_t   log;
	pthread_mutex_t cfg_lock;    /* protects config fields below */

	char  *host;                 /* OTel collector host */
	int    port;                 /* OTel collector port */
	int    buffer_limit;         /* data points before auto-flush; 0 = per-commit */
	long   timeout;              /* curl timeout in seconds */
} *store_otel_t;

/* --------------------------------------------------------------------------
 * Per-strgp state (lazily allocated on first commit, stored in
 * strgp->store_handle)
 * -------------------------------------------------------------------------- */

/* A single buffered data point */
struct otel_dp {
	char   *metric_name;         /* strdup of col->name */
	double  value;
	uint64_t time_unix_nano;

	/* attributes from set */
	char   *producer;
	uint64_t component_id;
	uint64_t job_id;
	uint64_t app_id;
	uint64_t uid;
	uint64_t gid;

	TAILQ_ENTRY(otel_dp) entry;
};

typedef struct otel_instance_s {
	store_otel_t       plugin;   /* back-pointer to plugin instance */
	pthread_mutex_t    lock;     /* protects buffer */

	TAILQ_HEAD(, otel_dp) buffer;
	int                buf_count; /* current number of buffered data points */

	/* assembled URL, e.g. http://localhost:4318/v1/metrics */
	char               url[256];
} *otel_instance_t;

/* --------------------------------------------------------------------------
 * JSON builder — simple dynamic string
 * -------------------------------------------------------------------------- */
struct strbuf {
	char  *data;
	size_t len;
	size_t cap;
};

static int strbuf_init(struct strbuf *b, size_t initial)
{
	b->data = malloc(initial);
	if (!b->data)
		return ENOMEM;
	b->data[0] = '\0';
	b->len = 0;
	b->cap = initial;
	return 0;
}

static void strbuf_free(struct strbuf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

static int strbuf_grow(struct strbuf *b, size_t need)
{
	if (b->len + need < b->cap)
		return 0;
	size_t newcap = b->cap * 2 + need + 256;
	char *p = realloc(b->data, newcap);
	if (!p)
		return ENOMEM;
	b->data = p;
	b->cap  = newcap;
	return 0;
}

static int strbuf_appendf(struct strbuf *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static int strbuf_appendf(struct strbuf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	/* try once with remaining space */
	va_start(ap, fmt);
	n = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
	va_end(ap);

	if (n < 0)
		return EINVAL;

	if ((size_t)n >= b->cap - b->len) {
		/* grow and retry */
		int rc = strbuf_grow(b, n + 1);
		if (rc)
			return rc;
		va_start(ap, fmt);
		n = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
		va_end(ap);
		if (n < 0)
			return EINVAL;
	}

	b->len += n;
	return 0;
}

/* Escape a string for JSON — only handles the common control characters */
static int strbuf_append_json_str(struct strbuf *b, const char *s)
{
	int rc;
	rc = strbuf_appendf(b, "\"");
	if (rc) return rc;
	for (; *s; s++) {
		switch (*s) {
		case '"':  rc = strbuf_appendf(b, "\\\""); break;
		case '\\': rc = strbuf_appendf(b, "\\\\"); break;
		case '\n': rc = strbuf_appendf(b, "\\n");  break;
		case '\r': rc = strbuf_appendf(b, "\\r");  break;
		case '\t': rc = strbuf_appendf(b, "\\t");  break;
		default:
			if ((unsigned char)*s < 0x20)
				rc = strbuf_appendf(b, "\\u%04x", (unsigned char)*s);
			else
				rc = strbuf_appendf(b, "%c", *s);
			break;
		}
		if (rc) return rc;
	}
	return strbuf_appendf(b, "\"");
}

/* --------------------------------------------------------------------------
 * OTLP/HTTP POST via libcurl
 * -------------------------------------------------------------------------- */

/* curl write callback — discards response body */
static size_t curl_discard(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	(void)ptr; (void)userdata;
	return size * nmemb;
}

static int http_post(otel_instance_t oi, const char *json_body, size_t body_len)
{
	store_otel_t plugin = oi->plugin;
	CURL *curl;
	CURLcode res;
	struct curl_slist *headers = NULL;
	int rc = 0;

	curl = curl_easy_init();
	if (!curl) {
		LOG_(plugin, OVIS_LERROR, "store_otel: curl_easy_init() failed\n");
		return ENOMEM;
	}

	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (!headers) {
		LOG_(plugin, OVIS_LERROR, "store_otel: curl_slist_append() failed\n");
		rc = ENOMEM;
		goto out;
	}

	curl_easy_setopt(curl, CURLOPT_URL, oi->url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discard);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, plugin->timeout);

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		LOG_(plugin, OVIS_LERROR,
		     "store_otel: POST to %s failed: %s\n",
		     oi->url, curl_easy_strerror(res));
		rc = EIO;
	} else {
		long http_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code < 200 || http_code >= 300) {
			LOG_(plugin, OVIS_LERROR,
			     "store_otel: POST to %s returned HTTP %ld\n",
			     oi->url, http_code);
			rc = EIO;
		}
	}

out:
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return rc;
}

/* --------------------------------------------------------------------------
 * Serialize buffered data points into OTLP JSON and POST
 *
 * OTLP/JSON metrics structure:
 * {
 *   "resourceMetrics": [
 *     {
 *       "resource": { "attributes": [ { "key": "ldms.producer", "value": { "stringValue": "..." } } ] },
 *       "scopeMetrics": [
 *         {
 *           "scope": { "name": "store_otel" },
 *           "metrics": [
 *             {
 *               "name": "<col_name>",
 *               "gauge": {
 *                 "dataPoints": [
 *                   {
 *                     "attributes": [ ... ],
 *                     "timeUnixNano": "<ns>",
 *                     "asDouble": <value>
 *                   }
 *                 ]
 *               }
 *             },
 *             ...
 *           ]
 *         }
 *       ]
 *     },
 *     ...
 *   ]
 * }
 *
 * We group data points by producer so each producer becomes one
 * resourceMetrics entry.  Within a producer all data points share the
 * same scope.  Each metric column becomes a separate "metrics" entry
 * containing all data points for that column name.
 *
 * For simplicity in this first implementation we emit one resourceMetrics
 * block per data point — correct, if slightly verbose.  A future pass can
 * group by producer.
 * -------------------------------------------------------------------------- */
static int flush_buffer_locked(otel_instance_t oi)
{
	struct otel_dp *dp;
	struct strbuf buf;
	int rc, first_rm, first_m;

	if (TAILQ_EMPTY(&oi->buffer))
		return 0;

	rc = strbuf_init(&buf, 4096);
	if (rc)
		return rc;

	rc = strbuf_appendf(&buf, "{\"resourceMetrics\":[");
	if (rc) goto out;

	first_rm = 1;
	TAILQ_FOREACH(dp, &oi->buffer, entry) {
		if (!first_rm) {
			rc = strbuf_appendf(&buf, ",");
			if (rc) goto out;
		}
		first_rm = 0;

		/* resourceMetrics entry */
		rc = strbuf_appendf(&buf,
			"{"
			  "\"resource\":{"
			    "\"attributes\":["
			      "{\"key\":\"ldms.producer\","
			       "\"value\":{\"stringValue\":");
		if (rc) goto out;
		rc = strbuf_append_json_str(&buf,
			dp->producer ? dp->producer : "");
		if (rc) goto out;
		rc = strbuf_appendf(&buf,
			      "}}"
			    "]"
			  "},"
			  "\"scopeMetrics\":["
			    "{"
			      "\"scope\":{\"name\":\"store_otel\"},"
			      "\"metrics\":["
			        "{"
			          "\"name\":");
		if (rc) goto out;
		rc = strbuf_append_json_str(&buf, dp->metric_name);
		if (rc) goto out;
		rc = strbuf_appendf(&buf,
			          ","
			          "\"gauge\":{"
			            "\"dataPoints\":["
			              "{"
			                "\"attributes\":["
			                  "{\"key\":\"ldms.component_id\","
			                   "\"value\":{\"intValue\":\"%"PRIu64"\"}},"
			                  "{\"key\":\"ldms.job_id\","
			                   "\"value\":{\"intValue\":\"%"PRIu64"\"}},"
			                  "{\"key\":\"ldms.app_id\","
			                   "\"value\":{\"intValue\":\"%"PRIu64"\"}},"
			                  "{\"key\":\"ldms.uid\","
			                   "\"value\":{\"intValue\":\"%"PRIu64"\"}},"
			                  "{\"key\":\"ldms.gid\","
			                   "\"value\":{\"intValue\":\"%"PRIu64"\"}}"
			                "],"
			                "\"timeUnixNano\":\"%"PRIu64"\","
			                "\"asDouble\":%.17g"
			              "}"
			            "]"
			          "}"
			        "}"
			      "]"
			    "}"
			  "]"
			"}",
			dp->component_id,
			dp->job_id,
			dp->app_id,
			dp->uid,
			dp->gid,
			dp->time_unix_nano,
			dp->value);
		if (rc) goto out;
	}

	rc = strbuf_appendf(&buf, "]}");
	if (rc) goto out;

	rc = http_post(oi, buf.data, buf.len);

out:
	strbuf_free(&buf);

	/* free all buffered data points regardless of POST result */
	while ((dp = TAILQ_FIRST(&oi->buffer)) != NULL) {
		TAILQ_REMOVE(&oi->buffer, dp, entry);
		free(dp->metric_name);
		free(dp->producer);
		free(dp);
	}
	oi->buf_count = 0;

	return rc;
}

/* --------------------------------------------------------------------------
 * Per-strgp instance init (lazy, called on first commit)
 * -------------------------------------------------------------------------- */
static otel_instance_t init_otel_instance(store_otel_t plugin,
					  ldmsd_strgp_t strgp)
{
	otel_instance_t oi = calloc(1, sizeof(*oi));
	if (!oi)
		return NULL;

	oi->plugin = plugin;
	pthread_mutex_init(&oi->lock, NULL);
	TAILQ_INIT(&oi->buffer);
	oi->buf_count = 0;

	pthread_mutex_lock(&plugin->cfg_lock);
	snprintf(oi->url, sizeof(oi->url),
		 "http://%s:%d/v1/metrics",
		 plugin->host ? plugin->host : "localhost",
		 plugin->port > 0 ? plugin->port : 4318);
	pthread_mutex_unlock(&plugin->cfg_lock);

	return oi;
}

static void free_otel_instance(otel_instance_t oi)
{
	struct otel_dp *dp;
	if (!oi)
		return;
	while ((dp = TAILQ_FIRST(&oi->buffer)) != NULL) {
		TAILQ_REMOVE(&oi->buffer, dp, entry);
		free(dp->metric_name);
		free(dp->producer);
		free(dp);
	}
	pthread_mutex_destroy(&oi->lock);
	free(oi);
}

/* --------------------------------------------------------------------------
 * Value extraction helpers
 * -------------------------------------------------------------------------- */
static int ldms_type_is_numeric_scalar(enum ldms_value_type t)
{
	switch (t) {
	case LDMS_V_U8:
	case LDMS_V_S8:
	case LDMS_V_U16:
	case LDMS_V_S16:
	case LDMS_V_U32:
	case LDMS_V_S32:
	case LDMS_V_U64:
	case LDMS_V_S64:
	case LDMS_V_F32:
	case LDMS_V_D64:
		return 1;
	default:
		return 0;
	}
}

static double ldms_mval_to_double(ldms_mval_t mv, enum ldms_value_type t)
{
	switch (t) {
	case LDMS_V_U8:  return (double)mv->v_u8;
	case LDMS_V_S8:  return (double)mv->v_s8;
	case LDMS_V_U16: return (double)mv->v_u16;
	case LDMS_V_S16: return (double)mv->v_s16;
	case LDMS_V_U32: return (double)mv->v_u32;
	case LDMS_V_S32: return (double)mv->v_s32;
	case LDMS_V_U64: return (double)mv->v_u64;
	case LDMS_V_S64: return (double)mv->v_s64;
	case LDMS_V_F32: return (double)mv->v_f;
	case LDMS_V_D64: return mv->v_d;
	default:         return 0.0;
	}
}

/* --------------------------------------------------------------------------
 * commit() — the main entry point
 * -------------------------------------------------------------------------- */
static int commit_rows(ldmsd_plug_handle_t handle, ldmsd_strgp_t strgp,
		       ldms_set_t set, ldmsd_row_list_t row_list, int row_count)
{
	store_otel_t plugin = ldmsd_plug_ctxt_get(handle);
	otel_instance_t oi;
	ldmsd_row_t row;
	ldmsd_col_t col;
	int i, rc = 0;

	/* Lazy init of per-strgp state */
	if (!strgp->store_handle) {
		oi = init_otel_instance(plugin, strgp);
		if (!oi) {
			LOG_(plugin, OVIS_LERROR,
			     "store_otel: failed to allocate instance for strgp '%s'\n",
			     strgp->obj.name);
			return ENOMEM;
		}
		strgp->store_handle = oi;
	}
	oi = strgp->store_handle;

	/* Extract set-level identity once per commit */
	const char *producer   = ldms_set_producer_name_get(set);
	uint64_t component_id  = ldms_set_component_id_get(set);
	uint64_t job_id        = ldms_set_job_id_get(set);
	uint64_t app_id        = ldms_set_app_id_get(set);
	uint64_t uid           = ldms_set_uid_get(set);
	uint64_t gid           = ldms_set_gid_get(set);

	/* Timestamp: microseconds -> nanoseconds */
	struct ldms_timestamp ts = ldms_set_timestamp_get(set);
	uint64_t time_unix_nano = (uint64_t)ts.sec * 1000000000ULL
				+ (uint64_t)ts.usec * 1000ULL;

	/* Get config snapshot */
	pthread_mutex_lock(&plugin->cfg_lock);
	int buffer_limit = plugin->buffer_limit;
	pthread_mutex_unlock(&plugin->cfg_lock);

	pthread_mutex_lock(&oi->lock);

	TAILQ_FOREACH(row, row_list, entry) {
		for (i = 0; i < row->col_count; i++) {
			col = &row->cols[i];

			/* Skip non-scalar or non-numeric columns (arrays,
			 * strings, timestamps, record types) */
			if (!ldms_type_is_numeric_scalar(col->type))
				continue;

			struct otel_dp *dp = calloc(1, sizeof(*dp));
			if (!dp) {
				LOG_(plugin, OVIS_LERROR,
				     "store_otel: OOM allocating data point\n");
				rc = ENOMEM;
				goto unlock;
			}

			dp->metric_name  = strdup(col->name);
			dp->producer     = producer ? strdup(producer) : NULL;
			dp->value        = ldms_mval_to_double(col->mval, col->type);
			dp->time_unix_nano = time_unix_nano;
			dp->component_id = component_id;
			dp->job_id       = job_id;
			dp->app_id       = app_id;
			dp->uid          = uid;
			dp->gid          = gid;

			if (!dp->metric_name) {
				free(dp->producer);
				free(dp);
				rc = ENOMEM;
				goto unlock;
			}

			TAILQ_INSERT_TAIL(&oi->buffer, dp, entry);
			oi->buf_count++;
		}
	}

	/* Flush if buffer_limit==0 (every commit) or threshold reached */
	if (buffer_limit == 0 || oi->buf_count >= buffer_limit)
		rc = flush_buffer_locked(oi);

unlock:
	pthread_mutex_unlock(&oi->lock);
	return rc;
}

/* --------------------------------------------------------------------------
 * flush() — drain whatever is buffered immediately
 * -------------------------------------------------------------------------- */
static int flush_store(ldmsd_plug_handle_t handle, ldmsd_store_handle_t sh)
{
	otel_instance_t oi = sh;
	int rc;

	if (!oi)
		return 0;

	pthread_mutex_lock(&oi->lock);
	rc = flush_buffer_locked(oi);
	pthread_mutex_unlock(&oi->lock);
	return rc;
}

/* --------------------------------------------------------------------------
 * config()
 * -------------------------------------------------------------------------- */
static int config(ldmsd_plug_handle_t handle,
		  struct attr_value_list *kwl,
		  struct attr_value_list *avl)
{
	store_otel_t plugin = ldmsd_plug_ctxt_get(handle);
	char *value;

	pthread_mutex_lock(&plugin->cfg_lock);

	value = av_value(avl, "host");
	if (value) {
		free(plugin->host);
		plugin->host = strdup(value);
		if (!plugin->host) {
			pthread_mutex_unlock(&plugin->cfg_lock);
			return ENOMEM;
		}
	}

	value = av_value(avl, "port");
	if (value)
		plugin->port = (int)strtol(value, NULL, 10);

	value = av_value(avl, "buffer_limit");
	if (value)
		plugin->buffer_limit = (int)strtol(value, NULL, 10);

	value = av_value(avl, "timeout");
	if (value)
		plugin->timeout = strtol(value, NULL, 10);

	LOG_(plugin, OVIS_LINFO,
	     "store_otel: configured host=%s port=%d buffer_limit=%d timeout=%lds\n",
	     plugin->host ? plugin->host : "localhost",
	     plugin->port > 0 ? plugin->port : 4318,
	     plugin->buffer_limit,
	     plugin->timeout);

	pthread_mutex_unlock(&plugin->cfg_lock);
	return 0;
}

/* --------------------------------------------------------------------------
 * usage()
 * -------------------------------------------------------------------------- */
static const char *usage(ldmsd_plug_handle_t handle)
{
	return
	"    config name=<NAME> plugin=store_otel\n"
	"           [host=<host>]          OTel collector host (default: localhost)\n"
	"           [port=<port>]          OTel collector OTLP/HTTP port (default: 4318)\n"
	"           [buffer_limit=<N>]     Flush after N data points; 0 = every commit (default: 0)\n"
	"           [timeout=<secs>]       HTTP POST timeout in seconds (default: 5)\n"
	"\n"
	"    Requires a decomposition (decomp=) on the strgp. open()/close() are unused.\n"
	"    Posts OTLP/JSON to http://<host>:<port>/v1/metrics.\n";
}

/* --------------------------------------------------------------------------
 * constructor / destructor
 * -------------------------------------------------------------------------- */
static int constructor(ldmsd_plug_handle_t handle)
{
	store_otel_t plugin = calloc(1, sizeof(*plugin));
	if (!plugin)
		return ENOMEM;

	pthread_mutex_init(&plugin->cfg_lock, NULL);
	plugin->log          = ldmsd_plug_log_get(handle);
	plugin->host         = strdup("localhost");
	plugin->port         = 4318;
	plugin->buffer_limit = 0;    /* flush every commit by default */
	plugin->timeout      = 5;

	if (!plugin->host) {
		free(plugin);
		return ENOMEM;
	}

	ldmsd_plug_ctxt_set(handle, plugin);
	return 0;
}

static void destructor(ldmsd_plug_handle_t handle)
{
	store_otel_t plugin = ldmsd_plug_ctxt_get(handle);
	if (!plugin)
		return;
	free(plugin->host);
	pthread_mutex_destroy(&plugin->cfg_lock);
	free(plugin);
}

/* --------------------------------------------------------------------------
 * close_store — called when strgp stops; free per-strgp state
 * -------------------------------------------------------------------------- */
static void close_store(ldmsd_plug_handle_t handle, ldmsd_store_handle_t sh)
{
	otel_instance_t oi = sh;
	if (!oi)
		return;
	/* best-effort flush before teardown */
	pthread_mutex_lock(&oi->lock);
	flush_buffer_locked(oi);
	pthread_mutex_unlock(&oi->lock);
	free_otel_instance(oi);
}

/* --------------------------------------------------------------------------
 * Plugin interface
 * -------------------------------------------------------------------------- */
struct ldmsd_store ldmsd_plugin_interface = {
	.base.type        = LDMSD_PLUGIN_STORE,
	.base.flags       = LDMSD_PLUGIN_MULTI_INSTANCE,
	.base.config      = config,
	.base.usage       = usage,
	.base.constructor = constructor,
	.base.destructor  = destructor,

	/* open/store are unused — this plugin requires decomposition */
	.open    = NULL,
	.store   = NULL,
	.flush   = flush_store,
	.close   = close_store,
	.commit  = commit_rows,
};

/* --------------------------------------------------------------------------
 * Module init/fini — initialize libcurl global state
 * -------------------------------------------------------------------------- */
static void __attribute__((constructor)) store_otel_init(void);
static void store_otel_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

static void __attribute__((destructor)) store_otel_fini(void);
static void store_otel_fini(void)
{
	curl_global_cleanup();
}