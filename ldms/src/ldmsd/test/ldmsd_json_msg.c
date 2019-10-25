#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <semaphore.h>
#include "json/json_util.h"
#include "coll/rbt.h"
#include "ldms.h"
#include "../ldmsd_request.h"

#define TEST_ECHO         1
#define TEST_RSP_MULT_REC 2
#define TEST_REQ_MULT_REC 3
#define TEST_PING_PONG    4
#define TEST_LONG_RSP     5

sem_t done;
static int test_mode;
char *mode_value;
json_entity_t exp_result; /* json object containing expecting result */

static int msg_comparator(void *a, const void *b)
{
	ldmsd_msg_key_t ak = (ldmsd_msg_key_t)a;
	ldmsd_msg_key_t bk = (ldmsd_msg_key_t)b;
	int rc;
	rc = ak->conn_id - bk->conn_id;
	if (rc)
		return rc;
	return ak->msg_no - bk->msg_no;
}
struct rbt remote_req_tree = RBT_INITIALIZER(msg_comparator);

static int json_entity_cmp(json_entity_t a, json_entity_t b)
{
	json_entity_t x, y;
	enum json_value_e a_type, b_type;
	json_str_t jstr_a, jstr_b;
	a_type = json_entity_type(a);
	b_type = json_entity_type(b);

	if (a_type != b_type)
		return 0;

	switch (a_type) {
	case JSON_INT_VALUE:
		return (json_value_int(a) == json_value_int(b));
	case JSON_FLOAT_VALUE:
		return (json_value_float(a) == json_value_float(b));
	case JSON_BOOL_VALUE:
		return (json_value_bool(a) == json_value_bool(b));
	case JSON_STRING_VALUE:
		jstr_a = json_value_str(a);
		jstr_b = json_value_str(b);
		return (0 == strcmp(jstr_a->str, jstr_b->str));
	case JSON_ATTR_VALUE:
		jstr_a = json_attr_name(a);
		jstr_b = json_attr_name(b);
		if (0 == strcmp(jstr_a->str, jstr_b->str)) {
			x = json_attr_value(a);
			y = json_attr_value(b);
			return json_entity_cmp(x, y);
		} else {
			return 0;
		}
		break;
	case JSON_LIST_VALUE:
		for (x = json_item_first(a), y = json_item_first(b); x && y;
				x = json_item_next(x), y = json_item_next(y)) {
			if (!json_entity_cmp(x, y))
				return 0;
		}
		break;
	case JSON_DICT_VALUE:
		for (x = json_attr_first(a); x; x = json_attr_next(x)) {
			y = json_attr_find(b, json_attr_name(x)->str);
			if (!y)
				return 0;
			if (!json_entity_cmp(x, y))
				return 0;
		}
		for (y = json_attr_first(b); y; y = json_attr_next(y)) {
			x = json_attr_find(a, json_attr_name(y)->str);
			if (!x)
				return 0;
			if (!json_entity_cmp(x, y))
				return 0;
		}
		break;
	default:
		fprintf(stderr, "unrecognized JSON type '%d'\n", a_type);
		assert(0);
	}
	return 1;
}

typedef struct msg_ctxt {
	ldms_t x;
	struct ldmsd_msg_key key;
	struct rbn rbn;
	ldmsd_req_buf_t recv_buf;
	json_entity_t json;
	int num_rec; /* Number of received records of the message */
} *msg_ctxt_t;

static const char *short_opts = "h:p:x:m:";

static struct option long_opts[] = {
		{ "host",	required_argument, 0, 'h' },
		{ "port",	required_argument, 0, 'p' },
		{ "xprt",	required_argument, 0, 'x' },
		{ "mode",	required_argument, 0, 'm' },
		{ 0,		0,		   0, 0 }
};

static void usage(int argc, char **argv)
{
	printf( "usage: %s -x <xprt> -h <host> -p <port>\n"
		"	-m <mode> ...\n"
		"\n"
		"		<mode>		<value>:\n"
		"		rsp_multi_rec	Number of records in the response message.\n"
		"				The default is 1.\n"
		"		echo		NO ARGUMENT\n"
		"		req_multi_rec	Number of records in the request message to LDMSD.\n"
		"				The default is 5.\n"
		"		long_rsp	The length of the response message sent back by LDMSD.\n"
		"				The default is twice of the transport maximum message length.\n"
		"\n",
			argv[0]);
	exit(1);
}

