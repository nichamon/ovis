/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2023 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2023 Open Grid Computing, Inc. All rights reserved.
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
#include <assert.h>
#include <malloc.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <amqp_tcp_socket.h>
#include <amqp_ssl_socket.h>
#include <amqp_framing.h>

#include "coll/rbt.h"
#include "ovis_log/ovis_log.h"
#include "ovis_auth/auth.h"
#include "ldms.h"
#include "ldmsd.h"

#define STORE_NAME "store_amqp"
#define DEF_AMQP_DEFAULT_CONT "_default_"
#define DEF_VALUE_BUF_LEN 1024
#define DEF_MSG_BUF_LEN (128 * 1024)	/* Should avoid realloc() for most sets */

static struct rbt s_amqp_cont_rbt; /* The container tree. */
struct store_amqp_instance;
LIST_HEAD(s_amqp_inst_list, store_amqp_instance);
static struct s_amqp_inst_list inst_list;
static ovis_log_t amqp_log;
static pthread_mutex_t cfg_lock;

#define LOG_ERROR(_fmt_, ...) do { \
	ovis_log(amqp_log, OVIS_LERROR, _fmt_, ## __VA_ARGS__); \
} while (0);

#define LOG_INFO(_fmt_, ...) do { \
	ovis_log(amqp_log, OVIS_LINFO, _fmt_, ## __VA_ARGS__); \
} while (0);

#define LOG_OOM() do { \
	ovis_log(amqp_log, OVIS_LCRITICAL, "Memory allocation failure.\n"); \
} while (0);

#define _STR(_x) #_x
#define STR(_x) _STR(_x)

enum store_amqp_formatter_type {
	S_AMQP_BIN_FMT,
	S_AMQP_JSON_FMT,
	S_AMQP_CSV_FMT,
};

const char *format_str[] = {
	[S_AMQP_BIN_FMT] = "binary",
	[S_AMQP_JSON_FMT] = "json",
	[S_AMQP_CSV_FMT] = "csv",
};

typedef struct store_amqp_instance *store_amqp_inst_t;
typedef int (*store_amqp_formatter_t)(store_amqp_inst_t inst, ldms_set_t set,
				      int *metric_array, size_t size_count);

typedef struct store_amqp_container {
	store_amqp_formatter_t formatter;
	enum store_amqp_formatter_type format;
	char *container;
	char *exchange;
	int declared;
	char *vhost;
	char *host;
	unsigned short port;
	char *user;
	char *pwd;
	char *routing_key;
	amqp_socket_t *socket;
	amqp_connection_state_t conn;
	int channel;
	char *ca_pem; /* CA .PEM */
	char *key;    /* key .PEM */
	char *cert;   /* cert .PEM */
	struct rbn rbn;
	pthread_mutex_t lock;
} *store_amqp_container_t;

typedef struct store_amqp_instance {
	struct ldmsd_store *store;
	void *ucontext;
	store_amqp_container_t container;
	char *value_buf;
	size_t value_buf_len;
	char *msg_buf;
	size_t msg_buf_len;
	LIST_ENTRY(store_amqp_instance) entry;
	pthread_mutex_t lock;
} *store_amqp_inst_t;

static size_t realloc_msg_buf(store_amqp_inst_t inst, size_t buf_len)
{
	if (inst->msg_buf)
		free(inst->msg_buf);
	inst->msg_buf_len = buf_len;
	inst->msg_buf = malloc(inst->msg_buf_len);
	if (inst->msg_buf)
		return inst->msg_buf_len;
	else
		LOG_OOM();
	return 0;
}

size_t u8_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "%hhd", val->a_u8[i]);
}
size_t s8_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "0x%02hhx", val->a_s8[i]);
}
size_t u16_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "%hd", val->a_u16[i]);
}
size_t s16_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "%hd", val->a_s16[i]);
}
size_t u32_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "%u", val->a_u32[i]);
}
size_t s32_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "%d", val->a_s32[i]);
}
size_t u64_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "%"PRIu64, val->a_u64[i]);
}
size_t s64_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "%"PRId64, val->a_s64[i]);
}
size_t f32_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "%f", val->a_f[i]);
}
size_t d64_printer(char *buf, size_t rem, ldms_mval_t val, int i)
{
	return snprintf(buf, rem, "%lf", val->a_d[i]);
}


