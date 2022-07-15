/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else 
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

#ifdef HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef WIN32
extern const struct eventop win32ops;
#endif

/* In order of preference */
static const struct eventop *eventops[] = {
#ifdef HAVE_EVENT_PORTS
	&evportops,
#endif
#ifdef HAVE_WORKING_KQUEUE
	&kqops,
#endif
#ifdef HAVE_EPOLL
	&epollops,
#endif
#ifdef HAVE_DEVPOLL
	&devpollops,
#endif
#ifdef HAVE_POLL
	&pollops,
#endif
#ifdef HAVE_SELECT
	&selectops,
#endif
#ifdef WIN32
	&win32ops,
#endif
	NULL
};

/* Global state */
struct event_base *current_base = NULL;
extern struct event_base *evsignal_base;
static int use_monotonic;

/* Handle signals - This is a deprecated interface */
int (*event_sigcb)(void);		/* Signal callback when gotsig is set */
volatile sig_atomic_t event_gotsig;	/* Set in signal handler */

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

static void
detect_monotonic(void)	// 检测系统是否支持monotonic时间
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	struct timespec	ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		use_monotonic = 1;  // 系统支持monotonic时间
#endif
}

static int
gettime(struct event_base *base, struct timeval *tp)  // 获取当前时间
{	// 如果时间缓存已设置，就直接使用
	if (base->tv_cache.tv_sec) {
		*tp = base->tv_cache;
		return (0);
	}
	// 如果支持monotonic，就用clock_gettime获取monotonic时间
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	if (use_monotonic) {
		struct timespec	ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
			return (-1);

		tp->tv_sec = ts.tv_sec;
		tp->tv_usec = ts.tv_nsec / 1000;
		return (0);
	}
#endif
	// 否则获取系统当前时间
	return (evutil_gettimeofday(tp, NULL));
}

struct event_base *
event_init(void)  // 创建一个event_base对象
{ 
	struct event_base *base = event_base_new();

	if (base != NULL)
		current_base = base;

	return (base);
}

struct event_base *
event_base_new(void)  // 初始化event_base
{ 
	int i;
	struct event_base *base;

	if ((base = calloc(1, sizeof(struct event_base))) == NULL)
		event_err(1, "%s: calloc", __func__);

	event_sigcb = NULL;
	event_gotsig = 0;

	detect_monotonic();  // 检测是否使用monotonic时间
	gettime(base, &base->event_tv);  // base->event_tv设置为当前时间
	
	min_heap_ctor(&base->timeheap);  // 构造timeheap
	TAILQ_INIT(&base->eventqueue);  // 构造链表，用于保存所有注册事件的指针
	base->sig.ev_signal_pair[0] = -1;  // socket pair
	base->sig.ev_signal_pair[1] = -1;  // socket pair
	
	base->evbase = NULL;  // I/O 复用相关
	for (i = 0; eventops[i] && !base->evbase; i++) {  // 选择系统I/O多路复用机制
		base->evsel = eventops[i];

		base->evbase = base->evsel->init(base);
	}

	if (base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);

	if (evutil_getenv("EVENT_SHOW_METHOD")) 
		event_msgx("libevent using: %s\n",
			   base->evsel->name);

	/* allocate a single active event queue */
	event_base_priority_init(base, 1);  // 创建一条激活事件链表

	return (base);
}

void
event_base_free(struct event_base *base)  // 释放event_base资源
{
	int i, n_deleted=0;
	struct event *ev;

	if (base == NULL && current_base)
		base = current_base;
	if (base == current_base)
		current_base = NULL;

	/* XXX(niels) - check for internal events first */
	assert(base);
	/* Delete all non-internal events. */
	for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) {  // 先删除eventqueue里的
		struct event *next = TAILQ_NEXT(ev, ev_next);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {  // ev_flags!=EVLIST_INTERNAL
			event_del(ev);  // 删除事件
			++n_deleted;
		}
		ev = next;
	}
	while ((ev = min_heap_top(&base->timeheap)) != NULL) {  // 再删除timer heap里的
		event_del(ev);  // 删除事件
		++n_deleted;
	}

	for (i = 0; i < base->nactivequeues; ++i) {  // 再删除activequeues里的
		for (ev = TAILQ_FIRST(base->activequeues[i]); ev; ) {
			struct event *next = TAILQ_NEXT(ev, ev_active_next);
			if (!(ev->ev_flags & EVLIST_INTERNAL)) {  // ev_flags!=EVLIST_INTERNA
				event_del(ev);  // 删除事件
				++n_deleted;
			}
			ev = next;
		}
	}

	if (n_deleted)
		event_debug(("%s: %d events were still set in base",
			__func__, n_deleted));

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);  // I/O复用注销，释放资源

	for (i = 0; i < base->nactivequeues; ++i)
		assert(TAILQ_EMPTY(base->activequeues[i]));  // activequeues都为空

	assert(min_heap_empty(&base->timeheap));  // timer heap为空
	min_heap_dtor(&base->timeheap);  // 析构timer heap

	for (i = 0; i < base->nactivequeues; ++i)  // 释放activequeues资源
		free(base->activequeues[i]);
	free(base->activequeues);

	assert(TAILQ_EMPTY(&base->eventqueue));  // eventqueue都为空

	free(base);  // 释放base资源
}

