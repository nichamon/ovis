/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2011-2019 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2011-2019 Open Grid Computing, Inc. All rights reserved.
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
#include <inttypes.h>
#include <unistd.h>
#include <stdarg.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <libgen.h>
#include <signal.h>
#include <search.h>
#include <semaphore.h>
#include <assert.h>
#include <netdb.h>
#include <time.h>
#include <ctype.h>
#include "json/json_util.h"
#include "ldms.h"
#include "ldmsd_request.h"
#include "config.h"

#define _GNU_SOURCE

#ifdef HAVE_LIBREADLINE
#  if defined(HAVE_READLINE_READLINE_H)
#    include <readline/readline.h>
#  elif defined(HAVE_READLINE_H)
#    include <readline.h>
#  else /* !defined(HAVE_READLINE_H) */
extern char *readline ();
#  endif /* !defined(HAVE_READLINE_H) */
#else /* !defined(HAVE_READLINE_READLINE_H) */
  /* no readline */
#endif /* HAVE_LIBREADLINE */

#ifdef HAVE_READLINE_HISTORY
#  if defined(HAVE_READLINE_HISTORY_H)
#    include <readline/history.h>
#  elif defined(HAVE_HISTORY_H)
#    include <history.h>
#  else /* !defined(HAVE_HISTORY_H) */
extern void add_history ();
extern int write_history ();
extern int read_history ();
#  endif /* defined(HAVE_READLINE_HISTORY_H) */
  /* no history */
#endif /* HAVE_READLINE_HISTORY */

#include "ldmsd.h"
#include "ldmsd_request.h"

#define FMT "h:p:a:A:S:x:s:X:i"
#define ARRAY_SIZE(a)  (sizeof(a) / sizeof(a[0]))

#define LDMSD_SOCKPATH_ENV "LDMSD_SOCKPATH"

static char *linebuf;
static size_t linebuf_len;
static char *buffer;
static size_t buffer_len;

struct ldmsctl_ctrl;
typedef int (*ctrl_send_fn_t)(struct ldmsctl_ctrl *ctrl, ldmsd_req_hdr_t req, size_t len);
typedef char *(*ctrl_recv_fn_t)(struct ldmsctl_ctrl *ctrl);
typedef void (*ctrl_close_fn_t)(struct ldmsctl_ctrl *ctrl);
struct ldmsctl_ctrl {
	union {
		struct ldmsctl_sock {
			int sock;
		} sock;
		struct ldmsctl_ldms_xprt {
			ldms_t x;
			sem_t connected_sem;
			sem_t recv_sem;
		} ldms_xprt;
	};
	ctrl_send_fn_t send_req;
	ctrl_recv_fn_t recv_resp;
	ctrl_close_fn_t close;
};

struct command {
	char *token;
	int (*action)(struct ldmsctl_ctrl *ctrl, char *args);
	void (*help)();
	void (*resp)(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err);
};

static int command_comparator(const void *a, const void *b)
{
	struct command *_a = (struct command *)a;
	struct command *_b = (struct command *)b;
	return strcmp(_a->token, _b->token);
}

#define LDMSCTL_HELP LDMSD_NOTSUPPORT_REQ + 1
#define LDMSCTL_QUIT LDMSD_NOTSUPPORT_REQ + 2
#define LDMSCTL_SCRIPT LDMSD_NOTSUPPORT_REQ + 3
#define LDMSCTL_SOURCE LDMSD_NOTSUPPORT_REQ + 4

static void ldmsctl_log(enum ldmsd_loglevel level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
}

static void usage(char *argv[])
{
	printf("%s: [%s]\n"
	       "    -h <host>       Hostname of ldmsd to connect to.\n"
	       "    -p <port>       LDMS daemon listener port to connect to.\n"
	       "    -x <xprt>       Transports one of sock, ugni, and rdma.\n"
	       "    -a              Authentication plugin (default: 'none').\n"
	       "    -A <K>=<VAL>    Authentication plugin options (repeatable).\n"
	       "    -s <source>     Path to a configuration file\n"
	       "    -X <script>     Path to a script file that generates a configuration file\n"
	       "DEPRECATED OPTIONS:\n"
	       "    -S <socket>     **DEPRECATED** The UNIX socket that the ldms daemon is listening on.\n"
	       "                    [" LDMSD_CONTROL_SOCKNAME "].\n"
	       "    -i              **DEPRECATED** Specify to connect to the data channel\n"
	       ,
	       argv[0], FMT);
	exit(0);
}

/* Caller must free the returned string. */
char *ldmsctl_ts_str(uint32_t sec, uint32_t usec)
{
	struct tm *tm;
	char dtsz[200];
	char *str = malloc(200);
	if (!str)
		return NULL;
	time_t t = sec;
	tm = localtime(&t);
	strftime(dtsz, sizeof(dtsz), "%D %H:%M:%S %z", tm);
	snprintf(str, 200, "%s [%dus]", dtsz, usec);
	return str;
}

static int __handle_cmd(struct ldmsctl_ctrl *ctrl, char *cmd_str);

static void help_greeting()
{
	printf("\nGreet ldmsd\n\n"
	       "Parameters:"
	       "     [name=]   The string ldmsd will echo back.\n"
	       "               If 'name' is not given, nothing will be returned\n"
	       "     [offset=] The response will contain <offset> characters\n"
	       "     [level=]  The response will consist of <level> + 1 records\n"
	       "     [test]    The response is 'Hi'\n"
	       "     [path]    The response is a string 'XXX:YYY:...:ZZZ',\n"
	       "               where 'XXX', 'YYY' and 'ZZZ' are myhostname of\n"
	       "               the first producer in the list of each daemon");
}

static void resp_greeting(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	ldmsd_req_attr_t next_attr;
	int count = 0;
	while (attr->discrim) {
		next_attr = ldmsd_next_attr(attr);
		if ((0 == next_attr->discrim) && (count == 0)) {
			char *str = strdup((char*)attr->attr_value);
			char *tok = strtok(str, " ");
			if (!isdigit(tok[0])) {
				/* The attribute 'level' isn't used. */
				printf("%s\n", attr->attr_value);
			} else {
				printf("%s\n", str);
			}
			free(str);
		} else {
			printf("%s\n", strtok((char*)attr->attr_value, " "));
		}
		attr = next_attr;
		count++;
	}
}

static int handle_quit(struct ldmsctl_ctrl *ctrl, char *kw)
{
	printf("bye ... :)\n");
	ctrl->close(ctrl);
	exit(0);
	return 0;
}

static void help_script()
{
	printf("\nExecute the command and send the output to ldmsd.\n\n"
	       "  script <CMD>   CMD is the command to be executed.\n");
}

static void help_source()
{
	printf("\nSend the commands in the configuration file to the ldmsd\n\n"
	       "  source <PATH>  PATH is the path to the configuration file.\n");
}

static void resp_usage(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (attr->discrim && (attr->attr_id == LDMSD_ATTR_STRING))
		printf("%s\n", attr->attr_value);
}

static void help_usage()
{
	printf( "usage\n"
		"   - Show plugin usage information.\n"
		"Parameters:\n"
		"  At least of one attribute must be provided.\n"
		"    [name=]     The plugin instance.\n"
		"    [type=]     [true|sampler|store|all]\n"
		"                'true' can be given when 'name' is given as well and the\n"
		"                common attributes of such plugin type will also be provided.\n"
		"                If 'sampler' is given, the common attributes of the sampler\n"
		"                plugin instances will be provided.\n"
		"                If 'store' is given, the common attributes of the store\n"
		"                plugin instances will be provided.\n"
		"                If 'all' is given, the common attributes of both 'sampler'\n"
		"                and 'store' will be provided.\n");
}

static void help_load()
{
	printf(	"\nLoads the specified plugin. The library that implements\n"
		"the plugin should be in the directory specified by the\n"
		"LDMSD_PLUGIN_LIBPATH environment variable.\n\n"
		"Parameters:\n"
		"     name=       The plugin instance name."
		"     plugin=     The plugin name, this is used to locate a loadable\n"
		"                 library named \"lib<plugin>.so\"\n"
		"                 It is optional if the plugin instance name is the plugin name.");
}

static void resp_list(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (attr->discrim && (attr->attr_id == LDMSD_ATTR_STRING))
		printf("%s\n", attr->attr_value);
}

static void help_list()
{
	printf("\nList plugin instances.\n");
}

static void help_term()
{
	printf(	"\nUnload the specified plugin.\n\n"
		"Parameters:\n"
		"     name=       The plugin instance name.\n");
}

static void help_config()
{
	printf(	"Provides a mechanism to specify configuration options\n\n"
		"Parameters:\n"
		"     name=       The plugin instance name.\n"
		"     <attr>=     Plugin specific attr=value tuples.\n");
}

static void help_daemon_status()
{
	printf( "\nCauses the ldmsd to dump out information about its internal state.\n");
}

