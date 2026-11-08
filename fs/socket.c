// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/socket.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * socket层
 */
#include <socket.h>
#include <anon.h>
#include <poll.h>
#include <fs.h>
#include <task.h>
#include <syscalls.h>
#include <signals.h>
#include <pipe.h>
#include <kstring.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define SKBUF_SIZE     8192 //每方向接收缓冲
#define SK_NAME_MAX    108  //sun_path长度
#define SK_LISTEN_MAX  16   //每个监听socket的排队长度
#define SK_FD_MAX       8   //单次SCM_RIGHTS最多传fd数
#define SK_ANC_QUEUE    8   //流式socket辅助数据队列深度

//socket状态
#define SS_FREE        0
#define SS_UNCONNECTED 1
#define SS_LISTENING   2
#define SS_CONNECTED   3

typedef struct ksocket ksocket_t;

//一次发送携带的辅助数据
typedef struct sk_anc {
    struct sk_anc *next;
    int            nfds;
    fs_file_t      fds[SK_FD_MAX];//已各自持有一份引用的描述符副本
    _Bool          has_cred;
    int32_t        pid;
    uint32_t       uid;
    uint32_t       gid;
} sk_anc_t;

static void sk_anc_free(sk_anc_t *a);

//socket内核对象
struct ksocket {
    anon_obj_t  hdr;
    int         family;
    int         type;
    int         state;
    int         nonblock;
    int         flags;
    ksocket_t  *peer;      //已连接的对端
    _Bool       peer_eof;  //对端已关闭写方向
    _Bool       shut_rd;   //本端已关闭读方向
    //接收环形缓冲
    char        rbuf[SKBUF_SIZE];
    uint32_t    rh;
    uint32_t    rc;
    //绑定名与监听
    char        path[SK_NAME_MAX];
    int         backlog;
    _Bool       listening;
    //accept 队列
    ksocket_t  *pending[SK_LISTEN_MAX];
    int         npending;
    //全局名表链
    ksocket_t  *next_named;
    //对端凭据
    pid_t       peer_pid;
    //数据报队列(SOCK_DGRAM)
    struct sk_dgram *dgram_head;
    struct sk_dgram *dgram_tail;
    int         dgram_count;
    //辅助数据
    struct sk_anc *anc_head;
    struct sk_anc *anc_tail;
    _Bool       passcred;
    //netlink本地地址
    uint32_t    nl_pid;
    uint32_t    nl_groups;
};

static ksocket_t *g_named = NULL;//已bind的socket链

static ksocket_t *sk_find_named(const char *path);//按路径找监听socket

//一个排队的数据报
typedef struct sk_dgram {
    struct sk_dgram *next;
    uint32_t         len;
    char             from[SK_NAME_MAX];
    struct sk_anc   *anc;
    char             data[1];
} sk_dgram_t;

#define SK_DGRAM_MAXLEN 1024//单条数据报最大负载
#define SK_DGRAM_QUEUE  32  //队列深度

static uint32_t sk_free(const ksocket_t *s){
    return SKBUF_SIZE - s->rc;
}
static uint32_t sk_put(ksocket_t *s, const char *buf, uint32_t len){
    uint32_t space = sk_free(s);
    if(len > space)len = space;
    for(uint32_t i = 0;i < len;i ++)s->rbuf[(s->rh + s->rc + i) % SKBUF_SIZE] = buf[i];
    s->rc += len;
    return len;
}
static uint32_t sk_get(ksocket_t *s, char *buf, uint32_t len){
    if(len > s->rc)len = s->rc;
    for(uint32_t i = 0;i < len;i ++)buf[i] = s->rbuf[(s->rh + i) % SKBUF_SIZE];
    s->rh = (s->rh + len) % SKBUF_SIZE;
    s->rc -= len;
    return len;
}