/* reinitialized the event base after a fork */
int
event_reinit(struct event_base *base)  // fork后重新设置I/O复用机制
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;
	struct event *ev;

#if 0
	/* Right now, reinit always takes effect, since even if the
	   backend doesn't require it, the signal socketpair code does.
	 */
	/* check if this event mechanism requires reinit */
	if (!evsel->need_reinit)
		return (0);
#endif

	/* prevent internal delete */
	if (base->sig.ev_signal_added) { // 如果ev_signal事件已注册
		/* we cannot call event_del here because the base has
		 * not been reinitialized yet. */
		event_queue_remove(base, &base->sig.ev_signal,
		    EVLIST_INSERTED);
		if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)
			event_queue_remove(base, &base->sig.ev_signal,
			    EVLIST_ACTIVE);
		base->sig.ev_signal_added = 0;
	}

	if (base->evsel->dealloc != NULL)  // I/O复用释放资源
		base->evsel->dealloc(base, base->evbase);
	evbase = base->evbase = evsel->init(base);  // 初始化I/O复用策略
	if (base->evbase == NULL)
		event_errx(1, "%s: could not reinitialize event mechanism",
		    __func__);

	TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {
		if (evsel->add(evbase, ev) == -1)
			res = -1;
	}

	return (res);
}

int
event_priority_init(int npriorities)  // 设置npriorities个不同的event优先级
{
  return event_base_priority_init(current_base, npriorities);
}

int
event_base_priority_init(struct event_base *base, int npriorities)  // 生成npriorities个activequeues链表
{
	int i;

	if (base->event_count_active)
		return (-1);

	if (npriorities == base->nactivequeues)
		return (0);

	if (base->nactivequeues) {
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]);
		}
		free(base->activequeues);
	}

	/* Allocate our priority queues */
	base->nactivequeues = npriorities;
	base->activequeues = (struct event_list **)
	    calloc(base->nactivequeues, sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);

	for (i = 0; i < base->nactivequeues; ++i) {
		base->activequeues[i] = malloc(sizeof(struct event_list));
		if (base->activequeues[i] == NULL)
			event_err(1, "%s: malloc", __func__);
		TAILQ_INIT(base->activequeues[i]);
	}

	return (0);
}

int
event_haveevents(struct event_base *base)  // 返回events数量
{
	return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */

static void
event_process_active(struct event_base *base)  // 调用cb执行一个就绪（active)事件
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;

	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	assert(activeq != NULL);

	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {
		if (ev->ev_events & EV_PERSIST)
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		else
			event_del(ev);
		
		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;
		ev->ev_pncalls = &ncalls;
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);  // cb执行event
			if (event_gotsig || base->event_break) {
			  	ev->ev_pncalls = NULL;
				return;
			}
		}
		ev->ev_pncalls = NULL;
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

int
event_dispatch(void)  // 执行event_loop
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)  // 执行event_base_loop
{
  return (event_base_loop(event_base, 0));
}

const char *
event_base_get_method(struct event_base *base)  // 获取base采用的I/O复用机制
{
	assert(base);
	return (base->evsel->name);
}

static void
event_loopexit_cb(int fd, short what, void *arg)  // event_loopexit使用的cb
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

/* not thread safe */
int
event_loopexit(const struct timeval *tv)  // 使用event_once设置退出event_loop的定时事件
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    current_base, tv));
}

int
event_base_loopexit(struct event_base *event_base, const struct timeval *tv)  // event_loopexit的线程安全版
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,
		    event_base, tv));
}

/* not thread safe */
int
event_loopbreak(void)  // 立刻退出event_loop
{
	return (event_base_loopbreak(current_base));
}

int
event_base_loopbreak(struct event_base *event_base)  // event_loopbreak的线程安全版
{
	if (event_base == NULL)
		return (-1);

	event_base->event_break = 1;
	return (0);
}



/* not thread safe */