static void help_daemon_exit()
{
	printf(" \nExit the connected LDMS daemon\n\n");
}

static void help_udata()
{
	printf( "\nSet the user data of the specified metric in the given set\n\n"
		"Parameters:\n"
		"     set=           The metric set name\n"
		"     metric_name=   The metric name\n"
		"     user_data=     The user data value\n");
}

static void help_udata_regex()
{
	printf( "\nSet the user data of multiple metrics using regular expression.\n"
		"The user data of the first matched metric is set to the base value.\n"
		"The base value is incremented by the given 'incr' value and then\n"
		"sets to the user data of the consecutive matched metric and so on.\n"
		"Parameters:\n"
		"     set=           The metric set name\n"
		"     regex=         A regular expression to match metric names to be set\n"
		"     base=          The base value of user data (uint64).\n"
		"     [incr=]        Increment value (int). The default is 0. If incr is 0,\n"
		"                    the user data of all matched metrics are set\n"
		"                    to the base value.\n");
}

static void help_oneshot()
{
	printf( "\nSchedule a one-shot sample event\n\n"
		"Parameters:\n"
		"     name=       The sampler name.\n"
		"     time=       A Unix timestamp or a special keyword 'now+<second>'\n"
		"                 The sample will occur at the specified timestamp or at\n"
		"                 the second= from now.\n");
}

static void help_loglevel()
{
	printf( "\nChange the verbosity level of ldmsd\n\n"
		"Parameters:\n"
		"	level=	levels [DEBUG, INFO, ERROR, CRITICAL, QUIET]\n");
}

static void help_quit()
{
	printf( "\nquit\n"
		"   - Exit.\n");
}

static void help_prdcr_add()
{
	printf( "\nAdd an LDMS Producer to the Aggregator\n\n"
		"Parameters:\n"
		"     name=     A unique name for this Producer\n"
		"     xprt=     The transport name [sock, rdma, ugni]\n"
		"     host=     The hostname of the host\n"
		"     port=     The port number on which the LDMS is listening\n"
		"     type=     The connection type [active, passive]\n"
		"     interval= The connection retry interval (us)\n"
		"     [perm=]   The permission to modify the producer in the future.\n"
		"     [auth=]   The authentication method to with the connection\n"
		"     [auth_arg=value]    The authentication arguments and values, e.g., conf=/path/to/secretword\n");
}

static void help_prdcr_del()
{
	printf( "\nDelete an LDMS Producer from the Aggregator. The producer\n"
		"cannot be in use or running.\n\n"
		"Parameters:\n"
		"     name=    The Producer name\n");
}

static void help_prdcr_start()
{
	printf( "\nStart the specified producer.\n\n"
		"Parameters:\n"
		"     name=       The name of the producer\n"
		"     [interval=] The connection retry interval in micro-seconds.\n"
		"                 If this is not specified, the previously\n"
		"                 configured value will be used.\n");
}

static void help_prdcr_stop()
{
	printf( "\nStop the specified producer.\n\n"
		"Parameters:\n"
		"     name=     THe producer name\n");
}

static void help_prdcr_start_regex()
{
	printf( "\nStart all producers matching a regular expression.\n\n"
		"Parameters:\n\n"
		"     regex=        A regular expression\n"
		"     [interval=]   The connection retry interval in micro-seconds.\n"
		"                   If this is not specified, the previously configured\n"
		"                   value will be used.\n");
}

static void help_prdcr_stop_regex()
{
	printf( "\nStop all producers matching a regular expression.\n\n"
		"Parameters:\n"
		"     regex=        A regular expression\n");
}

static void help_prdcr_subscribe_regex()
{
	printf( "\nRegister for stream data from the producer.\n\n"
		"Parameters:\n"
		"     regex=        A regular expression to match producers\n"
		"     stream=       The stream name\n");
}

static void resp_generic(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	ldmsd_req_attr_t attr;
	if (rsp_err) {
		attr = ldmsd_first_attr(resp);
		if (attr->discrim && (attr->attr_id == LDMSD_ATTR_STRING))
			printf("%s\n", attr->attr_value);
	}
}

static void resp_daemon_status(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	int rc;
	ldmsd_req_attr_t attr;
	json_parser_t parser;
	json_entity_t json, thread;
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}

	attr = ldmsd_first_attr(resp);
	if (!attr->discrim || (attr->attr_id != LDMSD_ATTR_JSON))
		return;

	parser = json_parser_new(0);
	if (!parser) {
		printf("Error creating a JSON parser.\n");
		return;
	}
	rc = json_parse_buffer(parser, (char*)attr->attr_value, len, &json);
	if (rc) {
		printf("syntax error parsing JSON string\n");
		json_parser_free(parser);
		return;
	}
	json_parser_free(parser);

	if (json->type != JSON_LIST_VALUE) {
		printf("Unrecognized JSON daemon status format\n");
		goto out;
	}

	printf("Thread           Task Counts\n");
	printf("---------------- -----------\n");

	json_entity_t thr_id, count;

	for (thread = json_item_first(json); thread; thread = json_item_next(thread)) {
		if (thread->type != JSON_DICT_VALUE) {
			printf("Unrecognized JSON daemon status format\n");
			goto out;
		}
		thr_id = json_value_find(thread, "thread");
		count = json_value_find(thread, "task_count");
		if (!thr_id || !count) {
			printf("Unrecognized status format\n");
			goto out;
		}
		printf("%15s %10s\n", json_value_str(thr_id)->str,
					json_value_str(count)->str);
	}
out:
	json_entity_free(json);
}

static void resp_daemon_exit(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	ldmsd_req_attr_t attr;
	attr = ldmsd_first_attr(resp);
	if (attr->discrim && (attr->attr_id == LDMSD_ATTR_STRING))
		printf("%s\n", attr->attr_value);
	else
		printf("Please 'quit' the ldmsd_controller interface\n");
}

void __print_prdcr_status(json_entity_t prdcr)
{
	json_entity_t name, host, xprt, state, port;

	name = json_value_find(prdcr, "name");
	host = json_value_find(prdcr, "host");
	port = json_value_find(prdcr, "port");
	xprt = json_value_find(prdcr, "transport");
	state = json_value_find(prdcr, "state");
	if (!name || !host || !port || !xprt || !state)
		goto invalid_result_format;

	printf("%-16s %-16s %-12" PRId64 "%-12s %-12s\n",
			json_value_str(name)->str,
			json_value_str(host)->str,
			json_value_int(port),
			json_value_str(xprt)->str,
			json_value_str(state)->str);

	json_entity_t prd_sets_attr, prd_sets;
	prd_sets_attr = json_attr_find(prdcr, "sets");
	if (!prd_sets_attr)
		goto invalid_result_format;
	prd_sets = json_attr_value(prd_sets_attr);
	if (prd_sets->type != JSON_LIST_VALUE)
		goto invalid_result_format;

	json_entity_t prd_set, inst_name, schema_name, set_state;
	for (prd_set = json_item_first(prd_sets); prd_set; prd_set = json_item_next(prd_set)) {
		if (prd_set->type != JSON_DICT_VALUE)
			goto invalid_result_format;
		inst_name = json_value_find(prd_set, "inst_name");
		schema_name = json_value_find(prd_set, "schema_name");
		set_state = json_value_find(prd_set, "state");
		if (!inst_name || !schema_name || !set_state)
			goto invalid_result_format;

		printf("    %-16s %-16s %s\n",
				json_value_str(inst_name)->str,
				json_value_str(schema_name)->str,
				json_value_str(set_state)->str);
	}
	return;

invalid_result_format:
	printf("---Invalid result format---\n");
	return;
}

static void resp_prdcr_status(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	int rc;
	json_parser_t parser;
	json_entity_t json, prdcr;
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}
	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (!attr->discrim || (attr->attr_id != LDMSD_ATTR_JSON))
		return;

	parser = json_parser_new(0);
	if (!parser) {
		printf("Error creating a JSON parser.\n");
		return;
	}
	rc = json_parse_buffer(parser, (char*)attr->attr_value, len, &json);
	if (rc) {
		printf("syntax error parsing JSON string\n");
		json_parser_free(parser);
		return;
	}
	json_parser_free(parser);

	if (json->type != JSON_LIST_VALUE) {
		printf("Unrecognized JSON producer status format\n");
		goto out;
	}

	printf("Name             Host             Port         Transport    State\n");
	printf("---------------- ---------------- ------------ ------------ ------------\n");

	for (prdcr = json_item_first(json); prdcr; prdcr = json_item_next(prdcr)) {
		if (prdcr->type != JSON_DICT_VALUE) {
			printf("---Invalid producer status format---\n");
			goto out;
		}
		__print_prdcr_status(prdcr);
	}
out:
	json_entity_free(json);
}

static void help_prdcr_status()
{
	printf( "\nGet status of all producers\n");
}

