/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2010-2018 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2010-2018 Open Grid Computing, Inc. All rights reserved.
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

#include <unistd.h>
#include <inttypes.h>
#include <stdarg.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/un.h>
#include <ctype.h>
#include <netdb.h>
#include <dlfcn.h>
#include <assert.h>
#include <libgen.h>
#include <glob.h>
#include <time.h>
#include <coll/rbt.h>
#include <coll/str_map.h>
#include <ovis_util/util.h>
#include <mmalloc/mmalloc.h>
#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plugin.h"
#include "ldms_xprt.h"
#include "ldmsd_request.h"
#include "config.h"

pthread_mutex_t host_list_lock = PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(host_list_s, hostspec) host_list;
pthread_mutex_t sp_list_lock = PTHREAD_MUTEX_INITIALIZER;

#define LDMSD_PLUGIN_LIBPATH_MAX	1024

void ldmsd_cfg_xprt_cleanup(ldmsd_cfg_xprt_t xprt)
{
	free(xprt);
}

void ldmsd_cfg_xprt_ldms_cleanup(ldmsd_cfg_xprt_t xprt)
{
	ldms_xprt_put(xprt->ldms.ldms);
	ldmsd_cfg_xprt_cleanup(xprt);
}

const char *prdcr_state_str(enum ldmsd_prdcr_state state)
{
	switch (state) {
	case LDMSD_PRDCR_STATE_STOPPED:
		return "STOPPED";
	case LDMSD_PRDCR_STATE_DISCONNECTED:
		return "DISCONNECTED";
	case LDMSD_PRDCR_STATE_CONNECTING:
		return "CONNECTING";
	case LDMSD_PRDCR_STATE_CONNECTED:
		return "CONNECTED";
	case LDMSD_PRDCR_STATE_STOPPING:
		return "STOPPING";
	}
	return "BAD STATE";
}


const char *match_selector_str(enum ldmsd_name_match_sel sel)
{
	switch (sel) {
	case LDMSD_NAME_MATCH_INST_NAME:
		return "INST_NAME";
	case LDMSD_NAME_MATCH_SCHEMA_NAME:
		return "SCHEMA_NAME";
	}
	return "BAD SELECTOR";
}

int ldmsd_compile_regex(regex_t *regex, const char *regex_str,
				char *errbuf, size_t errsz)
{
	memset(regex, 0, sizeof *regex);
	int rc = regcomp(regex, regex_str, REG_EXTENDED | REG_NOSUB);
	if (rc) {
		snprintf(errbuf, errsz, "22");
		(void)regerror(rc,
			       regex,
			       &errbuf[2],
			       errsz - 2);
		strcat(errbuf, "\n");
	}
	return rc;
}

/*
 * Load a plugin
 */
int ldmsd_load_plugin(const char *inst_name, const char *plugin_name,
		      char *errstr, size_t errlen)
{
	ldmsd_plugin_inst_t inst = ldmsd_plugin_inst_load(inst_name,
							  plugin_name,
							  errstr,
							  errlen);
	if (!inst) {
		if (errno == EEXIST) {
			snprintf(errstr, errlen, "Plugin '%s' already loaded",
				 inst_name);
		} else {
			snprintf(errstr, errlen, "Plugin '%s' load error: %d",
				 inst_name, errno);
		}
		return errno;
	}
	return 0;
}

/*
 * Destroy and unload the plugin
 */
int ldmsd_term_plugin(const char *inst_name)
{
	ldmsd_plugin_inst_t inst = ldmsd_plugin_inst_find(inst_name);
	if (!inst)
		return ENOENT;
	ldmsd_plugin_inst_del(inst);
	ldmsd_plugin_inst_put(inst); /* put ref from find */
	return 0;
}

int _ldmsd_set_udata(ldms_set_t set, char *metric_name, uint64_t udata,
						char err_str[LEN_ERRSTR])
{
	int i = ldms_metric_by_name(set, metric_name);
	if (i < 0) {
		snprintf(err_str, LEN_ERRSTR, "Metric '%s' not found.",
			 metric_name);
		return ENOENT;
	}

	ldms_metric_user_data_set(set, i, udata);
	return 0;
}

int ldmsd_set_access_check(ldms_set_t set, int acc, ldmsd_sec_ctxt_t ctxt)
{
	uid_t uid;
	gid_t gid;
	int perm;

	uid = ldms_set_uid_get(set);
	gid = ldms_set_gid_get(set);
	perm = ldms_set_perm_get(set);

	return ovis_access_check(ctxt->crd.uid, ctxt->crd.gid, acc,
				 uid, gid, perm);
}

/*
 * Assign user data to a metric
 */
