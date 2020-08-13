/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2010-2018 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2010-2018 Open Grid Computing, Inc. All rights reserved.
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
#ifndef __LDMSD_H__
#define __LDMSD_H__
#include <limits.h>
#include <regex.h>
#include <sys/queue.h>
#include <pthread.h>
#include <sys/errno.h>

#ifdef LDMSD_UPDATE_TIME
#include <sys/time.h>
#include <coll/idx.h>
#endif /* LDMSD_UPDATE_TIME */

#include <ev/ev.h>
#include <ovis_event/ovis_event.h>
#include <ovis_util/util.h>
#include <json/json_util.h>
#include "ldms.h"
#include "ref.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

#define LDMSD_PLUGIN_LIBPATH_DEFAULT PLUGINDIR

#define LDMSD_VERSION_MAJOR	0x03
#define LDMSD_VERSION_MINOR	0x02
#define LDMSD_VERSION_PATCH	0x02
#define LDMSD_VERSION_FLAGS	0x00

#define LDMSD_DEFAULT_FILE_PERM 0600

#define LDMSD_FAILOVER_NAME_PREFIX "#"

#define LDMSD_DEFAULT_GLOBAL_AUTH "none"

#define LDMSD_MEM_SIZE_ENV "LDMSD_MEM_SZ"
#define LDMSD_MEM_SIZE_DEFAULT "512kB"
#define LDMSD_BANNER_DEFAULT 1
#define LDMSD_VERBOSITY_DEFAULT LDMSD_LERROR
#define LDMSD_EV_THREAD_CNT_DEFAULT 1
#define LDMSD_SETFILE "/proc/sys/kldms/set_list"
#define LDMSD_LOGFILE "/var/log/ldmsd.log"
#define LDMSD_PIDFILE_FMT "/var/run/%s.pid"

struct ldmsd_version {
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
	uint8_t flags;
};

typedef struct ldmsd_str_ent {
	char *str;
	LIST_ENTRY(ldmsd_str_ent) entry;
} *ldmsd_str_ent_t;
LIST_HEAD(ldmsd_str_list, ldmsd_str_ent);

typedef struct ldmsd_regex_ent {
	char *regex_str;
	regex_t regex;
	LIST_ENTRY(ldmsd_regex_ent) entry;
} *ldmsd_regex_ent_t;
LIST_HEAD(ldmsd_regex_list, ldmsd_regex_ent);

#define LDMSD_STR_WRAP(NAME) #NAME
#define LDMSD_LWRAP(NAME) LDMSD_L ## NAME
#define TOSTRING(x) LDMSD_STR_WRAP(x)
/**
 * \brief ldmsd log levels
 *
 * The ldmsd log levels, in order of increasing importance, are
 *  - DEBUG
 *  - INFO
 *  - WARNING
 *  - ERROR
 *  - CRITICAL
 *  - ALL
 *
 * ALL is for messages printed to the log file per users requests,
 * e.g, messages printed from the 'info' command.
 */
#define LOGLEVELS(WRAP) \
	WRAP (DEBUG), \
	WRAP (INFO), \
	WRAP (WARNING), \
	WRAP (ERROR), \
	WRAP (CRITICAL), \
	WRAP (ALL), \
	WRAP (LASTLEVEL),

enum ldmsd_loglevel {
	LDMSD_LNONE = -1,
	LOGLEVELS(LDMSD_LWRAP)
};

unsigned long ldmsd_time_str2us(const char *s);
char *ldmsd_time_us2str(unsigned long us);

short ldmsd_is_initialized();
short ldmsd_is_foreground();
short ldmsd_is_syntax_check();
const char *ldmsd_progname_get();

/** Get the ldmsd version  */
void ldmsd_version_get(struct ldmsd_version *v);
#pragma weak ldmsd_version_get

/** Update hint */
#define LDMSD_SET_INFO_UPDATE_HINT_KEY "updt_hint_us"
#define LDMSD_UPDT_HINT_OFFSET_NONE LONG_MIN

typedef struct ldmsd_plugin_inst_s *ldmsd_plugin_inst_t;
typedef struct ldmsd_plugin_type_s *ldmsd_plugin_type_t;

/** Set information */
#define LDMSD_SET_INFO_INTERVAL_KEY "interval_us"
#define LDMSD_SET_INFO_OFFSET_KEY "offset_us"

/** Request that the task stop */
#define LDMSD_TASK_F_STOP		0x01
/** Use 'synchronous' scheduling. This is set when offset_us is !0 */
#define LDMSD_TASK_F_SYNCHRONOUS	0x02
/** Ignore the schedule interval for the initial call to task_fn */
#define LDMSD_TASK_F_IMMEDIATE		0x04

struct ldmsd_task;
typedef void (*ldmsd_task_fn_t)(struct ldmsd_task *, void *arg);
typedef struct ldmsd_task {
	int thread_id;
	int flags;
	long sched_us;
	long offset_us;
	pthread_mutex_t lock;
	pthread_cond_t join_cv;
	struct timeval timeout;
	enum ldmsd_task_state {
		LDMSD_TASK_STATE_STOPPED,
		LDMSD_TASK_STATE_STARTED,
		LDMSD_TASK_STATE_RUNNING
	} state;
	ldmsd_task_fn_t fn;
	void *fn_arg;
	ovis_scheduler_t os;
	struct ovis_event_s oev;
} *ldmsd_task_t;

typedef struct ldmsd_sec_ctxt {
	struct ldms_cred crd;
} *ldmsd_sec_ctxt_t;

typedef enum ldmsd_cfgobj_type {
	LDMSD_CFGOBJ_FIRST = 1,
	LDMSD_CFGOBJ_PRDCR = LDMSD_CFGOBJ_FIRST,
	LDMSD_CFGOBJ_UPDTR,
	LDMSD_CFGOBJ_STRGP,
	LDMSD_CFGOBJ_SMPLR,
	LDMSD_CFGOBJ_LISTEN,
	LDMSD_CFGOBJ_SETGRP,
	LDMSD_CFGOBJ_AUTH,
	LDMSD_CFGOBJ_ENV,
	LDMSD_CFGOBJ_DAEMON,
	LDMSD_CFGOBJ_PLUGIN,
	LDMSD_CFGOBJ_LAST,
} ldmsd_cfgobj_type_t;

struct ldmsd_cfgobj;
typedef struct ldmsd_req_buf (*ldmsd_req_buf_t);
typedef void (*ldmsd_cfgobj_del_fn_t)(struct ldmsd_cfgobj *);
typedef json_entity_t (*ldmsd_cfgobj_create_fn_t)(const char *name, /* cfgobj name */
					short enabled,
					json_entity_t dft, /* default attribute values */
					json_entity_t spc, /* attribute values specific for this obj */
					uid_t uid, gid_t gid);
typedef json_entity_t (*ldmsd_cfgobj_update_fn_t)(struct ldmsd_cfgobj *obj,
					short enabled, /* 0 means disabled, 1 means enabled */
					json_entity_t dft,
					json_entity_t spc);
/*
 * The delete function must remove the obj from the cfgobj tree and
 * the function must not take the cfgobj tree lock.
 */
typedef json_entity_t (*ldmsd_cfgobj_delete_fn_t)(struct ldmsd_cfgobj *obj);