//就绪判定
static short sk_poll(anon_obj_t *o, short want){
    (void)want;
    ksocket_t *s = (ksocket_t*)o;
    short rv = 0;
    if(s->dgram_count > 0)rv |= FSPOLL_IN;
    //数据报socket对端关闭不算可读
    if(s->rc > 0)rv |= FSPOLL_IN;
    if(s->peer_eof && (s->type != SOCK_DGRAM))rv |= FSPOLL_IN;
    if(s->peer && (sk_free(s->peer) > 0))rv |= FSPOLL_OUT;
    if(s->type == SOCK_DGRAM && s->state == SS_CONNECTED)rv |= FSPOLL_OUT;
    if(!s->peer && (s->state == SS_CONNECTED) && (s->type != SOCK_DGRAM))rv |= FSPOLL_HUP;
    return rv;
}

//向目标socket压入一条数据报
static int sk_dgram_push_anc(ksocket_t *dst, const char *buf, uint32_t len, const char *from, sk_anc_t *anc){
    if(!dst){
        if(anc)sk_anc_free(anc);
        return -ECONNREFUSED;
    }
    if(dst->dgram_count >= SK_DGRAM_QUEUE){
        if(anc)sk_anc_free(anc);
        return -ENOBUFS;
    }
    if(len > SK_DGRAM_MAXLEN)len = SK_DGRAM_MAXLEN;
    sk_dgram_t *d = (sk_dgram_t*)PHYS_TO_VIRT(Pmm_Malloc(1));
    if(!d){
        if(anc)sk_anc_free(anc);
        return -ENOMEM;
    }
    memset(d, 0, PAGE_SIZE);
    d->len = len;
    memcpy(d->data, buf, (uint64_t)len);
    d->anc = anc;
    if(from){
        int n = (int)strlen(from);
        if(n > SK_NAME_MAX - 1)n = SK_NAME_MAX - 1;
        memcpy(d->from, from, (uint64_t)n);
    }
    if(dst->dgram_tail)dst->dgram_tail->next = d;
    else               dst->dgram_head = d;
    dst->dgram_tail = d;
    dst->dgram_count ++;
    PollWakeAll();
    return 0;
}

//向目标socket压入一条数据报
static int sk_dgram_push(ksocket_t *dst, const char *buf, uint32_t len, const char *from){
    return sk_dgram_push_anc(dst, buf, len, from, NULL);
}

//取出一条数据报
static int64_t sk_dgram_pop_anc(ksocket_t *s, char *buf, uint64_t len, char *from, sk_anc_t **anc_out){
    if(!s->dgram_head)return -EAGAIN;
    sk_dgram_t *d = s->dgram_head;
    s->dgram_head = d->next;
    if(!s->dgram_head)s->dgram_tail = NULL;
    s->dgram_count --;
    uint64_t n = d->len;
    if(n > len)n = len;
    memcpy(buf, d->data, n);
    if(from){
        int m = (int)strlen(d->from);
        memcpy(from, d->from, (uint64_t)m);
        from[m] = 0;
    }
    if(anc_out)*anc_out = d->anc;
    else if(d->anc)sk_anc_free(d->anc);
    Pmm_Free((void*)VIRT_TO_PHYS(d), 1);
    PollWakeAll();
    return (int64_t)n;
}

static int64_t sk_dgram_pop(ksocket_t *s, char *buf, uint64_t len, char *from){
    return sk_dgram_pop_anc(s, buf, len, from, NULL);
}

//取数据报来源地址
static const char *sk_dgram_from(ksocket_t *s){
    return s->dgram_head ? s->dgram_head->from : "";
}

//释放一个描述符副本
static void sk_fd_release(fs_file_t *f){
    if(!f || !f->used)return;
    if(f->anon)AnonPut(f->anon);
    else if(f->pipe)pipeCloseEnd((struct pipe*)f->pipe,(f->flags & O_ACCMODE) == O_WRONLY);
    else if(f->f)FilePut(f->f);
    f->used = false;
    f->anon = NULL;
    f->pipe = NULL;
    f->f = NULL;
    f->inode = NULL;
}

//释放辅助数据
static void sk_anc_free(sk_anc_t *a){
    if(!a)return;
    for(int i = 0;i < a->nfds;i ++)sk_fd_release(&a->fds[i]);
    Pmm_Free((void*)VIRT_TO_PHYS(a), 1);
}