static void oom_exit()
{
	fprintf(stderr, "Out of memory\n");
	exit(ENOMEM);
}

static void assert_mode(const char *expecting, json_str_t mode_s)
{
	if (0 != strncmp(mode_s->str, expecting, mode_s->str_len)) {
		fprintf(stderr, "Expecting greeting mode '%s', "
				"but received '%s' instead.\n",
				expecting, mode_s->str);
		assert(0);
	}
}

static msg_ctxt_t msg_ctxt_alloc(ldms_t x, uint32_t msg_no)
{
	msg_ctxt_t mctxt = calloc(1, sizeof(*mctxt));
	if (!mctxt) {
		oom_exit();
	}
	mctxt->key.msg_no = msg_no;
	mctxt->key.conn_id = (uint64_t)(unsigned long)x;

	mctxt->recv_buf = ldmsd_req_buf_alloc(ldms_xprt_msg_max(x));
	if (!mctxt->recv_buf) {
		free(mctxt);
		oom_exit();
	}
	mctxt->num_rec = 1;
	rbn_init(&mctxt->rbn, &mctxt->key);
	rbt_ins(&remote_req_tree, &mctxt->rbn);
	return mctxt;
}

static msg_ctxt_t msg_ctxt_find(ldmsd_msg_key_t key)
{
	msg_ctxt_t mctxt = NULL;
	struct rbn *rbn = rbt_find(&remote_req_tree, key);
	if (rbn)
		mctxt = container_of(rbn, struct msg_ctxt, rbn);
	return mctxt;
}

static void msg_ctxt_free(msg_ctxt_t mctxt)
{
	rbt_del(&remote_req_tree, &mctxt->rbn);
	free(mctxt->recv_buf);
	free(mctxt);
}

static void get_key(ldmsd_msg_key_t key, ldms_t x)
{
	static int msg_no = 0;
	key->msg_no = msg_no++;
	key->conn_id = (uint64_t)(long unsigned)x;
}

static inline int __get_remaining(ldms_t x, ldmsd_req_buf_t buf)
{
	return ldms_xprt_msg_max(x) - buf->off;
}

void __append_send_buffer(ldms_t x, ldmsd_msg_key_t key, ldmsd_req_buf_t buf,
			int msg_flags, int msg_type, const char *fmt, ...)
{
	ldmsd_rec_hdr_t req_buff;
	size_t remaining;
	int flags, rc;
	char *data = NULL;
	size_t data_len;
	va_list ap;

	va_start(ap, fmt);
	data_len = vsnprintf(data, 0, fmt, ap);
	va_end(ap);
	data = malloc(data_len + 1);
	if (!data)
		oom_exit();
	va_start(ap, fmt);
	data_len = vsnprintf(data, data_len + 1, fmt, ap);
	va_end(ap);

	req_buff = (ldmsd_rec_hdr_t)buf->buf;
	if (0 == buf->off) {
		/* This is a new buffer. Set the offset to the header size. */
		buf->off = sizeof(struct ldmsd_rec_hdr_s);
	}

	do {
		remaining = __get_remaining(x, buf);
		if (data_len < remaining)
			remaining = data_len;

		if (remaining && data) {
			memcpy(&buf->buf[buf->off], data, remaining);
			buf->off += remaining;
			data_len -= remaining;
			data += remaining;
		}

		if ((remaining == 0) ||
		    ((data_len == 0) && (msg_flags & LDMSD_REC_EOM_F))) {
			/* If this is the first record in the response, set the
			 * SOM_F bit. If the caller set the EOM_F bit and we've
			 * exhausted data_len, set the EOM_F bit.
			 * If we've exhausted the reply buffer, unset the EOM_F bit.
			 */
			flags = msg_flags & ((!remaining && data_len)?(~LDMSD_REC_EOM_F):LDMSD_REC_EOM_F);
			flags |= (buf->num_rec == 0?LDMSD_REC_SOM_F:0);
			/* Record is full, send it on it's way */
			req_buff->type = msg_type;
			req_buff->flags = flags;
			req_buff->msg_no = key->msg_no;
			req_buff->rec_len = buf->off;
			ldmsd_hton_rec_hdr(req_buff);
			rc = ldms_xprt_send(x, (char *)req_buff, buf->off);
			if (rc) {
				/* The content in reqc->rep_buf hasn't been sent. */
				fprintf(stderr, "failed to send a message to "
						"LDMSD with error %d\n", rc);
				exit(1);
			}
			buf->num_rec++;
			/* Reset the reply buffer for the next record for this message */
			buf->off = sizeof(*req_buff);
			buf->buf[buf->off] = '\0';
		}
	} while (data_len);

	return;
}

