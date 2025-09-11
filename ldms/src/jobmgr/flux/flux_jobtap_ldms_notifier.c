/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2025 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2025 Open Grid Computing, Inc. All rights reserved.
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

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <jansson.h>
#include <stdarg.h>
#include <semaphore.h>
#include <time.h>
#include <pwd.h>

#include "flux/core.h"
#include "flux/jobtap.h"

#include "core/ldms.h"

extern char **environ;

char hostname[64];

struct conf_s {
	char host[256];
	char port[64];
	char xprt[64];
	char auth[64];
	char msg_chan[256];
};

static struct conf_s conf = {
	.host = "localhost",
	.port = "411",
	.xprt = "sock",
	.auth = "munge",
	.msg_chan = "flux",
};

struct flux_plugin_arg {
	json_error_t error;
	json_t *in;
	json_t *out;
};

/* for debugging */
#if 0
#define DLOG(...) /* no-op */
#else
#define LOG_DIR "/tmp/flux-ldms"
static int DLOG(const char *fmt, ...)
{
	pid_t pid = getpid();
	char path[512];
	FILE *f;
	va_list ap;
	mkdir(LOG_DIR, 0777);
	sprintf(path, "%s/jobtap-%s-%d.log", LOG_DIR, hostname, pid);
	f = fopen(path, "a");
	if (!f)
		return -1;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	fclose(f);
	return 0;
}
#endif

struct id_array {
	int n;
	uint64_t ids[128];
};

static void walk_pid(int start_pid, struct id_array *a)
{
	pid_t pid, _pid, ppid;
	FILE *f;
	char path[1024];
	char cmd[1024];
	char st;
	char *id;
	pid = start_pid;
 loop:
	if (pid == 1)
		goto done;
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	DLOG("path: %s\n", path);
	f = fopen(path, "r");
	fscanf(f, "%d %s %c %d", &_pid, cmd, &st, &ppid);
	fclose(f);
	if (0 != strcmp(cmd, "(flux-shell)")) /* not a flux shell */
		goto next;
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	DLOG("path: %s\n", path);
	f = fopen(path, "r");
	fread(cmd, 1, sizeof(cmd), f);
	fclose(f);
	id = cmd + strlen(cmd) + 1;
	sscanf(id, "%lu", &a->ids[a->n++]);
 next:
	pid = ppid;
	goto loop;
 done:
	/* no-op */;
}

struct xprt_ctxt {
	sem_t sem;
	int connected;
	int err;
	const char *err_str;

	int msg_len;
	int deposited;
};

void xprt_cb(ldms_t x, ldms_xprt_event_t e, void *cb_arg)
{
	struct xprt_ctxt *xctxt = cb_arg;

	switch (e->type) {
	case LDMS_XPRT_EVENT_CONNECTED:
		xctxt->connected = 1;
		sem_post(&xctxt->sem);
		break;
	case LDMS_XPRT_EVENT_REJECTED:
		xctxt->err = ECONNREFUSED;
		xctxt->err_str = "Connection refused";
		sem_post(&xctxt->sem);
		break;
	case LDMS_XPRT_EVENT_ERROR:
		xctxt->err = EINVAL;
		xctxt->err_str = "Connection error";
		sem_post(&xctxt->sem);
		break;
	case LDMS_XPRT_EVENT_DISCONNECTED:
		xctxt->connected = 0;
		sem_post(&xctxt->sem);
		break;
	case LDMS_XPRT_EVENT_SEND_QUOTA_DEPOSITED:
		DLOG("<<<DEPOSIT>>>\n");
		xctxt->deposited = 1;
		sem_post(&xctxt->sem);
		break;
	case LDMS_XPRT_EVENT_SEND_COMPLETE:
	case LDMS_XPRT_EVENT_SET_DELETE:
	case LDMS_XPRT_EVENT_RECV:
	case LDMS_XPRT_EVENT_QGROUP_ASK:
	case LDMS_XPRT_EVENT_QGROUP_DONATE:
	case LDMS_XPRT_EVENT_QGROUP_DONATE_BACK:
	case LDMS_XPRT_EVENT_LAST:
		/* no-op */;
	}
}

static int run_cb(flux_plugin_t *p, const char *topic,
	      flux_plugin_arg_t *args, void *arg)
{
	int rc;
	flux_jobid_t job_id;
	char job_id_f[16];

	flux_plugin_arg_unpack(args, FLUX_PLUGIN_ARG_IN, "{s:I}", "id", &job_id);
	flux_job_id_encode(job_id, "f58plain", job_id_f, sizeof(job_id_f));
	rc = flux_jobtap_job_subscribe(p, FLUX_JOBTAP_CURRENT_JOB);
	DLOG("%s subscribe rc: %d\n", job_id_f, rc);
	return 0;
}

static int finish_cb(flux_plugin_t *p, const char *topic,
	      flux_plugin_arg_t *args, void *arg)
{
	flux_jobid_t job_id;
	char job_id_f[16];

	flux_plugin_arg_unpack(args, FLUX_PLUGIN_ARG_IN, "{s:I}", "id", &job_id);
	flux_job_id_encode(job_id, "f58plain", job_id_f, sizeof(job_id_f));
	flux_jobtap_job_unsubscribe(p, FLUX_JOBTAP_CURRENT_JOB);
	DLOG("%s unsubscribed\n", job_id_f);
	return 0;
}

