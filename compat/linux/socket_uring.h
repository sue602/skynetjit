/*
 * Optional Linux io_uring poll backend for Skynet's readiness-based socket
 * server. It preserves socket_server.c's nonblocking I/O state machine:
 * io_uring provides readiness notifications while the existing code performs
 * accept, recv, and send. If io_uring setup fails, native epoll is used.
 */
#ifndef SKYNETJIT_SOCKET_URING_H
#define SKYNETJIT_SOCKET_URING_H

#include <errno.h>
#include <liburing.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* Keep epoll as a runtime fallback without changing the upstream header. */
#define sp_invalid skynetjit_epoll_invalid
#define sp_create skynetjit_epoll_create
#define sp_release skynetjit_epoll_release
#define sp_add skynetjit_epoll_add
#define sp_del skynetjit_epoll_del
#define sp_enable skynetjit_epoll_enable
#define sp_wait skynetjit_epoll_wait
#define sp_nonblocking skynetjit_epoll_nonblocking
#include "socket_epoll.h"
#undef sp_invalid
#undef sp_create
#undef sp_release
#undef sp_add
#undef sp_del
#undef sp_enable
#undef sp_wait
#undef sp_nonblocking

#define SKYNETJIT_URING_FD 0x7ffffffe
#define SKYNETJIT_URING_QUEUE_DEPTH 4096

struct skynetjit_uring_watch {
	struct skynetjit_uring_watch *next;
	int fd;
	void *ud;
	unsigned mask;
	bool active;
	bool armed;
	bool cancel_pending;
};

struct skynetjit_uring_context {
	struct io_uring ring;
	struct skynetjit_uring_watch *watches;
};

static struct skynetjit_uring_context *SKYNETJIT_URING;

static uint64_t
skynetjit_uring_data(const struct skynetjit_uring_watch *watch, bool cancel) {
	uintptr_t value = (uintptr_t)watch;
	return cancel ? (uint64_t)(value | 1) : (uint64_t)value;
}

static struct io_uring_sqe *
skynetjit_uring_get_sqe(void) {
	struct io_uring_sqe *sqe = io_uring_get_sqe(&SKYNETJIT_URING->ring);
	if (sqe != NULL) {
		return sqe;
	}
	if (io_uring_submit(&SKYNETJIT_URING->ring) < 0) {
		return NULL;
	}
	return io_uring_get_sqe(&SKYNETJIT_URING->ring);
}

static int
skynetjit_uring_arm(struct skynetjit_uring_watch *watch) {
	if (!watch->active || watch->armed || watch->cancel_pending || watch->mask == 0) {
		return 0;
	}
	struct io_uring_sqe *sqe = skynetjit_uring_get_sqe();
	if (sqe == NULL) {
		return 1;
	}
	io_uring_prep_poll_add(sqe, watch->fd, watch->mask);
	io_uring_sqe_set_data64(sqe, skynetjit_uring_data(watch, false));
	watch->armed = true;
	return 0;
}

static int
skynetjit_uring_cancel(struct skynetjit_uring_watch *watch) {
	if (!watch->armed || watch->cancel_pending) {
		return 0;
	}
	struct io_uring_sqe *sqe = skynetjit_uring_get_sqe();
	if (sqe == NULL) {
		return 1;
	}
	io_uring_prep_cancel(sqe, watch, 0);
	io_uring_sqe_set_data64(sqe, skynetjit_uring_data(watch, true));
	watch->cancel_pending = true;
	return 0;
}

static struct skynetjit_uring_watch *
skynetjit_uring_find(int fd) {
	struct skynetjit_uring_watch *watch;
	for (watch = SKYNETJIT_URING->watches; watch != NULL; watch = watch->next) {
		if (watch->fd == fd) {
			return watch;
		}
	}
	return NULL;
}

static void
skynetjit_uring_remove(struct skynetjit_uring_watch *watch) {
	struct skynetjit_uring_watch **current = &SKYNETJIT_URING->watches;
	while (*current != NULL) {
		if (*current == watch) {
			*current = watch->next;
			free(watch);
			return;
		}
		current = &(*current)->next;
	}
}

static bool
sp_invalid(poll_fd fd) {
	return SKYNETJIT_URING != NULL && fd == SKYNETJIT_URING_FD
		? false
		: skynetjit_epoll_invalid(fd);
}

static poll_fd
sp_create() {
	if (SKYNETJIT_URING != NULL) {
		return -1;
	}
	struct skynetjit_uring_context *context = calloc(1, sizeof(*context));
	if (context != NULL &&
		io_uring_queue_init(SKYNETJIT_URING_QUEUE_DEPTH, &context->ring, 0) == 0) {
		SKYNETJIT_URING = context;
		return SKYNETJIT_URING_FD;
	}
	free(context);
	fprintf(stderr, "[skynetjit] io_uring unavailable; falling back to epoll\n");
	return skynetjit_epoll_create();
}

