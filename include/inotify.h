// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/inotify.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 目录/文件变更通知
 */
#ifndef _INOTIFY_H_
#define _INOTIFY_H_

#include <klib.h>

//事件掩码
#define IN_ACCESS        0x00000001 //文件被读取
#define IN_MODIFY        0x00000002 //文件被写入
#define IN_ATTRIB        0x00000004 //文件属性改变
#define IN_CLOSE_WRITE   0x00000008 //可写文件被关闭
#define IN_CLOSE_NOWRITE 0x00000010 //不可写文件被关闭
#define IN_OPEN          0x00000020 //文件被打开
#define IN_MOVED_FROM    0x00000040 //文件被移走
#define IN_MOVED_TO      0x00000080 //文件被移入
#define IN_CREATE        0x00000100 //文件被创建
#define IN_DELETE        0x00000200 //文件被删除
#define IN_DELETE_SELF   0x00000400 //被监控对象自身被删除
#define IN_MOVE_SELF     0x00000800 //被监控对象自身被移动
#define IN_UNMOUNT       0x00002000 //所在文件系统被卸载
#define IN_Q_OVERFLOW    0x00004000 //事件队列溢出
#define IN_IGNORED       0x00008000 //监控项被内核移除
#define IN_ONLYDIR       0x01000000 //仅监控目录（添加监控时的标志）
#define IN_DONT_FOLLOW   0x02000000 //不跟随符号链接（添加监控时的标志）
#define IN_EXCL_UNLINK   0x04000000 //不报告已删除文件的子事件（添加监控时的标志）
#define IN_MASK_ADD      0x20000000 //将事件掩码追加到已有监控项（添加监控时的标志）
#define IN_ISDIR         0x40000000 //事件对应的对象是目录（事件返回时的标志）
#define IN_ONESHOT       0x80000000 //仅报告一次事件后移除监控项（添加监控时的标志）
#define IN_ALL_EVENTS    0x00000FFF //所有事件掩码的合集

#define INOTIFY_WATCH_MAX 16//单个实例最多监视路径个数
#define INOTIFY_NAME_MAX  32//事件名最大长度
#define INOTIFY_EVENT_MAX 64//事件队列深度

//用户可见的事件头
typedef struct kinotify_event {
    int32_t  wd;        //监视项的wd
    uint32_t mask;      //事件掩码
    uint32_t cookie;    //关联事件的cookie
    uint32_t len;       //name长度
} kinotify_event_t;

int AnonInotifyInit(int flags);//创建inotify实例
int AnonInotifyAddWatch(int fd, const char *path, uint32_t mask);//添加/更新监视项
int AnonInotifyRmWatch(int fd, int wd);//删除监视项
void InotifyNotify(const char *path, uint32_t mask);//通知事件发生

#endif