/*
 * \brief query all attributes of a config object
 */
typedef json_entity_t (*ldmsd_cfgobj_query_fn_t)(struct ldmsd_cfgobj *obj);
/**
 * \brief Export the configuration attribute.
 *
 * The difference between \c ldmsd_cfgobj_query_fn_t and \c ldmsd_cfgobj_export_fn_t
 * is that \c ldmsd_cfgobj_export_fn_t exports only the attributes that can be
 * configured such as name, interval, host.
 */
typedef json_entity_t (*ldmsd_cfgobj_export_fn_t)(struct ldmsd_cfgobj *obj);

/**
 * \brief Take the associated actions of the enabled obj.
 */
typedef int (*ldmsd_cfgobj_enable_fn_t)(struct ldmsd_cfgobj *obj);

/**
 * \brief Take the associated actions of the disabled obj
 */
typedef int (*ldmsd_cfgobj_disable_fn_t)(struct ldmsd_cfgobj *obj);

#define LDMSD_PERM_UEX 0100
#define LDMSD_PERM_UWR 0200
#define LDMSD_PERM_URD 0400
#define LDMSD_PERM_GEX 0010
#define LDMSD_PERM_GWR 0020
#define LDMSD_PERM_GRD 0040
#define LDMSD_PERM_OEX 0001
#define LDMSD_PERM_OWR 0002
#define LDMSD_PERM_ORD 0004

/* for deferred start */
#define LDMSD_PERM_DSTART 01000

/* for failover internal requests */
#define LDMSD_PERM_FAILOVER_INTERNAL 02000

/* can execute even if the failover is turned on */
#define LDMSD_PERM_FAILOVER_ALLOWED 04000

#define LDMSD_ATTR_NA -2
#define LDMSD_ATTR_INVALID -3

typedef struct ldmsd_cfgobj {
	char *name;		/* Unique object name */
	uint32_t ref_count;
	ldmsd_cfgobj_type_t type;
	ldmsd_cfgobj_del_fn_t __del; /* This is called when the ref_count reaches 0 */

	short enabled;
	/* These callbacks are called upon receiving a configuration request. */
	ldmsd_cfgobj_update_fn_t update;
	ldmsd_cfgobj_delete_fn_t delete; /* This is called when there is a delete request. The object may not be freed right away. */
	ldmsd_cfgobj_query_fn_t query;
	ldmsd_cfgobj_export_fn_t export;
	ldmsd_cfgobj_enable_fn_t enable;
	ldmsd_cfgobj_disable_fn_t disable;

	ev_t enabled_ev;
	ev_t disabled_ev;

	struct rbn rbn;
	pthread_mutex_t lock;
	uid_t uid;
	gid_t gid;
	int perm;
} *ldmsd_cfgobj_t;

/*
 * Environment variables shouldn't be used as variables in configuration files anymore.
 */
typedef struct ldmsd_env {
	struct ldmsd_cfgobj obj;
	char *name;
	char *value;
} *ldmsd_env_t;

typedef struct ldmsd_daemon {
	struct ldmsd_cfgobj obj;
	json_entity_t attr;
} *ldmsd_daemon_t;

typedef struct ldmsd_smplr {
	struct ldmsd_cfgobj obj;

	enum ldmsd_smplr_state {
		LDMSD_SMPLR_STATE_STOPPED,
		LDMSD_SMPLR_STATE_RUNNING,
	} state;

	ldmsd_plugin_inst_t pi;

	ev_t start_ev;
	ev_t stop_ev;
	ev_t sample_ev;

	long interval_us;
	long offset_us;
	int synchronous;

} *ldmsd_smplr_t;

/*
 * The maximum number of authentication options
 */
#define LDMSD_AUTH_OPT_MAX 128

/**
 * LDMSD object of the listener transport/port
 */
typedef struct ldmsd_listen {
	struct ldmsd_cfgobj obj;
	char *xprt;
	unsigned short port_no;
	char *host;
	char *auth_name; /* Name of an authentication configuration object */
	ldms_t x;
} *ldmsd_listen_t;

/**
 * Producer: Named instance of an LDMSD
 *
 * The Producer name, by policy, equals the name of this configuration object.
 */
typedef struct ldmsd_prdcr {
	struct ldmsd_cfgobj obj;

	struct sockaddr_storage ss;	/* Host address */
	socklen_t ss_len;
	char *host_name;	/* Host name */
	unsigned short port_no;		/* Port number */
	char *xprt_name;	/* Transport name */
	ldms_t xprt;
	long conn_intrvl_us;	/* connect interval */
	char *conn_auth;			/* auth method for the connection */
	struct attr_value_list *conn_auth_args;  /* auth options of the connection auth */

	enum ldmsd_prdcr_state {
		/** Producer task has stopped & no outstanding xprt */
		LDMSD_PRDCR_STATE_STOPPED,
		/** Ready for connect attempts (no outstanding xprt) */
		LDMSD_PRDCR_STATE_DISCONNECTED,
		/** Connection request is outstanding */
		LDMSD_PRDCR_STATE_CONNECTING,
		/** Connect complete */
		LDMSD_PRDCR_STATE_CONNECTED,
		/** Waiting for task join and xprt cleanup */
		LDMSD_PRDCR_STATE_STOPPING,
	} conn_state;

	enum ldmsd_prdcr_type {
		/** Connection initiated at this side */
		LDMSD_PRDCR_TYPE_ACTIVE,
		/** Connection initiated by peer */
		LDMSD_PRDCR_TYPE_PASSIVE,
		/** Producer is local to this daemon */
		LDMSD_PRDCR_TYPE_LOCAL
	} type;

	ev_t connect_ev;	/* sent to updater */
	ev_t start_ev;		/* sent to updater */
	ev_t stop_ev;		/* sent to updater */

	/**
	 * list of subscribed streams from this producer
	 */
	json_entity_t stream_list;

	/**
	 * Maintains a tree of all metric sets available from this
	 * producer. It is a tree to allow quick lookup by the logic
	 * that handles dir_add and dir_del directory updates from the
	 * producer.
	 */
	struct rbt set_tree;

#ifdef LDMSD_UPDATE_TIME
	double sched_update_time;
#endif /* LDMSD_UPDATE_TIME */
} *ldmsd_prdcr_t;

struct ldmsd_strgp;
typedef struct ldmsd_strgp *ldmsd_strgp_t;

typedef struct ldmsd_strgp_ref {
	ldmsd_strgp_t strgp;
	ev_t store_ev;
	LIST_ENTRY(ldmsd_strgp_ref) entry;
} *ldmsd_strgp_ref_t;

#define LDMSD_PRDCR_SET_F_PUSH_REG	1

typedef struct ldmsd_set_ctxt_s {
	enum {
		LDMSD_SET_CTXT_PRDCR,
		LDMSD_SET_CTXT_SAMP,
	} type;
} *ldmsd_set_ctxt_t;

typedef struct ldmsd_updt_hint_set_list {
	struct rbn rbn;
	LIST_HEAD(, ldmsd_prdcr_set) list;
} *ldmsd_updt_hint_set_list_t;

struct ldmsd_updtr_schedule {
	long intrvl_us;
	long offset_us;
	long offset_skew;
};

