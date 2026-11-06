// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/anonfd.c — eventfd/timerfd/signalfd/pidfd
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 语义要点: 读在未就绪时一律返回 -EAGAIN, 由调用方先用 epoll/poll 等就绪;
 *           就绪状态变化必须调用 PollWakeAll()。
 * 
 * 由DeepSeek V4.1 Flash生成
 */
#include <anon.h>
#include <fs.h>
#include <task.h>
#include <poll.h>
#include <signals.h>
#include <irq.h>
#include <syscalls.h>
#include <kstring.h>

//---- eventfd ----
typedef struct eventfd_obj {
    anon_obj_t hdr;
    uint64_t   count;
    _Bool      semaphore;//EFD_SEMAPHORE: 每次读只减 1
} eventfd_obj_t;

#define EFD_SEMAPHORE_FLAG 1
#define EFD_NONBLOCK_FLAG  0x800

static short ev_poll(anon_obj_t *o, short want){
    (void)want;
    eventfd_obj_t *e = (eventfd_obj_t*)o;
    short rv = FSPOLL_OUT;
    if(e->count > 0)rv |= FSPOLL_IN;
    return rv;
}

static int64_t ev_read(anon_obj_t *o, char *buf, uint64_t len){
    eventfd_obj_t *e = (eventfd_obj_t*)o;
    if(len < sizeof(uint64_t))return -EINVAL;
    if(e->count == 0)return -EAGAIN;
    uint64_t v;
    if(e->semaphore){
        v = 1;
        e->count --;
    }else{
        v = e->count;
        e->count = 0;
    }
    memcpy(buf, &v, sizeof(v));
    PollWakeAll();
    return (int64_t)sizeof(v);
}

static int64_t ev_write(anon_obj_t *o, const char *buf, uint64_t len){
    eventfd_obj_t *e = (eventfd_obj_t*)o;
    if(len < sizeof(uint64_t))return -EINVAL;
    uint64_t v;
    memcpy(&v, buf, sizeof(v));
    if(v == 0xFFFFFFFFFFFFFFFFULL)return -EINVAL;
    if(e->count > 0xFFFFFFFFFFFFFFFEULL - v)return -EAGAIN;
    e->count += v;
    PollWakeAll();
    return (int64_t)sizeof(v);
}

static const anon_ops_t g_ev_ops = { .read = ev_read, .write = ev_write, .poll = ev_poll };

//创建 eventfd
int AnonEventFd(uint32_t initval, int flags){
    eventfd_obj_t *e = (eventfd_obj_t*)AnonAlloc(ANON_EVENTFD, &g_ev_ops, sizeof(*e));
    if(!e)return -ENOMEM;
    e->count     = initval;
    e->semaphore = (flags & EFD_SEMAPHORE_FLAG) != 0;
    return AnonInstallFd(e, (flags & EFD_NONBLOCK_FLAG) ? 1 : 0);
}

//---- timerfd ----
typedef struct timerfd_obj {
    anon_obj_t    hdr;
    uint64_t      interval_ticks;//重复间隔(tick, 10ms)
    uint64_t      next_tick;//下次到期 tick, 0 表示未装载
    uint64_t      expirations;//已到期次数
    struct timerfd_obj *next;//全局链表, 供定时器中断扫描
} timerfd_obj_t;

static timerfd_obj_t *g_timers = NULL;//所有 timerfd 的单链表

static short tf_poll(anon_obj_t *o, short want){
    (void)want;
    return ((timerfd_obj_t*)o)->expirations ? FSPOLL_IN : 0;
}

static int64_t tf_read(anon_obj_t *o, char *buf, uint64_t len){
    timerfd_obj_t *t = (timerfd_obj_t*)o;
    if(len < sizeof(uint64_t))return -EINVAL;
    if(!t->expirations)return -EAGAIN;
    uint64_t v = t->expirations;
    t->expirations = 0;
    memcpy(buf, &v, sizeof(v));
    return (int64_t)sizeof(v);
}