void __print_prdcr_set_status(json_entity_t prd_set)
{
	if (prd_set->type != JSON_DICT_VALUE) {
		printf("---Invalid producer set status format---\n");
		return;
	}

	json_entity_t name, schema, state, origin, prdcr;
	json_entity_t ts_sec, ts_usec, dur_sec_str, dur_usec_str;
	uint32_t dur_sec, dur_usec;

	name = json_value_find(prd_set, "inst_name");
	schema = json_value_find(prd_set, "schema_name");
	state = json_value_find(prd_set, "state");
	origin = json_value_find(prd_set, "origin");
	prdcr = json_value_find(prd_set, "producer");
	ts_sec = json_value_find(prd_set, "timestamp.sec");
	ts_usec = json_value_find(prd_set, "timestamp.usec");
	dur_sec_str = json_value_find(prd_set, "duration.sec");
	dur_usec_str = json_value_find(prd_set, "duration.usec");

	if (!name || !schema || !state || !origin || !prdcr || !ts_sec ||
			!ts_usec) {
		printf("Invalid status format\n");
		return;
	}

	if (dur_sec_str)
		dur_sec = strtoul(json_value_str(dur_sec_str)->str, NULL, 0);
	else
		dur_sec = 0;
	if (dur_usec_str)
		dur_usec = strtoul(json_value_str(dur_usec_str)->str, NULL, 0);
	else
		dur_usec = 0;

	char ts[64];
	char dur[64];
	snprintf(ts, 63, "%s [%s]",
			json_value_str(ts_sec)->str, json_value_str(ts_usec)->str);
	snprintf(dur, 63, "%" PRIu32 ".%06" PRIu32, dur_sec, dur_usec);

	printf("%-20s %-16s %-10s %-16s %-16s %-25s %-12s\n",
			json_value_str(name)->str,
			json_value_str(schema)->str,
			json_value_str(state)->str,
			json_value_str(origin)->str,
			json_value_str(prdcr)->str,
			ts, dur);
}

static void resp_prdcr_set_status(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}
	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (!attr->discrim || (attr->attr_id != LDMSD_ATTR_JSON))
		return;

	json_parser_t parser;
	json_entity_t json, prd_set;
	int rc;

	parser = json_parser_new(0);
	if (!parser) {
		printf("Error creating a JSON parser.\n");
		return;
	}
	rc = json_parse_buffer(parser, (char*)attr->attr_value, len, &json);
	if (rc) {
		printf("syntax error parsing JSON string\n");
		json_parser_free(parser);
		return;
	}
	json_parser_free(parser);

	if (json->type != JSON_LIST_VALUE) {
		printf("Unrecognized producer set status format\n");
		goto out;
	}
	printf("Name                 Schema Name      State      Origin           "
			"Producer         timestamp                 duration (sec)\n");
	printf("-------------------- ---------------- ---------- ---------------- "
			"---------------- ------------------------- ---------------\n");

	for (prd_set = json_item_first(json); prd_set;
			prd_set = json_item_next(prd_set)) {
		__print_prdcr_set_status(prd_set);
	}
out:
	json_entity_free(json);
}

static void help_prdcr_set_status()
{
	printf( "\nGet status of all producer sets\n");
}

static void help_updtr_add()
{
	printf( "\nAdd an updater process that will periodically sample\n"
		"Producer metric sets\n\n"
		"Parameters:\n"
		"     name=       The update policy name\n"
		"     interval=   The update/collect interval\n"
		"     [offset=]   Offset for synchronized aggregation\n"
		"     [push=]     [onchange|true] 'onchange' means the Updater \n"
		"                 will get an update whenever the set source ends a\n"
		"                 transaction or pushes the update. 'true' means the Updater\n"
		"                 will receive an update only when the set source explicitly pushes the\n"
		"                 update. If `push` is used, `auto_interval` cannot be `true`.\n"
		"    [auto_interval=]   [true|false] If true, the updater will schedule\n"
		"                       set updates according to the update hint. The sets\n"
		"                       with no hints will not be updated. If false, the\n"
		"                       updater will schedule the set updates according to\n"
		"                       the given interval and offset values. If not\n"
		"                       specified, the value is `false`.\n"
		"     [perm=]      The permission to modify the updater in the future.\n"
		);

}

static void help_updtr_del()
{
	printf( "\nRemove an updater from the configuration\n\n"
		"Parameters:\n"
		"     name=     The update policy name\n");
}

static void help_updtr_match_add()
{
	printf( "\nAdd a match condition that specifies the sets to an Updater policy.\n\n"
		"Parameters:\n"
		"     name=   The update policy name\n"
		"     regex=  The regular expression string\n"
		"     match=  The value with which to compare; if match=inst,\n"
		"	      the expression will match the set's instance name, if\n"
		"	      match=schema, the expression will match the set's\n"
		"	      schema name.\n");
}

static void help_updtr_match_del()
{
	printf( "\nRemove a match condition that specifies the sets from an Updater policy.\n\n"
		"Parameters:\n"
		"     name=   The update policy name\n"
		"     regex=  The regular expression string\n"
		"     match=  The value with which to compare; if match=inst,\n"
		"	      the expression will match the set's instance name, if\n"
		"	      match=schema, the expression will match the set's\n"
		"	      schema name.\n");
}

static void help_updtr_prdcr_add()
{
	printf( "\nAdd matching Producers to an Updater policy.\n\n"
		"Parameters:\n"
		"     name=   The update policy name\n"
		"     regex=  A regular expression matching zero or more producers\n");
}

static void help_updtr_prdcr_del()
{
	printf( "\nRemove matching Producers from an Updater policy.\n\n"
		"Parameters:\n"
		"     name=   The update policy name\n"
		"     regex=  A regular expression matching zero or more producers\n");
}

static void help_updtr_start()
{
	printf( "\nStart an update policy.\n\n"
		"Parameters:\n"
		"     name=       The update policy name\n"
		"     [interval=] The update interval in micro-seconds.\n"
		"                 If this is not specified, the previously\n"
		"                 configured value will be used.\n"
		"     [offset=]   Offset for synchronization\n"
		"                 If 'interval' is given but not 'offset',\n"
		"                 the updater will update sets asynchronously.\n"
		"     [auto_interval=]   [true|false] If true, the updater will schedule\n"
		"                        set updates according to the update hint. If false,\n"
		"                        the updater will schedule the set updates according\n"
		"                        to the default schedule, i.e., the given interval and offset values.\n");
}

static void help_updtr_stop()
{
	printf( "\nStop an update policy. The Updater must be stopped in order to\n"
		"change it's configuration.\n\n"
		"Parameters:\n"
		"     name=   The update policy name\n");
}

void __print_updtr_status(json_entity_t updtr)
{
	json_entity_t name, interval, mode, state, offset;

	if (updtr->type != JSON_DICT_VALUE)
		goto invalid_result_format;

	name = json_value_find(updtr, "name");
	interval = json_value_find(updtr, "interval");
	offset = json_value_find(updtr, "offset");
	mode = json_value_find(updtr, "mode");
	state = json_value_find(updtr, "state");
	if (!name || !interval || !mode || !state || !offset)
		goto invalid_result_format;

	printf("%-16s %-12s %-12s %-15s %s\n",
			json_value_str(name)->str,
			json_value_str(interval)->str,
			json_value_str(offset)->str,
			json_value_str(mode)->str,
			json_value_str(state)->str);

	json_entity_t prdcrs;
	prdcrs = json_value_find(updtr, "producers");
	if (!prdcrs || (prdcrs->type != JSON_LIST_VALUE))
		goto invalid_result_format;

	json_entity_t prdcr_name, host, xprt, prdcr_state, port, prdcr;
	for (prdcr = json_item_first(prdcrs); prdcr; prdcr = json_item_next(prdcr)) {
		if (prdcr->type != JSON_DICT_VALUE)
			goto invalid_result_format;

		prdcr_name = json_value_find(prdcr, "name");
		host = json_value_find(prdcr, "host");
		xprt = json_value_find(prdcr, "transport");
		prdcr_state = json_value_find(prdcr, "state");
		port = json_value_find(prdcr, "port");
		if (!prdcr_name || !host || !xprt || !prdcr_state || !port)
			goto invalid_result_format;

		printf("    %-16s %-16s %-12" PRId64 "%-12s %s\n",
				json_value_str(prdcr_name)->str,
				json_value_str(host)->str,
				json_value_int(port),
				json_value_str(xprt)->str,
				json_value_str(prdcr_state)->str);
	}
	return;

invalid_result_format:
	printf("---Invalid result format---\n");
	return;
}