int ldmsd_set_udata(const char *set_name, const char *metric_name,
		    int64_t udata, ldmsd_sec_ctxt_t sctxt)
{
	ldms_set_t set;
	int rc = 0;

	set = ldms_set_by_name(set_name);
	if (!set) {
		rc = ENOENT;
		goto out;
	}

	rc = ldmsd_set_access_check(set, 0222, sctxt);
	if (rc)
		goto out;

	int mid = ldms_metric_by_name(set, metric_name);
	if (mid < 0) {
		rc = -ENOENT;
		goto out;
	}

	ldms_metric_user_data_set(set, mid, udata);

out:
	if (set)
		ldms_set_put(set);
	return rc;
}

int ldmsd_set_udata_regex(char *set_name, char *regex_str,
		int64_t base, int64_t inc, char *errstr, size_t errsz,
		ldmsd_sec_ctxt_t sctxt)
{
	int rc = 0;
	ldms_set_t set;
	set = ldms_set_by_name(set_name);
	if (!set) {
		snprintf(errstr, errsz, "Set '%s' not found.", set_name);
		rc = ENOENT;
		goto out;
	}

	rc = ldmsd_set_access_check(set, 0222, sctxt);
	if (rc) {
		snprintf(errstr, errsz, "Permission denied.");
		goto out;
	}

	regex_t regex;
	rc = ldmsd_compile_regex(&regex, regex_str, errstr, errsz);
	if (rc)
		goto out;

	int i;
	uint64_t udata = base;
	char *mname;
	for (i = 0; i < ldms_set_card_get(set); i++) {
		mname = (char *)ldms_metric_name_get(set, i);
		if (0 == regexec(&regex, mname, 0, NULL, 0)) {
			ldms_metric_user_data_set(set, i, udata);
			udata += inc;
		}
	}
	regfree(&regex);

out:
	if (set)
		ldms_set_put(set);
	return rc;
}

static void __config_file_msgno_get(uint64_t file_no, uint32_t obj_cnt, uint32_t *msg_no)
{
	*msg_no = obj_cnt;
}

//static uint16_t __config_file_msgno2lineno(uint32_t msgno)
//{
//	return msgno & 0xFFFF;
//}

static int log_response_fn(ldmsd_cfg_xprt_t xprt, char *data, size_t data_len)
{
	json_entity_t rsp = NULL, type, errcode, msg;
	json_parser_t parser = NULL;
	char *type_s;
	int rc;
	ldmsd_rec_hdr_t req_reply = (ldmsd_rec_hdr_t)data;
	ldmsd_ntoh_rec_hdr(req_reply);

	parser = json_parser_new(0);
	if (!parser) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return ENOMEM;
	}

	rc = json_parse_buffer(parser, (char *)(req_reply + 1),
					req_reply->rec_len, &rsp);
	if (rc) {
		ldmsd_log(LDMSD_LERROR, "Failed to parse a response object\n");
		goto out;
	}
	type = json_value_find(rsp, "type");
	type_s = json_value_str(type)->str;

	if (0 == strncmp("err", type_s, 3)) {
		errcode = json_value_find(rsp, "errcode");
		if (0 != json_value_int(errcode)) {
			msg = json_value_find(rsp, "msg");
			xprt->file.errcode = json_value_int(errcode);
			ldmsd_log(LDMSD_LERROR, "%s: Error %" PRIu64 ": %s\n",
					xprt->file.filename,
					json_value_int(errcode),
					json_value_str(msg)->str);
		}
	} else if (0 == strncmp("info", type_s, 4)) {
		/* for debugging */
		jbuf_t jb = json_entity_dump(NULL, rsp);
		if (!jb) {
			ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
			return ENOMEM;
		}
		ldmsd_log(LDMSD_LDEBUG, "%s\n", jb->buf);
	} else {
		ldmsd_log(LDMSD_LERROR, "Unexpected response '%s'\n", type_s);
	}

out:
	if (parser)
		json_parser_free(parser);
	if (rsp)
		json_entity_free(rsp);
	return 0;
}

/* find # standing alone in a line, indicating rest of line is comment.
 * e.g. ^# rest is comment
 * or: ^         # indented comment
 * or: ^dosomething foo a=b c=d #rest is comment
 * or: ^dosomething foo a=b c=d # rest is comment
 * but not: ^dosomething foo a=#channel c=foo#disk1"
 * \return pointer of first # comment or NULL.
 */
char *find_comment(const char *line)
{
	char *s = (char *)line;
	int leadingspc = 1;
	while (*s != '\0') {
		if (*s == '#' && leadingspc)
			return s;
		if (isspace(*s))
			leadingspc = 1;
		else
			leadingspc = 0;
		s++;
	}
	return NULL;
}

/*
 * rc = 0, filter applied OK
 * rc > 0, rc == -errno, error
 * rc = -1, filter not applied (but not an error)
 */
int __req_filter(ldmsd_req_ctxt_t reqc, void *ctxt)
{
	int rc = 0;

//	switch (reqc->req_id) {
//	case LDMSD_FAILOVER_START_REQ:
//		ldmsd_use_failover = 1;
//	case LDMSD_PLUGN_CONFIG_REQ:
//	case LDMSD_PRDCR_START_REGEX_REQ:
//	case LDMSD_PRDCR_START_REQ:
//	case LDMSD_UPDTR_START_REQ:
//	case LDMSD_STRGP_START_REQ:
//	case LDMSD_SMPLR_START_REQ:
//	case LDMSD_SETGROUP_ADD_REQ:
//		reqc->flags |= LDMSD_REQ_DEFER_FLAG;
//		break;
//	default:
//		break;
//	}
	return rc;
}