typedef size_t (*value_printer)(char *buf, size_t rem, ldms_mval_t val, int i);
static value_printer printers[] = {
	[LDMS_V_U8_ARRAY] = u8_printer,
	[LDMS_V_S8_ARRAY] = s8_printer,
	[LDMS_V_U16_ARRAY] = u16_printer,
	[LDMS_V_S16_ARRAY] = s16_printer,
	[LDMS_V_U32_ARRAY] = u32_printer,
	[LDMS_V_S32_ARRAY] = s32_printer,
	[LDMS_V_U64_ARRAY] = u64_printer,
	[LDMS_V_S64_ARRAY] = s64_printer,
	[LDMS_V_F32_ARRAY] = f32_printer,
	[LDMS_V_D64_ARRAY] = d64_printer,
};

char *json_value_printer(store_amqp_inst_t ai, ldms_set_t s, int idx)
{
	enum ldms_value_type type;
	ldms_mval_t val;
	int n, i;
	size_t cnt, remaining;

	if (!ai->value_buf) {
		ai->value_buf = malloc(DEF_VALUE_BUF_LEN);
		if (!ai->value_buf)
			return NULL;
		ai->value_buf_len = DEF_VALUE_BUF_LEN;
	}
 retry:
	type = ldms_metric_type_get(s, idx);
	n = ldms_metric_array_get_len(s, idx);
	val = ldms_metric_get(s, idx);
	remaining = ai->value_buf_len;

	switch (type) {
	case LDMS_V_CHAR_ARRAY:
		sprintf(ai->value_buf, "\"%s\"", val->a_char);
		break;
	case LDMS_V_CHAR:
		sprintf(ai->value_buf, "'%c'", val->v_char);
		break;
	case LDMS_V_U8:
		sprintf(ai->value_buf, "%hhu", val->v_u8);
		break;
	case LDMS_V_S8:
		sprintf(ai->value_buf, "%hhd", val->v_s8);
		break;
	case LDMS_V_U16:
		sprintf(ai->value_buf, "%hu", val->v_u16);
		break;
	case LDMS_V_S16:
		sprintf(ai->value_buf, "%hd", val->v_s16);
		break;
	case LDMS_V_U32:
		sprintf(ai->value_buf, "%u", val->v_u32);
		break;
	case LDMS_V_S32:
		sprintf(ai->value_buf, "%d", val->v_s32);
		break;
	case LDMS_V_U64:
		sprintf(ai->value_buf, "%"PRIu64, val->v_u64);
		break;
	case LDMS_V_S64:
		sprintf(ai->value_buf, "%"PRId64, val->v_s64);
		break;
	case LDMS_V_F32:
		sprintf(ai->value_buf, "%f", val->v_f);
		break;
	case LDMS_V_D64:
		sprintf(ai->value_buf, "%lf", val->v_d);
		break;
	default: /* arrays */
		remaining = ai->value_buf_len;
		cnt = snprintf(ai->value_buf, remaining, "[");
		remaining -= cnt;
		for (i = 0; i < n; i++) {
			if (i) {
				cnt = snprintf(&ai->value_buf[ai->value_buf_len - remaining], remaining, ",");
				remaining -= cnt;
			}
			assert(printers[type]);
			cnt = printers[type](&ai->value_buf[ai->value_buf_len - remaining], remaining, val, i);
			remaining -= cnt;
			if (remaining <= 0)
				break;
		}
		cnt = snprintf(&ai->value_buf[ai->value_buf_len - remaining], remaining, "]");
		remaining -= cnt;
		break;
	}
	if (remaining <= 0) {
		free(ai->value_buf);
		ai->value_buf = malloc(ai->value_buf_len * 2);
		ai->value_buf_len *= 2;
		if (ai->value_buf)
			goto retry;
	}
	if (ai->value_buf)
		return ai->value_buf;
	return "None";
}