typedef struct ldmsd_updtr *ldmsd_updtr_ptr;
typedef struct ldmsd_prdcr_set {
	char *inst_name;
	char *schema_name;
	char *producer_name;
	ldmsd_prdcr_t prdcr;
	ldms_set_t set;
	int push_flags;
	enum ldmsd_prdcr_set_state {
		LDMSD_PRDCR_SET_STATE_START,
		LDMSD_PRDCR_SET_STATE_LOOKUP,
		LDMSD_PRDCR_SET_STATE_READY,
		LDMSD_PRDCR_SET_STATE_UPDATING,
		LDMSD_PRDCR_SET_STATE_ERROR,
	} state;
	uint64_t last_gn;
	pthread_mutex_t lock;
	LIST_HEAD(ldmsd_strgp_ref_list, ldmsd_strgp_ref) strgp_list;
	struct rbn rbn;

	struct ldmsd_updtr_schedule updt_hint;

	struct timeval updt_start;
	struct timeval updt_end;

	ev_t update_ev;
	ev_t state_ev;

	int updt_interval;
	int updt_offset;
	uint8_t updt_sync;

#ifdef LDMSD_UPDATE_TIME
	struct ldmsd_updt_time *updt_time;
	double updt_duration;
#endif /* LDMSD_UPDATE_TIME */

	struct ref_s ref;

	struct ldmsd_set_ctxt_s set_ctxt;
} *ldmsd_prdcr_set_t;

#ifdef LDMSD_UPDATE_TIME
double ldmsd_timeval_diff(struct timeval *start, struct timeval *end);
#endif /* LDMSD_UPDATE_TIME */

typedef struct ldmsd_prdcr_ref {
	ldmsd_prdcr_t prdcr;
	struct rbn rbn;
} *ldmsd_prdcr_ref_t;

/**
 * Updater: Named set of rules for updating remote metric sets
 *
 * The prdcr_list specifies the set of LDMS from which to gather
 * metric sets. The match_list specifies which metric sets from each
 * producer will be updated. If the match_list is empty, all metric
 * sets on each producer will be updated.
 *
 */
#ifdef LDMSD_UPDATE_TIME
struct ldmsd_updt_time {
	struct timeval sched_start;
	struct timeval update_start;
	int ref;
	ldmsd_updtr_ptr updtr;
	pthread_mutex_t lock;
};
#endif /* LDMSD_UPDATE_TIME */

#define LDMSD_UPDTR_F_PUSH		1
#define LDMSD_UPDTR_F_PUSH_CHANGE	2
#define LDMSD_UPDTR_OFFSET_INCR_DEFAULT	100000
#define LDMSD_UPDTR_OFFSET_INCR_VAR	"LDMSD_UPDTR_OFFSET_INCR"

struct ldmsd_name_match;
typedef struct ldmsd_updtr {
	struct ldmsd_cfgobj obj;

	int push_flags;

	enum ldmsd_updtr_state {
		/** Initial updater state */
		LDMSD_UPDTR_STATE_STOPPED = 0,
		/** Ready for update attempts */
		LDMSD_UPDTR_STATE_RUNNING,
		/** Stopping, waiting for callback tasks to finish */
		LDMSD_UPDTR_STATE_STOPPING,
	} state;

	/*
	 * flag to enable or disable the functionality
	 * that automatically schedules set updates according to
	 * the update hint.
	 *
	 * 0 is disabled. Otherwise, it is enabled.
	 *
	 * If this value is 0, \c task_tree must contain
	 * only the default task.
	 */
	uint8_t is_auto_task;

	ev_t update_ev;
	ev_t start_ev;
	ev_t stop_ev;

	struct ldmsd_updtr_schedule sched;
	ev_worker_t worker;

#ifdef LDMSD_UPDATE_TIME
	struct ldmsd_updt_time *curr_updt_time;
	double duration;
	double sched_duration;
#endif /* LDMSD_UPDATE_TIME */

	/*
	 * For quick search when query for updater that updates a prdcr_set.
	 */
	struct rbt prdcr_tree;
	struct ldmsd_regex_list *prdcr_regex_list;
	LIST_HEAD(updtr_match_list, ldmsd_name_match) *match_list;
} *ldmsd_updtr_t;

typedef struct ldmsd_name_match {
	/** Regular expression matching schema or instance name */
	char *regex_str;
	regex_t regex;

	/** see man recomp */
	int regex_flags;

	enum ldmsd_name_match_sel {
		LDMSD_NAME_MATCH_INST_NAME,
		LDMSD_NAME_MATCH_SCHEMA_NAME,
	} selector;

	LIST_ENTRY(ldmsd_name_match) entry;
} *ldmsd_name_match_t;

/** Storage Policy: Defines which producers and metrics are
 * saved when an update completes. Must include meta vs data metric flags.
 */
typedef void *ldmsd_store_handle_t;
typedef struct ldmsd_strgp_metric {
	char *name;
	enum ldms_value_type type;
	int flags;
	int idx;
	TAILQ_ENTRY(ldmsd_strgp_metric) entry;
} *ldmsd_strgp_metric_t;

typedef void (*strgp_update_fn_t)(ldmsd_strgp_t strgp, ldmsd_prdcr_set_t prd_set);
struct ldmsd_strgp {
	struct ldmsd_cfgobj obj;

	/** A set of match strings to select a subset of all producers */
	LIST_HEAD(ldmsd_strgp_prdcr_list, ldmsd_regex_ent) *prdcr_list;

	/** A list of the names of the metrics in the set specified by schema */
	TAILQ_HEAD(ldmsd_strgp_metric_list, ldmsd_strgp_metric) *metric_list;
	int metric_count;
	int *metric_arry;	/* Array of metric ids */

	/** Schema name of the metric set on the producer */
	char *schema;

	ldmsd_plugin_inst_t inst;

	enum ldmsd_strgp_state {
		LDMSD_STRGP_STATE_STOPPED,
		LDMSD_STRGP_STATE_RUNNING,
		LDMSD_STRGP_STATE_OPENED,
	} state;

	ev_worker_t worker;
	ev_t start_ev;
	ev_t stop_ev;

	/** Update function */
	strgp_update_fn_t update_fn;
};

typedef struct ldmsd_set_info {
	ldms_set_t set;
	char *origin_name;
	enum ldmsd_set_origin_type {
		LDMSD_SET_ORIGIN_SMPLR = 1,
		LDMSD_SET_ORIGIN_PRDCR,
	} origin_type; /* who is responsible of the set. */
	unsigned long interval_us; /* sampling interval or update interval */
	long offset_us; /* sampling offset or update offset */
	int sync; /* 1 if synchronous */
	struct timeval start; /* Latest sampling/update timestamp */
	struct timeval end; /* latest sampling/update timestamp */
	union {
		ldmsd_smplr_t smplr;
		ldmsd_prdcr_set_t prd_set;
	};
} *ldmsd_set_info_t;

/**
 * \brief Get the set information
 *
 * \return pointer to struct ldmsd_set_info is returned.
 */
ldmsd_set_info_t ldmsd_set_info_get(const char *inst_name);
#pragma weak ldmsd_set_info_get

/**
 * Delete the set info \c info
 */
void ldmsd_set_info_delete(ldmsd_set_info_t info);
#pragma weak ldmsd_set_info_delete