int process_config_file(const char *path, int trust)
{
	static uint64_t file_no = 1; /* Config File ID */
	int rc = 0;
	long fsize;
	char *buffer;
	ldmsd_cfg_xprt_t xprt = NULL;
	json_parser_t parser;
	json_entity_t json = NULL, e;
	jbuf_t jbuf = NULL;
	size_t hdr_len;
	uint32_t cnt = 0;
	struct ldmsd_rec_hdr_s *request;
	ldmsd_req_ctxt_t reqc;

	FILE *f = fopen(path, "r");
	if (!f) {
		ldmsd_log(LDMSD_LERROR, "Failed to open config file '%s'\n", path);
		return errno;
	}
	rc = fseek(f, 0, SEEK_END);
	if (rc) {
		ldmsd_log(LDMSD_LERROR, "fseek failed with error %d\n", rc);
		fclose(f);
		return rc;
	}
	fsize = ftell(f);

	rc = fseek(f, 0, SEEK_SET);
	if (rc) {
		ldmsd_log(LDMSD_LERROR, "fseek failed with error %d\n", rc);
		fclose(f);
		return rc;
	}

	buffer = malloc(fsize + 1);
	if (!buffer) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		goto out;
	}
	rc = fread(buffer, 1, fsize, f);
	if (rc < fsize) {
		rc = ferror(f);
		ldmsd_log(LDMSD_LERROR, "fread failed with error %d\n", rc);
		clearerr(f);
		fclose(f);
		return rc;
	}
	fclose(f);

	buffer[fsize] = 0;

	parser = json_parser_new(0);
	if (!parser) {
		ldmsd_log(LDMSD_LERROR, "Out of memory\n");
		return ENOMEM;
	}
	rc = json_parse_buffer(parser, buffer, fsize + 1, &json);
	if (rc) {
		ldmsd_log(LDMSD_LERROR, "Failed to parse '%s'\n", path);
		json_parser_free(parser);
		return rc;
	}
	free(buffer);
	json_parser_free(parser);

	xprt = ldmsd_cfg_xprt_new();
	if (!xprt) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return ENOMEM;
	}

	xprt->type = LDMSD_CFG_XPRT_CONFIG_FILE;
	xprt->file.filename = path;
	xprt->file.errcode = 0;
	xprt->send_fn = log_response_fn;
	xprt->max_msg = LDMSD_CFG_FILE_XPRT_MAX_REC;
	xprt->trust = trust;
	ref_init(&xprt->ref, "create", (ref_free_fn_t)ldmsd_cfg_xprt_cleanup, xprt);

	hdr_len = sizeof(*request);
	jbuf = jbuf_new();
	if (!jbuf) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		rc = ENOMEM;
		goto out;
	}
	request = (ldmsd_rec_hdr_t)jbuf->buf;
	jbuf->cursor = hdr_len;
	for (e = json_item_first(json); e; e = json_item_next(e)) {
		cnt++;
		jbuf = json_entity_dump(jbuf, e);
		if (!jbuf) {
			ldmsd_log(LDMSD_LERROR, "Failed to dump a JSON entity\n");
			rc = errno;
			goto out;
		}
		request->rec_len = jbuf->cursor;
		request->flags = LDMSD_REC_SOM_F | LDMSD_REC_EOM_F;
		request->type = LDMSD_MSG_TYPE_REQ;
		__config_file_msgno_get(file_no, cnt, &request->msg_no);
		reqc = ldmsd_handle_record(request, xprt);
		if (!reqc) {
			rc = errno;
			goto out;
		}

		rc = ldmsd_process_msg_request(reqc);
		/* stop processing the config file if there is a config error */
		if (xprt->file.errcode)
			rc = (int)xprt->file.errcode;
		if (rc)
			goto out;
		/* reset jbuf to contain only the record header */
		jbuf->cursor = hdr_len;
	}
out:
	if (json)
		json_entity_free(json);
	if (jbuf)
		jbuf_free(jbuf);
	if (xprt)
		ldmsd_cfg_xprt_ref_put(xprt, "create");
	return rc;
}