static int
json_msg_formatter(store_amqp_inst_t inst, ldms_set_t s,
		   int *metric_array, size_t metric_count)
{
	struct ldms_timestamp timestamp;
	size_t cnt, remaining;
	char *msg;
	double t;
	int i;

	if (!inst->msg_buf) {
		if (!realloc_msg_buf(inst, DEF_MSG_BUF_LEN))
			return ENOMEM;
	}
	timestamp = ldms_transaction_timestamp_get(s);
 restart:
	remaining = inst->msg_buf_len;
	msg = inst->msg_buf;
	t = (double)timestamp.sec + (((double)timestamp.usec) / 1.0e6);
	cnt = snprintf(msg, remaining,
		       "{ instance_name : \"%s\", schema_name : \"%s\", "
		       "timestamp : %.6f, metrics : {\n",
		       ldms_set_instance_name_get(s),
		       ldms_set_schema_name_get(s),
		       t);
	remaining -= cnt;
	if (remaining <= 0)
		goto realloc;
	msg += cnt;
	for (i = 0; i < metric_count; i++) {
		cnt = snprintf(msg, remaining, "    \"%s\" : %s,\n",
			       ldms_metric_name_get(s, i),
			       json_value_printer(inst, s, metric_array[i]));
		remaining -= cnt;
		if (remaining <= 0)
			goto realloc;
		msg += cnt;
	}
	cnt = snprintf(msg, remaining, "    }\n}");
	remaining -= cnt;
	if (remaining <= 0)
		goto realloc;
	return inst->msg_buf_len - remaining;
 realloc:
	cnt = realloc_msg_buf(inst, inst->msg_buf_len << 1);
	if (cnt)
		goto restart;

	return 0;
}

static int
csv_msg_formatter(store_amqp_inst_t inst, ldms_set_t s,
		  int *metric_array, size_t metric_count)
{
	struct ldms_timestamp timestamp;
	size_t cnt, remaining;
	char *msg;
	double t;
	int i;

	if (!inst->msg_buf) {
		if (!realloc_msg_buf(inst, DEF_MSG_BUF_LEN))
			return ENOMEM;
	}
	timestamp = ldms_transaction_timestamp_get(s);
 restart:
	remaining = inst->msg_buf_len;
	msg = inst->msg_buf;
	t = (double)timestamp.sec + (((double)timestamp.usec) / 1.0e6);
	cnt = snprintf(msg, remaining,
		       "\"%s\",\"%s\",%.6f",
		       ldms_set_instance_name_get(s),
		       ldms_set_schema_name_get(s),
		       t);
	remaining -= cnt;
	if (remaining <= 0)
		goto realloc;
	msg += cnt;
	for (i = 0; i < metric_count; i++) {
		cnt = snprintf(msg, remaining, ",%s",
			       json_value_printer(inst, s, metric_array[i]));
		remaining -= cnt;
		if (remaining <= 0)
			goto realloc;
		msg += cnt;
	}
	return inst->msg_buf_len - remaining;
 realloc:
	cnt = realloc_msg_buf(inst, inst->msg_buf_len << 1);
	if (cnt)
		goto restart;

	return 0;
}

/*
 * The binary format is <1B:TYPE><1B:COUNT><xB:VALUE>
 *
 * E.g.
 *   01 01 63               Is the character 'c'
 *   01 05 68 65 6c 6c 6f   Is the character string "hello"
 */
