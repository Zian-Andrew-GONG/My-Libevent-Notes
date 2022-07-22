/*
 * Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>
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

#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "log.h"

/* due to limitations in the epoll interface, we need to keep track of
 * all file descriptors outself.
 */
struct evepoll {
	struct event *evread;
	struct event *evwrite;
};

struct epollop {
	struct evepoll *fds;
	int nfds;
	struct epoll_event *events;  // epoll_event结构体数组指针，.events用于指定事件类型，.data存放“用户数据”
	int nevents;
	int epfd;  // 内核数据表的文件描述符
};

static void *epoll_init	(struct event_base *);
static int epoll_add	(void *, struct event *);
static int epoll_del	(void *, struct event *);
static int epoll_dispatch	(struct event_base *, void *, struct timeval *);
static void epoll_dealloc	(struct event_base *, void *);

const struct eventop epollops = {  // epoll的struct eventop
	"epoll",
	epoll_init,
	epoll_add,
	epoll_del,
	epoll_dispatch,
	epoll_dealloc,
	1 /* need reinit */
};

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

/* On Linux kernels at least up to 2.6.24.4, epoll can't handle timeout
 * values bigger than (LONG_MAX - 999ULL)/HZ.  HZ in the wild can be
 * as big as 1000, and LONG_MAX can be as small as (1<<31)-1, so the
 * largest number of msec we can support here is 2147482.  Let's
 * round that down by 47 seconds.
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)

#define INITIAL_NFILES 32  // epollop.nfds
#define INITIAL_NEVENTS 32  // epollop.nevents
#define MAX_NEVENTS 4096

static void *
epoll_init(struct event_base *base)  // 初始化，分配epollop空间
{
	int epfd;
	struct epollop *epollop;

	/* Disable epollueue when this environment variable is set */
	if (evutil_getenv("EVENT_NOEPOLL"))
		return (NULL);

	/* Initalize the kernel queue */
	if ((epfd = epoll_create(32000)) == -1) {  // epoll_create返回一个文件描述符，用来识别内核事件表
		if (errno != ENOSYS)
			event_warn("epoll_create");
		return (NULL);
	}

	FD_CLOSEONEXEC(epfd);

	if (!(epollop = calloc(1, sizeof(struct epollop))))
		return (NULL);

	epollop->epfd = epfd;  // 把epoll_create创建的内核事件表的文件描述符存放在epollop->epfd中

	/* Initalize fields */
	epollop->events = malloc(INITIAL_NEVENTS * sizeof(struct epoll_event));
	if (epollop->events == NULL) {
		free(epollop);
		return (NULL);
	}
	epollop->nevents = INITIAL_NEVENTS;

	epollop->fds = calloc(INITIAL_NFILES, sizeof(struct evepoll));
	if (epollop->fds == NULL) {
		free(epollop->events);
		free(epollop);
		return (NULL);
	}
	epollop->nfds = INITIAL_NFILES;

	evsignal_init(base);

	return (epollop);
}

static int
epoll_recalc(struct event_base *base, void *arg, int max)  // 重新分配arg(epollop)空间，nfds数值倍增
{
	struct epollop *epollop = arg;

	if (max >= epollop->nfds) {
		struct evepoll *fds;
		int nfds;

		nfds = epollop->nfds;
		while (nfds <= max)
			nfds <<= 1;

		fds = realloc(epollop->fds, nfds * sizeof(struct evepoll));
		if (fds == NULL) {
			event_warn("realloc");
			return (-1);
		}
		epollop->fds = fds;
		memset(fds + epollop->nfds, 0,
		    (nfds - epollop->nfds) * sizeof(struct evepoll));
		epollop->nfds = nfds;
	}

	return (0);
}

static int
epoll_dispatch(struct event_base *base, void *arg, struct timeval *tv)  // 在tv时间内，把所有就绪的I/O事件加入active链表
{
	struct epollop *epollop = arg;
	struct epoll_event *events = epollop->events;  // epoll_event结构体数组指针，.events用于指定事件类型，.data存放“用户数据”
	struct evepoll *evep;
	int i, res, timeout = -1;

	if (tv != NULL)
		timeout = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;

	if (timeout > MAX_EPOLL_TIMEOUT_MSEC) {
		/* Linux kernels can wait forever if the timeout is too big;
		 * see comment on MAX_EPOLL_TIMEOUT_MSEC. */
		timeout = MAX_EPOLL_TIMEOUT_MSEC;
	}

	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);
	// epollop->epfd参数指定要操作的内核事件表的文件描述符。
	// events参数是一个用户数组，这个数组仅仅在epoll_wait返回时保存内核检测到的所有就绪事件，
		// 而不像select和poll的数组参数那样既用于传入用户注册的事件，又用于输出内核检测到的就绪事件。
		// 这就极大地提高了应用程序索引就绪文件描述符的效率。
	// nevents参数指定用户数组的大小，即指定最多监听多少个事件，它必须大于0。
	// epollop->nevents参数指定超时时间，单位为毫秒，如果timeout为0，则epoll_wait会立即返回，
		// 如果timeout为-1，则epoll_wait会一直阻塞，直到有事件就绪。
	// 成功时返回就绪的文件描述符的个数，失败时返回-1，超时返回 0
	if (res == -1) {
		if (errno != EINTR) {
			event_warn("epoll_wait");
			return (-1);
		}

		evsignal_process(base);  // 把signal事件链表里的ev移动到active链表中
		return (0);
	} else if (base->sig.evsignal_caught) {
		evsignal_process(base);  // 把signal事件链表里的ev移动到active链表中
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));

	for (i = 0; i < res; i++) {  // 添加事件到激活链表
		int what = events[i].events;
		struct event *evread = NULL, *evwrite = NULL;
		int fd = events[i].data.fd;

		if (fd < 0 || fd >= epollop->nfds)
			continue;
		evep = &epollop->fds[fd];

		if (what & (EPOLLHUP|EPOLLERR)) {  // 挂起或错误
			evread = evep->evread;
			evwrite = evep->evwrite;
		} else {
			if (what & EPOLLIN) {  // 数据可读
				evread = evep->evread;
			}

			if (what & EPOLLOUT) {  // 数据可写
				evwrite = evep->evwrite;
			}
		}

		if (!(evread||evwrite))
			continue;

		if (evread != NULL)
			event_active(evread, EV_READ, 1);  // 添加事件evread到激活链表
		if (evwrite != NULL)
			event_active(evwrite, EV_WRITE, 1);  // 添加事件evwrite到激活链表
	}

	if (res == epollop->nevents && epollop->nevents < MAX_NEVENTS) {  // 为struct epollop准备更大的空间
		/* We used all of the event space this time.  We should
		   be ready for more events next time. */
		int new_nevents = epollop->nevents * 2;
		struct epoll_event *new_events;

		new_events = realloc(epollop->events,
		    new_nevents * sizeof(struct epoll_event));
		if (new_events) {
			epollop->events = new_events;
			epollop->nevents = new_nevents;
		}
	}

	return (0);
}