//int process_config_file(const char *path, int *lno, int trust)
//{
//	static uint16_t file_no = 0; /* Config file ID */
//	static int have_cmdline = 0;
//	static int have_cfgcmd = 0;
//	file_no++;
//	uint32_t msg_no; /* file_no:line_no */
//	uint16_t lineno = 0; /* The max number of lines is 65536. */
//	int rc = 0;
//	int i;
//	FILE *fin = NULL;
//	char *buff = NULL;
//	char *line = NULL;
//	char *tmp;
//	size_t line_sz = 0;
//	char *comment;
//	ssize_t off = 0;
//	ssize_t cnt;
//	size_t buf_len = 0;
//	struct ldmsd_cfg_xprt_s xprt;
//	ldmsd_rec_hdr_t request;
//	struct ldmsd_req_array *req_array = NULL;
//	ldmsd_req_filter_fn req_filter_fn;
//
//	if (ldmsd_is_initialized())
//		req_filter_fn = NULL;
//	else
//		req_filter_fn = __req_filter;
//
//	line = malloc(LDMSD_CFG_FILE_XPRT_MAX_REC);
//	if (!line) {
//		rc = errno;
//		ldmsd_log(LDMSD_LERROR, "Out of memory\n");
//		goto cleanup;
//	}
//	line_sz = LDMSD_CFG_FILE_XPRT_MAX_REC;
//
//	fin = fopen(path, "rt");
//	if (!fin) {
//		rc = errno;
//		strerror_r(rc, line, line_sz - 1);
//		ldmsd_log(LDMSD_LERROR, "Failed to open the config file '%s'. %s\n",
//				path, buff);
//		goto cleanup;
//	}
//
//	xprt.type = LDMSD_CFG_XPRT_CONFIG_FILE;
//	xprt.file.filename = path;
//	xprt.send_fn = log_response_fn;
//	xprt.max_msg = LDMSD_CFG_FILE_XPRT_MAX_REC;
//	xprt.trust = trust;
//	xprt.rsp_err = 0;
//
//next_line:
//	errno = 0;
//	if (buff)
//		memset(buff, 0, buf_len);
//	cnt = getline(&buff, &buf_len, fin);
//	if ((cnt == -1) && (0 == errno))
//		goto cleanup;
//
//	lineno++;
//	tmp = buff;
//	comment = find_comment(tmp);
//
//	if (comment)
//		*comment = '\0';
//
//	/* Get rid of trailing spaces */
//	while (cnt && isspace(tmp[cnt-1]))
//		cnt--;
//
//	if (!cnt) {
//		/* empty string */
//		goto parse;
//	}
//
//	tmp[cnt] = '\0';
//
//	/* Get rid of leading spaces */
//	while (isspace(*tmp)) {
//		tmp++;
//		cnt--;
//	}
//
//	if (!cnt) {
//		/* empty buffer */
//		goto parse;
//	}
//
//	if (tmp[cnt-1] == '\\') {
//		if (cnt == 1)
//			goto parse;
//	}
//
//	if (cnt + off > line_sz) {
//		line = realloc(line, ((cnt + off)/line_sz + 1) * line_sz);
//		if (!line) {
//			rc = errno;
//			ldmsd_log(LDMSD_LERROR, "Out of memory\n");
//			goto cleanup;
//		}
//		line_sz = ((cnt + off)/line_sz + 1) * line_sz;
//	}
//	off += snprintf(&line[off], line_sz, "%s", tmp);
//
//	/* attempt to merge multiple lines together */
//	if (off > 0 && line[off-1] == '\\') {
//		line[off-1] = ' ';
//		goto next_line;
//	}
//
//parse:
//	if (!off)
//		goto next_line;
//	msg_no = __config_file_msgno_get(file_no, lineno);
//	req_array = ldmsd_parse_config_str(line, msg_no, xprt.max_msg, ldmsd_log);
//	if (!req_array) {
//		rc = errno;
//		ldmsd_log(LDMSD_LERROR, "At line %d (%s): Failed to parse "
//				"config line. %s\n", lineno, path, strerror(rc));
//		goto cleanup_line;
//	}
//	for (i = 0; i < req_array->num_reqs; i++) {
//		request = req_array->reqs[i];
//		if (!ldmsd_is_initialized()) {
//			/*
//			 * Check the command type orders in the configuration files.
//			 * The order is as follows.
//			 * 1. Environment variables (LDMSD_ENV_REQ)
//			 * 2. Command-line options (LDMSD_CMD_LINE_SET_REQ)
//			 * 3. Configuration commands (Other requests, including 'listen')
//			 */
//			uint32_t req_id = ntohl(request->req_id);
//			if (req_id == LDMSD_ENV_REQ) {
//				if (have_cmdline || have_cfgcmd) {
//					ldmsd_log(LDMSD_LERROR,
//							"At line %d (%s): "
//							"The environment variable "
//							"is given after "
//							"a command-line option or"
//							"a config command.\n",
//							lineno, path);
//					rc = EINVAL;
//					goto cleanup_record;
//				}
//			} else if ((req_id == LDMSD_CMD_LINE_SET_REQ) ||
//					(req_id == LDMSD_LISTEN_REQ)) {
//				have_cmdline = 1;
//				if (have_cfgcmd) {
//					ldmsd_log(LDMSD_LERROR,
//						"At line %d (%s): "
//						"The set or listen command "
//						"is given after a configuration "
//						"command, e.g., load, prdcr_#.\n",
//						lineno, path);
//					rc = EINVAL;
//					goto cleanup_record;
//				}
//			} else if (req_id == LDMSD_INCLUDE_REQ) {
//				/* do nothing */
//			} else {
//				/*
//				 * The other commands are all
//				 * config commands.
//				 */
//				have_cfgcmd = 1;
//			}
//		}
//		rc = ldmsd_process_msg_request(&xprt, request, req_filter_fn);
//cleanup_record:
//		free(request);
//		if ((rc || xprt.rsp_err) && !ldmsd_is_check_syntax()) {
//			if (!rc)
//				rc = xprt.rsp_err;
//			goto cleanup_line;
//		}
//	}
//
//cleanup_line:
//	msg_no += 1;
//
//	off = 0;
//	if (req_array) {
//		free(req_array);
//		req_array = NULL;
//	}
//
//	if (ldmsd_is_check_syntax())
//		goto next_line;
//	if (!rc && !xprt.rsp_err)
//		goto next_line;
//
//cleanup:
//	if (fin)
//		fclose(fin);
//	if (buff)
//		free(buff);
//	if (line)
//		free(line);
//	if (lno)
//		*lno = lineno;
//	if (req_array) {
//		while (i < req_array->num_reqs) {
//			free(req_array->reqs[i]);
//			i++;
//		}
//		free(req_array);
//	}
//	return rc;
//}
//
//int __req_deferred_start_regex(ldmsd_rec_hdr_t req, ldmsd_cfgobj_type_t type)
//{
//	regex_t regex = {0};
//	ldmsd_req_attr_t attr;
//	ldmsd_cfgobj_t obj;
//	int rc;
//	char *val;
//	attr = ldmsd_req_attr_get_by_id((void*)req, LDMSD_ATTR_REGEX);
//	if (!attr) {
//		ldmsd_log(LDMSD_LERROR, "`regex` attribute is required.\n");
//		return EINVAL;
//	}
//	val = str_repl_env_vars((char *)attr->attr_value);
//	if (!val) {
//		ldmsd_log(LDMSD_LERROR, "Not enough memory.\n");
//		return ENOMEM;
//	}
//	rc = regcomp(&regex, val, REG_NOSUB);
//	if (rc) {
//		ldmsd_log(LDMSD_LERROR, "Bad regex: %s\n", val);
//		free(val);
//		return EBADMSG;
//	}
//	free(val);
//	ldmsd_cfg_lock(type);
//	LDMSD_CFGOBJ_FOREACH(obj, type) {
//		rc = regexec(&regex, obj->name, 0, NULL, 0);
//		if (rc == 0) {
//			obj->perm |= LDMSD_PERM_DSTART;
//		}
//	}
//	ldmsd_cfg_unlock(type);
//	return 0;
//}
//
//int __req_deferred_start(ldmsd_rec_hdr_t req, ldmsd_cfgobj_type_t type)
//{
//	ldmsd_req_attr_t attr;
//	ldmsd_cfgobj_t obj;
//	char *name;
//	attr = ldmsd_req_attr_get_by_id((void*)req, LDMSD_ATTR_NAME);
//	if (!attr) {
//		ldmsd_log(LDMSD_LERROR, "`name` attribute is required.\n");
//		return EINVAL;
//	}
//	name = str_repl_env_vars((char *)attr->attr_value);
//	if (!name) {
//		ldmsd_log(LDMSD_LERROR, "Not enough memory.\n");
//		return ENOMEM;
//	}
//	obj = ldmsd_cfgobj_find(name, type);
//	if (!obj) {
//		ldmsd_log(LDMSD_LERROR, "Config object not found: %s\n", name);
//		free(name);
//		return ENOENT;
//	}
//	free(name);
//	obj->perm |= LDMSD_PERM_DSTART;
//	ldmsd_cfgobj_put(obj);
//	return 0;
//}

