// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/eventpoll.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 水平触发(LT)
 */
#include <poll.h>
#include <anon.h>
#include <fs.h>
#include <task.h>
#include <irq.h>
#include <syscalls.h>
#include <kstring.h>

//单个被监视的fd
typedef struct epitem {
    long     fd;    //被监视的fd
    uint32_t events;//被监视的事件位
    uint64_t data;  //用户数据
    _Bool    used;  //是否使用
} epitem_t;

//一个epoll实例
typedef struct epoll_inst {
    anon_obj_t hdr;//匿名对象头
    int        count;//当前监视的fd个数
    epitem_t   items[EP_MAX_WATCH];//监视表
} epoll_inst_t;

//实例是否有就绪事件
short EpollReadyMask(void *inst){
    epoll_inst_t *ep = (epoll_inst_t*)inst;
    if(!ep)return 0;
    for(int i = 0;i < EP_MAX_WATCH;i ++){
        epitem_t *it = &ep->items[i];
        if(!it->used)continue;
        short rv = PollFdReady(it->fd, (short)it->events);
        if(rv & (short)(it->events | FSPOLL_ERR | FSPOLL_HUP | FSPOLL_NVAL))return FSPOLL_IN;
    }
    return 0;
}

//epoll fd自身的就绪判定
static short ep_poll(anon_obj_t *o, short want){
    (void)want;
    return EpollReadyMask(o);
}

static const anon_ops_t g_ep_ops = { .poll = ep_poll };

//引用计数+1
void EpollGet(void *inst){
    AnonGet(inst);
}

//释放实例
void EpollDestroy(void *inst){
    AnonPut(inst);
}

//fd对应的epoll实例
static epoll_inst_t *ep_from_fd(int epfd){
    anon_obj_t *o = (anon_obj_t*)AnonFromFd(epfd);
    if(!o || o->kind != ANON_EPOLL)return NULL;
    return (epoll_inst_t*)o;
}

//实例里找某个fd对应的表项
static epitem_t *ep_find(epoll_inst_t *ep, long fd){
    for(int i = 0;i < EP_MAX_WATCH;i ++){
        if(ep->items[i].used && ep->items[i].fd == fd)return &ep->items[i];
    }
    return NULL;
}

//创建epoll实例并返回fd
int EpollCreate(void){
    epoll_inst_t *ep = (epoll_inst_t*)AnonAlloc(ANON_EPOLL,&g_ep_ops,sizeof(*ep));
    if(!ep)return -ENOMEM;
    return AnonInstallFd(ep,O_RDWR);
}

//增删改被监视的fd
int EpollCtl(int epfd, int op, long fd, uint32_t events, uint64_t data){
    epoll_inst_t *ep = ep_from_fd(epfd);
    if(!ep)return -EBADF;
    if((fd < 0) || (fd >= MAX_FD))return -EBADF;
    if(fd == epfd)return -EINVAL;
    if(!current_task->files[fd].used && (fd >= 3))return -EBADF;
    epitem_t *it = ep_find(ep, fd);
    if(op == EP_CTL_ADD){
        if(it)return -EEXIST;
        for(int i = 0;i < EP_MAX_WATCH;i ++){
            if(ep->items[i].used)continue;
            ep->items[i].fd     = fd;
            ep->items[i].events = events;
            ep->items[i].data   = data;
            ep->items[i].used   = true;
            ep->count ++;
            return 0;
        }
        return -ENOSPC;
    }
    if(op == EP_CTL_DEL){
        if(!it)return -ENOENT;
        it->used = false;
        ep->count --;
        return 0;
    }
    if(op == EP_CTL_MOD){
        if(!it)return -ENOENT;
        it->events = events;
        it->data   = data;
        return 0;
    }
    return -EINVAL;
}

//等待事件,返回就绪个数
int EpollWait(int epfd, kepoll_event_t *out, int maxevents, int64_t timeout_ms){
    epoll_inst_t *ep = ep_from_fd(epfd);
    if(!ep || !out)return -EBADF;
    if(maxevents <= 0)return -EINVAL;
    if(maxevents > EP_MAX_WATCH)maxevents = EP_MAX_WATCH;
    uint64_t start = SYSTEM_TimerTicks;
    while(1){
        int n = 0;
        //扫描实例里的表项,找就绪的
        for(int i = 0;i < EP_MAX_WATCH && n < maxevents;i ++){
            epitem_t *it = &ep->items[i];
            if(!it->used)continue;
            short rv = PollFdReady(it->fd, (short)it->events);
            rv &= (short)(it->events | FSPOLL_ERR | FSPOLL_HUP | FSPOLL_NVAL);
            if(!rv)continue;
            out[n].events = (uint32_t)(rv & 0xFFFF);
            out[n].data   = it->data;
            n ++;
        }
        if(n)return n;
        //没就绪,计算剩余超时
        int64_t left = -1;
        if(timeout_ms >= 0){
            uint64_t elapsed = (SYSTEM_TimerTicks - start) * 10;
            if(elapsed >= (uint64_t)timeout_ms)return 0;
            left = (int64_t)((uint64_t)timeout_ms - elapsed);
        }
        int w = PollWait(left);
        if(w == POLL_WAIT_TIMEOUT)return 0;
        if(w == POLL_WAIT_INTR)return -EINTR;
    }
}