/**
 * \brief Convert the set origin type from enum to string
 */
char *ldmsd_set_info_origin_enum2str(enum ldmsd_set_origin_type type);
#pragma weak ldmsd_set_info_origin_enum2str

#define LDMSD_MAX_PLUGIN_NAME_LEN 64
#define LDMSD_CFG_FILE_XPRT_MAX_REC 8192
struct attr_value_list;

#define LDMSD_DEFAULT_SAMPLE_INTERVAL 1000000
/** Metric name for component ids (u64). */
#define LDMSD_COMPID "component_id"
/** Metric name for job id number */
#define LDMSD_JOBID "job_id"

extern const char *ldmsd_loglevel_names[];

__attribute__((format(printf, 2, 3)))
void ldmsd_log(enum ldmsd_loglevel level, const char *fmt, ...);
#pragma weak ldmsd_log

short ldmsd_is_quiet();
#pragma weak ldmsd_is_quiet

enum ldmsd_loglevel ldmsd_loglevel_get();
#pragma weak ldmsd_loglevel_get

enum ldmsd_loglevel ldmsd_str_to_loglevel(const char *level_s);
#pragma weak ldmsd_str_to_loglevel
const char *ldmsd_loglevel_to_str(enum ldmsd_loglevel level);
#pragma weak ldmsd_loglevel_to_str

__attribute__((format(printf, 1, 2)))
void ldmsd_ldebug(const char *fmt, ...);
#pragma weak ldmsd_ldebug
__attribute__((format(printf, 1, 2)))
void ldmsd_linfo(const char *fmt, ...);
#pragma weak ldmsd_linfo
__attribute__((format(printf, 1, 2)))
void ldmsd_lwarning(const char *fmt, ...);
#pragma weak ldmsd_lwarning
__attribute__((format(printf, 1, 2)))
void ldmsd_lerror(const char *fmt, ...);
#pragma weak ldmsd_lerror
__attribute__((format(printf, 1, 2)))
void ldmsd_lcritical(const char *fmt, ...);
#pragma weak ldmsd_lcritical
__attribute__((format(printf, 1, 2)))
void ldmsd_lall(const char *fmt, ...);
#pragma weak ldmsd_lall

/** Get syslog int value for a level.
 *  \return LOG_CRIT for invalid inputs, NONE, & ENDLEVEL.
 */
int ldmsd_loglevel_to_syslog(enum ldmsd_loglevel level);
#pragma weak ldmsd_loglevel_to_syslog

/**
 * \brief Return the global authentication domain name
 */
const char *ldmsd_global_auth_name_get();
#pragma weak ldmsd_global_auth_name_get

/**
 * \brief Get the security context (uid, gid) of the daemon.
 *
 * \param [out] sctxt the security context output buffer.
 */
void ldmsd_sec_ctxt_get(ldmsd_sec_ctxt_t sctxt);
#pragma weak ldmsd_sec_ctxt_get

typedef void (*ldmsd_msg_log_f)(enum ldmsd_loglevel level, const char *fmt, ...);

/* ldmsctl command callback function definition */
typedef int (*ldmsctl_cmd_fn_t)(char *, struct attr_value_list*, struct attr_value_list *);

#define LDMSCTL_LIST_PLUGINS	0    /* List Plugins */
#define LDMSCTL_LOAD_PLUGIN	1    /* Load Plugin */
#define LDMSCTL_TERM_PLUGIN	2    /* Term Plugin */
#define LDMSCTL_CFG_PLUGIN	3    /* Configure Plugin */
#define LDMSCTL_START_SAMPLER	4    /* Start Sampler */
#define LDMSCTL_STOP_SAMPLER	5    /* Stop Sampler */
#define LDMSCTL_INFO_DAEMON	9   /* Query daemon status */
#define LDMSCTL_SET_UDATA	10   /* Set user data of a metric */
#define LDMSCTL_EXIT_DAEMON	11   /* Shut down ldmsd */
#define LDMSCTL_ONESHOT_SAMPLE	13   /* Sample a set at a specific timestamp once */
#define LDMSCTL_SET_UDATA_REGEX 14   /* Set user data of metrics using regex and increment */
#define LDMSCTL_VERSION		15   /* Get LDMS version */
#define LDMSCTL_VERBOSE	16   /* Change the log level */

#define LDMSCTL_INCLUDE		17  /* Include another configuration file */
#define LDMSCTL_ENV		18  /* Set environment variable */
#define LDMSCTL_LOGROTATE	19  /* Rotate the log file */

#define LDMSCTL_PRDCR_ADD	20   /* Add a producer specification */
#define LDMSCTL_PRDCR_DEL	21   /* Disable a producer specification */
#define LDMSCTL_PRDCR_START	22
#define LDMSCTL_PRDCR_STOP	23
#define LDMSCTL_PRDCR_START_REGEX	24
#define LDMSCTL_PRDCR_STOP_REGEX	25

#define LDMSCTL_UPDTR_ADD	30   /* Add an updater specification */
#define LDMSCTL_UPDTR_DEL	31   /* Delete an updater specification */
#define LDMSCTL_UPDTR_MATCH_ADD 32
#define LDMSCTL_UPDTR_MATCH_DEL 33
#define LDMSCTL_UPDTR_PRDCR_ADD 34
#define LDMSCTL_UPDTR_PRDCR_DEL 35
#define LDMSCTL_UPDTR_START	38
#define LDMSCTL_UPDTR_STOP	39

#define LDMSCTL_STRGP_ADD		40
#define LDMSCTL_STRGP_DEL		41
#define LDMSCTL_STRGP_PRDCR_ADD		42
#define LDMSCTL_STRGP_PRDCR_DEL		43
#define LDMSCTL_STRGP_METRIC_ADD	44
#define LDMSCTL_STRGP_METRIC_DEL	45
#define LDMSCTL_STRGP_START		48
#define LDMSCTL_STRGP_STOP		49

#define LDMSCTL_LAST_COMMAND	LDMSCTL_STRGP_STOP

extern ldmsctl_cmd_fn_t cmd_table[LDMSCTL_LAST_COMMAND + 1];

#define LDMSD_CONTROL_SOCKNAME "ldmsd/control"

#define LDMSD_CONNECT_TIMEOUT		20000000 /* 20 Seconds */
#define LDMSD_INITIAL_CONNECT_TIMEOUT	500000  /* 1/2 second */

/*
 * Max length of error strings while ldmsd is being configured.
 */
#define LEN_ERRSTR 256

FILE *ldmsd_open_log(const char *path);

int ldmsd_plugins_usage(const char *plugin_name);
void ldmsd_mm_status(enum ldmsd_loglevel level, const char *prefix);
#pragma weak ldmsd_mm_status

const char *ldmsd_get_max_mem_sz_str();
#pragma weak ldmsd_set_memory_str_get