extern int __ldmsd_req_ctxt_free_nolock(ldmsd_req_ctxt_t reqc);

/*
 * Process all outstanding request contexts in the request tree.
 */
int ldmsd_process_deferred_act_objs(int (*filter)(ldmsd_cfgobj_t))
{
	int rc = 0;
//	ldmsd_req_ctxt_t reqc;
//
//	ldmsd_req_ctxt_tree_lock(LDMSD_REQ_CTXT_REQ);
//	while ((reqc = ldmsd_req_ctxt_first(LDMSD_REQ_CTXT_REQ))) {
//		rc = ldmsd_process_cfgobj_requests(reqc);
//		if (rc)
//			goto out;
//		(void)__ldmsd_req_ctxt_free_nolock(reqc);
//	}
//out:
//	ldmsd_req_ctxt_tree_unlock(LDMSD_REQ_CTXT_REQ);
	return rc;
}

int __our_cfgobj_filter(ldmsd_cfgobj_t obj)
{
//	if (!cfgobj_is_failover(obj) && (obj->perm & LDMSD_PERM_DSTART))
//		return 0;
//	return -1;
	/*
	 * TODO: TEMP
	 */
	if (obj->perm & LDMSD_PERM_DSTART)
		return 0;
	return -1;
}

/*
 * ldmsd config start prodcedure
 */