static void resp_updtr_status(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}
	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (!attr->discrim || (attr->attr_id != LDMSD_ATTR_JSON))
		return;

	json_parser_t parser;
	json_entity_t json, updtr;
	int rc;

	parser = json_parser_new(0);
	if (!parser) {
		printf("Error creating a JSON parser.\n");
		return;
	}
	rc = json_parse_buffer(parser, (char*)attr->attr_value, len, &json);
	if (rc) {
		printf("syntax error parsing JSON string\n");
		json_parser_free(parser);
		return;
	}
	json_parser_free(parser);

	if (json->type != JSON_LIST_VALUE) {
		printf("Unrecognized updater status format\n");
		goto out;
	}
	printf("Name             Interval     Offset       Mode            State\n");
	printf("---------------- ------------ ------------ --------------- ------------\n");

	for (updtr = json_item_first(json); updtr; updtr = json_item_next(updtr)) {
		__print_updtr_status(updtr);
	}
out:
	json_entity_free(json);
}

static void help_updtr_status()
{
	printf("\nGet the statuses of all Updaters\n"
	       "Parameters:\n"
	       "      None\n");
}

static void help_strgp_add()
{
	printf( "\nCreate a Storage Policy and open/create the storage instance.\n"
		"The store plugin must be configured via the command 'config'\n\n"
		"Parameters:\n"
		"     name=        The unique storage policy name.\n"
		"     container=   The nameo of the storage plugin instance backend.\n"
		"     schema=      The schema name of the metric set to store.\n"
		"     [perm=]      The permission to modify the storage policy in the future.\n");
}

static void help_strgp_del()
{
	printf( "\nRemove a Storage Policy. All updaters must be stopped in order for\n"
		"a storage policy to be deleted.\n\n"
		"Parameters:\n"
		"     name=   The storage policy name\n");
}

static void help_strgp_prdcr_add()
{
	printf( "\nAdd a regular expression used to identify the producers this\n"
		"storage policy will apply to.\n\n"
		"Parameters:\n"
		"     name=   The storage policy name\n"
		"     regex=  A regular expression matching metric set producers\n");
}

static void help_strgp_prdcr_del()
{
	printf( "\nRemove a regular expression from the producer match list.\n\n"
		"Parameters:\n"
		"     name=   The storage policy name\n"
		"     regex=  The regular expression to remove\n");
}

static void help_strgp_metric_add()
{
	printf( "\nAdd the name of a metric to store. If the metric list is NULL,\n"
		"all metrics in the metric set will be stored.\n\n"
		"Parameters:\n"
		"     name=   The storage policy name\n"
		"     metric= The metric name\n");
}

static void help_strgp_metric_del()
{
	printf( "\nRemove a metric from the set of stored metrics\n\n"
		"Parameters:\n"
		"     name=   The storage policy name\n"
		"     metric= The metric name\n");
}

static void help_strgp_start()
{
	printf( "\nStart an storage policy\n\n"
		"Parameters:\n"
		"     name=   The storage policy name\n");
}

static void help_strgp_stop()
{
	printf( "\nStop an storage policy. A storage policy must be stopped\n"
		"in order to change its configuration.\n\n"
		"Parameters:\n"
		"     name=   The storage policy name\n");
}

void __print_strgp_status(json_entity_t strgp)
{
	if (strgp->type != JSON_DICT_VALUE)
		goto invalid_result_format;

	json_entity_t name, container, schema, plugin, state;

	name = json_value_find(strgp, "name");
	container = json_value_find(strgp, "container");
	schema = json_value_find(strgp, "schema");
	plugin = json_value_find(strgp, "plugin");
	state = json_value_find(strgp, "state");

	if (!name || !container || !schema || !plugin || !state)
		goto invalid_result_format;

	printf("%-16s %-16s %-16s %-16s %s\n",
			json_value_str(name)->str,
			json_value_str(container)->str,
			json_value_str(schema)->str,
			json_value_str(plugin)->str,
			json_value_str(state)->str);

	json_entity_t prdcrs, metrics;

	prdcrs = json_value_find(strgp, "producers");
	if (!prdcrs || (prdcrs->type != JSON_LIST_VALUE))
		goto invalid_result_format;
	printf("    producers:");

	json_entity_t prdcr, metric;
	for (prdcr = json_item_first(prdcrs); prdcr; prdcr = json_item_next(prdcr)) {
		if (!prdcr || (prdcr->type != JSON_STRING_VALUE))
			goto invalid_result_format;
		printf(" %s", json_value_str(prdcr)->str);
	}
	printf("\n");

	metrics = json_value_find(strgp, "metrics");
	if (!metrics || (metrics->type != JSON_LIST_VALUE))
		goto invalid_result_format;

	printf("     metrics:");
	for (metric = json_item_first(metrics); metric;
					metric = json_item_next(metric)) {
		if (!metric || (metric->type != JSON_STRING_VALUE)) {
			printf("---Invalid result format---\n");
			return;
		}
		printf(" %s", json_value_str(metric)->str);
	}
	printf("\n");
	return;
invalid_result_format:
	printf("---Invalid result format---\n");
	return;
}

static void resp_strgp_status(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}
	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (!attr->discrim || (attr->attr_id != LDMSD_ATTR_JSON))
		return;

	json_parser_t parser;
	json_entity_t json, strgp;
	int rc;
	parser = json_parser_new(0);
	if (!parser) {
		printf("Error creating a JSON parser.\n");
		return;
	}
	rc = json_parse_buffer(parser, (char*)attr->attr_value, len, &json);
	if (rc) {
		printf("syntax error parsing JSON string\n");
		json_parser_free(parser);
		return;
	}
	json_parser_free(parser);

	if (json->type != JSON_LIST_VALUE) {
		printf("Unrecognized producer status format\n");
		goto out;
	}
	printf("Name             Container        Schema           Plugin           State\n");
	printf("---------------- ---------------- ---------------- ---------------- ------------\n");

	for (strgp = json_item_first(json); strgp; strgp = json_item_next(strgp)) {
		__print_strgp_status(strgp);
	}
out:
	json_entity_free(json);
}

static void help_strgp_status()
{
	printf("\nGet the statuses of all Storage policies\n"
	       "Parameters:\n"
	       "      None\n");
}

static void __print_plugn_sets(json_entity_t plugin_sets)
{
	json_entity_t sets, set_name, pi_name;
	pi_name = json_value_find(plugin_sets, "plugin");
	if (!pi_name) {
		printf("---Invalid result format---\n");
		return;
	}
	printf("%s:\n", json_value_str(pi_name)->str);
	sets = json_value_find(plugin_sets, "sets");
	if (!sets) {
		printf("   None\n");
		return;
	}
	for (set_name = json_item_first(sets); set_name;
			set_name = json_item_next(set_name)) {
		printf("   %s\n", json_value_str(set_name)->str);
	}
	return;
}

static void resp_plugn_sets(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}

	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (!attr->discrim || (attr->attr_id != LDMSD_ATTR_JSON))
		return;

	json_parser_t parser;
	json_entity_t json, plugin;
	int rc;
	parser = json_parser_new(0);
	if (!parser) {
		printf("Error creating a JSON parser.\n");
		return;
	}
	rc = json_parse_buffer(parser, (char*)attr->attr_value, len, &json);
	if (rc) {
		printf("syntax error parsing JSON string\n");
		json_parser_free(parser);
		return;
	}
	json_parser_free(parser);

	if (json->type != JSON_LIST_VALUE) {
		printf("---Invalid result format---\n");
		goto out;
	}

	for (plugin = json_item_first(json); plugin;
				plugin = json_item_next(plugin)) {
		__print_plugn_sets(plugin);
	}
out:
	json_entity_free(json);
}

static void help_plugn_sets()
{
	printf("\nPrint sets by plugins\n"
	       "Parameters:\n"
	       "      [name]=   Plugin name\n");
}

static void __print_plugn_status(json_entity_t status)
{
	json_entity_t name, plugin, type, libpath;

	name = json_value_find(status, "name");
	plugin = json_value_find(status, "plugin");
	type = json_value_find(status, "type");
	libpath = json_value_find(status, "libpath");

	if (!name || !plugin || !type || !libpath) {
		printf("---Invalid result format---\n");
		return;
	}

	printf("%12s %12s %12s %12s\n",
	       json_value_str(name)->str,
	       json_value_str(plugin)->str,
	       json_value_str(type)->str,
	       json_value_str(libpath)->str);
}

static void resp_plugn_status(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}

	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (!attr->discrim || (attr->attr_id != LDMSD_ATTR_JSON))
		return;

	json_parser_t parser;
	json_entity_t json, status;
	int rc;
	parser = json_parser_new(0);
	if (!parser) {
		printf("Error creating a JSON parser.\n");
		return;
	}
	rc = json_parse_buffer(parser, (char*)attr->attr_value, len, &json);
	if (rc) {
		printf("syntax error parsing JSON string\n");
		json_parser_free(parser);
		return;
	}
	json_parser_free(parser);

	if (json->type != JSON_LIST_VALUE) {
		printf("Unrecognized result format\n");
		return;
	}

	printf("%12s %12s %12s %12s \n",
	       "Name", "Plugin", "Type", "Libpath");
	printf("------------ ------------ ------------ ------------\n");

	for (status = json_item_first(json); status;
			status = json_item_next(status)) {
		__print_plugn_status(status);
	}
	json_entity_free(status);

}