static void test_err(json_entity_t json)
{
	json_entity_t errcode, msg;
	errcode = json_value_find(json, "errcode");
	if (!errcode) {
		fprintf(stderr, "Missing the mandatory 'errcode'\n");
		assert(0);
	}
	msg = json_value_find(json, "msg");
	if (!msg) {
		fprintf(stderr, "Missing the mandatory 'msg'\n");
		assert(0);
	}
	fprintf(stderr, "Received an error from ldmsd. Error %" PRIu64 ": %s\n",
			json_value_int(errcode), json_value_str(msg)->str);
}

static void test_req_multi_rec(msg_ctxt_t msgc, json_entity_t info, json_str_t mode_s)
{
	json_entity_t list, item;
	json_str_t item_s;
	int rec_no = 0;
	int num_rec = atoi(mode_value);
	char *space;
	char str[1024];

	assert_mode("echo", mode_s);

	list = json_value_find(info, "list");
	if (!list) {
		fprintf(stderr, "Missing attr 'list'\n");
		assert(0);
	}
	if (JSON_LIST_VALUE != json_entity_type(list)) {
		fprintf(stderr, "Received a wrong json type '%s', expecting '%s'\n",
				json_type_str(json_entity_type(list)),
				json_type_str(JSON_LIST_VALUE));
		assert(0);
	}

	for (item = json_item_first(list); item; item = json_item_next(item)) {
		item_s = json_value_str(item);
		snprintf(str, 1024, "rec_%d", rec_no);
		space = strchr(item_s->str, ' ');
		space[0] = '\0';
		if (0 != strncmp(str, item_s->str, item_s->str_len)) {
			fprintf(stderr, "Received unexpected data back\n");
			assert(0);
		}
		rec_no++;
	}
	if (rec_no != num_rec) {
		fprintf(stderr, "Received wrong data\n");
		assert(0);
	}
	return;
}

static void test_rsp_multi_rec(msg_ctxt_t msgc, json_entity_t info, json_str_t mode_s)
{
	json_entity_t records, jstr;
	json_str_t str;
	char *space;
	int rec_cnt = 0;
	int num_rec = atoi(mode_value);

	assert_mode("rsp_multi_rec", mode_s);

	if (msgc->num_rec != num_rec) {
		fprintf(stderr, "received '%d' records instead of %d\n",
						msgc->num_rec, num_rec);
		assert(0);
	}
	records = json_value_find(info, "records");
	if (!records) {
		fprintf(stderr, "Missing attr 'records'\n");
		assert(0);
	}
	if (JSON_LIST_VALUE != json_entity_type(records)) {
		fprintf(stderr, "Records type is '%s' instead of '%s'\n",
				json_type_str(json_entity_type(records)),
				json_type_str(JSON_LIST_VALUE));
		assert(0);
	}
	for (jstr = json_item_first(records); jstr; jstr = json_item_next(jstr)) {
		str = json_value_str(jstr);

		if (str->str[str->str_len - 1] != ' ') {
			fprintf(stderr, "Received corrupted data\n");
			assert(0);
		}

		space = strchr(str->str, ' ');
		space[0] = '\0'; /* Trim the spaces */
		fprintf(stdout, "%s\n", str->str);
		rec_cnt++;
	}
	if (rec_cnt != num_rec) {
		fprintf(stderr, "Expecting to receive '%d' records, "
				"but received '%d' records instead.\n",
				num_rec, rec_cnt);
		assert(0);
	}
	return;
}