int ldmsd_ourcfg_start_proc()
{
	int rc;
	rc = ldmsd_process_deferred_act_objs(__our_cfgobj_filter);
	if (rc) {
		exit(100);
	}
	return 0;
}

static inline void __log_sent_req(ldmsd_cfg_xprt_t xprt, ldmsd_rec_hdr_t req)
{
	if (!ldmsd_req_debug) /* defined in ldmsd_request.c */
		return;
	/* req is in network byte order */
	struct ldmsd_rec_hdr_s hdr;
	hdr.type = ntohl(req->type);
	hdr.flags = ntohl(req->flags);
	hdr.msg_no = ntohl(req->msg_no);
	hdr.rec_len = ntohl(req->rec_len);
	switch (hdr.type) {
	case LDMSD_MSG_TYPE_REQ:
		ldmsd_lall("sending %s msg_no: %d:%lu, flags: %#o, "
			   "rec_len: %u\n",
			   "AAAAAAAAAAA", /* TODO: fix this */
			   hdr.msg_no, (uint64_t)(unsigned long)xprt->xprt,
			   hdr.flags, hdr.rec_len);
		break;
	case LDMSD_MSG_TYPE_RESP:
		ldmsd_lall("sending RESP msg_no: %d, rsp_err: %d, flags: %#o, "
			   "rec_len: %u\n",
			   hdr.msg_no,
			   256, /* TODO: fix this */
			   hdr.flags, hdr.rec_len);
		break;
	default:
		ldmsd_lall("sending BAD REQUEST\n");
	}
}

static int send_ldms_fn(ldmsd_cfg_xprt_t xprt, char *data, size_t data_len)
{
	__log_sent_req(xprt, (void*)data);
	return ldms_xprt_send(xprt->ldms.ldms, data, data_len);
}

void ldmsd_recv_msg(ldms_t x, char *data, size_t data_len)
{
	ldmsd_rec_hdr_t rec = (ldmsd_rec_hdr_t)data;
	char *errstr;
	ldmsd_cfg_xprt_t xprt;
	ldmsd_req_ctxt_t reqc;
	int rc;

	xprt = ldmsd_cfg_xprt_ldms_new(x);
	if (!xprt) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return;
	}
	ldmsd_ntoh_rec_hdr(rec);

	if (rec->rec_len > xprt->max_msg) {
		/* Send the record length advice */
		rc = ldmsd_send_err_rec_adv(xprt, rec->msg_no, xprt->max_msg);
		if (rc)
			goto oom;
		goto out;
	}

	errno = 0;
	reqc = ldmsd_handle_record(rec, xprt);
	if (!reqc)
		goto out;
	ldmsd_req_ctxt_ref_get(reqc, "handle");
	switch (rec->type) {
	case LDMSD_MSG_TYPE_REQ:
		rc = ldmsd_process_msg_request(reqc);
		break;
	case LDMSD_MSG_TYPE_RESP:
		rc = ldmsd_process_msg_response(reqc);
		break;
	default:
		errstr = "ldmsd received an unrecognized request type";
		ldmsd_log(LDMSD_LERROR, "%s\n", errstr);
		goto err;
	}
	ldmsd_req_ctxt_ref_put(reqc, "handle");
	if (rc) {
		/*
		 * Do nothing.
		 *
		 * Assume that the error message was logged and/or sent
		 * to the peer.
		 */
	}

out:
	ldmsd_cfg_xprt_ref_put(xprt, "create");
	return;

oom:
	errstr = "ldmsd out of memory";
	ldmsd_log(LDMSD_LCRITICAL, "%s\n", errstr);
err:
	__ldmsd_send_error(xprt, rec->msg_no, NULL, rc, errstr);
	ldmsd_cfg_xprt_ref_put(xprt, "create");
	return;
}

static void __listen_connect_cb(ldms_t x, ldms_xprt_event_t e, void *cb_arg)
{
	switch (e->type) {
	case LDMS_XPRT_EVENT_CONNECTED:
		break;
	case LDMS_XPRT_EVENT_DISCONNECTED:
	case LDMS_XPRT_EVENT_REJECTED:
	case LDMS_XPRT_EVENT_ERROR:
		/* TODO: cleanup all resources referenced to this endpoint */
		ldms_xprt_put(x);
		break;
	case LDMS_XPRT_EVENT_RECV:
		ldmsd_recv_msg(x, e->data, e->data_len);
		break;
	default:
		assert(0);
		break;
	}
}

int listen_on_ldms_xprt(ldmsd_listen_t listen)
{
	int ret;
	struct sockaddr_in sin;

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = 0;
	sin.sin_port = htons(listen->port_no);
	ret = ldms_xprt_listen(listen->x, (struct sockaddr *)&sin, sizeof(sin),
			       __listen_connect_cb, NULL);
	if (ret) {
		ldmsd_log(LDMSD_LERROR, "Error %d listening on the '%s' "
				"transport.\n", ret, listen->xprt);
		return 7; /* legacy error code */
	}
	ldmsd_log(LDMSD_LINFO, "Listening on transport %s:%hu\n",
			listen->xprt, listen->port_no);
	return 0;
}