static int
bin_msg_formatter(store_amqp_inst_t inst, ldms_set_t s,
		  int *metric_array, size_t metric_count)
{
	struct ldms_timestamp timestamp;
	size_t bytes = ldms_set_data_sz_get(s) + sizeof(double);
	int i;
	double t;
	size_t remaining, cnt;

	if (!inst->msg_buf) {
		if (!realloc_msg_buf(inst, bytes))
			return ENOMEM;
	}
	timestamp = ldms_transaction_timestamp_get(s);
 restart:
	remaining = inst->msg_buf_len;
	t = (double)timestamp.sec + (((double)timestamp.usec) / 1.0e6);
	memcpy(inst->msg_buf, &t, sizeof(double));
	remaining -= sizeof(double);
	if (remaining <= 0)
		goto realloc;

	for (i = 0; i < metric_count; i++) {
		ldms_mval_t val = ldms_metric_get(s, metric_array[i]);
		char *msg = &inst->msg_buf[inst->msg_buf_len - remaining];
		enum ldms_value_type type = ldms_metric_type_get(s, i);
		cnt = 2;
		remaining -= cnt;
		if (remaining <= 0)
			goto realloc;
		msg[0] = type;
		switch (type) {
		case LDMS_V_CHAR:
		case LDMS_V_U8:
		case LDMS_V_S8:
			cnt = sizeof(uint8_t);
			msg[1] = 1;
			memcpy(&msg[2], &val->v_u8, cnt);
			break;
		case LDMS_V_U16:
		case LDMS_V_S16:
			cnt = sizeof(uint16_t);
			msg[1] = 1;
			memcpy(&msg[2], &val->v_u16, cnt);
			break;
		case LDMS_V_U32:
		case LDMS_V_S32:
		case LDMS_V_F32:
			cnt = sizeof(uint32_t);
			msg[1] = 1;
			memcpy(&msg[2], &val->v_u32, cnt);
			break;
		case LDMS_V_U64:
		case LDMS_V_S64:
		case LDMS_V_D64:
			cnt = sizeof(uint64_t);
			msg[1] = 1;
			memcpy(&msg[2], &val->v_u64, cnt);
			break;
		case LDMS_V_CHAR_ARRAY:
		case LDMS_V_U8_ARRAY:
		case LDMS_V_S8_ARRAY:
			cnt = ldms_metric_array_get_len(s, i);
			msg[1] = cnt;
			memcpy(&msg[2], val->a_char, cnt);
			break;
		case LDMS_V_U16_ARRAY:
		case LDMS_V_S16_ARRAY:
			cnt = ldms_metric_array_get_len(s, i);
			msg[1] = cnt;
			cnt *= sizeof(uint16_t);
			memcpy(&msg[2], val->a_u16, cnt);
			break;
		case LDMS_V_U32_ARRAY:
		case LDMS_V_S32_ARRAY:
		case LDMS_V_F32_ARRAY:
			cnt = ldms_metric_array_get_len(s, i);
			msg[1] = cnt;
			cnt *= sizeof(uint32_t);
			memcpy(&msg[2], val->a_u32, cnt);
			break;
		case LDMS_V_U64_ARRAY:
		case LDMS_V_S64_ARRAY:
		case LDMS_V_D64_ARRAY:
			cnt = ldms_metric_array_get_len(s, i);
			msg[1] = cnt;
			cnt *= sizeof(uint64_t);
			memcpy(&msg[2], val->a_u64, cnt);
			break;
		default:
			assert(0 == "Invalid metric type");
		}
		remaining -= cnt;
		if (remaining <= 0)
			goto realloc;
	}
	return inst->msg_buf_len - remaining;
 realloc:
	cnt = realloc_msg_buf(inst, inst->msg_buf_len << 1);
	if (cnt)
		goto restart;

	return 0;
}

static store_amqp_formatter_t formatters[] = {
	[S_AMQP_JSON_FMT] = json_msg_formatter,
	[S_AMQP_CSV_FMT] = csv_msg_formatter,
	[S_AMQP_BIN_FMT] = bin_msg_formatter,
};

static int comparator(void *a, const void *b)
{
	return strcmp((char *)a, (char *)b);
}

static void __store_amqp_cont_free(store_amqp_container_t cont)
{
	free(cont->vhost);
	free(cont->host);
	free(cont->user);
	free(cont->pwd);
	free(cont->routing_key);
	free(cont->ca_pem);
	free(cont->key);
	free(cont->cert);
	free(cont);
}

static void __store_amqp_inst_free(store_amqp_inst_t inst)
{
	free(inst);
}

static int check_reply(amqp_rpc_reply_t r, const char *file, int line)
{
	switch (r.reply_type) {
	case AMQP_RESPONSE_NORMAL:
		return 0;
	case AMQP_RESPONSE_NONE:
		LOG_ERROR("AMQP protocol error; missing RPC reply type\n");
		break;
	case AMQP_RESPONSE_LIBRARY_EXCEPTION:
		LOG_ERROR("%s\n", amqp_error_string2(r.library_error));
		break;
	case AMQP_RESPONSE_SERVER_EXCEPTION:
		switch (r.reply.id) {
		case AMQP_CONNECTION_CLOSE_METHOD: {
			amqp_connection_close_t *m =
				(amqp_connection_close_t *) r.reply.decoded;
			LOG_ERROR("server connection error %d, message: %.*s\n",
			     m->reply_code,
			     (int)m->reply_text.len, (char *)m->reply_text.bytes);
			break;
		}
		case AMQP_CHANNEL_CLOSE_METHOD: {
			amqp_channel_close_t *m = (amqp_channel_close_t *) r.reply.decoded;
			LOG_ERROR("server channel error %d, message: %.*s\n",
			     m->reply_code,
			     (int)m->reply_text.len, (char *)m->reply_text.bytes);
			break;
		}
		default:
			LOG_ERROR("unknown server error, method id 0x%08X\n", r.reply.id);
			break;
		}
		break;
	}
	return -1;
}
#define CHECK_REPLY(_qrc_) check_reply(_qrc_, __FILE__, __LINE__)