static void help_plugn_status()
{
	printf("\nPrint plugin status\n");
}

static void help_version()
{
	printf( "\nGet the LDMS version.\n");
}

static void help_set_route()
{
	printf("\nDisplay the route of the set from aggregators to the sampler daemon.\n"
	       "Parameters:\n"
	       "     instance=   Set instance name\n");
}

static void resp_set_route(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}

	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (!attr->discrim || (attr->attr_id != LDMSD_ATTR_JSON))
		return;

	json_parser_t parser;
	json_entity_t json, route, hop, hinfo;;
	json_entity_t inst_name, schema_name;
	int rc;

	parser = json_parser_new(0);
	if (!parser) {
		printf("Error creating a JSON parser.\n");
		return;
	}
	json = NULL;
	rc = json_parse_buffer(parser, (char*)attr->attr_value, len, &json);
	if (rc) {
		printf("syntax error parsing JSON string\n");
		json_parser_free(parser);
		return;
	}
	json_parser_free(parser);

	if (json->type != JSON_DICT_VALUE) {
		printf("---Invalid result format---\n");
		goto out;
	}

	inst_name = json_value_find(json, "instance");
	schema_name = json_value_find(json, "schema");

	if (!inst_name || !schema_name)
		goto invalid_result_format;

	printf("-----------------------------\n");
	printf("instance: %s\n", json_value_str(inst_name)->str);
	printf("schema_name: %s\n", json_value_str(schema_name)->str);
	printf("=============================\n");
	printf("%20s %15s %15s %15s %10s %10s %5s %25s %25s\n",
			"host", "type", "name", "prdcr_host",
			"interval", "offset", "sync", "start", "end");
	printf("-------------------- --------------- --------------- --------------- "
		"---------- ---------- ----- ------------------------- -------------------------\n");
	route = json_value_find(json, "route");
	if (!route || (route->type != JSON_LIST_VALUE))
		goto invalid_result_format;

	json_entity_t host, type, name, prdcr_host, intrvl, offset, is_sync;
	json_entity_t start_sec, start_usec, end_sec, end_usec;
	char *prdcr_host_s, *type_s, *start, *end;
	uint32_t sec, usec;

	for (hop = json_item_first(route); hop; hop = json_item_next(hop)) {
		hinfo = json_value_find(hop, "detail");
		if (!hinfo || (hinfo->type != JSON_DICT_VALUE))
			goto invalid_result_format;
		type = json_value_find(hop, "type");
		host = json_value_find(hop, "host");
		name = json_value_find(hinfo, "name");

		if (!type || !host || !name)
			goto invalid_result_format;
		type_s = json_value_str(type)->str;

		if (0 == strcmp(type_s, "producer")) {
			prdcr_host = json_value_find(hinfo, "host");
			if (!prdcr_host)
				goto invalid_result_format;
			else
				prdcr_host_s = json_value_str(prdcr_host)->str;
			intrvl = json_value_find(hinfo, "update_int");
			offset = json_value_find(hinfo, "update_off");
			is_sync = json_value_find(hinfo, "update_sync");
			start_sec = json_value_find(hinfo, "last_start_sec");
			start_usec = json_value_find(hinfo, "last_start_usec");
			end_sec = json_value_find(hinfo, "last_end_sec");
			end_usec = json_value_find(hinfo, "last_end_usec");
		} else {
			prdcr_host_s = "---";
			intrvl = json_value_find(hinfo, "interval_us");
			offset = json_value_find(hinfo, "offset_us");
			is_sync = json_value_find(hinfo, "sync");
			start_sec = json_value_find(hinfo, "trans_start_sec");
			start_usec = json_value_find(hinfo, "trans_start_usec");
			end_sec = json_value_find(hinfo, "trans_end_sec");
			end_usec = json_value_find(hinfo, "trans_end_usec");
		}
		if (!intrvl || !offset || ! is_sync ||
			!start_sec || !start_usec || !end_sec || !end_usec) {
			goto invalid_result_format;
		}

		sec = strtoul(json_value_str(start_sec)->str, NULL, 0);
		usec = strtoul(json_value_str(start_usec)->str, NULL, 0);
		start = ldmsctl_ts_str(sec, usec);
		sec = strtoul(json_value_str(end_sec)->str, NULL, 0);
		usec = strtoul(json_value_str(end_usec)->str, NULL, 0);
		end = ldmsctl_ts_str(sec, usec);
		printf("%20s %15s %15s %15s %10s %10s %5s %25s %25s\n",
					json_value_str(host)->str,
					json_value_str(type)->str,
					json_value_str(name)->str,
					prdcr_host_s,
					json_value_str(intrvl)->str,
					json_value_str(offset)->str,
					json_value_str(is_sync)->str,
					start,
					end);
		free(start);
		free(end);
	}
	return;


invalid_result_format:
	printf("---Invalid result format---\n");
out:
	if (json)
		json_entity_free(json);
	return;
}

/* failover related functions */

static void help_failover_peercfg_stop()
{
	printf("Stop peer configuration.\n\n");
}

static void help_failover_peercfg_start()
{
	printf("Start peer configuration.\n\n");
}

static void help_failover_config()
{
	printf("Configure LDMSD failover.\n\n");
	printf("Parameters:\n");
	printf("    host=             The host name of the failover partner.\n");
	printf("                      This is optional in re-configuration.\n");
	printf("    xprt=             The transport of the failover partner.\n");
	printf("                      This is optional in re-configuration.\n");
	printf("    port=             The LDMS port of the failover partner.\n");
	printf("                      This is optional in re-configuration.\n");
	printf("    [auto_switch=0|1] Auto switching (failover/failback).\n");
	printf("    [interval=]       The interval of the heartbeat.\n");
	printf("    [timeout_factor=] The heartbeat timeout factor.\n");
	printf("    [peer_name=]      The failover partner name. If not given,\n");
	printf("                      the ldmsd will accept any partner.\n");
}

static void help_failover_status()
{
	printf("Get failover status.\n\n");
}

static void help_failover_start()
{
	printf("Start LDMSD failover service.\n\n");
	printf("    NOTE: After the failover service has started,\n");
	printf("    aggregator configuration objects (prdcr, updtr, and \n");
	printf("    strgp) are not allowed to be altered (start, stop, or \n");
	printf("    reconfigure).\n\n");
}

static void help_failover_stop()
{
	printf("Stop LDMSD failover service.\n\n");
}

static void help_setgroup_add()
{
	printf("Create a new setgroup.\n");
	printf("Parameters:\n");
	printf("    name=           The set group name.\n");
	printf("    [producer=]     The producer name of the set group.\n");
	printf("    [interval=]     The update interval hint (in usec).\n");
	printf("    [offset=]       The update offset hint (in usec).\n");
	printf("    [perm=]         The permission to modify the setgroup in the future.\n");
}

static void help_setgroup_mod()
{
	printf("Modify attributes of a set group.\n");
	printf("Parameters:\n");
	printf("    name=           The set group name.\n");
	printf("    [interval=]     The update interval hint (in usec).\n");
	printf("    [offset=]       The update offset hint (in usec).\n");
}

static void help_setgroup_del()
{
	printf("Delete a set group\n");
	printf("Parameters:\n");
	printf("    name=    The set group name to delete.\n");
}

static void help_setgroup_ins()
{
	printf("Insert sets into the set group\n");
	printf("Parameters:\n");
	printf("    name=     The set group name.\n");
	printf("    instance= The comma-separated list of set instances to add.\n");
}

static void help_setgroup_rm()
{
	printf("Remove sets from the set group\n");
	printf("Parameters:\n");
	printf("    name=     The set group name.\n");
	printf("    instance= The comma-separated list of set instances to remove.\n");
}

static void __indent_print(int indent)
{
	int i;
	for (i = 0; i < indent; i++) {
		printf("    ");
	}
}

static void __json_value_print(json_entity_t v, int indent)
{
	json_entity_t item, attr;
	switch (v->type) {
	case JSON_DICT_VALUE:
		for (attr = json_attr_first(v); attr; attr = json_attr_next(attr)) {
			printf("\n");
			__indent_print(indent);
			printf("%s: ", json_attr_name(attr)->str);
			__json_value_print(json_attr_value(attr), indent + 1);
		}
		break;
	case JSON_LIST_VALUE:
		for (item = json_item_first(v); item; item = json_item_next(item)) {
			printf("\n");
			__indent_print(indent);
			printf("* ");
			__json_value_print(item, indent + 1);
		}
		break;
	case JSON_NULL_VALUE:
		printf("NULL");
		break;
	case JSON_INT_VALUE:
		printf("%ld", v->value.int_);
		break;
	case JSON_FLOAT_VALUE:
		printf("%lf", v->value.double_);
		break;
	case JSON_STRING_VALUE:
		printf("%s", json_value_str(v)->str);
		break;
	case JSON_BOOL_VALUE:
		printf("%s", v->value.bool_?"True":"False");
		break;
	case JSON_ATTR_VALUE:
			printf("\n");
			__indent_print(indent);
			printf("%s: ", json_attr_name(v)->str);
			__json_value_print(json_attr_value(v), indent + 1);
		break;
	}
}