//释放时从全局链表摘掉
static void tf_release(anon_obj_t *o){
    timerfd_obj_t **pp = &g_timers;
    while(*pp){
        if(*pp == (timerfd_obj_t*)o){
            *pp = ((timerfd_obj_t*)o)->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static const anon_ops_t g_tf_ops = { .read = tf_read, .poll = tf_poll, .release = tf_release };

//ns 转 tick(10ms)
static uint64_t ns_to_ticks(uint64_t ns){
    uint64_t t = ns / 10000000ULL;
    if(ns % 10000000ULL)t ++;
    return t;
}

//创建 timerfd
int AnonTimerFdCreate(int clockid, int flags){
    (void)clockid;
    timerfd_obj_t *t = (timerfd_obj_t*)AnonAlloc(ANON_TIMERFD, &g_tf_ops, sizeof(*t));
    if(!t)return -ENOMEM;
    t->next = g_timers;
    g_timers = t;
    return AnonInstallFd(t, flags & 1);
}

//装载/解除定时器
int AnonTimerFdSetTime(int fd, uint64_t value_ns, uint64_t interval_ns, _Bool disarm){
    timerfd_obj_t *t = (timerfd_obj_t*)AnonFromFd(fd);
    if(!t || t->hdr.kind != ANON_TIMERFD)return -EBADF;
    t->interval_ticks = ns_to_ticks(interval_ns);
    if(disarm || value_ns == 0){
        t->next_tick = 0;
        return 0;
    }
    uint64_t d = ns_to_ticks(value_ns);
    if(d == 0)d = 1;
    t->next_tick = SYSTEM_TimerTicks + d;
    return 0;
}

//查询剩余时间与间隔
int AnonTimerFdGetTime(int fd, uint64_t *value_ns, uint64_t *interval_ns){
    timerfd_obj_t *t = (timerfd_obj_t*)AnonFromFd(fd);
    if(!t || t->hdr.kind != ANON_TIMERFD)return -EBADF;
    if(interval_ns)*interval_ns = t->interval_ticks * 10000000ULL;
    if(value_ns){
        uint64_t v = 0;
        if(t->next_tick > SYSTEM_TimerTicks)v = (t->next_tick - SYSTEM_TimerTicks) * 10000000ULL;
        *value_ns = v;
    }
    return 0;
}

//定时器中断里推进所有timerfd
void AnonTimerTick(void){
    _Bool woke = false;
    for(timerfd_obj_t *t = g_timers;t;t = t->next){
        if(!t->next_tick)continue;
        if(SYSTEM_TimerTicks < t->next_tick)continue;
        t->expirations ++;
        if(t->interval_ticks){
            t->next_tick += t->interval_ticks;
            if(t->next_tick <= SYSTEM_TimerTicks)t->next_tick = SYSTEM_TimerTicks + 1;//防止落后太多
        }else{
            t->next_tick = 0;
        }
        woke = true;
    }
    if(woke)PollWakeAll();
}

//---- signalfd ----
typedef struct signalfd_obj {
    anon_obj_t hdr;
    uint64_t   mask;//内核信号掩码(bit N 表示信号 N)
} signalfd_obj_t;

//用户可见的 signalfd_siginfo(128 字节, 只填前几个字段)
typedef struct sfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    uint8_t  pad[108];
} sfd_siginfo_t;

static short sf_poll(anon_obj_t *o, short want){
    (void)want;
    signalfd_obj_t *s = (signalfd_obj_t*)o;
    if(!current_task)return 0;
    return (current_task->sigpending & s->mask) ? FSPOLL_IN : 0;
}

//取最低位待处理信号
static int64_t sf_read(anon_obj_t *o, char *buf, uint64_t len){
    signalfd_obj_t *s = (signalfd_obj_t*)o;
    if(!current_task)return -EINVAL;
    if(len < sizeof(sfd_siginfo_t))return -EINVAL;
    uint64_t p = current_task->sigpending & s->mask;
    if(!p)return -EAGAIN;
    int sig = 0;
    for(int i = 1;i < 64;i ++){
        if(p & (1ULL << i)){ sig = i; break; }
    }
    if(!sig)return -EAGAIN;
    current_task->sigpending &= ~(1ULL << sig);
    sfd_siginfo_t si;
    memset(&si, 0, sizeof(si));
    si.ssi_signo = (uint32_t)sig;
    si.ssi_code  = (int32_t)current_task->sig_last_code;
    si.ssi_pid   = (uint32_t)current_task->sig_last_pid;
    memcpy(buf, &si, sizeof(si));
    return (int64_t)sizeof(si);
}

static const anon_ops_t g_sf_ops = { .read = sf_read, .poll = sf_poll };

//创建 signalfd
int AnonSignalFd(uint64_t kmask, int flags){
    signalfd_obj_t *s = (signalfd_obj_t*)AnonAlloc(ANON_SIGNALFD, &g_sf_ops, sizeof(*s));
    if(!s)return -ENOMEM;
    s->mask = kmask;
    return AnonInstallFd(s, flags & 1);
}

//---- pidfd ----
typedef struct pidfd_obj {
    anon_obj_t hdr;
    long       pid;
} pidfd_obj_t;

//目标进程不存在(或已终止)即视为可读
static short pf_poll(anon_obj_t *o, short want){
    (void)want;
    pidfd_obj_t *p = (pidfd_obj_t*)o;
    task_struct *t = TaskFind((pid_t)p->pid);
    if(!t || t->state == TASK_TERMINATED)return FSPOLL_IN | FSPOLL_HUP;
    return 0;
}

static const anon_ops_t g_pf_ops = { .poll = pf_poll };

//打开一个指向进程的 pidfd
int AnonPidFdOpen(long pid, int flags){
    if(pid <= 0)return -EINVAL;
    if(!TaskFind((pid_t)pid))return -ESRCH;
    pidfd_obj_t *p = (pidfd_obj_t*)AnonAlloc(ANON_PIDFD, &g_pf_ops, sizeof(*p));
    if(!p)return -ENOMEM;
    p->pid = pid;
    return AnonInstallFd(p, flags & 1);
}

//通过 pidfd 给目标进程发信号
int AnonPidFdSignal(int fd, int sig){
    pidfd_obj_t *p = (pidfd_obj_t*)AnonFromFd(fd);
    if(!p || p->hdr.kind != ANON_PIDFD)return -EBADF;
    task_struct *t = TaskFind((pid_t)p->pid);
    if(!t || t->state == TASK_TERMINATED)return -ESRCH;
    if(sig > 0)SignalSend(t, sig, SI_USER, current_task ? current_task->pid : 0, 0);
    return 0;
}