//为当前任务的一个fd取一份引用,存到fs_file_t副本里
static int sk_fd_ref(long fd, fs_file_t *out){
    if(!current_task)return -EBADF;
    if(fd < 0 || fd >= MAX_FD)return -EBADF;
    fs_file_t *f = &current_task->files[fd];
    if(!f->used && fd >= 3)return -EBADF;
    memset(out, 0, sizeof(*out));
    *out = *f;
    out->used = true;
    if(f->anon)AnonGet(f->anon);
    else if(f->pipe)pipeForkRef((struct pipe*)f->pipe,(f->flags & O_ACCMODE) == O_WRONLY);
    else if(f->f)FileGet(f->f);
    return 0;
}

//把一份描述符副本安装到当前任务的fd表
static int sk_fd_install(fs_file_t *src){
    if(!current_task)return -EBADF;
    int fd = -1;
    for(int i = 3;i < MAX_FD;i ++){
        if(!current_task->files[i].used){
            fd = i;
            break;
        }
    }
    if(fd < 0)return -EMFILE;
    current_task->files[fd] = *src;
    current_task->files[fd].used = true;
    src->used = false;
    return fd;
}

//往辅助数据里加一个fd
static int sk_anc_add_fd(sk_anc_t *a, long fd){
    if(!a)return -EINVAL;
    if(a->nfds >= SK_FD_MAX)return -EINVAL;
    int r = sk_fd_ref(fd,&a->fds[a->nfds]);
    if(r)return r;
    a->nfds ++;
    return 0;
}

static void sk_anc_push(ksocket_t *dst, sk_anc_t *a){
    if(!dst || !a){ if(a)sk_anc_free(a); return; }
    a->next = NULL;
    if(dst->anc_tail)dst->anc_tail->next = a;
    else dst->anc_head = a;
    dst->anc_tail = a;
}

static sk_anc_t *sk_anc_pop(ksocket_t *s){
    if(!s->anc_head)return NULL;
    sk_anc_t *a = s->anc_head;
    s->anc_head = a->next;
    if(!s->anc_head)s->anc_tail = NULL;
    a->next = NULL;
    return a;
}

//读
static int64_t sk_read(anon_obj_t *o, char *buf, uint64_t len){
    ksocket_t *s = (ksocket_t*)o;
    if(!s || !buf)return -EINVAL;
    if(s->shut_rd)return 0;
    while(1){
        if(s->dgram_count > 0)return sk_dgram_pop(s, buf, len, NULL);
        if(s->rc > 0){
            uint32_t n = sk_get(s,buf,(uint32_t)(len > SKBUF_SIZE ? SKBUF_SIZE : len));
            PollWakeAll();
            return (int64_t)n;
        }
        if(s->type == SOCK_DGRAM)return -EAGAIN;
        if(s->peer_eof)return 0;
        if(s->nonblock)return -EAGAIN;
        {
            int w = PollWait(s->peer_eof ? 0 : -1);            if(w == POLL_WAIT_INTR)return -EINTR;
            if(w == POLL_WAIT_TIMEOUT)return -EAGAIN;
        }
    }
}

//写
static int64_t sk_write(anon_obj_t *o, const char *buf, uint64_t len){
    ksocket_t *s = (ksocket_t*)o;
    if(!s || !buf)return -EINVAL;
    if(s->family == AF_NETLINK)return (int64_t)len;
    if(s->type == SOCK_DGRAM){
        ksocket_t *dst = s->peer;
        if(!dst && s->state == SS_CONNECTED && s->path[0])dst = sk_find_named(s->path);
        if(!dst)return -EDESTADDRREQ;
        int r = sk_dgram_push(dst, buf, (uint32_t)(len > SK_DGRAM_MAXLEN ? SK_DGRAM_MAXLEN : len), s->path);
        if(r)return r;
        return (int64_t)(len > SK_DGRAM_MAXLEN ? SK_DGRAM_MAXLEN : len);
    }
    if(s->shut_rd || s->peer_eof || !s->peer)return -EPIPE;
    int64_t total = 0;
    while((uint64_t)total < len){
        uint32_t space = sk_free(s->peer);
        if(space == 0){
            if(s->nonblock)return total ? total : -EAGAIN;
            int w = PollWait(-1);
            if(w == POLL_WAIT_INTR)return total ? total : -EINTR;
            continue;
        }
        uint32_t chunk = (uint32_t)(len - (uint64_t)total);
        if(chunk > space)chunk = space;
        sk_put(s->peer,(buf + total),chunk);
        total += chunk;
        PollWakeAll();
        if(s->type == SOCK_DGRAM)break;
    }
    return total;
}