static void test_echo(msg_ctxt_t msgc, json_entity_t info, json_str_t mode_s)
{
	json_entity_t list;
	jbuf_t exp_jb = NULL;
	jbuf_t recv_jb = NULL;

	assert_mode("echo", mode_s);

	list = json_value_find(info, "list");
	if (!list) {
		fprintf(stderr, "Missing attr 'list'\n");
		assert(0);
	}
	if (JSON_LIST_VALUE != json_entity_type(list)) {
		fprintf(stderr, "Received a wrong json type '%s', expecting '%s'\n",
				json_type_str(json_entity_type(list)),
				json_type_str(JSON_LIST_VALUE));
		assert(0);
	}
	if (!json_entity_cmp(exp_result, list)) {
		exp_jb = json_entity_dump(exp_jb, exp_result);
		recv_jb = json_entity_dump(recv_jb, list);
		fprintf(stderr, "expecting: %s\n", exp_jb->buf);
		fprintf(stderr, "recv: %s\n", recv_jb->buf);
		assert(0);
	}
}

static void test_ping_pong(msg_ctxt_t msgc, json_entity_t info, json_str_t mode_s)
{
	json_entity_t round, words;
	jbuf_t exp_jb, jb;
	int cnt, num_round;
	exp_jb = jb = NULL;

	num_round = atoi(mode_value);

	assert_mode("ping_pong", mode_s);

	round = json_value_find(info, "round");
	if (!round) {
		fprintf(stderr, "Missing attr 'list'\n");
		assert(0);
	}
	cnt = json_value_int(round);

	if (cnt != num_round) {
		fprintf(stderr, "Expecting number of rounds '%d' but %d "
				"rounds occurred\n", num_round, cnt);
		assert(0);
	}

	words = json_value_find(info, "words");
	if (0 == json_entity_cmp(exp_result, words)) {
		exp_jb = json_entity_dump(exp_jb, exp_result);
		jb = json_entity_dump(jb, words);
		fprintf(stderr, "expecting: %s\n", exp_jb->buf);
		fprintf(stderr, "received: %s\n", jb->buf);
		assert(0);
	}
}

static void test_long_rsp(msg_ctxt_t msgc, json_entity_t info, json_str_t mode_s)
{
	json_entity_t msg;

	assert_mode("long_rsp", mode_s);

	msg = json_value_find(info, "msg");
	if (!msg) {
		fprintf(stderr, "Missing attr 'msg'\n");
		assert(0);
	}

	if (0 == json_entity_cmp(exp_result, msg)) {
		fprintf(stderr, "expecting: %s\n", json_value_str(exp_result)->str);
		fprintf(stderr, "received : %s\n", json_value_str(msg)->str);
		assert(0);
	}
}

static void handle_resp(msg_ctxt_t msgc)
{
	char *attr_name;
	json_entity_t type, name, info, mode;
	json_str_t type_s, name_s, mode_s;

	type = json_value_find(msgc->json, "type");
	if (!type) {
		attr_name = "type";
		goto missing;
	}
	type_s = json_value_str(type);
	if (0 == strncmp(type_s->str, "err", type_s->str_len)) {
		test_err(msgc->json);
		return;
	} else if (0 == strncmp(type_s->str, "info", type_s->str_len)){
		name = json_value_find(msgc->json, "name");
		if (!name) {
			attr_name = "name";
			goto missing;
		}
		name_s = json_value_str(name);
		if (0 == strncmp(name_s->str, "greeting", name_s->str_len)) {
			info = json_value_find(msgc->json, "info");
			if (!info) {
				attr_name = "info";
				goto missing;
			}

			mode = json_value_find(info, "mode");
			if (!mode) {
				attr_name = "mode";
				goto missing;
			}
			mode_s = json_value_str(mode);

			switch (test_mode) {
			case TEST_RSP_MULT_REC:
				test_rsp_multi_rec(msgc, info, mode_s);
				break;
			case TEST_ECHO:
				test_echo(msgc, info, mode_s);
				break;
			case TEST_REQ_MULT_REC:
				test_req_multi_rec(msgc, info, mode_s);
				break;
			case TEST_PING_PONG:
				test_ping_pong(msgc, info, mode_s);
				break;
			case TEST_LONG_RSP:
				test_long_rsp(msgc, info, mode_s);
				break;
			default:
				fprintf(stderr, "Unrecognized test mode '%s'\n", mode_s->str);
				assert(0);
				break;
			}
		} else {
			fprintf(stderr, "Unexpected response '%s'\n", name_s->str);
			assert(0);
		}
	}
	sem_post(&done);
	return;

missing:
	fprintf(stderr, "Missing mandatory attr '%s'\n", attr_name);
	assert(0);
}

