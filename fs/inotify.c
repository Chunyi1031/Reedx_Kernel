// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/inotify.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * inotify实例
 */
#include <inotify.h>
#include <anon.h>
#include <poll.h>
#include <fs.h>
#include <task.h>
#include <syscalls.h>
#include <kstring.h>
#include <print.h>

//一条已排队的事件
typedef struct ino_event {
    int32_t  wd;                    //监视项的wd
    uint32_t mask;                  //事件掩码
    uint32_t cookie;                //关联cookie
    uint32_t len;                   //name长度
    char     name[INOTIFY_NAME_MAX];//事件名(不包含路径, 以 '\0' 结尾)
} ino_event_t;

//一个监视项
typedef struct ino_watch {
    _Bool    used;      //是否使用
    int      wd;        //监视项的wd
    uint32_t mask;      //监视的事件掩码
    char     path[256]; //被监视的路径
} ino_watch_t;

//一个inotify实例
typedef struct inotify_obj {
    anon_obj_t   hdr;                       //匿名对象头
    int          next_wd;                   //下一个监视项的wd
    ino_event_t  events[INOTIFY_EVENT_MAX]; //事件环形队列
    int          ev_head;                   //事件队列头索引
    int          ev_count;                  //事件队列中事件个数
    ino_watch_t  watches[INOTIFY_WATCH_MAX];//监视项数组
    struct inotify_obj *next;               //全局实例链
} inotify_obj_t;

static inotify_obj_t *g_inotify_list = NULL;

//取字符串的末段名字
static const char *inotify_basename(const char *path){
    const char *b = path;
    for(const char *p = path;*p;p ++){
        if(*p == '/' && p[1])b = p + 1;
    }
    return b;
}

//child否是dir本身或其直接子项
static _Bool inotify_match(const char *dir, const char *child){
    int n = (int)strlen(dir);
    if(n == 0)return false;
    if(strncmp(dir,child,(uint64_t)n) != 0)return false;
    if(child[n] == 0)return true;
    const char *rest = (dir[n - 1] == '/') ? (child + n) : (child + n + 1);
    if(dir[n - 1] != '/' && child[n] != '/')return false;
    if(!rest[0])return false;
    return strchr(rest, '/') == NULL;
}

//事件入队
static void inotify_push(inotify_obj_t *ino, int wd, uint32_t mask, uint32_t cookie, const char *name){
    //队列满时丢弃最旧的事件
    if(ino->ev_count >= INOTIFY_EVENT_MAX){
        ino->ev_head = (ino->ev_head + 1) % INOTIFY_EVENT_MAX;
        ino->ev_count --;
    }
    //入队新事件
    int idx = (ino->ev_head + ino->ev_count) % INOTIFY_EVENT_MAX;
    ino_event_t *e = &ino->events[idx];
    //初始化
    memset(e, 0, sizeof(*e));
    e->wd = wd;
    e->mask = mask;
    e->cookie = cookie;
    //复制name
    if(name && name[0]){
        int n = (int)strlen(name);
        if(n > INOTIFY_NAME_MAX - 1)n = INOTIFY_NAME_MAX - 1;
        memcpy(e->name, name, (uint64_t)n);
        e->name[n] = 0;
        e->len = (uint32_t)((n + 1 + 3) & ~3);
    }
    ino->ev_count ++;
}

static short ino_poll(anon_obj_t *o, short want){
    (void)want;
    return ((inotify_obj_t*)o)->ev_count > 0 ? FSPOLL_IN : 0;
}

//读事件
static int64_t ino_read(anon_obj_t *o, char *buf, uint64_t len){
    inotify_obj_t *ino = (inotify_obj_t*)o;
    if(ino->ev_count == 0)return -EAGAIN;
    uint64_t off = 0;
    while(ino->ev_count > 0){
        ino_event_t *e = &ino->events[ino->ev_head];
        uint64_t need = sizeof(kinotify_event_t) + e->len;
        if(off + need > len)break;
        kinotify_event_t h;
        h.wd = e->wd;
        h.mask = e->mask;
        h.cookie = e->cookie;
        h.len = e->len;
        memcpy((buf + off),&h,sizeof(h));
        off += sizeof(h);
        if(e->len){
            memcpy(buf + off,e->name,e->len);
            off += e->len;
        }
        ino->ev_head = (ino->ev_head + 1) % INOTIFY_EVENT_MAX;
        ino->ev_count --;
    }
    if(off == 0)return -EINVAL;
    return (int64_t)off;
}