/** Configuration object management */
enum ldmsd_cfgobj_type ldmsd_cfgobj_type_str2enum(const char *s);
const char *ldmsd_cfgobj_type2str(enum ldmsd_cfgobj_type type);
void ldmsd_cfgobj___del(ldmsd_cfgobj_t obj);
int ldmsd_cfgobj_init(ldmsd_cfgobj_t obj, const char *name,
				ldmsd_cfgobj_type_t type,
				ldmsd_cfgobj_del_fn_t __del,
				ldmsd_cfgobj_update_fn_t update,
				ldmsd_cfgobj_delete_fn_t delete,
				ldmsd_cfgobj_query_fn_t query,
				ldmsd_cfgobj_export_fn_t export,
				ldmsd_cfgobj_enable_fn_t enable,
				ldmsd_cfgobj_disable_fn_t disable,
				uid_t uid,
				gid_t gid,
				int perm,
				short enabled);
void ldmsd_cfg_lock(ldmsd_cfgobj_type_t type);
void ldmsd_cfg_unlock(ldmsd_cfgobj_type_t type);
void ldmsd_cfgobj_lock(ldmsd_cfgobj_t obj);
void ldmsd_cfgobj_unlock(ldmsd_cfgobj_t obj);
ldmsd_cfgobj_t ldmsd_cfgobj_new(const char *name, ldmsd_cfgobj_type_t type,
				size_t obj_size, ldmsd_cfgobj_del_fn_t __del,
				ldmsd_cfgobj_update_fn_t update,
				ldmsd_cfgobj_delete_fn_t delete,
				ldmsd_cfgobj_query_fn_t query,
				ldmsd_cfgobj_export_fn_t export,
				ldmsd_cfgobj_enable_fn_t enable,
				ldmsd_cfgobj_disable_fn_t disable,
				uid_t uid,
				gid_t gid,
				int perm,
				short enabled);
ldmsd_cfgobj_t ldmsd_cfgobj_get(ldmsd_cfgobj_t obj);
void ldmsd_cfgobj_put(ldmsd_cfgobj_t obj);
int ldmsd_cfgobj_refcount(ldmsd_cfgobj_t obj);
ldmsd_cfgobj_t ldmsd_cfgobj_find(const char *name, ldmsd_cfgobj_type_t type);
void ldmsd_cfgobj_del(const char *name, ldmsd_cfgobj_type_t type);
ldmsd_cfgobj_t ldmsd_cfgobj_first(ldmsd_cfgobj_type_t type);
ldmsd_cfgobj_t ldmsd_cfgobj_first_re(ldmsd_cfgobj_type_t type, regex_t regex);
ldmsd_cfgobj_t ldmsd_cfgobj_next_re(ldmsd_cfgobj_t obj, regex_t regex);
ldmsd_cfgobj_t ldmsd_cfgobj_next(ldmsd_cfgobj_t obj);
int ldmsd_cfgobj_access_check(ldmsd_cfgobj_t obj, int acc, ldmsd_sec_ctxt_t ctxt);
json_entity_t ldmsd_cfgobj_query_result_new(ldmsd_cfgobj_t obj);
/*
 * \brief Generic cfgobj delete callback function.
 *
 * The caller must hold the cfgobj tree lock.
 */
json_entity_t ldmsd_cfgobj_delete(ldmsd_cfgobj_t obj);

json_entity_t ldmsd_result_new(int errcode, const char *msg, json_entity_t value);
int __ldmsd_reply_result_add(json_entity_t reply, const char *key, int errcode,
					const char *msg, json_entity_t value);


#define LDMSD_CFGOBJ_FOREACH(obj, type) \
	for ((obj) = ldmsd_cfgobj_first(type); (obj);  \
			(obj) = ldmsd_cfgobj_next(obj))

/** Sampler configuration object management */
static inline const char *ldmsd_smplr_state_str(enum ldmsd_smplr_state state)
{
	switch (state) {
	case LDMSD_SMPLR_STATE_RUNNING:
		return "RUNNING";
	case LDMSD_SMPLR_STATE_STOPPED:
		return "STOPPED";
	}
	return "UNKNOWN STATE";
}

static inline void ldmsd_smplr_lock(ldmsd_smplr_t smplr) {
	ldmsd_cfgobj_lock(&smplr->obj);
}
static inline void ldmsd_smplr_unlock(ldmsd_smplr_t smplr) {
	ldmsd_cfgobj_unlock(&smplr->obj);
}
static inline ldmsd_smplr_t ldmsd_smplr_find(const char *name)
{
	return (ldmsd_smplr_t)ldmsd_cfgobj_find(name, LDMSD_CFGOBJ_SMPLR);
}
static inline ldmsd_smplr_t ldmsd_smplr_first()
{
	return (ldmsd_smplr_t)ldmsd_cfgobj_first(LDMSD_CFGOBJ_SMPLR);
}

static inline ldmsd_smplr_t ldmsd_smplr_next(ldmsd_smplr_t smplr)
{
	return (ldmsd_smplr_t)ldmsd_cfgobj_next(&smplr->obj);
}
static inline void ldmsd_smplr_put(ldmsd_smplr_t smplr) {
	ldmsd_cfgobj_put(&smplr->obj);
}

int ldmsd_smplr_oneshot(char *smplr_name, char *ts, ldmsd_sec_ctxt_t sctxt);

int ldmsd_smplr_start(char *smplr_name, char *interval, char *offset,
					int is_one_shot, int flags,
					ldmsd_sec_ctxt_t sctxt);
int __ldmsd_smplr_start(ldmsd_smplr_t smplr, int is_one_shot);

int ldmsd_smplr_stop(const char *name, ldmsd_sec_ctxt_t sctxt);


/** Producer configuration object management */
int ldmsd_prdcr_str2type(const char *type);
const char *ldmsd_prdcr_type2str(enum ldmsd_prdcr_type type);
const char *ldmsd_prdcr_state2str(enum ldmsd_prdcr_state state);
static inline ldmsd_prdcr_t ldmsd_prdcr_first()
{
	return (ldmsd_prdcr_t)ldmsd_cfgobj_first(LDMSD_CFGOBJ_PRDCR);
}

static inline ldmsd_prdcr_t ldmsd_prdcr_next(struct ldmsd_prdcr *prdcr)
{
	return (ldmsd_prdcr_t)ldmsd_cfgobj_next(&prdcr->obj);
}

ldmsd_prdcr_set_t ldmsd_prdcr_set_first(ldmsd_prdcr_t prdcr);
ldmsd_prdcr_set_t ldmsd_prdcr_set_next(ldmsd_prdcr_set_t prv_set);
ldmsd_prdcr_set_t ldmsd_prdcr_set_find(ldmsd_prdcr_t prdcr, const char *setname);
ldmsd_prdcr_set_t ldmsd_prdcr_set_first_by_hint(ldmsd_prdcr_t prdcr,
					struct ldmsd_updtr_schedule *hint);