static void ping_pong_request(msg_ctxt_t msgc, json_entity_t spec)
{
	json_entity_t round;
	ldmsd_req_buf_t buf;
	int num_round = atoi(mode_value);
	int cnt;

	round = json_value_find(spec, "round");
	if (!round) {
		fprintf(stderr, "Missing mandatory attr 'round'\n");
		assert(0);
	}
	cnt = json_value_int(round);

	buf = ldmsd_req_buf_alloc(ldms_xprt_msg_max(msgc->x));
	if (!buf)
		oom_exit();

	__append_send_buffer(msgc->x, &msgc->key, buf, LDMSD_REC_SOM_F,
			LDMSD_MSG_TYPE_RESP, "{\"type\":\"cmd_obj\","
					     "\"cmd\":\"greeting\","
					     "\"spec\":{\"mode\":\"ping_pong\","
					     "\"value\":{\"word\":\"%d\"", cnt);
	if (cnt < num_round) {
		__append_send_buffer(msgc->x, &msgc->key, buf,
				LDMSD_REC_EOM_F, LDMSD_MSG_TYPE_RESP,
				",\"more\":true}}}");
	} else {
		__append_send_buffer(msgc->x, &msgc->key, buf,
				LDMSD_REC_EOM_F, LDMSD_MSG_TYPE_RESP,
				"}}}");
	}
	ldmsd_req_buf_free(buf);
}

static void handle_request(msg_ctxt_t msgc)
{
	char *attr_name;
	json_entity_t type, cmd, spec;
	json_str_t type_s, cmd_s;

	type = json_value_find(msgc->json, "type");
	if (!type) {
		attr_name = "type";
		goto missing;
	}
	type_s = json_value_str(type);
	if (0 == strncmp(type_s->str, "cmd_obj", type_s->str_len)) {
		cmd = json_value_find(msgc->json, "cmd");
		if (!cmd) {
			attr_name = "cmd";
			goto missing;
		}
		cmd_s = json_value_str(cmd);

		spec = json_value_find(msgc->json, "spec");
		if (!spec) {
			attr_name = "spec";
			goto missing;
		}

		if (0 == strncmp(cmd_s->str, "ping_pong", cmd_s->str_len)) {
			ping_pong_request(msgc, spec);
		} else {
			fprintf(stderr, "Unexpected request command %s\n", cmd_s->str);
			assert(0);
		}
	} else {
		fprintf(stderr, "Unexpected request object type '%s'\n", type_s->str);
		assert(0);
	}
	return;
missing:
	fprintf(stderr, "Missing mandatory attr '%s'\n", attr_name);
	assert(0);
}

static msg_ctxt_t process_records(ldms_t x, ldmsd_rec_hdr_t hdr)
{
	int rc;
	msg_ctxt_t msgc;
	json_parser_t parser;
	size_t data_len = hdr->rec_len - sizeof(*hdr);

	msgc = msg_ctxt_find(&hdr->key);

	if (hdr->flags & LDMSD_REC_SOM_F) {
		msgc = msg_ctxt_alloc(x, &hdr->msg_no);
		if (!msgc)
			oom_exit();
		msgc->x = x;
	} else {
		if (!msgc) {
			fprintf(stderr, "Failed to find the previous record "
					"of msg_no %d", hdr->key.msg_no);
			assert(0);
		}
		msgc->num_rec++;
	}
	if (msgc->recv_buf->len - msgc->recv_buf->off < data_len) {
		msgc->recv_buf = ldmsd_req_buf_realloc(msgc->recv_buf,
					2 * (msgc->recv_buf->off + data_len));
		if (!msgc->recv_buf) {
			msg_ctxt_free(msgc);
			msgc = NULL;
			oom_exit();
		}
	}
	memcpy(&msgc->recv_buf->buf[msgc->recv_buf->off], hdr + 1, data_len);
	msgc->recv_buf->off += data_len;

	if (!(hdr->flags & LDMSD_REC_EOM_F)) {
		return NULL;
	}

	/* Parse the JSON string */
	parser = json_parser_new(0);
	if (!parser)
		oom_exit();
	rc = json_parse_buffer(parser, msgc->recv_buf->buf, msgc->recv_buf->off, &msgc->json);
	if (rc) {
		fprintf(stderr, "Failed to parse the JSON string. %s\n", msgc->recv_buf->buf);
		assert(0);
	}
	return msgc;
}

