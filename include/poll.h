// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/poll.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 就绪事件轮询与epoll
 */
#ifndef _POLL_H_
#define _POLL_H_

#include <klib.h>

//就绪事件位
#define FSPOLL_IN      0x0001
#define FSPOLL_PRI     0x0002
#define FSPOLL_OUT     0x0004
#define FSPOLL_ERR     0x0008
#define FSPOLL_HUP     0x0010
#define FSPOLL_NVAL    0x0020

//PollWait的返回值
#define POLL_WAIT_READY    0   //被唤醒
#define POLL_WAIT_TIMEOUT  1   //超时
#define POLL_WAIT_INTR     2   //有待投递信号,应返回-EINTR

void PollWakeAll(void);//唤醒所有等待者
int  PollWait(int64_t timeout_ms);//等待就绪或超时
short PollFdReady(long fd, short want);//按fd判定当前就绪事件位

//epoll事件
typedef struct __attribute__((packed)) kepoll_event {
    uint32_t events;
    uint64_t data;
} kepoll_event_t;

#define EP_MAX_WATCH   256
#define EP_CTL_ADD     1
#define EP_CTL_DEL     2
#define EP_CTL_MOD     3
#define EP_ET          0x80000000u

int EpollCreate(void);//创建epoll实例并返回fd
int EpollCtl(int epfd, int op, long fd, uint32_t events, uint64_t data);//增删改被监视的fd
int EpollWait(int epfd, kepoll_event_t *out, int maxevents, int64_t timeout_ms);//等待事件,返回就绪个数
short EpollReadyMask(void *inst);//实例当前是否有就绪事件
void EpollGet(void *inst);//引用计数+1
void EpollDestroy(void *inst);//引用计数-1,归零时释放

#endif
