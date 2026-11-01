// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/eventpoll.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 水平触发(LT)
 */
#include <poll.h>
#include <fs.h>
#include <task.h>
#include <irq.h>
#include <print.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

//单个被监视的fd
typedef struct epitem {
    long     fd;    //被监视的fd
    uint32_t events;//被监视的事件位
    uint64_t data;  //用户数据
    _Bool    used;  //是否使用
} epitem_t;

//一个epoll实例
typedef struct epoll_inst {
    uint32_t refs;                  //引用计数
    int      count;                 //当前被监视的fd个数
    epitem_t items[EP_MAX_WATCH];   //表项数组
} epoll_inst_t;

//fd对应的epoll实例
static epoll_inst_t *ep_from_fd(int epfd){
    if(!current_task)return NULL;
    if(epfd < 0 || epfd >= MAX_FD)return NULL;
    fs_file_t *f = &current_task->files[epfd];
    if(!f->used || !f->epoll)return NULL;
    return (epoll_inst_t*)f->epoll;
}

//实例里找某个fd对应的表项
static epitem_t *ep_find(epoll_inst_t *ep, long fd){
    for(int i = 0;i < EP_MAX_WATCH;i ++){
        if(ep->items[i].used && ep->items[i].fd == fd)return &ep->items[i];
    }
    return NULL;
}

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

//引用计数+1
void EpollGet(void *inst){
    epoll_inst_t *ep = (epoll_inst_t*)inst;
    if(ep)ep->refs++;
}

//释放实例
void EpollDestroy(void *inst){
    epoll_inst_t *ep = (epoll_inst_t*)inst;
    if(!ep)return;
    if(ep->refs > 1){
        ep->refs--;
        return;
    }
    Pmm_Free((void*)VIRT_TO_PHYS(ep),(int)((sizeof(epoll_inst_t) + PAGE_SIZE - 1) / PAGE_SIZE));
}

//创建epoll实例并返回fd
int EpollCreate(void){
    if(!current_task)return -EBADF;
    int fd = -1;
    //寻找空闲fd
    for(int i = 3;i < MAX_FD;i ++){
        if(!current_task->files[i].used){
            fd = i;
            break;
        }
    }
    if(fd < 0)return -EMFILE;
    //分配epoll实例
    int pages = (int)((sizeof(epoll_inst_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    epoll_inst_t *ep = (epoll_inst_t*)PHYS_TO_VIRT(Pmm_Malloc(pages));
    if(!ep)return -ENOMEM;
    //初始化实例
    memset(ep, 0, sizeof(*ep));
    ep->refs = 1;
    memset(&current_task->files[fd], 0, sizeof(fs_file_t));
    current_task->files[fd].used  = true;
    current_task->files[fd].flags = O_RDWR;
    current_task->files[fd].epoll = ep;
    return fd;
}

//增删改被监视的fd
int EpollCtl(int epfd, int op, long fd, uint32_t events, uint64_t data){
    epoll_inst_t *ep = ep_from_fd(epfd);
    if(!ep)return -EBADF;
    if((fd < 0) || (fd >= MAX_FD))return -EBADF;
    if(fd == epfd)return -EINVAL;
    if(!current_task->files[fd].used && !(fd < 3))return -EBADF;
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