static void resp_failover_status(ldmsd_req_hdr_t resp, size_t len,
				 uint32_t rsp_err)
{
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}
	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (!attr->discrim || (attr->attr_id != LDMSD_ATTR_JSON))
		return;

	json_parser_t parser;
	json_entity_t json;
	int rc;
	parser = json_parser_new(0);
	if (!parser) {
		printf("Error creating a JSON parser.\n");
		return;
	}
	rc = json_parse_buffer(parser, (char*)attr->attr_value, len, &json);
	if (rc) {
		printf("syntax error parsing JSON string\n");
		json_parser_free(parser);
		return;
	}
	json_parser_free(parser);

	printf("--- Failover Status ---");
	__json_value_print(json, 0);
	printf("\n\n");

	json_entity_free(json);
}

static void help_smplr_add()
{
	printf( "\nAdd a sampler policy\n\n"
		"Parameters:\n"
		"     name=       A unique name for the sampler policy\n"
		"     instance=   Sampler plugin instance name corresponding to this policy\n"
		"     interval=   Sampling interval (us)\n"
		"     [offset=]   Sampling offset (us)\n"
		"     [perm=]     The permission to modify the sampler in the future.\n");
}

static void help_smplr_del()
{
	printf( "\nDelete a sampler policy\n\n"
		"     name=      Sampler policy name\n");
}

static void help_smplr_start()
{
	printf( "\nStart a sampler policy\n\n"
		"     name=         Sampler policy name\n"
		"     [interval=]   Sampling interval (us)\n"
		"     [offset=]     Sampling offset (us)\n");
}

static void help_smplr_stop()
{
	printf( "\nStop a sampler policy\n\n"
		"     name=      Sampler policy name\n");
}

static void help_smplr_status()
{
	printf( "\nGet sampler policy status\n\n"
		"     [name=]    Sampler policy name\n");
}

static void resp_smplr_status(ldmsd_req_hdr_t resp, size_t len, uint32_t rsp_err)
{
	if (rsp_err) {
		resp_generic(resp, len, rsp_err);
		return;
	}

	ldmsd_req_attr_t attr = ldmsd_first_attr(resp);
	if (!attr->discrim)
		return;
	if (attr->attr_id != LDMSD_ATTR_JSON) {
		printf("Cannot interpret the result\n");
		return;
	}

	json_parser_t parser = NULL;
	json_entity_t json = NULL;
	json_entity_t smplr, sets, set;
	int rc, is_sync;

	parser = json_parser_new(0);
	if (!parser) {
		printf("Out of memory\n");
		return;
	}
	rc = json_parse_buffer(parser, (char*)attr->attr_value,
			strlen((char*)attr->attr_value), &json);
	if (rc) {
		printf("Error %d: cannot interpret the result.\n", rc);
		goto out;
	}

	if (json->type != JSON_LIST_VALUE)
		goto invalid_result_format;

	json_entity_t name, inst, plugn, interval, offset, sync, state;


	printf("%16s %16s %16s %-10s %-7s %10s %-8s %7s",
			"Name", "Plugin Instance", "Plugin",
			"Interval", "Offset", "Sync State",
			"State", "Sets");
	printf("--------------- ---------------- ---------------- ---------- "
			"-------- ----------- ------- --------------------");

	for (smplr = json_item_first(json); smplr; smplr = json_item_next(smplr)) {

		name = json_value_find(smplr, "name");
		inst = json_value_find(smplr, "instance");
		plugn = json_value_find(smplr, "plugin");
		interval = json_value_find(smplr, "interval_us");
		offset = json_value_find(smplr, "offset_us");
		sync = json_value_find(smplr, "synchronous");
		state = json_value_find(smplr, "state");

		if (!name || !inst || !plugn || !interval || !offset || !sync || !state)
			goto invalid_result_format;

		is_sync = json_value_bool(sync);
		printf("%16s %16s %16s %-10s %-7s %10s %-8s",
				json_value_str(name)->str,
				json_value_str(inst)->str,
				json_value_str(plugn)->str,
				json_value_str(interval)->str,
				json_value_str(offset)->str,
				((is_sync)?"True":"false"),
				json_value_str(state)->str);
		sets = json_value_find(smplr, "sets");

		if (!sets || (sets->type != JSON_LIST_VALUE))
			goto invalid_result_format;

		int cnt = 0;
		for (set = json_item_first(sets); set; set = json_item_next(set)) {
			if (cnt == 0)
				printf(" ");
			else
				printf(",");
			printf("%s", json_value_str(set)->str);
		}
		printf("\n");
	}
invalid_result_format:
	printf("Unrecognized result format\n");
out:
	if (parser)
		json_parser_free(parser);
	if (json)
		json_entity_free(json);
	return;
}

static void help_export()
{
	printf( "\nExport running configuration to a local file on the node the daemon is running.\n\n");
	printf( "By default, the environment variables set with the 'env' commands,\n"
		"the command-line options (given at the command line or with the 'set' command\n"
		"and the other configuration will be exported.\n"
		"If any of 'env', 'cmdline' and 'cfgcmd' is given, only those will be exported.\n");
	printf( "Parameters\n");
	printf( "     path=         The path to the exported file\n");
	printf( "     [env=]        true/false\n");
	printf( "     [cmdline=]    true/false\n");
	printf( "     [cfgcmd=]     true/false\n");
}

static void help_subscribe()
{
	printf( "\nSubscribe to a stream\n\n");
	printf( "The aggregator will listen for published data on the specified stream\n");
	printf( "Parameters\n");
	printf( "     name=     The stream name\n");
}

static void help_publish()
{
	printf( "\nPublish data to the named stream\n\n");
	printf( "Parameters\n");
	printf( "     name=     The stream name\n");
	printf( "     string=   The data to publish\n");
}

static void help_set()
{
	printf( "\n(Unsupported) Set command-line options\n\n"
		"It is only supported in the configuration files given at the command line.\n");
}

static int handle_set(struct ldmsctl_ctrl *ctrl, char *args)
{
	printf("Not supported in ldmsctl. It is only supported in config files "
			"given at the command line.\n");
	return 0;
}

static void help_listen()
{
	printf( "\n(Unsupported) Add a listening endpoint\n\n"
		"It is only supported in the configuration files given at the command line.\n");
}

static int handle_listen(struct ldmsctl_ctrl *ctrl, char *args)
{
	printf("Not supported in ldmsctl. It is only supported in config files "
			"given at the command line.\n");
	return 0;
}

static int handle_help(struct ldmsctl_ctrl *ctrl, char *args);
static int handle_source(struct ldmsctl_ctrl *ctrl, char *path);
static int handle_script(struct ldmsctl_ctrl *ctrl, char *cmd);