//释放时从全局链摘掉
static void ino_release(anon_obj_t *o){
    inotify_obj_t **pp = &g_inotify_list;
    while(*pp){
        if(*pp == (inotify_obj_t*)o){
            *pp = ((inotify_obj_t*)o)->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static const anon_ops_t g_ino_ops = {.read = ino_read,.poll = ino_poll,.release = ino_release };

//fd对应的实例
static inotify_obj_t *ino_from_fd(int fd){
    anon_obj_t *o = (anon_obj_t*)AnonFromFd(fd);
    if(!o || o->kind != ANON_INOTIFY)return NULL;
    return (inotify_obj_t*)o;
}

//创建inotify实例
int AnonInotifyInit(int flags){
    //分配
    inotify_obj_t *ino = (inotify_obj_t*)AnonAlloc(ANON_INOTIFY,&g_ino_ops,sizeof(*ino));
    if(!ino)return -ENOMEM;
    //初始化
    ino->next_wd = 1;
    ino->next = g_inotify_list;
    g_inotify_list = ino;
    int fd = AnonInstallFd(ino,(flags & 1));//挂到fd
    return fd;
}

//添加/更新监视项
int AnonInotifyAddWatch(int fd, const char *path, uint32_t mask){
    inotify_obj_t *ino = ino_from_fd(fd);
    if(!ino || !path || !path[0])return -EINVAL;
    if(strlen(path) > 255)return -ENAMETOOLONG;
    //同一路径重复添加,更新掩码
    for(int i = 0;i < INOTIFY_WATCH_MAX;i ++){
        ino_watch_t *w = &ino->watches[i];
        if(w->used && strcmp(w->path, path) == 0){
            if(mask & IN_MASK_ADD)w->mask |= (mask & IN_ALL_EVENTS);
            else w->mask  = (mask & IN_ALL_EVENTS);
            return w->wd;
        }
    }
    //找空闲监视项
    for(int i = 0;i < INOTIFY_WATCH_MAX;i ++){
        ino_watch_t *w = &ino->watches[i];
        if(w->used)continue;
        w->used = true;
        w->wd   = ino->next_wd ++;
        w->mask = mask & IN_ALL_EVENTS;
        int n = (int)strlen(path);
        memcpy(w->path, path, (uint64_t)n);
        w->path[n] = 0;
        if(w->mask == 0)w->mask = IN_ALL_EVENTS;
        return w->wd;
    }
    return -ENOSPC;
}

//删除监视项
int AnonInotifyRmWatch(int fd, int wd){
    inotify_obj_t *ino = ino_from_fd(fd);
    if(!ino)return -EINVAL;
    for(int i = 0;i < INOTIFY_WATCH_MAX;i ++){
        if(ino->watches[i].used && ino->watches[i].wd == wd){
            ino->watches[i].used = false;
            return 0;
        }
    }
    return -EINVAL;
}

//通知事件发生
void InotifyNotify(const char *path, uint32_t mask){
    if(!path || !mask || !g_inotify_list)return;
    _Bool woke = false;
    for(inotify_obj_t *ino = g_inotify_list;ino;ino = ino->next){
        for(int i = 0;i < INOTIFY_WATCH_MAX;i ++){
            ino_watch_t *w = &ino->watches[i];
            if(!w->used)continue;
            if(!(w->mask & mask))continue;
            if(!inotify_match(w->path, path))continue;
            const char *name = (strcmp(w->path, path) == 0) ? "" : inotify_basename(path);
            inotify_push(ino, w->wd, mask, 0, name);
            woke = true;
        }
    }
    if(woke)PollWakeAll();
}