int ldmsd_handle_plugin_config()
{
	struct ldmsd_deferred_pi_config *cfg, *nxt_cfg;
	ldmsd_plugin_inst_t inst;
	json_entity_t item;
	int rc;
	cfg = ldmsd_deferred_pi_config_first();
	while (cfg) {
		nxt_cfg = ldmsd_deffered_pi_config_next(cfg);
		inst = ldmsd_plugin_inst_find(cfg->name);
		if (JSON_LIST_VALUE == json_entity_type(cfg->config)) {
			for (item = json_item_first(cfg->config); item;
					item = json_item_next(item)) {
				rc = ldmsd_plugin_inst_config(inst, item,
						cfg->buf, cfg->buflen);
				if (rc) {
					jbuf_t jb = json_entity_dump(NULL, cfg->config);
					ldmsd_log(LDMSD_LERROR, "%s: Error %d:"
							"Error config plugin instance "
							"'%s': %s\n", cfg->config_file,
							rc,  cfg->name, cfg->buf);
					jbuf_free(jb);
					if (!ldmsd_is_syntax_check())
						return rc;
				}
			}
		} else {
			rc = ldmsd_plugin_inst_config(inst, cfg->config, cfg->buf, cfg->buflen);
			if (rc) {
				jbuf_t jb = json_entity_dump(NULL, cfg->config);
				ldmsd_log(LDMSD_LERROR, "%s: Error %d:"
						"Error config plugin instance "
						"'%s': %s\n", cfg->config_file,
						rc,  cfg->name, cfg->buf);
				jbuf_free(jb);
				if (!ldmsd_is_syntax_check())
					return rc;
			}

		}
		ldmsd_deferred_pi_config_free(cfg);
		cfg = nxt_cfg;
	}
	return 0;
}

void __cfg_xprt_del(ldmsd_cfg_xprt_t xprt)
{
	free(xprt);
}

ldmsd_cfg_xprt_t ldmsd_cfg_xprt_new()
{
	ldmsd_cfg_xprt_t xprt = calloc(1, sizeof(*xprt));
	if (!xprt) {
		ldmsd_log(LDMSD_LCRITICAL, "Out of memory\n");
		return NULL;
	}
	return xprt;
}

void ldmsd_cfg_xprt_cli_init(ldmsd_cfg_xprt_t xprt)
{
	xprt->type = LDMSD_CFG_XPRT_CLI;
	xprt->send_fn = NULL;
	xprt->max_msg = LDMSD_CFG_FILE_XPRT_MAX_REC;
	xprt->trust = 1;
	ref_init(&xprt->ref, "create", (ref_free_fn_t)ldmsd_cfg_xprt_cleanup, xprt);
}

void ldmsd_cfg_xprt_ldms_init(ldmsd_cfg_xprt_t xprt, ldms_t ldms)
{
	ldms_xprt_get(ldms);
	xprt->type = LDMSD_CFG_XPRT_LDMS;
	xprt->ldms.ldms = ldms;
	xprt->send_fn = send_ldms_fn;
	xprt->max_msg = ldms_xprt_msg_max(ldms);
	xprt->trust = 0; /* don't trust any network for CMD expansion */
	ref_init(&xprt->ref, "create", (ref_free_fn_t)ldmsd_cfg_xprt_ldms_cleanup, xprt);
}

ldmsd_cfg_xprt_t ldmsd_cfg_xprt_ldms_new(ldms_t x)
{
	ldmsd_cfg_xprt_t xprt = ldmsd_cfg_xprt_new();
	if (!xprt)
		return NULL;
	ldmsd_cfg_xprt_ldms_init(xprt, x);
	return xprt;
}

void ldmsd_mm_status(enum ldmsd_loglevel level, const char *prefix)
{
	struct mm_stat s;
	mm_stats(&s);
	/* compute bound based on current usage */
	size_t used = s.size - s.grain*s.largest;
	ldmsd_log(level, "%s: mm_stat: size=%zu grain=%zu chunks_free=%zu grains_free=%zu grains_largest=%zu grains_smallest=%zu bytes_free=%zu bytes_largest=%zu bytes_smallest=%zu bytes_used+holes=%zu\n",
	prefix,
	s.size, s.grain, s.chunks, s.bytes, s.largest, s.smallest,
	s.grain*s.bytes, s.grain*s.largest, s.grain*s.smallest, used);
}

const char * blacklist[] = {
	"libtsampler.so",
	"libtimer_base.so",
	"liblustre_sampler.so",
	"libzap.so",
	"libzap_rdma.so",
	"libzap_sock.so",
	NULL
};

#define APP "ldmsd"

static int ldmsd_plugins_usage_dir(const char *dir, const char *plugname);

