#pragma once

#include <stdint.h>

enum EPOLL_EVENTS {
	EPOLLIN = (int)(1U << 0),
	EPOLLPRI = (int)(1U << 1),
	EPOLLOUT = (int)(1U << 2),
	EPOLLERR = (int)(1U << 3),
	EPOLLHUP = (int)(1U << 4),
	EPOLLRDNORM = (int)(1U << 6),
	EPOLLRDBAND = (int)(1U << 7),
	EPOLLWRNORM = (int)(1U << 8),
	EPOLLWRBAND = (int)(1U << 9),
	EPOLLMSG = (int)(1U << 10),
	EPOLLRDHUP = (int)(1U << 13),
	EPOLLONESHOT = (int)(1U << 31),
};

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2
#define EPOLL_CTL_DEL 3

typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
};