static const char *usage()
{
	return	"config name= "STORE_NAME" [container=<name>] [host=<host>] [vhost=<vhost>] [port=<port>]\n"
		"       [user=<user>] [pwd=<pwd>] [pwfile=<path>]\n"
		"       [cacert=<path>] [key=<path>] [cert=<path>]\n"
		"       [format=<format>] [exchange=<name>] [routing_key=<string>]\n"
		"    container        A unique storage container name.\n"
		"                     Specify the attribute to tell the plugin to start multiple connections to one or more AMQP servers.\n"
		"                     Each container name maps to a connection. The container string\n"
		"                     must match the container attribute of a strgp_add line.\n"
		"                     If not specified, the plugin can connect to a single AMQP server.\n"
		"    host             The DNS hostname or IP address of the AMQP server.\n"
		"                     The default is 'localhost'.\n"
		"    vhost            Virtual host. The default is '/'.\n"
		"    port             The port number of the AMQP server. The default is "STR(AMQP_PROTOCAL_PORT)".\n"
		"    user             The SASL user name. The default is 'guest'.\n"
		"    pwd              The SASL password. If specified, 'pwfile' is ignored.\n"
		"    pwfile           The AMQP user password location.\n"
		"                     If none of 'pwd' and 'pwfile' are specified,\n"
		"                     the password is 'guest'.\n"
		"    cacert           Path to the CA certificate file in PEM format. If specified,\n"
		"                     key and cert must also be specified.\n"
		"    key              Path to the client key in PEM  format. If specified cacert and\n"
		"                     cert must also be specified.\n"
		"    cert             Path to the client cert in PEM format. If specified, cacert and\n"
		"                     key must also be specified.\n"
		"    exchange         Unique name of the AMQP exchange. The default is 'amq.topic'.\n"
		"    format           The AMQP message format. The choices are JSON, CSV, BIN.\n" /* TODO: Add more detail how each format looks like. */
		"                     The default is JSON.\n"
		"    routing_key      A string to prefix the routing key for all messages.\n"
		"                     If specified, the routing key is '<string>.<format>.<schema>.<producer>'.\n"
		"                     If not specified, the routing key is '<format>.<schema>.<producer>'.\n";
}

static int setup_certs(store_amqp_container_t cont, const char *cacert,
					   struct attr_value_list *avl)
{
	int rc;
	const char *key;
	const char *cert;

	rc = amqp_ssl_socket_set_cacert(cont->socket, cacert);
	if (rc) {
		LOG_ERROR("Error %d setting the CA cert file.\n", rc);
		goto out;
	}
	key = av_value(avl, "key");
	cert = av_value(avl, "cert");
	if (key) {
		if ((key && !cert) || (cert && !key)) {
			rc = EINVAL;
			LOG_ERROR("The key .PEM and cert.PEM files must"
				  "be specified together.\n");
			goto out;
		}
		rc = amqp_ssl_socket_set_key(cont->socket, cert, key);
		if (rc) {
			LOG_ERROR("Error %d setting key and cert files.\n");
			goto out;
		}
	}
 out:
	return rc;
}

static int config(struct ldmsd_plugin *self, struct attr_value_list *kwl,
					     struct attr_value_list *avl)
{
	int rc = 0;
	amqp_rpc_reply_t qrc;
	char *value;
	store_amqp_container_t cont;
	struct rbn *rbn;

	pthread_mutex_lock(&cfg_lock);

