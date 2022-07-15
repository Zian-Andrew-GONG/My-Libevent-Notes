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
#ifndef _EVENT_INTERNAL_H_
#define _EVENT_INTERNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "min_heap.h"
#include "evsignal.h"

struct eventop {  // 在libevent中，每种I/O demultiplex机制的实现都必须提供这五个函数接口，
									// 来完成自身的初始化、销毁释放；对事件的注册、注销和分发
	const char *name;
	void *(*init)(struct event_base *);  // 初始化
	int (*add)(void *, struct event *);  // 注册事件
	int (*del)(void *, struct event *);  // 删除事件
	int (*dispatch)(struct event_base *, void *, struct timeval *);  // 事件分发
	void (*dealloc)(struct event_base *, void *);  // 注销，释放资源
	/* set if we need to reinitialize the event base */
	int need_reinit;
};

struct event_base {
	const struct eventop *evsel;  // 初始化Reactor的时候选择一种I/O复用机制，并记录
	void *evbase;  // 指向I/O复用机制真正存储的数据，它通过evsel成员的init函数来初始化
	int event_count;		/* counts number of total events */
	int event_count_active;	/* counts number of active events */

	int event_gotterm;		/* Set to terminate loop */
	int event_break;		/* Set to terminate loop immediately */

	/* active event management */
	struct event_list **activequeues;  // activequeues[priority]是一个链表，
																		// 链表的每个节点指向一个优先级为priority对就绪事件event
	int nactivequeues;  // nactivequeues是activequeues的数量

	/* signal handling info */
	struct evsignal_info sig;  // 用来管理信号的结构体

	struct event_list eventqueue;  // 链表，保存了所有注册事件的指针
	struct timeval event_tv;  // 用于管理时间，为last old time

	struct min_heap timeheap;  // 管理定时事件的小根堆

	struct timeval tv_cache;  // 用于管理时间
};

/* Internal use only: Functions that might be missing from <sys/queue.h> */
#ifndef HAVE_TAILQFOREACH
#define	TAILQ_FIRST(head)		((head)->tqh_first)
#define	TAILQ_END(head)			NULL
#define	TAILQ_NEXT(elm, field)		((elm)->field.tqe_next)
#define TAILQ_FOREACH(var, head, field)					\
	for((var) = TAILQ_FIRST(head);					\
	    (var) != TAILQ_END(head);					\
	    (var) = TAILQ_NEXT(var, field))
#define	TAILQ_INSERT_BEFORE(listelm, elm, field) do {			\
	(elm)->field.tqe_prev = (listelm)->field.tqe_prev;		\
	(elm)->field.tqe_next = (listelm);				\
	*(listelm)->field.tqe_prev = (elm);				\
	(listelm)->field.tqe_prev = &(elm)->field.tqe_next;		\
} while (0)
#endif /* TAILQ_FOREACH */

int _evsignal_set_handler(struct event_base *base, int evsignal,
			  void (*fn)(int));
int _evsignal_restore_handler(struct event_base *base, int evsignal);

/* defined in evutil.c */
const char *evutil_getenv(const char *varname);

#ifdef __cplusplus
}
#endif

#endif /* _EVENT_INTERNAL_H_ */