ldmsd_prdcr_set_t ldmsd_prdcr_set_next_by_hint(ldmsd_prdcr_set_t prd_set);
static inline void ldmsd_prdcr_lock(ldmsd_prdcr_t prdcr) {
	ldmsd_cfgobj_lock(&prdcr->obj);
}
static inline void ldmsd_prdcr_unlock(ldmsd_prdcr_t prdcr) {
	ldmsd_cfgobj_unlock(&prdcr->obj);
}
static inline ldmsd_prdcr_t ldmsd_prdcr_get(ldmsd_prdcr_t prdcr) {
	ldmsd_cfgobj_get(&prdcr->obj);
	return prdcr;
}
static inline void ldmsd_prdcr_put(ldmsd_prdcr_t prdcr) {
	ldmsd_cfgobj_put(&prdcr->obj);
}
static inline ldmsd_prdcr_t ldmsd_prdcr_find(const char *name)
{
	return (ldmsd_prdcr_t)ldmsd_cfgobj_find(name, LDMSD_CFGOBJ_PRDCR);
}
static inline const char *ldmsd_prdcr_set_state_str(enum ldmsd_prdcr_set_state state) {
	switch (state) {
	case LDMSD_PRDCR_SET_STATE_START:
		return "START";
	case LDMSD_PRDCR_SET_STATE_LOOKUP:
		return "LOOKUP";
	case LDMSD_PRDCR_SET_STATE_READY:
		return "READY";
	case LDMSD_PRDCR_SET_STATE_UPDATING:
		return "UPDATING";
	case LDMSD_PRDCR_SET_STATE_ERROR:
		return "ERROR";
	}
	return "BAD STATE";
}
int ldmsd_prdcr_subscribe(ldmsd_prdcr_t prdcr, const char *stream);
int ldmsd_prdcr_subscribe_regex(const char *prdcr_regex, char *stream_name,
				char *rep_buf, size_t rep_len,
				ldmsd_sec_ctxt_t ctxt);

int __ldmsd_prdcr_start(ldmsd_prdcr_t prdcr, ldmsd_sec_ctxt_t ctxt);
int __ldmsd_prdcr_stop(ldmsd_prdcr_t prdcr, ldmsd_sec_ctxt_t ctxt);

/* updtr */
ldmsd_updtr_t
ldmsd_updtr_new(const char *name, long interval_us,
		long offset_us, int push_flags,
		int is_auto_interval);
int ldmsd_updtr_del(const char *updtr_name, ldmsd_sec_ctxt_t ctxt);
ldmsd_updtr_t ldmsd_updtr_first();
ldmsd_updtr_t ldmsd_updtr_next(struct ldmsd_updtr *updtr);
ldmsd_name_match_t ldmsd_updtr_match_first(ldmsd_updtr_t updtr);
ldmsd_name_match_t ldmsd_updtr_match_next(ldmsd_name_match_t match);
enum ldmsd_name_match_sel ldmsd_updtr_match_str2enum(const char *str);
const char *ldmsd_updtr_match_enum2str(enum ldmsd_name_match_sel sel);
ldmsd_prdcr_ref_t ldmsd_updtr_prdcr_first(ldmsd_updtr_t updtr);
ldmsd_prdcr_ref_t ldmsd_updtr_prdcr_next(ldmsd_prdcr_ref_t ref);
static inline ldmsd_updtr_t ldmsd_updtr_get(ldmsd_updtr_t updtr) {
	ldmsd_cfgobj_get(&updtr->obj);
	return updtr;
}
static inline void ldmsd_updtr_put(ldmsd_updtr_t updtr) {
	ldmsd_cfgobj_put(&updtr->obj);
}
static inline void ldmsd_updtr_lock(ldmsd_updtr_t updtr) {
	ldmsd_cfgobj_lock(&updtr->obj);
}
static inline void ldmsd_updtr_unlock(ldmsd_updtr_t updtr) {
	ldmsd_cfgobj_unlock(&updtr->obj);
}
static inline ldmsd_updtr_t ldmsd_updtr_find(const char *name) {
	return (ldmsd_updtr_t)ldmsd_cfgobj_find(name, LDMSD_CFGOBJ_UPDTR);
}

static inline const char *ldmsd_updtr_state_str(enum ldmsd_updtr_state state) {
	switch (state) {
	case LDMSD_UPDTR_STATE_STOPPED:
		return "STOPPED";
	case LDMSD_UPDTR_STATE_RUNNING:
		return "RUNNING";
	case LDMSD_UPDTR_STATE_STOPPING:
		return "STOPPING";
	}
	return "BAD STATE";
}

/* strgp */
ldmsd_strgp_t ldmsd_strgp_new(const char *name, const char *container,
		const char *schema);
ldmsd_strgp_t ldmsd_strgp_first();
ldmsd_strgp_t ldmsd_strgp_next(struct ldmsd_strgp *strgp);
ldmsd_regex_ent_t ldmsd_strgp_prdcr_first(ldmsd_strgp_t strgp);
ldmsd_regex_ent_t ldmsd_strgp_prdcr_next(ldmsd_regex_ent_t match);
ldmsd_strgp_metric_t ldmsd_strgp_metric_first(ldmsd_strgp_t strgp);
ldmsd_strgp_metric_t ldmsd_strgp_metric_next(ldmsd_strgp_metric_t metric);
static inline ldmsd_strgp_t ldmsd_strgp_get(ldmsd_strgp_t strgp) {
	ldmsd_cfgobj_get(&strgp->obj);
	return strgp;
}
static inline void ldmsd_strgp_put(ldmsd_strgp_t strgp) {
	ldmsd_cfgobj_put(&strgp->obj);
}
static inline void ldmsd_strgp_lock(ldmsd_strgp_t strgp) {
	ldmsd_cfgobj_lock(&strgp->obj);
}
static inline void ldmsd_strgp_unlock(ldmsd_strgp_t strgp) {
	ldmsd_cfgobj_unlock(&strgp->obj);
}
static inline ldmsd_strgp_t ldmsd_strgp_find(const char *name) {
	return (ldmsd_strgp_t)ldmsd_cfgobj_find(name, LDMSD_CFGOBJ_STRGP);
}
static inline const char *ldmsd_strgp_state_str(enum ldmsd_strgp_state state) {
	switch (state) {
	case LDMSD_STRGP_STATE_STOPPED:
		return "STOPPED";
	case LDMSD_STRGP_STATE_RUNNING:
		return "RUNNING";
	case LDMSD_STRGP_STATE_OPENED:
		return "RUNNING+OPENED";
	}
	return "BAD STATE";
}

/* Function to update inter-dependent configuration objects */
void ldmsd_prdcr_strgp_update(ldmsd_strgp_t strgp);
void ldmsd_strgp_prdset_update(ldmsd_prdcr_set_t prd_set);
int ldmsd_strgp_update_prdcr_set(ldmsd_strgp_t strgp, ldmsd_prdcr_set_t prd_set);
int ldmsd_updtr_prdcr_add(const char *updtr_name, const char *prdcr_regex,
			  char *rep_buf, size_t rep_len, ldmsd_sec_ctxt_t ctxt);
int ldmsd_updtr_prdcr_del(const char *updtr_name, const char *prdcr_regex,
			  char *rep_buf, size_t rep_len, ldmsd_sec_ctxt_t ctxt);
ldmsd_prdcr_ref_t ldmsd_updtr_prdcr_find(ldmsd_updtr_t updtr,
					const char *prdcr_name);
int ldmsd_updtr_schedule_cmp(void *a, const void *b);
int ldmsd_updtr_tasks_update(ldmsd_updtr_t updtr, ldmsd_prdcr_set_t prd_set);

/* Failover routines */
/*
 * active-side tasks:
 *   - pairing request
 *   - heartbeat
 *   - send redundant cfgobj
 *
 * passive-side tasks:
 *   - accept / reject pairing
 *   - receive + process redundant cfgobj
 *   - failover: activate redundant cfgobjs
 *   - failback: deactivate redundant cfgobjs
 */