//释放
static void sk_release(anon_obj_t *o){
    ksocket_t *s = (ksocket_t*)o;
    //从全局名表摘掉
    ksocket_t **pp = &g_named;
    while(*pp){
        if(*pp == s){
            *pp = s->next_named;
            break;
        }
        pp = &(*pp)->next_named;
    }
    //通知对端EOF
    if(s->peer){
        s->peer->peer_eof = true;
        s->peer->peer = NULL;
        if(s->peer->state == SS_CONNECTED)s->peer->state = SS_UNCONNECTED;
        s->peer = NULL;
        PollWakeAll();
    }
}

//socket匿名对象操作表
static const anon_ops_t g_sk_ops = {
    .read    = sk_read,
    .write   = sk_write,
    .poll    = sk_poll,
    .release = sk_release,
};

//取fd对应的socket对象
void *SockFromFd(long fd){
    anon_obj_t *o = (anon_obj_t*)AnonFromFd(fd);
    if(!o || o->kind != ANON_SOCKET)return NULL;
    return o;
}

//分配一个socket对象
static ksocket_t *sk_alloc(int family, int type){
    ksocket_t *s = (ksocket_t*)AnonAlloc(ANON_SOCKET, &g_sk_ops, sizeof(*s));
    if(!s)return NULL;
    s->family = family;
    s->type = type;
    s->state = SS_UNCONNECTED;
    return s;
}

//把socket对象装到新fd上
static int sk_install(ksocket_t *s, int flags){
    if(!s)return -ENOMEM;
    return AnonInstallFd(s, flags);
}

//创建socket
int SockCreate(int family, int type, int proto){
    (void)proto;
    if((family != AF_UNIX) && (family != AF_NETLINK))return -EAFNOSUPPORT;
    int t = (type & 0xff);
    if(t == SOCK_SEQPACKET)t = SOCK_STREAM;
    if(family == AF_NETLINK)t = SOCK_DGRAM;
    if((t != SOCK_STREAM) && (t != SOCK_DGRAM))return -ESOCKTNOSUPPORT;
    //分配
    ksocket_t *s = sk_alloc(family,t);
    if(!s)return -ENOMEM;
    //初始化并挂到fd
    s->nonblock = (type & SOCK_NONBLOCK) ? 1 : 0;
    s->flags = type;
    int fd = sk_install(s, (type & SOCK_CLOEXEC) ? O_CLOEXEC : 0);
    return fd;
}

//创建一对已连接的socket
int SockPair(int family, int type, int proto, long *fd0, long *fd1){
    (void)proto;
    if(!fd0 || !fd1)return -EFAULT;
    if(family != AF_UNIX)return -EAFNOSUPPORT;
    int t = (type & 0xff);
    if(t == SOCK_SEQPACKET)t = SOCK_STREAM;
    if((t != SOCK_STREAM) && (t != SOCK_DGRAM))return -ESOCKTNOSUPPORT;
    //分配两个对象
    ksocket_t *a = sk_alloc(family,t);
    if(!a)return -ENOMEM;
    ksocket_t *b = sk_alloc(family,t);
    if(!b){
        AnonPut(a);
        return -ENOMEM;
    }
    //互连
    a->nonblock = b->nonblock = (type & SOCK_NONBLOCK) ? 1 : 0;
    a->peer = b;
    b->peer = a;
    a->state = b->state = SS_CONNECTED;
    a->peer_pid = b->peer_pid = current_task ? current_task->pid : 0;
    int cflags = (type & SOCK_CLOEXEC) ? O_CLOEXEC : 0;
    int f0 = sk_install(a, cflags);
    if(f0 < 0){
        AnonPut(b);
        return f0;
    }
    int f1 = sk_install(b, cflags);
    if(f1 < 0)return f1;
    *fd0 = f0;
    *fd1 = f1;
    return 0;
}