static struct command command_tbl[] = {
	{ "?",		handle_help,	NULL,		NULL },
	{ "config",	NULL,	help_config,		resp_generic },
	{ "daemon_exit",
			NULL,	help_daemon_exit,	resp_daemon_exit },
	{ "daemon_status",
			NULL,	help_daemon_status,	resp_daemon_status },
	{ "export",
			NULL,	help_export,		resp_generic },
	{ "failover_config",
			NULL,	help_failover_config,	resp_generic },
	{ "failover_peercfg_start",
			NULL,	help_failover_peercfg_start,	resp_generic },
	{ "failover_peercfg_stop",
			NULL,	help_failover_peercfg_stop,	resp_generic },
	{ "failover_start",
			NULL,	help_failover_start,	resp_generic },
	{ "failover_status",
			NULL,	help_failover_status,	resp_failover_status },
	{ "failover_stop",
			NULL,	help_failover_stop,	resp_generic },
	{ "greeting",	NULL,	help_greeting,		resp_greeting },
	{ "help",	handle_help,	NULL,		NULL },
	{ "list",	NULL,	help_list,		resp_list },
	{ "listen",	handle_listen,	help_listen,	NULL },
	{ "load",	NULL,	help_load,		resp_generic },
	{ "loglevel",	NULL,	help_loglevel,		resp_generic },
	{ "oneshot",	NULL,	help_oneshot,		resp_generic },
	{ "plugn_sets",	NULL,	help_plugn_sets,	resp_plugn_sets },
	{ "plugn_status",
			NULL,	help_plugn_status,	resp_plugn_status },
	{ "prdcr_add",	NULL,	help_prdcr_add,		resp_generic },
	{ "prdcr_del",	NULL,	help_prdcr_del,		resp_generic },
	{ "prdcr_set_status",
			NULL,	help_prdcr_set_status,	resp_prdcr_set_status },
	{ "prdcr_start",
			NULL,	help_prdcr_start,	resp_generic },
	{ "prdcr_start_regex",
			NULL,	help_prdcr_start_regex,	resp_generic },
	{ "prdcr_status",
			NULL,	help_prdcr_status,	resp_prdcr_status },
	{ "prdcr_stop",
			NULL,	help_prdcr_stop,	resp_generic },
	{ "prdcr_stop_regex",
			NULL,	help_prdcr_stop_regex,	resp_generic },
	{ "prdcr_subscribe",
			NULL,	help_prdcr_subscribe_regex,	resp_generic },
	{ "publish",
			NULL,	help_publish,		resp_generic },
	{ "quit",	handle_quit,	help_quit,	resp_generic },
	{ "script",	handle_script,	help_script,	resp_generic },
	{ "set",	handle_set,	help_set,	NULL },
	{ "set_route",	NULL,	help_set_route,		resp_set_route },
	{ "setgroup_add",
			NULL,	help_setgroup_add,	resp_generic },
	{ "setgroup_del",
			NULL,	help_setgroup_del,	resp_generic },
	{ "setgroup_ins",
			NULL,	help_setgroup_ins,	resp_generic },
	{ "setgroup_mod",
			NULL,	help_setgroup_mod,	resp_generic },
	{ "setgroup_rm",
			NULL,	help_setgroup_rm,	resp_generic },
	{ "smplr_add",	NULL,	help_smplr_add,		resp_generic },
	{ "smplr_del",	NULL,	help_smplr_del,		resp_generic },
	{ "smplr_start",
			NULL,	help_smplr_start,	resp_generic },
	{ "smplr_status",
			NULL,	help_smplr_status,	resp_smplr_status },
	{ "smplr_stop",	NULL,	help_smplr_stop,	resp_generic },
	{ "source",	handle_source,	help_source,	resp_generic },
	{ "strgp_add",	NULL,	help_strgp_add,		resp_generic },
	{ "strgp_del",	NULL,	help_strgp_del,		resp_generic },
	{ "strgp_metric_add",
			NULL,	help_strgp_metric_add,	resp_generic },
	{ "strgp_metric_del",
			NULL,	help_strgp_metric_del,	resp_generic },
	{ "strgp_prdcr_add",
			NULL,	help_strgp_prdcr_add,	resp_generic },
	{ "strgp_prdcr_del",
			NULL,	help_strgp_prdcr_del,	resp_generic },
	{ "strgp_start",
			NULL,	help_strgp_start,	resp_generic },
	{ "strgp_status",
			NULL,	help_strgp_status,	resp_strgp_status },
	{ "strgp_stop",	NULL,	help_strgp_stop,	resp_generic },
	{ "subscribe",	NULL,	help_subscribe,		resp_generic },
	{ "term",	NULL,	help_term,		resp_generic },
	{ "udata",	NULL,	help_udata,		resp_generic },
	{ "udata_regex",
			NULL,	help_udata_regex,	resp_generic },
	{ "updtr_add",	NULL,	help_updtr_add,		resp_generic },
	{ "updtr_del",	NULL,	help_updtr_del,		resp_generic },
	{ "updtr_match_add",
			NULL,	help_updtr_match_add,	resp_generic },
	{ "updtr_match_del",
			NULL,	help_updtr_match_del,	resp_generic },
	{ "updtr_prdcr_add",
			NULL,	help_updtr_prdcr_add,	resp_generic },
	{ "updtr_prdcr_del",
			NULL,	help_updtr_prdcr_del,	resp_generic },
	{ "updtr_start",
			NULL,	help_updtr_start,	resp_generic },
	{ "updtr_status",
			NULL,	help_updtr_status,	resp_updtr_status },
	{ "updtr_stop",	NULL,	help_updtr_stop,	resp_generic },
	{ "usage",	NULL,	help_usage,		resp_usage },
	{ "version",	NULL,	help_version,		resp_generic },
};

void __print_all_command()
{
	printf( "The available commands are as follows. To see help for\n"
		"a command, do 'help <command>'\n\n");
	size_t tbl_len = sizeof(command_tbl)/sizeof(command_tbl[0]);

	int max_width = 20;
	int i = 0;
	printf("%-*s", max_width, command_tbl[i].token);
	for (i = 1; i < tbl_len; i++) {
		printf("%-*s", max_width, command_tbl[i].token);
		if (i % 5 == 4)
			printf("\n");
	}
	printf("\n");
}

static int handle_help(struct ldmsctl_ctrl *ctrl, char *args)
{
	if (!args) {
		__print_all_command();
	} else {
		char *_args, *ptr;
		_args = strtok_r(args, " \t\n", &ptr);
		if (!_args) {
			__print_all_command();
			return 0;
		}

		struct command *help_cmd;
		help_cmd = bsearch(&_args, command_tbl, ARRAY_SIZE(command_tbl),
			     sizeof(*help_cmd), command_comparator);
		if (!help_cmd) {
			printf("Unrecognized command '%s'.\n", _args);
			return EINVAL;
		}
		if (help_cmd->help) {
			help_cmd->help();
		} else {
			printf("No help found for the command '%s'.\n",
					help_cmd->token);
		}
		return 0;
	}

	return 0;
}

static int __ldms_xprt_send(struct ldmsctl_ctrl *ctrl, ldmsd_req_hdr_t req, size_t len)
{
	char *req_buf = malloc(len);
	if (!req_buf) {
		printf("Out of memory\n");
		return ENOMEM;
	}

	memcpy(req_buf, req, len);
	int rc = ldms_xprt_send(ctrl->ldms_xprt.x, req_buf, len);
	free(req_buf);
	return rc;
}

static void __ldms_xprt_close(struct ldmsctl_ctrl *ctrl)
{
	sem_destroy(&ctrl->ldms_xprt.connected_sem);
	sem_destroy(&ctrl->ldms_xprt.recv_sem);
	ldms_xprt_close(ctrl->ldms_xprt.x);
}

static char *__ldms_xprt_recv(struct ldmsctl_ctrl *ctrl)
{
	sem_wait(&ctrl->ldms_xprt.recv_sem);
	if (!buffer) {
		return NULL;
	}
	return buffer;
}

static int __handle_cmd(struct ldmsctl_ctrl *ctrl, char *cmd_str)
{
	static int msg_no = 0;
	ldmsd_req_hdr_t request;
	struct ldmsd_req_array *req_array = NULL;
	size_t len;
	int rc, i;

	struct command key, *cmd;
	char *ptr, *args, *dummy;

	/* Strip the new-line character */
	char *newline = strrchr(cmd_str, '\n');
	if (newline)
		*newline = '\0';

	dummy = strdup(cmd_str);
	if (!dummy) {
		printf("Out of memory\n");
		exit(ENOMEM);
	}

	key.token = strtok_r(dummy, " \t\n", &ptr);
	args = strtok_r(NULL, "\n", &ptr);
	cmd = bsearch(&key, command_tbl, ARRAY_SIZE(command_tbl),
			sizeof(struct command), command_comparator);
	if (!cmd) {
		printf("Unrecognized command '%s'\n", key.token);
		return 0;
	}

	if (cmd->action) {
		(void)cmd->action(ctrl, args);
		free(dummy);
		return 0;
	}
	free(dummy);

	memset(buffer, 0, buffer_len);
	req_array = ldmsd_parse_config_str(cmd_str, msg_no,
					   ldms_xprt_msg_max(ctrl->ldms_xprt.x),
					   ldmsctl_log);
	if (!req_array) {
		printf("Failed to process the request. ");
		if (errno == ENOMEM)
			printf("Out of memory\n");
		else
			printf("Please make sure that there is no typo.\n");
		return EINVAL;
	}
	msg_no++;

	for (i = 0; i < req_array->num_reqs; i++) {
		request = req_array->reqs[i];
		len = ntohl(request->rec_len);

		rc = ctrl->send_req(ctrl, request, len);
		if (rc) {
			printf("Failed to send data to ldmsd. %s\n", strerror(errno));
			return rc;
		}
	}
	/*
	 * Send all the records and handle the response now.
	 */
	ldmsd_req_hdr_t resp;
	size_t req_hdr_sz = sizeof(*resp);
	size_t lbufsz = 1024;
	char *lbuf = malloc(lbufsz);
	if (!lbuf) {
		printf("Out of memory\n");
		exit(1);
	}

	char *rec;
	size_t reclen = 0;
	size_t msglen = 0;
	rc = 0;
	while (1) {
		resp = (ldmsd_req_hdr_t)ctrl->recv_resp(ctrl);
		if (!resp) {
			printf("Failed to receive the response\n");
			rc = -1;
			goto out;
		}
		if (ntohl(resp->flags) & LDMSD_REC_SOM_F) {
			reclen = ntohl(resp->rec_len);
			rec = (char *)resp;
		} else {
			reclen = ntohl(resp->rec_len) - req_hdr_sz;
			rec = (char *)(resp + 1);
		}
		if (lbufsz < msglen + reclen) {
			lbuf = realloc(lbuf, msglen + (reclen * 2));
			if (!lbuf) {
				printf("Out of memory\n");
				exit(1);
			}
			lbufsz = msglen + (reclen * 2);
			memset(&lbuf[msglen], 0, lbufsz - msglen);
		}
		memcpy(&lbuf[msglen], rec, reclen);
		msglen += reclen;
		if ((ntohl(resp->flags) & LDMSD_REC_EOM_F) != 0) {
			break;
		}
	}
	ldmsd_ntoh_req_msg((ldmsd_req_hdr_t)lbuf);

	cmd->resp((ldmsd_req_hdr_t)lbuf, msglen, resp->rsp_err);
out:
	free(lbuf);
	return rc;
}