typedef enum {
	FAILOVER_STATE_STOP,
	FAILOVER_STATE_START,
	FAILOVER_STATE_STOPPING,
	FAILOVER_STATE_LAST,
} failover_state_t;

typedef enum {
	FAILOVER_CONN_STATE_DISCONNECTED,
	FAILOVER_CONN_STATE_CONNECTING,
	FAILOVER_CONN_STATE_PAIRING,     /* connected, pairing in progress */
	FAILOVER_CONN_STATE_PAIRING_RETRY, /* connected, retry pairing */
	FAILOVER_CONN_STATE_RESETTING,   /* paired, resetting failover state */
	FAILOVER_CONN_STATE_CONFIGURING, /* paired, requesting peer config */
	FAILOVER_CONN_STATE_CONFIGURED,  /* peer config received */
	FAILOVER_CONN_STATE_UNPAIRING, /* unpairing (for stopping) */
	FAILOVER_CONN_STATE_ERROR,       /* error */
	FAILOVER_CONN_STATE_LAST,
} failover_conn_state_t;

typedef
struct ldmsd_failover {
	uint64_t flags;
	char host[256];
	char port[8];
	char xprt[16];
	char peer_name[512];
	int auto_switch;
	uint64_t ping_interval;
	uint64_t task_interval; /* interval for the task */
	double timeout_factor;
	pthread_mutex_t mutex;
	ldms_t ax; /* active xprt */

	failover_state_t state;
	failover_conn_state_t conn_state;

	struct timeval ping_ts;
	struct timeval echo_ts;
	struct timeval timeout_ts;

	struct ldmsd_task task;

	/* store redundant pdrcr and updtr names instead of relying on cfgobj
	 * tree so that we don't have to mess with cfgobj global locks */
	struct rbt prdcr_rbt;
	struct rbt updtr_rbt;
	struct rbt strgp_rbt;

	uint64_t moving_sum;
	int ping_idx;
	int ping_n;
	uint64_t ping_rtt[8]; /* ping round-trip time */
	uint64_t ping_max;    /* ping round-trip time max */
	uint64_t ping_avg;    /* ping round-trip time average */
	double ping_sse;      /* ping round-trip time sum of squared error */
	double ping_sd;       /* ping round-trip time standard deviation */

	int ping_skipped; /* the number of ping skipped due to outstanding */

	int perm; /* Similar to cfgobj perm */
} *ldmsd_failover_t;

extern int ldmsd_use_failover;
int ldmsd_failover_config(const char *host, const char *port, const char *xprt,
			  int auto_switch, uint64_t interval_us);
int ldmsd_failover_start();
int cfgobj_is_failover(ldmsd_cfgobj_t obj);
int ldmsd_process_deferred_act_objs(int (*filter)(ldmsd_cfgobj_t));

int ldmsd_ourcfg_start_proc();


/** Task scheduling */
void ldmsd_task_init(ldmsd_task_t task);
int ldmsd_task_start(ldmsd_task_t task,
		     ldmsd_task_fn_t task_fn, void *task_arg,
		     int flags, long sched_us, long offset_us);
int ldmsd_task_resched(ldmsd_task_t task,
		     int flags, long sched_us, long offset_us);
/**
 * Stop the task.
 *
 * \retval EINPROGRESS If `stop` has been issued and is in progress.
 * \retval EBUSY       If the task has already been `stopped`.
 * \retval 0           If `stop` command is issued successfully.
 */
int ldmsd_task_stop(ldmsd_task_t task);

void ldmsd_task_join(ldmsd_task_t task);

int ldmsd_set_update_hint_set(ldms_set_t set, long interval_us, long offset_us);
int ldmsd_set_update_hint_get(ldms_set_t set, long *interva_us, long *offset_us);

/** Regular expressions */
int ldmsd_compile_regex(regex_t *regex, const char *ex, char *errbuf, size_t errsz);

/* Listen for a connection request on an ldms xprt */
extern int listen_on_ldms_xprt(ldmsd_listen_t listen);

/* Receive a message from an ldms endpoint */
void ldmsd_recv_msg(ldms_t x, char *data, size_t data_len);

/* Get the name of this ldmsd */
const char *ldmsd_myname_get();
#pragma weak ldmsd_myname_get

/*
 * Get the auth name of this ldmsd
 *
 * If \c listen or \c listen->auth_name is NULL,
 * the default authentication is returned.
 *
 * \see struct ldmsd_cmd_line_args
 */
const char *ldmsd_auth_name_get(ldmsd_listen_t listen);

/*
 * Get the attribute of the auth method
 *
 * If \c listen or \c listen->auth_name is NULL,
 * the default authentication is returned.
 *
 * \see struct ldmsd_cmd_line_args
 */
struct attr_value_list *ldmsd_auth_attr_get(ldmsd_listen_t listen);

/*
 * Setgroup
 */
typedef struct ldmsd_setgrp {
	struct ldmsd_cfgobj obj;
	char *producer;
	long interval_us;
	long offset_us;
	struct ldmsd_str_list *member_list;
	ldms_set_t set;
} *ldmsd_setgrp_t;

static inline void ldmsd_setgrp_lock(ldmsd_setgrp_t grp) {
	ldmsd_cfgobj_lock(&grp->obj);
}
static inline void ldmsd_setgrp_unlock(ldmsd_setgrp_t grp) {
	ldmsd_cfgobj_unlock(&grp->obj);
}
static inline ldmsd_setgrp_t ldmsd_setgrp_get(ldmsd_setgrp_t grp) {
	ldmsd_cfgobj_get(&grp->obj);
	return grp;
}
static inline void ldmsd_setgrp_put(ldmsd_setgrp_t grp) {
	ldmsd_cfgobj_put(&grp->obj);
}

static inline ldmsd_setgrp_t ldmsd_setgrp_find(const char *name)
{
	return (ldmsd_setgrp_t)ldmsd_cfgobj_find(name, LDMSD_CFGOBJ_SETGRP);
}

static inline ldmsd_setgrp_t ldmsd_setgrp_first()
{
	return (ldmsd_setgrp_t)ldmsd_cfgobj_first(LDMSD_CFGOBJ_SETGRP);
}

static inline ldmsd_setgrp_t ldmsd_setgrp_next(struct ldmsd_setgrp *setgrp)
{
	return (ldmsd_setgrp_t)ldmsd_cfgobj_next(&setgrp->obj);
}

/*
 * \brief Create an LDMS set as a group set.
 *
 */

ldms_set_t
ldmsd_group_new_with_auth(const char *name, uid_t uid, gid_t gid, mode_t perm);
#pragma weak ldmsd_group_new_with_auth

ldms_set_t ldmsd_group_new(const char *name);
#pragma weak ldmsd_group_new

int ldmsd_setgrp_start(const char *name, ldmsd_sec_ctxt_t ctxt);

/*
 * \brief Delete a setgroup cfgobject
 */
int ldmsd_setgrp_del(const char *name, ldmsd_sec_ctxt_t ctxt);

/*
 * \brief Insert a set member to a set group cfgobjs
 *
 * \param name      setgroup name
 * \param instance  set instance name to be inserted or removed
 */