int
event_loop(int flags)  // 执行事件主循环
{
	return event_base_loop(current_base, flags);
}

int
event_base_loop(struct event_base *base, int flags)  // event_loop的线程安全版
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	struct timeval tv;
	struct timeval *tv_p;  // 指向tv的指针
	int res, done;

	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	if (base->sig.ev_signal_added)
		evsignal_base = base;  // evsignal_base是一个全局变量，在处理signal时，
													 // 用于指名signal所属的event_base实例
	done = 0;
	while (!done) {  // 事件主循环
		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) { // 可以调用event_loopexit_cb()设置event_gotterm
			base->event_gotterm = 0;
			break;
		}

		if (base->event_break) {  // 可以调用event_base_loopbreak()设置event_break标记 
			base->event_break = 0;
			break;
		}

		/* You cannot use this interface for multi-threaded apps */
		while (event_gotsig) {  // 处理signal事件
			event_gotsig = 0;
			if (event_sigcb) {
				res = (*event_sigcb)();
				if (res == -1) {
					errno = EINTR;
					return (-1);
				}
			}
		}

		timeout_correct(base, &tv);  // 校正timer heap时间

		tv_p = &tv;
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) {
			timeout_next(base, &tv_p);  // tv_p指向最大等待时间
		} else {  // 如果有未处理的就绪事件···
			/* 
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			evutil_timerclear(&tv);  // tv置0，也即*tv_p置0
		}
		
		/* If we have no events, we just exit */
		if (!event_haveevents(base)) {
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		/* update last old time */
		gettime(base, &base->event_tv);  // 更新base->event_tv为当前时间

		/* clear time cache */
		base->tv_cache.tv_sec = 0;  // time cache清零

		// 调用系统I/O demultiplexer等待就绪的I/O events，可能是epoll_wait或者select
		// 在evsel->dispatch中，会把就绪signal event、I/O event插入到激活链表中
		res = evsel->dispatch(base, evbase, tv_p);

		if (res == -1)
			return (-1);
		gettime(base, &base->tv_cache);  // 将time cache赋值为当前系统时间
		
		// 检查heap中的timer events，将就绪的timer events从heap中删除，并插入到激活链表中
		timeout_process(base);

		if (base->event_count_active) {  // 调用event_process_active处理激活链表中的就绪event，
																		 // 调用其回调函数执行事件处理
			// 该函数会寻找最高优先级（priority越小优先级越高）的激活事件链表，
			// 然后处理链表中的所有就绪事件，所以低优先级的就绪事件可能得不到及时处理
			event_process_active(base);  // 调用cb执行一个就绪（active)事件
			if (!base->event_count_active && (flags & EVLOOP_ONCE))
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)
			done = 1;
	}

	/* clear time cache */
	base->tv_cache.tv_sec = 0;  // 循环结束，清空时间缓存

	event_debug(("%s: asked to terminate loop.", __func__));
	return (0);
}

/* Sets up an event for processing once */

struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;  // cb中的第三个参数
};

/* One-time callback, it deletes itself */

static void
event_once_cb(int fd, short events, void *arg)  // 调用一次性cb
{ // arg是event_once结构类型指针
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	free(eonce);
}

/* not threadsafe, event scheduled once. */
int
event_once(int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	return event_base_once(current_base, fd, events, callback, arg, tv);
}

/* Schedules an event once */
int
event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;
	int res;

	/* We cannot support signals that just fire once */
	if (events & EV_SIGNAL)  // signal事件不能被设置为一次性的
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;

	if (events == EV_TIMEOUT) {  // 如果是定时事件到current base
		if (tv == NULL) {
			evutil_timerclear(&etv);  // (&etv)->tv_sec = (&etv)->tv_usec = 0
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce);  // 调用event_set，设置定时事件
	} else if (events & (EV_READ|EV_WRITE)) {  // 如果是I/O事件
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);  // 设置I/O事件到current base
	} else {
		/* Bad event combination */
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev);  // 关联event到指定base
	if (res == 0)
		res = event_add(&eonce->ev, tv);
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}

void
event_set(struct event *ev, int fd, short events,
	  void (*callback)(int, short, void *), void *arg)  // 设置event：ev->ev_base = current_base
{ // fd：对于I/O事件，为文件描述符；对于signal事件，为绑定的信号；对于定时事件，设为-1即可
	// events：设置事件类型，EV_TIMEOUT、EV_READ、EV_WRITE、EV_SIGNAL、EV_PERSIST
	/* Take the current base - caller needs to set the real base later */
	ev->ev_base = current_base;

	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;
	ev->ev_res = 0;
	ev->ev_flags = EVLIST_INIT;
	ev->ev_ncalls = 0;
	ev->ev_pncalls = NULL;

	min_heap_elem_init(ev);

	/* by default, we put new events into the middle priority */
	if(current_base)
		ev->ev_pri = current_base->nactivequeues/2;
}

