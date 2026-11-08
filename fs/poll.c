// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/poll.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 统一的就绪判定与等待
 */
#include <poll.h>
#include <anon.h>
#include <fs.h>
#include <task.h>
#include <pipe.h>
#include <irq.h>
#include <signals.h>
#include <drives/tty.h>

static wait_queue_head_t g_poll_wq = {&g_poll_wq, &g_poll_wq};//等待队列
static volatile uint64_t g_poll_seq = 0;//唤醒序号

//唤醒所有poll等待者
void PollWakeAll(void){
    g_poll_seq ++;
    wake_up_all(&g_poll_wq);
}

//fd是否为控制台
static _Bool poll_fd_is_console(long fd, fs_file_t *f){
    if(fd < 3 && !f->used)return true;
    return f->used && !f->inode && !f->pipe && !f->anon;
}

//控制台就绪位
static short poll_console_ready(fs_file_t *f, long fd){
    int acc = (fd < 3 && !f->used) ? O_RDWR : (f->flags & O_ACCMODE);
    short rv = 0;
    if(acc != O_WRONLY && TTY_ReadReady())rv |= FSPOLL_IN;
    if(acc != O_RDONLY)rv |= FSPOLL_OUT;
    return rv;
}

//管道就绪位
static short poll_pipe_ready(fs_file_t *f){
    int acc = f->flags & O_ACCMODE;
    int pr = pipeReady((struct pipe*)f->pipe);
    short rv = 0;
    if((pr & 1) && acc != O_WRONLY)rv |= FSPOLL_IN;
    if((pr & 2) && acc != O_RDONLY)rv |= FSPOLL_OUT;
    return rv;
}

//按fd判定就绪事件位
short PollFdReady(long fd, short want){
    (void)want;
    if(!current_task)return FSPOLL_NVAL;
    if(fd < 0 || fd >= MAX_FD)return FSPOLL_NVAL;
    fs_file_t *f = &current_task->files[fd];
    if(poll_fd_is_console(fd, f))return poll_console_ready(f, fd);//控制台，控制台就绪位
    if(!f->used)return FSPOLL_NVAL;
    if(f->anon)return AnonPoll(f->anon,want);
    if(f->pipe)return poll_pipe_ready(f);
    if(f->f && ProcInodeIsMountinfo(f->f->f_inode)){
        uint64_t seq = VfsMountSeq();
        if(f->f->f_version == seq)return 0;
        f->f->f_version = seq;
        return FSPOLL_IN;
    }
    //普通文件/设备，恒就绪
    int acc = f->flags & O_ACCMODE;
    short rv = 0;
    if(acc != O_WRONLY)rv |= FSPOLL_IN;
    if(acc != O_RDONLY)rv |= FSPOLL_OUT;
    return rv;
}

//等待就绪或超时
int PollWait(int64_t timeout_ms){
    if(SignalPending())return POLL_WAIT_INTR;
    if(timeout_ms == 0)return POLL_WAIT_TIMEOUT;
    uint64_t start = SYSTEM_TimerTicks;
    uint64_t seen  = g_poll_seq;
    while(1){
        uint64_t step = 5;
        if(timeout_ms > 0){
            uint64_t elapsed = (SYSTEM_TimerTicks - start) * 10;
            if(elapsed >= (uint64_t)timeout_ms)return POLL_WAIT_TIMEOUT;
            uint64_t left = (uint64_t)timeout_ms - elapsed;
            if(left < step)step = left;
        }
        if(step == 0)step = 1;
        msleep(step);
        if(g_poll_seq != seen)return POLL_WAIT_READY;
        if(SignalPending())return POLL_WAIT_INTR;
    }
}