	/* container, i.e., instance's key */
	value = av_value(avl, "container");
	if (value) {
		rbn = rbt_find(&s_amqp_cont_rbt, value);
		if (rbn) {
			LOG_ERROR("The specified container '%s' already exists.\n", value);
			rc = EINVAL;
			goto unlock;
		}
	} else {
		value = DEF_AMQP_DEFAULT_CONT;
		rbn = rbt_find(&s_amqp_cont_rbt, value);
		if (rbn) {
			LOG_ERROR("The plugin has already configured.\n");
			rc = EINVAL;
			goto unlock;
		}
	}

	cont = calloc(1, sizeof(*cont));
	if (!cont) {
		LOG_OOM();
		rc = ENOMEM;
		goto unlock;
	}
	cont->container = strdup(value);
	if (!cont->container) {
		LOG_OOM();
		goto free_inst;
	}
	/* format */
	cont->formatter = formatters[S_AMQP_JSON_FMT];
	cont->format = S_AMQP_JSON_FMT;
	value = av_value(avl, "format");
	if (value) {
		if (0 == strcasecmp(value, "csv")) {
			cont->formatter = formatters[S_AMQP_CSV_FMT];
			cont->format = S_AMQP_CSV_FMT;
		} else if (0 == strcasecmp(value, "binary")) {
			cont->formatter = formatters[S_AMQP_BIN_FMT];
			cont->format = S_AMQP_BIN_FMT;
		} else if (0 != strcasecmp(value, "json")) {
			LOG_INFO("Invalid formatter '%s' specified, defaulting to JSON.\n",
			     value);
		}
	}
	/* exchange */
	value = av_value(avl, "exchange");
	if (!value) {
		cont->exchange = strdup("amq.topic");
		cont->declared = 1;
	} else {
		cont->exchange = strdup(value);
	}
	if (!cont->exchange) {
		LOG_OOM();
		rc = ENOMEM;
		goto free_inst;
	}
	/* host */
	value = av_value(avl, "host");
	if (!value)
		cont->host = strdup("localhost");
	else
		cont->host = strdup(value);
	if (!cont->host) {
		LOG_OOM();
		rc = ENOMEM;
		goto free_inst;
	}
	/* vhost */
	value = av_value(avl, "vhost");
	if (!value)
		cont->vhost = strdup("/");
	else
		cont->vhost = strdup(value);
	if (!cont->vhost) {
		LOG_OOM();
		rc = ENOMEM;
		goto free_inst;
	}
	/* port */
	value = av_value(avl, "port");
	if (value) {
		char *ptr;
		long sl;
		sl = strtol(value, &ptr, 0);
		if ((ptr[0] != '\0') || (sl < 1) || (sl > USHRT_MAX)) {
			LOG_ERROR("The given port '%s' is invalid.\n", value);
			rc = EINVAL;
			goto free_inst;
		}
		cont->port = sl;
	} else {
		cont->port = AMQP_PROTOCOL_PORT;
	}
	/* routing key */
	value = av_value(avl, "routing_key");
	if (value) {
		rc = asprintf(&cont->routing_key, "%s.%s",
				value, format_str[cont->format]);
		if (rc < 0)
			cont->routing_key = NULL;
	} else {
		cont->routing_key = strdup(format_str[cont->format]);
	}
	if (!cont->routing_key) {
		LOG_OOM();
		rc = ENOMEM;
		goto free_inst;
	}

	/* user */
	value = av_value(avl, "user");
	if (!value)
		cont->user = strdup("guest");
	else
		cont->user = strdup(value);
	if (!cont->user) {
		LOG_OOM();
		rc = ENOMEM;
		goto free_inst;
	}
	/* pwd */
	value = av_value(avl, "pwd");
	if (!value) {
		value = av_value(avl, "pwfile");
		if (value) {
			cont->pwd = ovis_auth_get_secretword(value, NULL);
			if (!cont->pwd) {
				LOG_ERROR("Failed to get the password.\n");
				rc = EINVAL;
				goto free_inst;
			}
		} else {
			cont->pwd = strdup("guest");
		}
	} else {
		cont->pwd = strdup(value);
	}
	if (!cont->pwd) {
		LOG_OOM();
		rc = ENOMEM;
		goto free_inst;
	}

