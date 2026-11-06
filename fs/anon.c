// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/anon.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 定时器中断里推进所有timerfd
 */
#include <anon.h>
#include <fs.h>
#include <task.h>
#include <poll.h>
#include <syscalls.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define anon_pages(size) (((size) + PAGE_SIZE - 1) / PAGE_SIZE) //对象页数

//分配并清零一个匿名对象
void *AnonAlloc(int kind, const anon_ops_t *ops, uint64_t size){
    //分配
    int pages = anon_pages(size);
    anon_obj_t *o = (anon_obj_t*)PHYS_TO_VIRT(Pmm_Malloc(pages));
    if(!o)return NULL;
    //清0并初始化
    memset(o, 0, (uint64_t)pages * PAGE_SIZE);
    o->kind = kind;
    o->refs = 1;
    o->size = (uint32_t)size;
    o->ops = ops;
    return o;
}

//引用计数+1
void AnonGet(void *obj){
    anon_obj_t *o = (anon_obj_t*)obj;
    if(o)o->refs ++;
}

//引用计数-1,归零时释放
void AnonPut(void *obj){
    anon_obj_t *o = (anon_obj_t*)obj;
    if(!o)return;
    //>1,-1
    if(o->refs > 1){
        o->refs --;
        return;
    }
    //归0,释放
    if(o->ops && o->ops->release)o->ops->release(o);
    Pmm_Free((void*)VIRT_TO_PHYS(o),anon_pages(o->size));
}

//把对象挂到一个新的fd上
int AnonInstallFd(void *obj, int flags){
    if(!obj)return -EBADF;
    if(!current_task){
        AnonPut(obj);
        return -EBADF;
    }
    //分配描述符
    int fd = -1;
    for(int i = 3;i < MAX_FD;i ++){
        if(!current_task->files[i].used){
            fd = i;
            break;
        }
    }
    if(fd < 0){
        AnonPut(obj);
        return -EMFILE;
    }
    //初始化描述符
    memset(&current_task->files[fd], 0, sizeof(fs_file_t));
    current_task->files[fd].used = true;
    current_task->files[fd].flags = flags;
    current_task->files[fd].anon = obj;
    return fd;
}

//取fd对应的匿名对象
void *AnonFromFd(long fd){
    if(!current_task)return NULL;
    if((fd < 0) || (fd >= MAX_FD))return NULL;
    if(!current_task->files[fd].used)return NULL;
    return current_task->files[fd].anon;
}

//读
int64_t AnonRead(void *obj, char *buf, uint64_t len){
    anon_obj_t *o = (anon_obj_t*)obj;
    if(!o || !buf)return -EINVAL;
    if(!o->ops || !o->ops->read)return -EINVAL;
    return o->ops->read(o,buf,len);
}

//写
int64_t AnonWrite(void *obj, const char *buf, uint64_t len){
    anon_obj_t *o = (anon_obj_t*)obj;
    if(!o || !buf)return -EINVAL;
    if(!o->ops || !o->ops->write)return -EINVAL;
    return o->ops->write(o, buf, len);
}

//就绪判定
short AnonPoll(void *obj, short want){
    anon_obj_t *o = (anon_obj_t*)obj;
    if(!o)return 0;
    if(!o->ops || !o->ops->poll)return (FSPOLL_IN | FSPOLL_OUT);
    return o->ops->poll(o, want);
}