//按路径找监听socket
static ksocket_t *sk_find_named(const char *path){
    for(ksocket_t *s = g_named;s;s = s->next_named){
        if(strcmp(s->path, path) == 0)return s;
    }
    return NULL;
}

//绑定路径名
int SockBind(int fd, const char *path){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    if(s->family == AF_NETLINK)return -EINVAL;
    if(!path || !path[0])return -EINVAL;
    if(strlen(path) >= SK_NAME_MAX)return -ENAMETOOLONG;
    if(sk_find_named(path))return -EADDRINUSE;
    int n = (int)strlen(path);
    memcpy(s->path,path,(uint64_t)n);
    s->path[n] = 0;
    s->next_named = g_named;
    g_named = s;
    return 0;
}

//该fd是否为netlink套接字
int SockIsNetlink(int fd){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    return s && (s->family == AF_NETLINK);
}

//绑定netlink地址
int SockBindNetlink(int fd, uint32_t nl_pid, uint32_t nl_groups){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s || s->family != AF_NETLINK)return -ENOTSOCK;
    s->nl_pid    = nl_pid ? nl_pid : (uint32_t)(current_task ? current_task->pid : 1);
    s->nl_groups = nl_groups;
    s->state     = SS_CONNECTED;
    return 0;
}

//取netlink本端地址
int SockGetNameNetlink(int fd, uint32_t *out_pid, uint32_t *out_groups){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s || s->family != AF_NETLINK)return -ENOTSOCK;
    if(out_pid)*out_pid = s->nl_pid ? s->nl_pid : (uint32_t)(current_task ? current_task->pid : 1);
    if(out_groups)*out_groups = s->nl_groups;
    return 0;
}

//开始监听
int SockListen(int fd, int backlog){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    if(s->type != SOCK_STREAM)return -EOPNOTSUPP;
    if(!s->path[0])return -EINVAL;
    if(backlog <= 0)backlog = 1;
    if(backlog > SK_LISTEN_MAX)backlog = SK_LISTEN_MAX;
    s->backlog = backlog;
    s->listening = true;
    s->state = SS_LISTENING;
    return 0;
}

//连接已监听路径
int SockConnect(int fd, const char *path){
    ksocket_t *c = (ksocket_t*)SockFromFd(fd);
    if(!c)return -ENOTSOCK;
    if(!path || !path[0])return -EINVAL;
    ksocket_t *l = sk_find_named(path);
    if(!l)return -ENOENT;
    if(c->type == SOCK_DGRAM){
        int n = (int)strlen(path);
        if(n > SK_NAME_MAX - 1)n = SK_NAME_MAX - 1;
        memcpy(c->path, path, (uint64_t)n);
        c->path[n] = 0;
        c->state   = SS_CONNECTED;
        return 0;
    }
    if(!l->listening)return -ECONNREFUSED;
    if(l->npending >= l->backlog)return -EAGAIN;
    ksocket_t *srv = sk_alloc(AF_UNIX,c->type);
    if(!srv)return -ENOMEM;
    srv->nonblock = l->nonblock;
    srv->peer = c;
    c->peer = srv;
    memcpy(srv->path, l->path, sizeof(srv->path));
    srv->state = c->state = SS_CONNECTED;
    srv->peer_pid = current_task ? current_task->pid : 0;
    c->peer_pid = current_task ? current_task->pid : 0;
    l->pending[l->npending ++] = srv;
    PollWakeAll();
    return 0;
}