/* Dump plugin names and usages (where available) before ldmsd redirects
 * io. Loads and terms all plugins, which provides a modest check on some
 * coding and deployment issues.
 * \param plugname: list usage only for plugname. If NULL, list all plugins.
 */
int ldmsd_plugins_usage(const char *plugname)
{
	char library_path[LDMSD_PLUGIN_LIBPATH_MAX];
	char *pathdir = library_path;
	char *libpath;
	char *saveptr = NULL;

	if (0 == strcasecmp(plugname, "all"))
		plugname = NULL;

	char *path = getenv("LDMSD_PLUGIN_LIBPATH");
	if (!path)
		path = PLUGINDIR;

	if (! path ) {
		fprintf(stderr, "%s: need plugin path input.\n", APP);
		fprintf(stderr, "Did not find env(LDMSD_PLUGIN_LIBPATH).\n");
		return EINVAL;
	}
	strncpy(library_path, path, sizeof(library_path) - 1);

	int trc=0, rc = 0;
	while ((libpath = strtok_r(pathdir, ":", &saveptr)) != NULL) {
		pathdir = NULL;
		trc = ldmsd_plugins_usage_dir(libpath, plugname);
		if (trc)
			rc = trc;
	}
	return rc;
}

static int ldmsd_plugins_usage_dir(const char *path, const char *plugname)
{
	assert( path || "null dir name in ldmsd_plugins_usage" == NULL);
	struct stat buf;
	const char *type_name = NULL;
	glob_t pglob;

	if (stat(path, &buf) < 0) {
		int err = errno;
		fprintf(stderr, "%s: unable to stat library path %s (%d).\n",
			APP, path, err);
		return err;
	}

	int rc = 0;
	bool matchtype = false;
	if (plugname && strcmp(plugname,"store") == 0) {
		matchtype = true;
		type_name = plugname;
		plugname = NULL;
	}
	if (plugname && strcmp(plugname,"sampler") == 0) {
		matchtype = true;
		type_name = plugname;
		plugname = NULL;
	}


	const char *match1 = "/lib";
	const char *match2 = ".so";
	size_t patsz = strlen(path) + strlen(match1) + strlen(match2) + 2;
	if (plugname) {
		patsz += strlen(plugname);
	}
	char *pat = malloc(patsz);
	if (!pat) {
		fprintf(stderr, "%s: out of memory?!\n", APP);
		rc = ENOMEM;
		goto out;
	}
	snprintf(pat, patsz, "%s%s%s%s", path, match1,
		(plugname ? plugname : "*"), match2);
	int flags = GLOB_ERR |  GLOB_TILDE | GLOB_TILDE_CHECK;

	int err = glob(pat, flags, NULL, &pglob);
	switch(err) {
	case 0:
		break;
	case GLOB_NOSPACE:
		fprintf(stderr, "%s: out of memory!?\n", APP);
		rc = ENOMEM;
		break;
	case GLOB_ABORTED:
		fprintf(stderr, "%s: error reading %s\n", APP, path);
		rc = 1;
		break;
	case GLOB_NOMATCH:
		fprintf(stderr, "%s: no libraries in %s for %s\n", APP, path, pat);
		rc = 1;
		break;
	default:
		fprintf(stderr, "%s: unexpected glob error for %s\n", APP, path);
		rc = 1;
		break;
	}
	if (err)
		goto out2;

	size_t i = 0;
	if (pglob.gl_pathc > 0) {
		printf("LDMSD plugins in %s : \n", path);
	}
	for ( ; i  < pglob.gl_pathc; i++) {
		ldmsd_plugin_inst_t inst = NULL;
		char * library_name = pglob.gl_pathv[i];
		char *tmp = strdup(library_name);
		if (!tmp) {
			rc = ENOMEM;
			goto out2;
		} else {
			char *b = basename(tmp);
			int j = 0;
			int blacklisted = 0;
			while (blacklist[j]) {
				if (strcmp(blacklist[j], b) == 0) {
					blacklisted = 1;
					break;
				}
				j++;
			}
			if (blacklisted)
				goto next;
			/* strip lib prefix and .so suffix*/
			b+= 3;
			char *suff = rindex(b, '.');
			assert(suff != NULL || NULL == "plugin glob match means . will be found always");
			*suff = '\0';
			inst = ldmsd_plugin_inst_load(b, b, NULL, 0);
			if (!inst) {
				/* EINVAL suggests non-inst load */
				if (errno != EINVAL)
					fprintf(stderr, "Unable to load plugin %s\n", b);
				goto next;
			}

			if (matchtype && strcmp(type_name, inst->type_name))
				goto next;

			printf("======= %s %s:\n", inst->type_name, b);
			const char *u = ldmsd_plugin_inst_help(inst);
			printf("%s\n", u);
			printf("=========================\n");
 next:
			if (inst) {
				ldmsd_plugin_inst_del(inst);
				inst = NULL;
			}
			free(tmp);
		}

	}


 out2:
	globfree(&pglob);
	free(pat);
 out:
	return rc;
}