__attribute__((unused))
static int jobtap_cb(flux_plugin_t *p, const char *topic,
	      flux_plugin_arg_t *args, void *arg)
{
	char *str = NULL;
	int rc;
	ldms_t x;
	uid_t uid = getuid();
	gid_t gid = getgid();
	struct xprt_ctxt xctxt = { };
	time_t ts = time(NULL);
	struct passwd *pwd_ent;

	DLOG("HERE!");

	flux_t *flux = flux_jobtap_get_flux(p);

	assert(flux);

	pid_t pid = getpid();

	pwd_ent = getpwuid(uid);

	/* --- DEBUG --- */
	DLOG("--------------- %s ----------------\n", topic);
	DLOG("%s (%d) topic: %s, uid: %d, euid: %d, "
	       "ppid: %d\n", hostname, pid, topic, getuid(), geteuid(), getppid());

	if (args) {
		DLOG("%p %p\n", args->in, args->out);
		rc = flux_plugin_arg_get(args, FLUX_PLUGIN_ARG_OUT, &str);
		if (rc) {
			DLOG("arg error: %s\n", flux_plugin_arg_strerror(args));
		}
		DLOG("args: %s\n", str);
	} else {
		DLOG("args is NULL\n");
	}
	if (arg) {
		DLOG("arg: %s\n", (char*)arg);
	}

	char *json_str = NULL;

	json_t *root;
	root = json_object();

	json_object_set(root, "event", json_string(topic));
	json_object_set(root, "uid", json_integer(uid));
	json_object_set(root, "gid", json_integer(gid));
	json_object_set(root, "user", json_string(pwd_ent?pwd_ent->pw_name:"<UNKNOWN>"));
	json_object_set(root, "ts", json_integer(ts));

	DLOG("--------------- END %s (debug) ----------------\n", topic);
	/* -------------- */

	json_t *shell_id = json_array();
	json_t *shell_id_f58 = json_array();
	int i;
	struct id_array a = {};
	walk_pid(pid, &a);
	DLOG("done walk_pid, n: %d\n", a.n);
	for (i = a.n - 1; i >= 0; i--) {
		char buf[16];
		json_array_append(shell_id, json_integer(a.ids[i]));
		flux_job_id_encode(a.ids[i], "f58plain", buf, sizeof(buf));
		json_array_append(shell_id_f58, json_string(buf));
	}
	json_object_set(root, "shell_id", shell_id);
	json_object_set(root, "shell_id_f58", shell_id_f58);

	sem_init(&xctxt.sem, 0, 0);

	x = ldms_xprt_new_with_auth(conf.xprt, conf.auth, NULL);
	if (!x) {
		flux_log_error(flux, "ldms xprt creation error: %d", errno);
		goto out;
	}

	rc = ldms_xprt_connect_by_name(x, conf.host, conf.port, xprt_cb, &xctxt);
	if (rc) {
		flux_log_error(flux, "ldms error: %d", rc);
		ldms_xprt_put(x, "rail_ref");
		goto out;
	}

	sem_wait(&xctxt.sem);

	if (!xctxt.connected) {
		flux_log_error(flux, "ldms connection error: %d, %s", xctxt.err, xctxt.err_str);
		ldms_xprt_put(x, "rail_ref");
		goto out;
	}

	DLOG("<<<CONNECTED>>>\n");

	/* connected, now build our data */

	json_str = json_dumps(root, 0);
	int len = strlen(json_str);

	DLOG("LEN: %d\n", len);
	DLOG("JSON: %s\n", json_str);

	/* send data */
	rc = ldms_msg_publish(x, conf.msg_chan, LDMS_MSG_JSON, NULL,
				0440, json_str, len+1);
	if (rc) {
		DLOG("ldms_msg_publish() error: %d", rc);
		flux_log_error(flux, "ldms_msg_publish() error: %d", rc);
		ldms_xprt_close(x);
		goto out;
	}

	DLOG("<<<SEM_WAIT>>>\n");
	sem_wait(&xctxt.sem);

	if (xctxt.deposited) {
		/* data sent */
	} else {
		/* disconnected or error */
	}

 out:
	return 0;
}

#if 0
   /* other shell/task topics */

	{"shell.post-init", shell_cb, NULL},
	{"task.init", shell_cb, "three"},
	{"task.exec", shell_cb, "four"}, /* calls from task process */
	{"shell.start", shell_cb, "six"},
#endif

static const struct flux_plugin_handler handlers[] = {
	{ "job.state.run", run_cb, "eight" },
	{ "job.event.finish", finish_cb, "eight" },
	{ 0 },
};

int flux_plugin_init(flux_plugin_t *p)
{
	int rc;
	int port = 0;
	const char *host, *auth, *xprt, *msg_chan;
	host = auth = xprt = msg_chan = NULL;
	flux_t *flux = flux_jobtap_get_flux(p);

	gethostname(hostname, sizeof(hostname));
	rc = flux_plugin_conf_unpack(p,
			"{s?s, s?i, s?s, s?s, s?s}",
			"host", &host,
			"port", &port,
			"xprt", &xprt,
			"auth", &auth,
			"message_channel", &msg_chan);
	if (rc) {
		flux_log_error(flux, "conf unpack error: %d", rc);
		return -1;
	}

	if (host)
		snprintf(conf.host, sizeof(conf.host), "%s", host);
	if (port)
		snprintf(conf.port, sizeof(conf.port), "%d", port);
	if (xprt)
		snprintf(conf.xprt, sizeof(conf.xprt), "%s", xprt);
	if (auth)
		snprintf(conf.auth, sizeof(conf.auth), "%s", auth);
	if (auth)
		snprintf(conf.auth, sizeof(conf.auth), "%s", auth);

	DLOG("conf.host: %s\n", conf.host);
	DLOG("conf.port: %s\n", conf.port);
	DLOG("conf.xprt: %s\n", conf.xprt);
	DLOG("conf.auth: %s\n", conf.auth);
	DLOG("conf.msg_chan: %s\n", conf.msg_chan);

	return (flux_plugin_register(p, "flux_jobtap_ldms_notifier", handlers) < 0)?-1:0;
}