int
event_base_set(struct event_base *base, struct event *ev)  // 设置event：ev->ev_base = base
{
	/* Only innocent events may be assigned to a different base */
	if (ev->ev_flags != EVLIST_INIT)
		return (-1);

	ev->ev_base = base;
	ev->ev_pri = base->nactivequeues/2;

	return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */

int
event_priority_set(struct event *ev, int pri)  // 设置event的优先级
{
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);

	ev->ev_pri = pri;

	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */

int
event_pending(struct event *ev, short event, struct timeval *tv)
{
	struct timeval	now, res;
	int flags = 0;

	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL));
	if (ev->ev_flags & EVLIST_ACTIVE)
		flags |= ev->ev_res;
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;

	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) {
		gettime(ev->ev_base, &now);
		evutil_timersub(&ev->ev_timeout, &now, &res);
		/* correctly remap to real time */
		evutil_gettimeofday(&now, NULL);
		evutil_timeradd(&now, &res, tv);
	}

	return (flags & event);
}

int
event_add(struct event *ev, const struct timeval *tv)  // 注册事件到其对应的base上
{ // ev事件，tv超时时间
	// 函数将ev注册到ev_base上，事件由ev->ev_events指明，如果注册成功，ev将被插入到已注册链表中；
	// 如果tv不是NULL，则同时会注册定时事件，将ev添加到timer堆上
	struct event_base *base = ev->ev_base;  // 要注册到的event_base
	const struct eventop *evsel = base->evsel;  // event_base的I/O复用机制
	void *evbase = base->evbase;  // event_base的I/O复用机制，通过evsel成员的init函数来初始化
	int res = 0;

	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/*
	 * prepare for timeout insertion further below, if we get a
	 * failure on any step, we should not change any state.
	 */
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {  // 如果是定时事件，且不在定时器堆中
		if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1) // 先在timeheap上预留一个位置
			return (-1);  /* ENOMEM == errno */
	}

	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {  // 如果是I/O或signal事件，且不在对应链表中
		res = evsel->add(evbase, ev);  // I/O demultiplexer注册事件
		if (res != -1)  // 注册成功，插入event到已注册链表中
			event_queue_insert(base, ev, EVLIST_INSERTED);
	}

	/* 
	 * we should change the timout state only if the previous event
	 * addition succeeded.
	 */
	if (res != -1 && tv != NULL) {  // 只有在插入event到已注册链表中成功后，才添加定时事件
		struct timeval now;

		/* 
		 * we already reserved memory above for the case where we
		 * are not replacing an exisiting timeout.
		 */
		if (ev->ev_flags & EVLIST_TIMEOUT)  // if true，表明已经在定时器堆中，从堆中删除
			event_queue_remove(base, ev, EVLIST_TIMEOUT);

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {  // 如果事件已激活且当前事件为TIMEOUT事件，则从激活链表中删除
			/* See if we are just active executing this
			 * event in a loop
			 */
			if (ev->ev_ncalls && ev->ev_pncalls) {  // 如果正在执行此事件
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}
			
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		}

		gettime(base, &now);  // get当前事件并存放到now
		evutil_timeradd(&now, tv, &ev->ev_timeout);  // ev_timeout保存计算得到的过期时间点

		event_debug((
			 "event_add: timeout in %ld seconds, call %p",
			 tv->tv_sec, ev->ev_callback));

		event_queue_insert(base, ev, EVLIST_TIMEOUT);  // 插入到timer小根堆中
	}

	return (res);
}

int
event_del(struct event *ev)  // 从base中删除事件ev
{ 
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
		 ev, ev->ev_callback));

	/* An event without a base has not been added */
	if (ev->ev_base == NULL)
		return (-1);

	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* See if we are just active executing this event in a loop */
	if (ev->ev_ncalls && ev->ev_pncalls) {  // 如果正在执行此事件
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	if (ev->ev_flags & EVLIST_TIMEOUT)  // 如果事件再定时器堆中
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	if (ev->ev_flags & EVLIST_ACTIVE)  // 如果事件已激活
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	if (ev->ev_flags & EVLIST_INSERTED) {  // 如果事件已注册
		event_queue_remove(base, ev, EVLIST_INSERTED);
		return (evsel->del(evbase, ev));  // I/O demultiplexer注销事件
	}

	return (0);
}