	/* Create the connection state */
	cont->conn = amqp_new_connection();
	if (!cont->conn) {
		rc = (errno ? errno : ENOMEM);
		LOG_ERROR("Error %d creating the AMQP connection state.\n", rc);
		goto free_inst;
	}
	/* Create the socket */
	cont->socket = amqp_tcp_socket_new(cont->conn);
	if (!cont->socket) {
		rc = (errno ? errno : ENOMEM);
		LOG_ERROR("Error %d creating the AMQP socket.\n", rc);
		goto destroy_conn;
	}

	/* cacert */
	value = av_value(avl, "cacert");
	if (value) {
		rc = setup_certs(cont, value, avl);
		if (rc) {
			LOG_ERROR("Error %d setting up the certificate.\n", rc);
			goto destroy_conn;
		}
	}
	rc = amqp_socket_open(cont->socket, cont->host, cont->port);
	if (rc) {
		LOG_ERROR("Error %d opening the AMQP socket.\n", rc);
		goto destroy_conn;
	}

	qrc = amqp_login(cont->conn, cont->vhost, 0, AMQP_DEFAULT_FRAME_SIZE, 0,
			AMQP_SASL_METHOD_PLAIN, cont->user, cont->pwd);
	if (CHECK_REPLY(qrc) < 0)
		goto close_conn;
	cont->channel = 1;
	amqp_channel_open(cont->conn, cont->channel);
	qrc = amqp_get_rpc_reply(cont->conn);
	if (CHECK_REPLY(qrc) < 0)
		goto close_conn;
	pthread_mutex_init(&cont->lock, NULL);
	rbn_init(&cont->rbn, cont->container);
	rbt_ins(&s_amqp_cont_rbt, &cont->rbn);
	pthread_mutex_unlock(&cfg_lock);

	/*
	 * TODO:
	 *   - meta interval?
	 *   - logmsg ?
	 *   - connect timeout? -> amqp_socket_open_noblock()
	 *   - connect retry?
	 *   - heartbeat?
	 */
	return 0;

close_conn:
	amqp_channel_close(cont->conn, cont->channel, AMQP_REPLY_SUCCESS);
	amqp_connection_close(cont->conn, AMQP_REPLY_SUCCESS);
destroy_conn:
	amqp_destroy_connection(cont->conn);
free_inst:
	__store_amqp_cont_free(cont);
unlock:
	pthread_mutex_unlock(&cfg_lock);
	return rc;
}

static void __store_amqp_cont_close(store_amqp_container_t cont)
{
	amqp_channel_close(cont->conn, cont->channel, AMQP_REPLY_SUCCESS);
	amqp_connection_close(cont->conn, AMQP_REPLY_SUCCESS);
	amqp_destroy_connection(cont->conn);
	rbt_del(&s_amqp_cont_rbt, &cont->rbn);
	__store_amqp_cont_free(cont);
}

static void close_store(ldmsd_store_handle_t sh)
{
	store_amqp_inst_t inst = sh;
	if (!inst)
		return;
	pthread_mutex_lock(&cfg_lock);
	LIST_REMOVE(inst, entry);
	pthread_mutex_unlock(&cfg_lock);
}

static int flush_store(ldmsd_store_handle_t sh)
{
	return 0;
}

static void *get_ucontext(ldmsd_store_handle_t sh)
{
	store_amqp_inst_t inst = sh;
	return inst->ucontext;
}

static void term(struct ldmsd_plugin *self)
{
	struct rbn *rbn;
	store_amqp_inst_t inst;
	store_amqp_container_t cont;
	pthread_mutex_lock(&cfg_lock);
	while ((inst = LIST_FIRST(&inst_list))) {
		LIST_REMOVE(inst, entry);
		__store_amqp_inst_free(inst);

	}
	while ((rbn = rbt_min(&s_amqp_cont_rbt)) != NULL) {
		cont = container_of(rbn, struct store_amqp_container, rbn);
		__store_amqp_cont_close(cont);
	}
	pthread_mutex_unlock(&cfg_lock);
}

static ldmsd_store_handle_t
open_store(struct ldmsd_store *s, const char *container, const char *schema,
	   struct ldmsd_strgp_metric_list *metric_list, void *ucontext)
{
	store_amqp_inst_t inst;
	store_amqp_container_t cont;
	struct rbn *rbn;
	amqp_rpc_reply_t qrc;

	inst = calloc(1, sizeof(*inst));
	if (!inst) {
		LOG_OOM();
		return NULL;
	}
	inst->ucontext = ucontext;