//从监听队列取一个连接
int SockAccept(int fd){
    //从fd取监听socket
    ksocket_t *l = (ksocket_t*)SockFromFd(fd);
    if(!l)return -ENOTSOCK;
    if(!l->listening)return -EINVAL;
    while(1){
        //有连接就取
        if(l->npending > 0){
            ksocket_t *s = l->pending[0];
            for(int i = 1;i < l->npending;i ++)l->pending[i - 1] = l->pending[i];
            l->npending --;
            return sk_install(s, 0);
        }
        if(l->nonblock)return -EAGAIN;
        int w = PollWait(-1);
        if(w == POLL_WAIT_INTR)return -EINTR;
    }
}

//关闭读写方向
int SockShutdown(int fd, int how){
    //从fd取socket
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    //按方向关
    if((how == KSHUT_RD) || (how == KSHUT_RDWR)){
        s->shut_rd = true;
        s->rh = 0;
        s->rc = 0;
    }
    if((how == KSHUT_WR) || (how == KSHUT_RDWR)){
        s->peer_eof = true;
        if(s->peer){
            s->peer->peer = NULL;
            s->peer->peer_eof = true;
            s->peer = NULL;
        }
    }
    PollWakeAll();//唤醒等待者
    return 0;
}

//取本端/对端地址
int SockGetName(int fd, ksockaddr_un_t *out, int *outlen, _Bool peer){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    if(!out)return -EFAULT;
    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;
    const char *src = peer ? (s->peer ? s->peer->path : "") : s->path;
    if(src[0]){
        int n = (int)strlen(src);
        if(n > SK_NAME_MAX - 1)n = SK_NAME_MAX - 1;
        memcpy(out->sun_path, src, (uint64_t)n);
        if(outlen)*outlen = (int)(sizeof(uint16_t) + n + 1);
    }else{
        if(outlen)*outlen = (int)sizeof(uint16_t);
    }
    return 0;
}

//设置/读取选项
int SockSetOpt(int fd, int level, int optname, const void *val, int len){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    if(level != SOL_SOCKET)return 0;
    switch(optname){
        case SO_PASSCRED:
            if(val && len >= 4)s->passcred = (*(const int*)val) != 0;
            return 0;
        case SO_PASSPIDFD:
        case SO_REUSEADDR:
        case SO_SNDBUF:
        case SO_RCVBUF:
        case SO_SNDTIMEO:
        case SO_RCVTIMEO:
            return 0;
        default:
            return 0;
    }
}

int SockGetOpt(int fd, int level, int optname, void *val, int *len){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    if(!val || !len)return -EFAULT;
    if(level != SOL_SOCKET)return 0;
    switch(optname){
        case SO_TYPE:
            if(*len < 4)return -EINVAL;
            *(int*)val = s->type;
            *len = 4;
            return 0;
        case SO_ERROR:
            if(*len < 4)return -EINVAL;
            *(int*)val = 0;
            *len = 4;
            return 0;
        case SO_PEERCRED:{
            if(*len < (int)sizeof(k_ucred_t))return -EINVAL;
            k_ucred_t *c = (k_ucred_t*)val;
            memset(c,0,sizeof(*c));
            c->pid = s->peer_pid;
            if(current_task){
                c->uid = current_task->cred.euid;
                c->gid = current_task->cred.egid;
            }
            *len = (int)sizeof(k_ucred_t);
            return 0;
        }
        case SO_SNDBUF: case SO_RCVBUF:
            if(*len < 4)return -EINVAL;
            *(int*)val = SKBUF_SIZE;
            *len = 4;
            return 0;
        default:
            if(*len >= 4){
                *(int*)val = 0;
                *len = 4;
            }
            return 0;
    }
}

//发送
int64_t SockSend(int fd, const char *buf, uint64_t len){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    return sk_write((anon_obj_t*)s, buf, len);
}

//接收
int64_t SockRecv(int fd, char *buf, uint64_t len){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    return sk_read((anon_obj_t*)s, buf, len);
}