void
event_active(struct event *ev, int res, short ncalls)  // 添加事件ev到timer heap中
{ // res一般为EV_READ、EV_WRITE、EV_ACTIVE等
	/* We get different kinds of events, add them together */
	if (ev->ev_flags & EVLIST_ACTIVE) {
		ev->ev_res |= res;
		return;
	}

	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

static int
timeout_next(struct event_base *base, struct timeval **tv_p)  // 更新tv_p
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;
	// 堆的首元素具有最小的超时值
	// 如果没有定时事件，将等待时间设置为NULL，表示一直阻塞直到有I/O事件发生 
	if ((ev = min_heap_top(&base->timeheap)) == NULL) {
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		return (0);
	}
	// 取得当前时间
	if (gettime(base, &now) == -1)
		return (-1);
	// 如果超时时间<=当前时间，不能等待，直接返回
	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {
		evutil_timerclear(tv);  // 参数tv_p置0
		return (0);
	}
	// 计算等待的时间=当前时间-最小的超时时间
	evutil_timersub(&ev->ev_timeout, &now, tv);  // 更新tv_p

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %ld seconds", tv->tv_sec));
	return (0);
}

/*
 * Determines if the time is running backwards by comparing the current
 * time against the last time we checked.  Not needed when using clock
 * monotonic.
 */

static void
timeout_correct(struct event_base *base, struct timeval *tv)  // 校正定时器堆的过期时间，并更新base->event_tv为当前时间
{
	struct event **pev;
	unsigned int size;
	struct timeval off;

	if (use_monotonic)  // 如果使用的是monotonic时间，不要校正
		return;

	/* Check if time is running backwards */
	gettime(base, tv);  // tv <--- tv_cache
	if (evutil_timercmp(tv, &base->event_tv, >=)) {  // 如果目前时间>=base记录时间，则更新base后返回
		base->event_tv = *tv;
		return;
	}  // 否则说明用户把系统时间向前调整了，例如15:00调整到12:00，需要矫正

	event_debug(("%s: time is running backwards, corrected",
		    __func__));
	evutil_timersub(&base->event_tv, tv, &off);  // 计算目前时间和base记录时间的差值

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	pev = base->timeheap.p;
	size = base->timeheap.n;
	for (; size-- > 0; ++pev) {  // 依次调整定时事件的小根堆
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
	/* Now remember what the new time turned out to be. */
	base->event_tv = *tv;  // 更新base->event_tv为当前时间
}

void
timeout_process(struct event_base *base)  // 将所有已超时的事件从链表中删除，然后添加到时间堆中
{
	struct timeval now;
	struct event *ev;

	if (min_heap_empty(&base->timeheap))  // 如果timer heap为空
		return;

	gettime(base, &now);

	while ((ev = min_heap_top(&base->timeheap))) {
		if (evutil_timercmp(&ev->ev_timeout, &now, >))  // 如果所有事件ev都未超时
			break;

		/* delete this event from the I/O queues */
		event_del(ev);  // 删除事件

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));
		event_active(ev, EV_TIMEOUT, 1);  // 添加事件ev到timer heap中
	}
}

void
event_queue_remove(struct event_base *base, struct event *ev, int queue)  // 从链表或堆中移除事件
{
	if (!(ev->ev_flags & queue))
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue);

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count--;

	ev->ev_flags &= ~queue;
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		base->event_count_active--;
		TAILQ_REMOVE(base->activequeues[ev->ev_pri],
		    ev, ev_active_next);
		break;
	case EVLIST_TIMEOUT:
		min_heap_erase(&base->timeheap, ev);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

void
event_queue_insert(struct event_base *base, struct event *ev, int queue)  // 向链表或堆中添加事件
{
	if (ev->ev_flags & queue) {  // 避免重复插入
		/* Double insertion is possible for active events */
		if (queue & EVLIST_ACTIVE)
			return;

		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++;

	ev->ev_flags |= queue;  // 记录queue标记
	switch (queue) {
	case EVLIST_INSERTED:  // I/O或Signal事件，加入已注册事件链表
		TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:  // 就绪事件，加入激活链表
		base->event_count_active++;
		TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
		    ev,ev_active_next);
		break;
	case EVLIST_TIMEOUT: {  // 定时事件，加入堆
		min_heap_push(&base->timeheap, ev);
		break;
	}
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/* Functions for debugging */

const char *
event_get_version(void)  // return (VERSION);
{
	return (VERSION);
}

/* 
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */

const char *
event_get_method(void)  // 获取current_base采用的I/O复用机制
{
	return (current_base->evsel->name);
}