	pthread_mutex_lock(&cfg_lock);
	/* search for the container */
	rbn = rbt_find(&s_amqp_cont_rbt, container);
	if (!rbn) {
		LOG_INFO("No configuration maps to container '%s'. "
			 "Use the default configuration.\n", container);
		rbn = rbt_find(&s_amqp_cont_rbt, DEF_AMQP_DEFAULT_CONT);
		if (!rbn) {
			LOG_ERROR("The plugin has not been configured.\n");
			goto err_0;
		}
	}

	cont = container_of(rbn, struct store_amqp_container, rbn);
	if (!cont->declared) {
		/* Create the exchange */
		amqp_exchange_declare(cont->conn, cont->channel,
				      amqp_cstring_bytes(cont->exchange),
				      amqp_cstring_bytes("direct"),
				      0, 0,
				      #if AMQP_VERSION >= 0x00060001
				      0, 0,
				      #endif
				      amqp_empty_table);
		qrc = amqp_get_rpc_reply(cont->conn);
		if (CHECK_REPLY(qrc) < 0)
			goto err_0;
		cont->declared = 1;
	}
	/* Point the instance to the container */
	inst->container = cont;
	LIST_INSERT_HEAD(&inst_list, inst, entry);
	pthread_mutex_unlock(&cfg_lock);
	return inst;
 err_0:
	pthread_mutex_unlock(&cfg_lock);
	free(inst);
	return NULL;
}

static int
store(ldmsd_store_handle_t _sh, ldms_set_t set,
      int *metric_arry, size_t metric_count)
{
	amqp_basic_properties_t props;
	store_amqp_inst_t inst = _sh;
	store_amqp_container_t cont;
	size_t msg_len;
	int rc;
	char *routing_key;
	const char *schema;
	const char *producer;
	amqp_bytes_t msg_bytes;

	if (!inst)
		return EINVAL;
	cont = inst->container;

	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG |
		AMQP_BASIC_DELIVERY_MODE_FLAG |
		AMQP_BASIC_TIMESTAMP_FLAG;
	props.content_type = amqp_cstring_bytes("text/json");
	props.delivery_mode = 1;
	props.timestamp = (uint64_t)time(NULL);

	schema = ldms_set_schema_name_get(set);
	producer = ldms_set_producer_name_get(set);
	/* no need to lock the container. We only read the routing key. */
	rc = asprintf(&routing_key, "%s.%s.%s", cont->routing_key,
						schema, producer);
	if (rc < 0) {
		LOG_OOM();
		rc = ENOMEM;
		goto out;
	}

	pthread_mutex_lock(&inst->lock);
	msg_len = cont->formatter(inst, set, metric_arry, metric_count);
	msg_bytes.len = msg_len;
	msg_bytes.bytes = inst->msg_buf;
	rc = amqp_basic_publish(cont->conn, cont->channel,
				amqp_cstring_bytes(cont->exchange),
				amqp_cstring_bytes(routing_key),
				0, 0, &props, msg_bytes);
	pthread_mutex_unlock(&inst->lock);
out:
	return rc;
}

static struct ldmsd_store store_amqp = {
	.base = {
		.name = STORE_NAME,
		.type = LDMSD_PLUGIN_STORE,
		.term = term,
		.config = config,
		.usage = usage,
	},
	.open = open_store,
	.get_context = get_ucontext,
	.store = store,
	.flush = flush_store,
	.close = close_store,
};

struct ldmsd_plugin *get_plugin(ldmsd_msg_log_f pf)
{
	if (!amqp_log) {
		amqp_log = ovis_log_register(STORE_NAME,
					     "Messages for " STORE_NAME);
		if (!amqp_log) {
			ovis_log(NULL, OVIS_LERROR,
				 STORE_NAME " : Failed to register the log.\n");
		}

	}
	return &store_amqp.base;
}

static void __attribute__ ((constructor)) store_amqp_init();
static void store_amqp_init()
{
	pthread_mutex_init(&cfg_lock, NULL);
	rbt_init(&s_amqp_cont_rbt, comparator);
	LIST_INIT(&inst_list);
}

static void __attribute__ ((destructor)) store_amqp_fini(void);
static void store_amqp_fini()
{
	pthread_mutex_destroy(&cfg_lock);
}
