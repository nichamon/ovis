#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <coll/rbt.h>
#include "ev.h"
#include "ev_priv.h"

#ifdef _EV_TRACK_
#include <stdio.h>
static int ev_cmp(void *a, const void *b)
{
	return (uint64_t)a - (uint64_t)b;
}
static pthread_mutex_t ev_tree_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rbt ev_tree = RBT_INITIALIZER(ev_cmp);
static pthread_mutex_t free_ev_tree_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rbt free_ev_tree = RBT_INITIALIZER(ev_cmp);

static void __print_leading(FILE *f, int leading)
{
	fprintf(f, "%*s", leading, "");
}
static void __ev_dump_detail(struct ev__s *e, FILE *f, int leading)
{
	__print_leading(f, leading);
	fprintf(f, "ID: %" PRIu64 "\n", e->id);
	__print_leading(f, leading);
	fprintf(f, "type: %s\n", e->e_type->t_name);
	__print_leading(f, leading);
	fprintf(f, "new: %s():%d\n", e->new_func, e->new_line);
	__print_leading(f, leading);
	fprintf(f, "del: ");
	if (e->del_func)
		fprintf(f, "%s():%d\n", e->del_func, e->del_line);
	else
		fprintf(f, "N/A\n");
	__print_leading(f, leading);
	fprintf(f, "status: %s\n", (e->e_status == EV_OK)?"OK":"FLUSH");
	__print_leading(f, leading);
	fprintf(f, "src -> dst: ");
	if (e->e_dst) {
		fprintf(f, "%s [%s():%d] -> %s -- %s\n",
				(e->e_src)?(e->e_src->w_name):"NA",
				e->post_func, e->post_line,
				e->e_dst->w_name,
				(e->e_posted)?"NOT DELIVERED":"DELIVERED");
	} else {
		fprintf(f, "not posted\n");
	}
}

static void __ev_dump_single(struct ev__s *e, FILE *f, int leading)
{
	struct rbn *rbn;
	__print_leading(f, leading);
	fprintf(f, "%" PRIu64 " \"%s\" [%s():%d] -- %s ",
					e->id, e->e_type->t_name,
					e->new_func, e->new_line,
					(e->e_status == EV_OK)?"OK":"FLUSH");

	rbn = rbt_find(&ev_tree, (void *)e->id);
	if (!rbn)
		fprintf(f, " --- not in ev_tree.");

	if (e->e_dst) {
		fprintf(f, "-- %s [%s():%d] -> %s -- %s ",
				(e->e_src)?(e->e_src->w_name):"NA",
				e->post_func, e->post_line,
				e->e_dst->w_name,
				(e->e_posted)?"NOT DELIVERED":"DELIVERED");
	} else {
		fprintf(f, "-- not posted ");
	}
}

static void __ev_dump(struct ev__s *e, FILE *f, int is_print_parent, int leading, int detail)
{
	struct ev__s *c;

	if (detail)
		__ev_dump_detail(e, f, leading);
	else
		__ev_dump_single(e, f, leading);

	if (is_print_parent) {
		fprintf(f, "-- parent: ");
		if (e->parent) {
			fprintf(f, "%" PRIu64 " ", e->parent->id);
			fprintf(f, "%s", ((!e->is_parent_alive)?"app not used anymore":""));
		} else {
			fprintf(f, "NONE ");
		}
	}
	fprintf(f, "\n");

	TAILQ_FOREACH(c, &e->children, child_ent) {
		__ev_dump(c, f, 0, leading + 2, 0);
	}
}

__attribute__((unused))
static void ev_dump(ev_t ev, uint64_t id, FILE *f)
{
	struct rbn *rbn;
	struct ev__s *e;
	if (!ev) {
		rbn = rbt_find(&ev_tree, (void *)id);
		if (!rbn) {
			rbn = rbt_find(&free_ev_tree, (void *)id);
			if (!rbn) {
				fprintf(f, "ev %" PRIu64 " not found\n", id);
				return;
			} else {
				fprintf(f, "-- ev's ref_count is 0. --\n");
			}
		}
		e = container_of(rbn, struct ev__s, rbn);
	} else {
		e = EV(ev);
	}
	__ev_dump(e, f, 1, 1, 1);
}

__attribute__((unused))
static void __ev_tree_dump(FILE *f, int is_free_tree)
{
	struct ev__s *e;
	struct rbn *rbn;
	char *s;
	struct rbt *t;
	pthread_mutex_t l;

	if (is_free_tree) {
		fprintf(f, "free_ev_tree\n");
		t = &free_ev_tree;
		l = free_ev_tree_lock;
	} else {
		fprintf(f, "ev_tree\n");
		t = &ev_tree;
		l = ev_tree_lock;
	}

	fprintf(f, "%5s %16s %3s %20s %7s %32s %s\n",
			"ID", "type", "ref",
			"new_func", "newline",
			"src->dst", "status");
	pthread_mutex_lock(&l);
	rbn = rbt_min(t);
	while (rbn) {
		e = container_of(rbn, struct ev__s, rbn);
		fprintf(f, "%5" PRIu64 " %16s %3d %20s %7d ",
					e->id, e->e_type->t_name,
					e->e_refcount,
					e->new_func, e->new_line);
		(void) asprintf(&s, "(%s -> %s)",
				(e->e_src)?(e->e_src->w_name):"",
				(e->e_dst)?(e->e_dst->w_name):"");
		fprintf(f, "%32s ", s);
		free(s);
		fprintf(f, "%s", (e->e_status == EV_OK)?"OK":"FLUSH");
		fprintf(f, "\n");
		rbn = rbn_succ(rbn);
	}
	pthread_mutex_unlock(&l);
}