static void recv_msg(ldms_t x, char *data, size_t data_len)
{
	msg_ctxt_t msgc;
	ldmsd_rec_hdr_t hdr = (ldmsd_rec_hdr_t)data;
	ldmsd_ntoh_rec_hdr(hdr);

	if (hdr->rec_len > ldms_xprt_msg_max(x)) {
		fprintf(stderr, "A record size is larger than the max transport length\n");
		assert(0);
	}

	msgc = process_records(x, hdr);
	if (!msgc) {
		/* Nothing to do */
		return;
	}

	switch (hdr->type) {
	case LDMSD_MSG_TYPE_RESP:
		/*
		 * Assume that the whole message is in one record.
		 */
		handle_resp(msgc);
		msg_ctxt_free(msgc);
		break;
	case LDMSD_MSG_TYPE_REQ:
		handle_request(msgc);
		msg_ctxt_free(msgc);
		break;
	default:
		assert(0);
	}
}

static json_entity_t rsp_multiple_records(json_entity_t spec)
{
	int num_rec;
	if (!mode_value) {
		fprintf(stderr, "Required the number records\n");
		exit(EINVAL);
	} else {
		num_rec = atoi(mode_value);
	}
	json_entity_t attr, name, value;

	value = json_entity_new(JSON_STRING_VALUE, "rsp_multi_rec");
	attr = json_entity_new(JSON_ATTR_VALUE, "mode", value);
	json_attr_add(spec, attr);

	value = json_entity_new(JSON_INT_VALUE, num_rec);
	attr = json_entity_new(JSON_ATTR_VALUE, "value", value);
	json_attr_add(spec, attr);

	return spec;
}

static json_entity_t echo(json_entity_t spec)
{
	json_entity_t name, value, attr;

	value = json_entity_new(JSON_STRING_VALUE, "echo");
	attr = json_entity_new(JSON_ATTR_VALUE, "mode", value);
	json_attr_add(spec, attr);

	value = json_entity_new(JSON_LIST_VALUE);
	attr = json_entity_new(JSON_ATTR_VALUE, "value", value);
	json_attr_add(spec, attr);

	name = json_entity_new(JSON_STRING_VALUE, "Hello!");
	json_item_add(value, name);
	exp_result = value;
	return spec;
}

static json_entity_t ping_pong(json_entity_t spec)
{
	json_entity_t name, value, attr, dict;
	char str[256];
	int num_round, i;

	num_round = 5;
	if (mode_value)
		num_round = atoi(mode_value);
	else
		mode_value = "5";

	value = json_entity_new(JSON_STRING_VALUE, "ping_pong");
	attr = json_entity_new(JSON_ATTR_VALUE, "mode", value);
	json_attr_add(spec, attr);

	dict = json_entity_new(JSON_DICT_VALUE);
	attr = json_entity_new(JSON_ATTR_VALUE, "value", dict);
	json_attr_add(spec, attr);

	value = json_entity_new(JSON_STRING_VALUE, "0");
	attr = json_entity_new(JSON_ATTR_VALUE, "word", value);
	json_attr_add(dict, attr);

	if (num_round > 1) {
		value = json_entity_new(JSON_BOOL_VALUE, 1);
		attr = json_entity_new(JSON_ATTR_VALUE, "more", value);
		json_attr_add(dict, attr);
	}

	exp_result = json_entity_new(JSON_LIST_VALUE);
	for (i = num_round; i >= 1; i--) {
		snprintf(str, 256, "%d", i);
		value = json_entity_new(JSON_STRING_VALUE, str);
		json_item_add(exp_result, value);
	}

	return spec;
}