static void
sp_release(poll_fd fd) {
	if (SKYNETJIT_URING == NULL || fd != SKYNETJIT_URING_FD) {
		skynetjit_epoll_release(fd);
		return;
	}
	struct skynetjit_uring_watch *watch = SKYNETJIT_URING->watches;
	while (watch != NULL) {
		struct skynetjit_uring_watch *next = watch->next;
		free(watch);
		watch = next;
	}
	io_uring_queue_exit(&SKYNETJIT_URING->ring);
	free(SKYNETJIT_URING);
	SKYNETJIT_URING = NULL;
}

static int
sp_add(poll_fd fd, int sock, void *ud) {
	if (SKYNETJIT_URING == NULL || fd != SKYNETJIT_URING_FD) {
		return skynetjit_epoll_add(fd, sock, ud);
	}
	struct skynetjit_uring_watch *watch = calloc(1, sizeof(*watch));
	if (watch == NULL) {
		return 1;
	}
	watch->fd = sock;
	watch->ud = ud;
	watch->mask = POLLIN;
	watch->active = true;
	watch->next = SKYNETJIT_URING->watches;
	SKYNETJIT_URING->watches = watch;
	if (skynetjit_uring_arm(watch)) {
		SKYNETJIT_URING->watches = watch->next;
		free(watch);
		return 1;
	}
	return 0;
}

static void
sp_del(poll_fd fd, int sock) {
	if (SKYNETJIT_URING == NULL || fd != SKYNETJIT_URING_FD) {
		skynetjit_epoll_del(fd, sock);
		return;
	}
	struct skynetjit_uring_watch *watch = skynetjit_uring_find(sock);
	if (watch != NULL) {
		watch->active = false;
		if (watch->armed) {
			(void)skynetjit_uring_cancel(watch);
		} else {
			skynetjit_uring_remove(watch);
		}
	}
}

static int
sp_enable(poll_fd fd, int sock, void *ud, bool read_enable, bool write_enable) {
	if (SKYNETJIT_URING == NULL || fd != SKYNETJIT_URING_FD) {
		return skynetjit_epoll_enable(fd, sock, ud, read_enable, write_enable);
	}
	struct skynetjit_uring_watch *watch = skynetjit_uring_find(sock);
	if (watch == NULL) {
		return 1;
	}
	watch->ud = ud;
	watch->mask = (read_enable ? POLLIN : 0) | (write_enable ? POLLOUT : 0);
	if (watch->armed) {
		return skynetjit_uring_cancel(watch);
	}
	return skynetjit_uring_arm(watch);
}

static int
sp_wait(poll_fd fd, struct event *events, int max) {
	if (SKYNETJIT_URING == NULL || fd != SKYNETJIT_URING_FD) {
		return skynetjit_epoll_wait(fd, events, max);
	}
	int emitted = 0;
	while (emitted == 0) {
		if (io_uring_submit_and_wait(&SKYNETJIT_URING->ring, 1) < 0) {
			return -1;
		}
		struct io_uring_cqe *cqe;
		unsigned head;
		unsigned count = 0;
		io_uring_for_each_cqe(&SKYNETJIT_URING->ring, head, cqe) {
			uint64_t data = io_uring_cqe_get_data64(cqe);
			struct skynetjit_uring_watch *watch =
				(struct skynetjit_uring_watch *)(uintptr_t)(data & ~(uint64_t)1);
			if ((data & 1) != 0) {
				watch->cancel_pending = false;
				watch->armed = false;
				if (watch->active) {
					(void)skynetjit_uring_arm(watch);
				} else {
					skynetjit_uring_remove(watch);
				}
			} else {
				watch->armed = false;
				int result = cqe->res;
				if (watch->active && result != -ECANCELED && emitted < max) {
					unsigned flags = result < 0 ? POLLERR : (unsigned)result;
					events[emitted].s = watch->ud;
					events[emitted].read = (flags & POLLIN) != 0;
					events[emitted].write = (flags & POLLOUT) != 0;
					events[emitted].error = (flags & (POLLERR | POLLNVAL)) != 0;
					events[emitted].eof = (flags & POLLHUP) != 0;
					++emitted;
				}
				if (!watch->active && !watch->cancel_pending) {
					skynetjit_uring_remove(watch);
				} else {
					(void)skynetjit_uring_arm(watch);
				}
			}
			++count;
		}
		io_uring_cq_advance(&SKYNETJIT_URING->ring, count);
	}
	return emitted;
}

static void
sp_nonblocking(int fd) {
	skynetjit_epoll_nonblocking(fd);
}

#endif