void ev_tree_dump()
{
	__ev_tree_dump(stdout, 0);
}

void free_ev_tree_dump()
{
	__ev_tree_dump(stdout, 1);
}

#endif /* _EV_TRACK_ */

static uint32_t next_type_id = 1;

static int type_cmp(void *a, const void *b)
{
	return strcmp(a, b);
}

static pthread_mutex_t type_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rbt type_tree = RBT_INITIALIZER(type_cmp);

ev_type_t ev_type_new(const char *name, size_t size)
{
	ev_type_t evt = calloc(1, sizeof(*evt));
	struct rbn *rbn;

	evt = calloc(1, sizeof(*evt));
	if (!evt)
		goto err_0;
	evt->t_name = strdup(name);
	if (!evt->t_name)
		goto err_1;
	evt->t_size = size;
	evt->t_id = __sync_fetch_and_add(&next_type_id, 1);

	pthread_mutex_lock(&type_lock);
	rbn = rbt_find(&type_tree, name);
	if (rbn)
		goto err_2;
	rbn_init(&evt->t_rbn, evt->t_name);
	rbt_ins(&type_tree, &evt->t_rbn);
	pthread_mutex_unlock(&type_lock);
	return evt;

 err_2:
	pthread_mutex_unlock(&type_lock);
 err_1:
	free(evt->t_name);
 err_0:
	free(evt);
	return NULL;
}

ev_type_t ev_type_get(const char *name)
{
	ev_type_t evt = NULL;
	struct rbn *rbn;

	pthread_mutex_lock(&type_lock);
	rbn = rbt_find(&type_tree, name);
	if (rbn) {
		evt = container_of(rbn, struct ev_type_s, t_rbn);
	} else {
		errno = ENOENT;
	}
	pthread_mutex_unlock(&type_lock);
	return evt;
}

ev_type_t ev_type(ev_t ev)
{
	ev__t e = EV(ev);
	return e->e_type;
}

uint32_t ev_type_id(ev_type_t t)
{
	return t->t_id;
}

const char *ev_type_name(ev_type_t t)
{
	return t->t_name;
}

ev_t _ev_new(ev_type_t evt, const char *func, int line)
{
	ev__t e = malloc(sizeof(*e) + evt->t_size);
	if (!e)
		return NULL;

	e->e_refcount = 1;
	e->e_type = evt;
	e->e_posted = 0;

#ifdef _EV_TRACK_
	static uint64_t id = 1;
	TAILQ_INIT(&e->children);
	e->num_children = 0;
	e->id = id++;
	e->new_func = func;
	e->new_line = line;
	rbn_init(&e->rbn, (void *)e->id);
	pthread_mutex_init(&e->lock, NULL);
	pthread_mutex_lock(&ev_tree_lock);
	rbt_ins(&ev_tree, &e->rbn);
	pthread_mutex_unlock(&ev_tree_lock);
	e->e_src = e->e_dst = NULL;
	e->e_status = EV_OK;

	extern ev_worker_t worker_find_by_thr(pthread_t thread);
	pthread_t thr = pthread_self();
	struct ev_worker_s *src = worker_find_by_thr(thr);
	if (!src) {
		e->parent = NULL;
	} else {
		e->parent = src->cur_ev;
		__sync_fetch_and_add(&e->parent->num_children, 1);
		pthread_mutex_lock(&src->cur_ev->lock);
		TAILQ_INSERT_TAIL(&src->cur_ev->children, e, child_ent);
		pthread_mutex_unlock(&src->cur_ev->lock);
		e->is_parent_alive = 1;
	}

#endif /* _EV_TRACK_ */
	return (ev_t)&e->e_ev;
}