//数据报发送到指定路径
int64_t SockSendTo(int fd, const char *buf, uint64_t len, const char *dst_path){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    if(s->type != SOCK_DGRAM || !dst_path || !dst_path[0])return sk_write((anon_obj_t*)s, buf, len);
    ksocket_t *dst = sk_find_named(dst_path);
    if(!dst)return -ENOENT;
    uint32_t n = (uint32_t)(len > SK_DGRAM_MAXLEN ? SK_DGRAM_MAXLEN : len);
    int r = sk_dgram_push(dst, buf, n, s->path);
    if(r)return r;
    return (int64_t)n;
}

//接收并回填来源路径
int64_t SockRecvFrom(int fd, char *buf, uint64_t len, char *from_path){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    if(s->dgram_count > 0)return sk_dgram_pop(s, buf, len, from_path);
    if(from_path)from_path[0] = 0;
    return sk_read((anon_obj_t*)s, buf, len);
}

//cmsghdr
typedef struct k_cmsghdr {
    uint64_t cmsg_len;
    int32_t  cmsg_level;
    int32_t  cmsg_type;
} k_cmsghdr_t;

#define KCMSG_ALIGN(n) (((n) + 7u) & ~7u)

//解析发送端的控制消息,构造辅助数据
static sk_anc_t *sk_anc_parse(const char *ctl, uint64_t ctllen){
    if(!ctl || ctllen < sizeof(k_cmsghdr_t))return NULL;
    sk_anc_t *a = (sk_anc_t*)PHYS_TO_VIRT(Pmm_Malloc(1));
    if(!a)return NULL;
    memset(a, 0, PAGE_SIZE);
    uint64_t off = 0;
    while(off + sizeof(k_cmsghdr_t) <= ctllen){
        k_cmsghdr_t *h = (k_cmsghdr_t*)(ctl + off);
        if(h->cmsg_len < sizeof(k_cmsghdr_t) || off + h->cmsg_len > ctllen)break;
        uint64_t plen = h->cmsg_len - sizeof(k_cmsghdr_t);
        const char *payload = ctl + off + sizeof(k_cmsghdr_t);
        if(h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS){
            int n = (int)(plen / sizeof(int32_t));
            for(int i = 0;i < n;i ++){
                int32_t ufd;
                memcpy(&ufd, payload + i * sizeof(int32_t), sizeof(ufd));
                if(sk_anc_add_fd(a, ufd) != 0)break;
            }
        }else if(h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_CREDENTIALS){
            k_ucred_t uc;
            if(plen >= sizeof(uc)){
                memcpy(&uc, payload, sizeof(uc));
                a->has_cred = true;
                a->pid = uc.pid;
                a->uid = uc.uid;
                a->gid = uc.gid;
            }
        }
        off += KCMSG_ALIGN(h->cmsg_len);
    }
    if(a->nfds == 0 && !a->has_cred){
        Pmm_Free((void*)VIRT_TO_PHYS(a), 1);
        return NULL;
    }
    return a;
}

//把辅助数据编码成控制消息写回用户
static int sk_anc_encode(sk_anc_t *a, char *ctl, uint64_t cap, uint64_t *outlen, _Bool passcred){
    if(!ctl || cap < sizeof(k_cmsghdr_t)){
        if(a)sk_anc_free(a);
        return -EMSGSIZE;
    }
    uint64_t off = 0;
    if(a && a->nfds > 0){
        uint64_t plen = (uint64_t)a->nfds * sizeof(int32_t);
        uint64_t clen = sizeof(k_cmsghdr_t) + plen;
        if(off + KCMSG_ALIGN(clen) > cap){
            sk_anc_free(a);
            return -EMSGSIZE;
        }
        int32_t fds[SK_FD_MAX];
        for(int i = 0;i < a->nfds;i ++)fds[i] = sk_fd_install(&a->fds[i]);
        k_cmsghdr_t h;
        h.cmsg_len   = clen;
        h.cmsg_level = SOL_SOCKET;
        h.cmsg_type  = SCM_RIGHTS;
        memcpy(ctl + off, &h, sizeof(h));
        memcpy(ctl + off + sizeof(h), fds, (size_t)plen);
        off += KCMSG_ALIGN(clen);
    }
    if(passcred && a && a->has_cred){
        k_ucred_t uc;
        uc.pid = a->pid;
        uc.uid = a->uid;
        uc.gid = a->gid;
        uint64_t clen = sizeof(k_cmsghdr_t) + sizeof(uc);
        if(off + KCMSG_ALIGN(clen) > cap){ sk_anc_free(a); return -EMSGSIZE; }
        k_cmsghdr_t h;
        h.cmsg_len   = clen;
        h.cmsg_level = SOL_SOCKET;
        h.cmsg_type  = SCM_CREDENTIALS;
        memcpy(ctl + off, &h, sizeof(h));
        memcpy(ctl + off + sizeof(h), &uc, sizeof(uc));
        off += KCMSG_ALIGN(clen);
    }
    if(a)sk_anc_free(a);
    *outlen = off;
    return 0;
}