void __ldms_event_cb(ldms_t x, ldms_xprt_event_t e, void *cb_arg)
{
	struct ldmsctl_ctrl *ctrl = cb_arg;
	switch (e->type) {
	case LDMS_XPRT_EVENT_CONNECTED:
		sem_post(&ctrl->ldms_xprt.connected_sem);
		break;
	case LDMS_XPRT_EVENT_REJECTED:
		printf("The connected request is rejected.\n");
		ldms_xprt_put(ctrl->ldms_xprt.x);
		exit(0);
	case LDMS_XPRT_EVENT_DISCONNECTED:
		ldms_xprt_put(ctrl->ldms_xprt.x);
		printf("The connection is disconnected.\n");
		exit(0);
	case LDMS_XPRT_EVENT_ERROR:
		printf("Connection error\n");
		exit(0);
		break;
	case LDMS_XPRT_EVENT_RECV:
		if (buffer_len < e->data_len) {
			free(buffer);
			buffer = malloc(e->data_len);
			if (!buffer) {
				printf("Out of memory\n");
				buffer = NULL;
				buffer_len = 0;
				ldms_xprt_close(ctrl->ldms_xprt.x);
				sem_post(&ctrl->ldms_xprt.recv_sem);
				break;
			}
			buffer_len = e->data_len;
		}
		memset(buffer, 0, buffer_len);
		memcpy(buffer, e->data, e->data_len);
		sem_post(&ctrl->ldms_xprt.recv_sem);
		break;
	default:
		assert(0);
	}
}

struct ldmsctl_ctrl *__ldms_xprt_ctrl(const char *host, const char *port,
			const char *xprt, const char *auth,
			struct attr_value_list *auth_opt)
{
	struct ldmsctl_ctrl *ctrl;
	int rc;

	ctrl = calloc(1, sizeof(*ctrl));
	if (!ctrl)
		return NULL;

	sem_init(&ctrl->ldms_xprt.connected_sem, 0, 0);
	sem_init(&ctrl->ldms_xprt.recv_sem, 0, 0);

	ctrl->send_req = __ldms_xprt_send;
	ctrl->recv_resp = __ldms_xprt_recv;
	ctrl->close = __ldms_xprt_close;

	ctrl->ldms_xprt.x = ldms_xprt_new_with_auth(xprt, NULL, auth, auth_opt);
	if (!ctrl->ldms_xprt.x) {
		printf("Failed to create an ldms transport. %s\n",
						strerror(errno));
		return NULL;
	}

	rc = ldms_xprt_connect_by_name(ctrl->ldms_xprt.x, host, port,
						__ldms_event_cb, ctrl);
	if (rc) {
		ldms_xprt_put(ctrl->ldms_xprt.x);
		sem_destroy(&ctrl->ldms_xprt.connected_sem);
		sem_destroy(&ctrl->ldms_xprt.recv_sem);
		free(ctrl);
		return NULL;
	}

	sem_wait(&ctrl->ldms_xprt.connected_sem);
	return ctrl;
}

static int handle_source(struct ldmsctl_ctrl *ctrl, char *path)
{
	FILE *f;
	int rc = 0;
	ssize_t cnt = 0;

	f = fopen(path, "r");
	if (!f) {
		rc = errno;
		printf("Error %d: Failed to open the configuration file '%s'\n",
								rc, path);
		return rc;
	}
	fseek(f, 0, SEEK_SET);
	cnt = getline(&linebuf, &linebuf_len, f);
	while (cnt != -1) {
		rc = __handle_cmd(ctrl, linebuf);
		if (rc)
			break;
		cnt = getline(&linebuf, &linebuf_len, f);
	}
	fclose(f);
	return rc;
}

static int handle_script(struct ldmsctl_ctrl *ctrl, char *cmd)
{
	int rc = 0;
	FILE *f;
	ssize_t cnt;

	f = popen(cmd, "r");
	if (!f) {
		rc = errno;
		printf("Error %d: Failed to open pipe of the command '%s'\n",
								rc, cmd);

		return rc;
	}

	cnt = getline(&linebuf, &linebuf_len, f);
	while (cnt != -1) {
		rc = __handle_cmd(ctrl, linebuf);
		if (rc)
			break;
		cnt = getline(&linebuf, &linebuf_len, f);
	}
	pclose(f);
	return rc;
}

int main(int argc, char *argv[])
{
	int op;
	char *host, *port, *auth, *sockname, *xprt;
	char *lval, *rval;
	host = port = sockname = xprt = NULL;
	char *source, *script;
	source = script = NULL;
	int rc, is_inband = 1;
	struct attr_value_list *auth_opt = NULL;
	const int AUTH_OPT_MAX = 128;
	ssize_t cnt;

	auth = "none";

	auth_opt = av_new(AUTH_OPT_MAX);
	if (!auth_opt) {
		printf("ERROR: Not enough memory.\n");
		exit(1);
	}

	while ((op = getopt(argc, argv, FMT)) != -1) {
		switch (op) {
		case 'h':
			host = strdup(optarg);
			break;
		case 'p':
			port = strdup(optarg);
			break;
		case 'x':
			xprt = strdup(optarg);
			break;
		case 'a':
			auth = strdup(optarg);
			break;
		case 'A':
			/* (multiple) auth options */
			lval = strtok(optarg, "=");
			if (!lval) {
				printf("ERROR: Expecting -A name=value\n");
				exit(1);
			}
			rval = strtok(NULL, "");
			if (!rval) {
				printf("ERROR: Expecting -A name=value\n");
				exit(1);
			}
			if (auth_opt->count == auth_opt->size) {
				printf("ERROR: Too many auth options\n");
				exit(1);
			}
			auth_opt->list[auth_opt->count].name = lval;
			auth_opt->list[auth_opt->count].value = rval;
			auth_opt->count++;
			break;
		case 's':
			source = strdup(optarg);
			break;
		case 'X':
			script = strdup(optarg);
			break;
		default:
			usage(argv);
			exit(0);
		}
	}

	buffer_len = LDMSD_CFG_FILE_XPRT_MAX_REC;
	buffer = malloc(buffer_len);
	linebuf = NULL;
	linebuf_len = 0;
	if (!buffer) {
		printf("Out of memory\n");
		exit(ENOMEM);
	}

	if (!host || !port || !xprt)
		goto arg_err;

	struct ldmsctl_ctrl *ctrl;
	if (is_inband) {
		ctrl = __ldms_xprt_ctrl(host, port, xprt, auth, auth_opt);
		if (!ctrl) {
			printf("Failed to connect to ldmsd.\n");
			exit(-1);
		}
	}
	/* At this point ldmsctl is connected to the ldmsd */

	if (source) {
		(void) handle_source(ctrl, source);
		return 0;
	}

	if (script) {
		(void) handle_script(ctrl, script);
		return 0;
	}

	do {
#ifdef HAVE_LIBREADLINE
#ifndef HAVE_READLINE_HISTORY
		if (linebuf != NULL) {
			free(linebuf)); /* previous readline output must be freed if not in history */
			linebuf = NULL;
			linebuf_len = 0;
		}
#endif /* HAVE_READLINE_HISTORY */
		if (isatty(0)) {
			linebuf = readline("ldmsctl> ");
			cnt = linebuf?strlen(linebuf):-1;
		} else {
			cnt = getline(&linebuf, &linebuf_len, stdin);
		}
#else /* HAVE_LIBREADLINE */
		if (isatty(0)) {
			fputs("ldmsctl> ", stdout);
		}
		cnt = getline(&linebuf, &linebuf_len, stdin);
#endif /* HAVE_LIBREADLINE */
		if (cnt == -1)
			break;
		if (linebuf && linebuf[0] == '\0')
			continue;
#ifdef HAVE_READLINE_HISTORY
		add_history(linebuf);
#endif /* HAVE_READLINE_HISTORY */

		rc = __handle_cmd(ctrl, linebuf);
		if (rc) {
			if (isatty(0)) {
				/* Not exit in an interactive session */
				continue;
			} else {
				break;
			}
		}
	} while (linebuf);

	ctrl->close(ctrl);
	return 0;
arg_err:
	printf("Please specify the host, port and transport type.\n");
	usage(argv);
	return 0;
}
