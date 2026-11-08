// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/socket.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * socket层
 */
#ifndef _SOCKET_H_
#define _SOCKET_H_

#include <klib.h>

//地址族
#define AF_UNSPEC    0
#define AF_UNIX      1
#define AF_INET      2
#define AF_INET6     10
#define AF_NETLINK   16

//套接字类型
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000

//shutdown
#define KSHUT_RD    0
#define KSHUT_WR    1
#define KSHUT_RDWR  2

//协议级选项
#define SOL_SOCKET    1
#define SO_DEBUG      1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_DONTROUTE  5
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_PASSCRED   16
#define SO_PEERCRED   17
#define SO_SNDTIMEO   21
#define SO_RCVTIMEO   20
#define SO_PASSPIDFD  76

//控制消息类型
#define SCM_RIGHTS       1
#define SCM_CREDENTIALS  2

//通用套接字地址
typedef struct ksockaddr {
    uint16_t sa_family;
    char     sa_data[14];
} ksockaddr_t;

//AF_UNIX套接字地址
typedef struct ksockaddr_un {
    uint16_t sun_family;
    char     sun_path[108];
} ksockaddr_un_t;

//AF_NETLINK套接字地址
typedef struct ksockaddr_nl {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
} ksockaddr_nl_t;

//netlink协议号
#define NETLINK_ROUTE            0
#define NETLINK_KOBJECT_UEVENT   15

//ucred(SO_PEERCRED)
typedef struct k_ucred {
    int32_t  pid;
    uint32_t uid;
    uint32_t gid;
} k_ucred_t;

int SockCreate(int family, int type, int proto);//创建socket,返回fd
int SockPair(int family, int type, int proto, long *fd0, long *fd1);//创建一对已连接的socket
int SockBind(int fd, const char *path);//绑定路径名
int SockIsNetlink(int fd);//该fd是否为netlink套接字
int SockBindNetlink(int fd, uint32_t nl_pid, uint32_t nl_groups);//绑定netlink地址
int SockGetNameNetlink(int fd, uint32_t *out_pid, uint32_t *out_groups);//取netlink本端地址
int SockListen(int fd, int backlog);//开始监听
int SockConnect(int fd, const char *path);//连接已监听路径
int SockAccept(int fd);//从监听队列取一个连接
int SockShutdown(int fd, int how);//关闭读写方向
int SockGetName(int fd, ksockaddr_un_t *out, int *outlen, _Bool peer);//取本端/对端地址
int SockSetOpt(int fd, int level, int optname, const void *val, int len);//设置/读取选项
int SockGetOpt(int fd, int level, int optname, void *val, int *len);//读取选项
int64_t SockSend(int fd, const char *buf, uint64_t len);//发送
int64_t SockRecv(int fd, char *buf, uint64_t len);//接收
int64_t SockSendTo(int fd, const char *buf, uint64_t len, const char *dst_path);//数据报发送到指定路径
int64_t SockRecvFrom(int fd, char *buf, uint64_t len, char *from_path);//接收并回填来源路径
int64_t SockSendMsg(int fd, const char *buf, uint64_t len, const char *ctl, uint64_t ctllen, const char *dst_path);//带控制消息发送
int64_t SockRecvMsg(int fd, char *buf, uint64_t len, char *ctl, uint64_t cap, uint64_t *ctlout, char *from_path);//带控制消息接收
void *SockFromFd(long fd);//取fd对应的socket对象

#endif