static json_entity_t long_rsp(json_entity_t spec, size_t max_msg)
{
	json_entity_t name, value, attr, dict;
	int len;
	char *msg;

	if (!mode_value) {
		char value_s[128];
		snprintf(value_s, 128, "%ld", max_msg * 2);
		mode_value = value_s;
	}
	len = atoi(mode_value);

	value = json_entity_new(JSON_STRING_VALUE, "long_rsp");
	attr = json_entity_new(JSON_ATTR_VALUE, "mode", value);
	json_attr_add(spec, attr);

	dict = json_entity_new(JSON_DICT_VALUE);
	attr = json_entity_new(JSON_ATTR_VALUE, "value", dict);
	json_attr_add(spec, attr);

	value = json_entity_new(JSON_INT_VALUE, len);
	attr = json_entity_new(JSON_ATTR_VALUE, "ength", value);
	json_attr_add(dict, attr);

	msg = malloc(len + 1);
	if (!msg)
		oom_exit();
	snprintf(msg, len + 1, "%0*d", len, len);
	exp_result = json_entity_new(JSON_STRING_VALUE, msg);
	free(msg);
	return spec;
}

static void req_multiple_records(ldms_t x)
{
	struct ldmsd_msg_key key;
	int num_rec = 5;
	if (mode_value) {
		num_rec = atoi(mode_value);
	} else {
		mode_value = "5";
	}

	ldmsd_req_buf_t buf;
	char *s;
	int rec_cnt = 0;

	get_key(&key, x);
	buf = ldmsd_req_buf_alloc(ldms_xprt_msg_max(x));
	if (!buf)
		oom_exit();
	s = "{\"type\":\"cmd_obj\","
	    "\"cmd\":\"greeting\","
	    "\"spec\":{\"mode\":\"echo\","
	    "\"value\":[";

	__append_send_buffer(x, &key, buf, LDMSD_REC_SOM_F,
					LDMSD_MSG_TYPE_REQ, "%s", s);

	do {
		__append_send_buffer(x, &key, buf, 0, LDMSD_MSG_TYPE_REQ,
							"\"rec_%d", rec_cnt);
		if (rec_cnt + 1 == num_rec) {
			__append_send_buffer(x, &key, buf, LDMSD_REC_EOM_F,
					LDMSD_MSG_TYPE_REQ, "%*s\"]}}",
					__get_remaining(x, buf) - 4, "");
			goto out;
		} else {
			__append_send_buffer(x, &key, buf, 0,
					LDMSD_MSG_TYPE_REQ, "%*s\",",
					__get_remaining(x, buf) - 1, "");
			rec_cnt++;
		}
	} while (rec_cnt < num_rec);
out:
	ldmsd_req_buf_free(buf);
	return;
}

static void start_test(ldms_t x)
{
	ldmsd_req_buf_t buf;
	jbuf_t jb = NULL;
	json_entity_t obj, name, value, attr, spec;
	struct ldmsd_msg_key key;

	if (TEST_REQ_MULT_REC == test_mode) {
		/*
		 * Handle this mode separately.
		 */
		req_multiple_records(x);
		return;
	}

	buf = ldmsd_req_buf_alloc(ldms_xprt_msg_max(x));
	if (!buf)
		oom_exit();

	obj = json_entity_new(JSON_DICT_VALUE);
	value = json_entity_new(JSON_STRING_VALUE, "cmd_obj");
	attr = json_entity_new(JSON_ATTR_VALUE, "type", value);
	json_attr_add(obj, attr);

	value = json_entity_new(JSON_STRING_VALUE, "greeting");
	attr = json_entity_new(JSON_ATTR_VALUE, "cmd", value);
	json_attr_add(obj, attr);

	spec = json_entity_new(JSON_DICT_VALUE);
	attr = json_entity_new(JSON_ATTR_VALUE, "spec", spec);
	json_attr_add(obj, attr);

	switch (test_mode) {
	case TEST_RSP_MULT_REC:
		spec = rsp_multiple_records(spec);
		break;
	case TEST_REQ_MULT_REC:
		/* handle separately */
		break;
	case TEST_ECHO:
		spec = echo(spec);
		break;
	case TEST_PING_PONG:
		spec = ping_pong(spec);
		break;
	case TEST_LONG_RSP:
		spec = long_rsp(spec, ldms_xprt_msg_max(x));
		break;
	default:
		break;
	}
	jb = json_entity_dump(jb, obj);
	if (!jb)
		goto oom;

	get_key(&key, x);
	__append_send_buffer(x, &key, buf,
			LDMSD_REC_SOM_F|LDMSD_REC_EOM_F,
			LDMSD_MSG_TYPE_REQ, "%s", jb->buf);
	jbuf_free(jb);
	ldmsd_req_buf_free(buf);
	return;
oom:
	oom_exit();
}