static int
epoll_add(void *arg, struct event *ev)  // 使用epoll_ctl添加epoll事件
{
	struct epollop *epollop = arg;
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, op, events;

	if (ev->ev_events & EV_SIGNAL)  // signal事件
		return (evsignal_add(ev));

	fd = ev->ev_fd;
	if (fd >= epollop->nfds) {
		/* Extent the file descriptor array as necessary */
		if (epoll_recalc(ev->ev_base, epollop, fd) == -1)  // 为新的epoll事件增加空间
			return (-1);
	}
	evep = &epollop->fds[fd];
	op = EPOLL_CTL_ADD;
	events = 0;
	if (evep->evread != NULL) {
		events |= EPOLLIN;
		op = EPOLL_CTL_MOD;
	}
	if (evep->evwrite != NULL) {
		events |= EPOLLOUT;
		op = EPOLL_CTL_MOD;
	}

	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	epev.data.fd = fd;  // 指定事件所从属的目标文件描述符
	epev.events = events;  // 表示事件类型
	if (epoll_ctl(epollop->epfd, op, ev->ev_fd, &epev) == -1)  
	// epoll_ctl用来操作内核事件表，epfd为内核事件表的文件描述符；
	// op参数指定操作类型
		// - EPOLL_CTL_ADD 往内核事件表中注册 fd 上的事件。
		// - EPOLL_CTL_MOD 修改 fd 上的注册事件。
		// - EPOLL_CTL_DEL 删除 fd 上的注册事件。
	// ev->ev_fd指定要操作的文件描述符
	// &epev指定事件，为epoll_event结构指针类型
			return (-1);

	/* Update events responsible */
	if (ev->ev_events & EV_READ)
		evep->evread = ev;
	if (ev->ev_events & EV_WRITE)
		evep->evwrite = ev;

	return (0);
}

static int
epoll_del(void *arg, struct event *ev)  // 使用epoll_ctl删除epoll事件
{
	struct epollop *epollop = arg;
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, events, op;
	int needwritedelete = 1, needreaddelete = 1;

	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_del(ev));

	fd = ev->ev_fd;
	if (fd >= epollop->nfds)
		return (0);
	evep = &epollop->fds[fd];

	op = EPOLL_CTL_DEL;
	events = 0;

	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {
		if ((events & EPOLLIN) && evep->evwrite != NULL) {
			needwritedelete = 0;
			events = EPOLLOUT;
			op = EPOLL_CTL_MOD;
		} else if ((events & EPOLLOUT) && evep->evread != NULL) {
			needreaddelete = 0;
			events = EPOLLIN;
			op = EPOLL_CTL_MOD;
		}
	}

	epev.events = events;  // 指定事件类型
	epev.data.fd = fd;  // 指定事件从属的目标文件描述符

	if (needreaddelete)
		evep->evread = NULL;
	if (needwritedelete)
		evep->evwrite = NULL;

	if (epoll_ctl(epollop->epfd, op, fd, &epev) == -1)
	// epoll_ctl用来操作内核事件表，epfd为内核事件表的文件描述符；
	// op参数指定操作类型
		// - EPOLL_CTL_ADD 往内核事件表中注册 fd 上的事件。
		// - EPOLL_CTL_MOD 修改 fd 上的注册事件。
		// - EPOLL_CTL_DEL 删除 fd 上的注册事件。
	// fd指定要操作的文件描述符
	// &epev指定事件，为epoll_event结构指针类型
		return (-1);

	return (0);
}

static void
epoll_dealloc(struct event_base *base, void *arg)  // 释放资源
{
	struct epollop *epollop = arg;

	evsignal_dealloc(base);
	if (epollop->fds)
		free(epollop->fds);
	if (epollop->events)
		free(epollop->events);
	if (epollop->epfd >= 0)
		close(epollop->epfd);

	memset(epollop, 0, sizeof(struct epollop));
	free(epollop);
}