void _ev_put(ev_t ev, const char *func, int line)
{
	ev__t e = EV(ev);
	assert(e->e_refcount);
	if (0 == __sync_sub_and_fetch(&e->e_refcount, 1)) {
#ifdef _EV_TRACK_
		ev__t p;
		e->del_func = func;
		e->del_line = line;
		pthread_mutex_lock(&ev_tree_lock);
		rbt_del(&ev_tree, &e->rbn);
		pthread_mutex_unlock(&ev_tree_lock);
		pthread_mutex_lock(&free_ev_tree_lock);
		rbn_init(&e->rbn, e->rbn.key);
		rbt_ins(&free_ev_tree, &e->rbn);
		pthread_mutex_unlock(&free_ev_tree_lock);

		struct ev__s *ch;
		TAILQ_FOREACH(ch, &e->children, child_ent)
			ch->is_parent_alive = 0;

		if ((p = e->parent)) {
			/* Remove itself from its parent's children list */
			pthread_mutex_lock(&p->lock);
			TAILQ_REMOVE(&p->children, e, child_ent);
			pthread_mutex_unlock(&p->lock);
			if (0 == __sync_sub_and_fetch(&p->num_children, 1)) {
				pthread_mutex_lock(&free_ev_tree_lock);
				struct rbn *n = rbt_find(&free_ev_tree, p->rbn.key);
				if (n) {
					rbt_del(&free_ev_tree, n);
					e->parent = NULL;
					free(p);
				}
				pthread_mutex_unlock(&free_ev_tree_lock);
			}
		}

		if (0 == e->num_children) {
			pthread_mutex_lock(&free_ev_tree_lock);
			struct rbn *n = rbt_find(&free_ev_tree, e->rbn.key);
			if (n) {
				rbt_del(&free_ev_tree, n);
				free(e);
			}
			pthread_mutex_unlock(&free_ev_tree_lock);
		}
#else /* _EV_TRAC_ */
		free(e);
#endif /* _EV_TRACK_ */
	}
}

ev_t ev_get(ev_t ev)
{
	ev__t e = EV(ev);
	assert(__sync_fetch_and_add(&e->e_refcount, 1) > 0);
	return ev;
}

int ev_posted(ev_t ev)
{
	ev__t e = EV(ev);
	return e->e_posted;
}

#ifdef _EV_TRACK_
__attribute__((unused))
static const char *ev_post_func(ev_t ev)
{
	ev__t e = EV(ev);
	return e->post_func;
}

__attribute__((unused))
static int ev_post_line(ev_t ev)
{
	ev__t e = EV(ev);
	return e->post_line;
}

#endif /* _EV_TRACK_ */

int _ev_post(ev_worker_t src, ev_worker_t dst, ev_t ev, struct timespec *to,
					const char *func, int line)
{
	ev__t e = EV(ev);
	int rc;
#ifdef _EV_TRACK_
	e->post_func = func;
	e->post_line = line;
#endif /* _EV_TRACK_ */

	/* If multiple threads attempt to post the same event all but
	 * one will receive EBUSY. If the event is already posted all
	 * will receive EBUSY */
	if (__sync_val_compare_and_swap(&e->e_posted, 0, 1))
		return EBUSY;


	e->e_status = EV_OK;

	if (to) {
		e->e_to = *to;
	} else {
		e->e_to.tv_sec = 0;
		e->e_to.tv_nsec = 0;
	}

	e->e_src = src;
	e->e_dst = dst;
	rbn_init(&e->e_to_rbn, &e->e_to);

	pthread_mutex_lock(&dst->w_lock);
	if (dst->w_state == EV_WORKER_FLUSHING)
		goto err;
	ev_get(&e->e_ev);
	if (to)
		rbt_ins(&dst->w_event_tree, &e->e_to_rbn);
	else
		TAILQ_INSERT_TAIL(&dst->w_event_list, e, e_entry);
	rc = (ev_time_cmp(&e->e_to, &dst->w_sem_wait) <= 0);
	pthread_mutex_unlock(&dst->w_lock);
	if (rc)
		sem_post(&dst->w_sem);

	return 0;
 err:
	e->e_posted = 0;
	pthread_mutex_unlock(&dst->w_lock);
	return EBUSY;
}

int ev_cancel(ev_t ev)
{
	ev__t e = EV(ev);
	int rc = EINVAL;
	struct timespec now;

	pthread_mutex_lock(&e->e_dst->w_lock);
	if (!e->e_posted)
		goto out;

	/*
	 * Set the status to EV_CANCEL so the actor will see that
	 * status
	 */
	e->e_status = EV_FLUSH;

	/*
	 * If the event is on the list or is soon to expire, do
	 * nothing. This avoids racing with the worker.
	 */
	rc = 0;
	(void)clock_gettime(CLOCK_REALTIME, &now);
	if (ev_time_diff(&e->e_to, &now) < 1.0) {
		rc = EBUSY;
		goto out;
	}

	rbt_del(&e->e_dst->w_event_tree, &e->e_to_rbn);
	TAILQ_INSERT_TAIL(&e->e_dst->w_event_list, e, e_entry);
 out:
	pthread_mutex_unlock(&e->e_dst->w_lock);
	if (!rc)
		sem_post(&e->e_dst->w_sem);
	return rc;
}

#ifdef _EV_TRACK_
inline struct ev__s *ev__s_get(ev_t e) {
	return EV(e);
}
#endif /* _EV_TRACK_ */

static void __attribute__ ((constructor)) ev_init(void)
{
}

static void __attribute__ ((destructor)) ev_term(void)
{
}