static void event_cb(ldms_t x, ldms_xprt_event_t e, void *cb_arg)
{
	switch (e->type) {
	case LDMS_XPRT_EVENT_CONNECTED:
		fprintf(stdout, "connected\n");
		start_test(x);
		break;
	case LDMS_XPRT_EVENT_REJECTED:
		fprintf(stdout, "rejected\n");
		break;
	case LDMS_XPRT_EVENT_DISCONNECTED:
		fprintf(stdout, "disconnected\n");
		exit(0);
	case LDMS_XPRT_EVENT_ERROR:
		fprintf(stderr, "connection error\n");
		exit(0);
	case LDMS_XPRT_EVENT_RECV:
		recv_msg(x, e->data, e->data_len);
		break;
	default:
		assert(0);
	}
}

static int connect_ldmsd(const char *host, const char *port, const char *xprt)
{
	ldms_t ldms;
	int rc;
	struct addrinfo *ai;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM
	};

	rc = getaddrinfo(host, port, &hints, &ai);
	if (rc)
		return EHOSTUNREACH;

	ldms = ldms_xprt_new(xprt, NULL);
	if (!ldms) {
		fprintf(stderr, "Failed to create ldms endpoint\n");
		return ENOMEM;
	}

	rc = ldms_xprt_connect(ldms,ai->ai_addr, ai->ai_addrlen, event_cb, NULL);
	if (rc) {
		ldms_xprt_put(ldms);
		return rc;
	}
	return 0;
}

int main(int argc, char **argv) {
	char *host = "localhost";
	char *port = NULL;
	char *xprt = "sock";
	char *mode = NULL;
	int rc;

	int opt, opt_idx;

	while ((opt = getopt_long(argc, argv,
				short_opts, long_opts,
				&opt_idx)) > 0) {
		switch (opt) {
		case 'h':
			host = strdup(optarg);
			break;
		case 'x':
			xprt = strdup(optarg);
			break;
		case 'p':
			port = strdup(optarg);
			break;
		case 'm':
			mode = strdup(optarg);
			break;
		default:
			usage(argc, argv);
		}
	}

	if (!port || !mode)
		usage(argc, argv);

	if (0 == strcmp(mode, "rsp_multi_rec")) {
		test_mode = TEST_RSP_MULT_REC;
	} else if (0 == strcmp(mode, "echo")) {
		test_mode = TEST_ECHO;
	} else if (0 == strcmp(mode, "req_multi_rec")) {
		test_mode = TEST_REQ_MULT_REC;
	} else if (0 == strcmp(mode, "ping_pong")) {
		test_mode = TEST_PING_PONG;
	} else if (0 == strcmp(mode, "long_rsp")) {
		test_mode = TEST_LONG_RSP;
	} else {
		fprintf(stderr, "Unrecognized test '%s'\n", mode);
		usage(argc, argv);
	}

	if (optind != argc) {
		mode_value = strdup(argv[optind]);
	}

	sem_init(&done, 0, 0);

	rc = connect_ldmsd(host, port, xprt);
	if (rc) {
		fprintf(stderr, "Failed to connect to ldmsd\n");
		return rc;
	}

	sem_wait(&done);
	fprintf(stdout, "PASSED\n");
	return 0;
}