//带控制消息发送
int64_t SockSendMsg(int fd, const char *buf, uint64_t len, const char *ctl, uint64_t ctllen, const char *dst_path){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    sk_anc_t *a = sk_anc_parse(ctl, ctllen);
    if(!a && ctllen >= sizeof(k_cmsghdr_t))return -EINVAL;//解析失败
    if(s->family == AF_NETLINK){
        if(a)sk_anc_free(a);
        return (int64_t)len;
    }
    if(s->type == SOCK_DGRAM){
        ksocket_t *dst = s->peer;
        if(!dst && dst_path && dst_path[0])dst = sk_find_named(dst_path);
        if(!dst && s->state == SS_CONNECTED && s->path[0])dst = sk_find_named(s->path);
        if(!dst){ if(a)sk_anc_free(a); return -EDESTADDRREQ; }
        uint32_t n = (uint32_t)(len > SK_DGRAM_MAXLEN ? SK_DGRAM_MAXLEN : len);
        if(!a && dst->passcred){
            a = (sk_anc_t*)PHYS_TO_VIRT(Pmm_Malloc(1));
            if(a)memset(a, 0, PAGE_SIZE);
        }
        if(a && dst->passcred){
            a->has_cred = true;
            a->pid = current_task ? current_task->pid : 0;
            a->uid = current_task ? current_task->cred.euid : 0;
            a->gid = current_task ? current_task->cred.egid : 0;
        }
        int r = sk_dgram_push_anc(dst, buf, n, s->path, a);
        if(r)return r;
        return (int64_t)n;
    }
    if(s->shut_rd || s->peer_eof || !s->peer){ if(a)sk_anc_free(a); return -EPIPE; }
    int64_t w = sk_write((anon_obj_t*)s, buf, len);
    if(w < 0){ if(a)sk_anc_free(a); return w; }
    if(!a && s->peer->passcred){
        a = (sk_anc_t*)PHYS_TO_VIRT(Pmm_Malloc(1));
        if(a)memset(a, 0, PAGE_SIZE);
    }
    if(a){
        if(s->peer->passcred){
            a->has_cred = true;
            a->pid = current_task ? current_task->pid : 0;
            a->uid = current_task ? current_task->cred.euid : 0;
            a->gid = current_task ? current_task->cred.egid : 0;
        }
        sk_anc_push(s->peer, a);
    }
    return w;
}

//带控制消息接收
int64_t SockRecvMsg(int fd, char *buf, uint64_t len, char *ctl, uint64_t cap, uint64_t *ctlout, char *from_path){
    ksocket_t *s = (ksocket_t*)SockFromFd(fd);
    if(!s)return -ENOTSOCK;
    if(ctlout)*ctlout = 0;
    sk_anc_t *a = NULL;
    int64_t n;
    if(s->dgram_count > 0)n = sk_dgram_pop_anc(s, buf, len, from_path, &a);
    else{
        if(from_path)from_path[0] = 0;
        a = sk_anc_pop(s);
        n = sk_read((anon_obj_t*)s, buf, len);
    }
    if(n < 0){
        if(a)sk_anc_push(s, a);
        return n;
    }
    if(ctl && cap){
        if(sk_anc_encode(a, ctl, cap, ctlout, s->passcred) != 0)return -EMSGSIZE;
    }else if(a)sk_anc_free(a);
    return n;
}
