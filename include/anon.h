// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/anon.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 匿名文件描述符对象
 */
#ifndef _ANON_H_
#define _ANON_H_

#include <klib.h>

//对象种类
#define ANON_NONE      0
#define ANON_EPOLL     1
#define ANON_EVENTFD   2
#define ANON_TIMERFD   3
#define ANON_SIGNALFD  4
#define ANON_PIDFD     5
#define ANON_INOTIFY   6
#define ANON_SOCKET    7
#define ANON_MEMFD     8

typedef struct anon_obj anon_obj_t;

//操作表
typedef struct anon_ops {
    int64_t (*read)    (anon_obj_t *o, char *buf, uint64_t len);
    int64_t (*write)   (anon_obj_t *o, const char *buf, uint64_t len);
    short   (*poll)    (anon_obj_t *o, short want);
    void    (*release) (anon_obj_t *o);
    int64_t (*seek)    (anon_obj_t *o, int64_t off, int whence);
    int64_t (*truncate)(anon_obj_t *o, uint64_t len);
} anon_ops_t;

//所有匿名对象头
struct anon_obj {
    int                kind;
    int                refs;
    uint32_t           size;
    const anon_ops_t  *ops;
};

void *AnonAlloc(int kind, const anon_ops_t *ops, uint64_t size);//分配一个对象(清零)
void AnonGet(void *obj);//引用计数+1
void AnonPut(void *obj);//引用计数-1,归零时调用release并释放内存
int AnonInstallFd(void *obj, int flags);//把对象挂到一个新fd上,返回fd
void *AnonFromFd(long fd);//取fd对应的对象
int64_t AnonRead(void *obj, char *buf, uint64_t len);//读数据
int64_t AnonWrite(void *obj, const char *buf, uint64_t len);//写数据
short AnonPoll(void *obj, short want);//判定就绪事件位
void AnonTimerTick(void);//定时器中断里推进所有timerfd
int AnonEventFd(uint32_t initval, int flags);//创建eventfd
int AnonTimerFdCreate(int clockid, int flags);//创建timerfd
int AnonTimerFdSetTime(int fd, uint64_t value_ns, uint64_t interval_ns, _Bool disarm);//装载/解除定时器
int AnonTimerFdGetTime(int fd, uint64_t *value_ns, uint64_t *interval_ns);//查询剩余时间与间隔
int AnonSignalFd(uint64_t kmask, int flags);//创建signalfd
int AnonPidFdOpen(long pid, int flags);//打开一个指向进程的pidfd
int AnonPidFdSignal(int fd, int sig);//通过pidfd给目标进程发信号
int AnonMemFd(int flags);//创建一个匿名内存文件,返回fd
uint64_t AnonMemFdSize(void *obj);//取匿名内存文件大小
int64_t AnonSeek(void *obj, int64_t off, int whence);//匿名对象定位
int64_t AnonTruncate(void *obj, uint64_t len);//匿名对象截断

#endif