int ldmsd_setgrp_ins(const char *name, const char *instance);

/*
 * \brief Remove a set member from a set group cfgobjs
 *
 * \param name      setgroup name
 * \param instance  set instance name to be inserted or removed
 */
int ldmsd_setgrp_rm(const char *name, const char *instance);

/**
 * \brief Add a set into the group.
 *
 * \param grp      The group handle (from \c ldmsd_group_new()).
 * \param set_name The name of the set to be added.
 */
int ldmsd_group_set_add(ldms_set_t grp, const char *set_name);
#pragma weak ldmsd_group_set_add

/**
 * \brief Remove a set from the group.
 *
 * \param grp      The group handle (from \c ldmsd_group_new()).
 * \param set_name The name of the set to be removed.
 */
int ldmsd_group_set_rm(ldms_set_t grp, const char *set_name);
#pragma weak ldmsd_group_set_rm

enum ldmsd_group_check_flag {
	LDMSD_GROUP_IS_GROUP = 0x00000001,
	LDMSD_GROUP_MODIFIED = 0x00000002,
	LDMSD_GROUP_ERROR    = 0xF0000000,
};

/**
 * \brief Check ldmsd group status.
 *
 * \retval flags LDMSD_GROUP check flags. The caller should check the returned
 *               flags against ::ldmsd_group_check_flag enumeration.
 */
int ldmsd_group_check(ldms_set_t set);
#pragma weak ldmsd_group_check

/**
 * \brief Group member iteration callback signature.
 *
 * The callback function will be called for each member of the group.
 *
 *
 * \param grp  The group handle.
 * \param name The member name.
 * \param arg  The application-supplied generic argument.
 *
 * \retval 0     If there is no error.
 * \retval errno If an error occurred. In this case, the iteration will be
 *               stopped.
 */
typedef int (*ldmsd_group_iter_cb_t)(ldms_set_t grp, const char *name, void *arg);

/**
 * \brief Iterate over the members of the group.
 *
 * Iterate over the members of the group, calling the \c cb function for each
 * of them.
 *
 * \param grp The group handle.
 * \param cb  The callback function.
 * \param arg The argument to be supplied to the callback function.
 *
 * \retval 0     If there is no error.
 * \retval errno If failed.
 */
int ldmsd_group_iter(ldms_set_t grp, ldmsd_group_iter_cb_t cb, void *arg);
#pragma weak ldmsd_group_iter

/**
 * \brief Get the member name from a set info key.
 *
 * \retval NULL If \c info_key is NOT for set member entry.
 * \retval name If \c info_key is for set member entry.
 */
const char *ldmsd_group_member_name(const char *info_key);
#pragma weak ldmsd_group_member_name

struct update_data {
	ldmsd_updtr_t updtr;
	ldmsd_prdcr_set_t prd_set;
	int reschedule;
};

struct store_data {
	ldmsd_strgp_t strgp;
	ldmsd_prdcr_set_t prd_set;
};

struct state_data {
	ldmsd_prdcr_set_t prd_set;
	int start_n_stop;
};

struct connect_data {
	ldmsd_prdcr_t prdcr;
	int is_connected;
};

struct sample_data {
	ldmsd_smplr_t smplr;
	int reschedule;
};

struct start_data {
	void *entity;
};

struct stop_data {
	void *entity;
};

typedef struct ldmsd_req_ctxt *ldmsd_req_ctxt_t;
struct msg_ctxt_free_data {
	ldmsd_req_ctxt_t reqc;
};

ev_type_t smplr_sample_type;
ev_type_t prdcr_connect_type;
ev_type_t prdcr_set_store_type;
ev_type_t prdcr_set_state_type;
ev_type_t prdcr_set_update_type;
ev_type_t prdcr_reconnect_type;
ev_type_t updtr_start_type;
ev_type_t prdcr_start_type;
ev_type_t strgp_start_type;
ev_type_t smplr_start_type;
ev_type_t updtr_stop_type;
ev_type_t prdcr_stop_type;
ev_type_t strgp_stop_type;
ev_type_t smplr_stop_type;
ev_type_t cfg_msg_ctxt_free_type;
ev_type_t cfgobj_enabled_type;
ev_type_t cfgobj_disabled_type;

ev_worker_t producer;
ev_worker_t updater;
ev_worker_t sampler;
ev_worker_t storage;
ev_worker_t cfg;

int default_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int sample_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int prdcr_set_update_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int prdcr_connect_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int prdcr_set_state_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int prdcr_reconnect_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int updtr_start_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int updtr_stop_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int prdcr_start_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int prdcr_stop_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int store_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int strgp_start_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int strgp_stop_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int smplr_start_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int smplr_stop_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int cfg_msg_ctxt_free_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int cfgobj_enabled_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);
int cfgobj_disabled_actor(ev_worker_t src, ev_worker_t dst, ev_status_t status, ev_t ev);

#define ldmsd_prdcr_set_ref_get(_s_, _n_) _ref_get(&((_s_)->ref), (_n_), __func__, __LINE__)
#define ldmsd_prdcr_set_ref_put(_s_, _n_) _ref_put(&((_s_)->ref), (_n_), __func__, __LINE__)

/**
 * \brief Process a command-line option
 *
 * \param opt    The short-form of the command-line option
 * \param value  Value of the cmd-line option.
 *
 * \retval non-zero is returned on error. Otherwise, 0 is returned.
 */
int ldmsd_process_cmd_line_arg(char opt, char *value);

/**
 * \brief Return the number of LDMSD worker threads (specified with -P or 'num-threads')
 */
int ldmsd_worker_count_get();

/**
 * \brief Add an authentication attribute to the list \c auth_attrs
 *
 * \param auth_attrs   The authentication attribute list
 * \param name         The attribute name
 * \param value        The attribute value
 */
int ldmsd_auth_opt_add(struct attr_value_list *auth_attrs, char *name, char *val);

/**
 * \brief Create an attribute value list (\c struct attr_value_list) of authentication options
 *
 * \param auth_args_s  String of authentication options, e.g., "foo=A bar=B"
 */
struct attr_value_list *ldmsd_auth_opts_str2avl(const char *auth_args_s);

/**
 * LDMSD Authentication Domain Configuration Object
 */
typedef struct ldmsd_auth {
	struct ldmsd_cfgobj obj; /* this contains the `name` */
	char *plugin; /* auth plugin name */
	struct attr_value_list *attrs; /* attributes for the plugin */
} *ldmsd_auth_t;


/* Key (name) of the default auth -- intentionally including SPACE as it is not
 * allowed in user-defined names */
#define DEFAULT_AUTH " _DEFAULT_AUTH_ "

ldmsd_auth_t
ldmsd_auth_new(const char *name, const char *plugin, json_entity_t attrs,
		uid_t uid, gid_t gid, int perm, short enabled);
int ldmsd_auth_del(const char *name, ldmsd_sec_ctxt_t ctxt);

static inline
ldmsd_auth_t ldmsd_auth_find(const char *name)
{
	return (ldmsd_auth_t)ldmsd_cfgobj_find(name, LDMSD_CFGOBJ_AUTH);
}

int ldmsd_daemon_create_cfgobjs();
int ldmsd_daemon_cfgobjs();
#endif
