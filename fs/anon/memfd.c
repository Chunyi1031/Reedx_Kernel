// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/anon/memfd.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 匿名内存文件(memfd): 自带缓冲区, 不依赖任何文件系统
 * 
 * 由DeepSeek V4.1 Flash生成
 */
#include <anon.h>
#include <fs.h>
#include <poll.h>
#include <syscalls.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define MEMFD_MAX_BYTES (16ULL * 1024 * 1024)//单个 memfd 上限

typedef struct memfd_obj {
    anon_obj_t hdr;
    uint8_t   *data;   //内容缓冲区
    uint64_t   size;   //当前文件大小
    uint64_t   cap;    //缓冲区容量
    uint64_t   pos;    //读写位置
} memfd_obj_t;

//把缓冲区扩到 need 字节
static int memfd_reserve(memfd_obj_t *m, uint64_t need){
    if(need <= m->cap)return 0;
    if(need > MEMFD_MAX_BYTES)return -1;
    uint64_t newcap = m->cap ? m->cap : PAGE_SIZE;
    while(newcap < need)newcap <<= 1;
    if(newcap > MEMFD_MAX_BYTES)newcap = MEMFD_MAX_BYTES;
    int np = (int)(newcap / PAGE_SIZE);
    void *pa = Pmm_Malloc(np);
    if(!pa)return -1;
    uint8_t *nd = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pa);
    memset(nd, 0, newcap);
    if(m->data){
        memcpy(nd, m->data, m->cap);
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)m->data), (int)(m->cap / PAGE_SIZE));
    }
    m->data = nd;
    m->cap  = newcap;
    return 0;
}

//读: 从当前位置开始
static int64_t memfd_read(anon_obj_t *o, char *buf, uint64_t len){
    memfd_obj_t *m = (memfd_obj_t*)o;
    if(!buf)return -EINVAL;
    if(m->pos >= m->size)return 0;
    uint64_t n = m->size - m->pos;
    if(n > len)n = len;
    memcpy(buf, m->data + m->pos, (size_t)n);
    m->pos += n;
    return (int64_t)n;
}

//写: 从当前位置开始, 越过文件尾的部分补 0
static int64_t memfd_write(anon_obj_t *o, const char *buf, uint64_t len){
    memfd_obj_t *m = (memfd_obj_t*)o;
    if(!buf)return -EINVAL;
    if(memfd_reserve(m, m->pos + len))return -ENOSPC;
    if(m->pos > m->size)memset(m->data + m->size, 0, (size_t)(m->pos - m->size));
    memcpy(m->data + m->pos, buf, (size_t)len);
    m->pos += len;
    if(m->pos > m->size)m->size = m->pos;
    return (int64_t)len;
}

//定位
static int64_t memfd_seek(anon_obj_t *o, int64_t off, int whence){
    memfd_obj_t *m = (memfd_obj_t*)o;
    int64_t base;
    if(whence == 0)base = 0;
    else if(whence == 1)base = (int64_t)m->pos;
    else if(whence == 2)base = (int64_t)m->size;
    else return -EINVAL;
    int64_t np = base + off;
    if(np < 0)return -EINVAL;
    m->pos = (uint64_t)np;
    return np;
}

//截断/扩展
static int64_t memfd_truncate(anon_obj_t *o, uint64_t len){
    memfd_obj_t *m = (memfd_obj_t*)o;
    if(len > MEMFD_MAX_BYTES)return -EINVAL;
    if(memfd_reserve(m, len))return -ENOSPC;
    if(len > m->size)memset(m->data + m->size, 0, (size_t)(len - m->size));
    m->size = len;
    if(m->pos > len)m->pos = len;
    return 0;
}

//就绪判定: 内存文件总是可读写
static short memfd_poll(anon_obj_t *o, short want){
    (void)o; (void)want;
    return (FSPOLL_IN | FSPOLL_OUT);
}

//释放
static void memfd_release(anon_obj_t *o){
    memfd_obj_t *m = (memfd_obj_t*)o;
    if(m->data){
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)m->data), (int)(m->cap / PAGE_SIZE));
        m->data = NULL;
        m->cap  = 0;
    }
}

static const anon_ops_t g_memfd_ops = {
    .read     = memfd_read,
    .write    = memfd_write,
    .poll     = memfd_poll,
    .release  = memfd_release,
    .seek     = memfd_seek,
    .truncate = memfd_truncate,
};

//创建一个 memfd, 返回文件描述符
int AnonMemFd(int flags){
    memfd_obj_t *m = (memfd_obj_t*)AnonAlloc(ANON_MEMFD, &g_memfd_ops, sizeof(memfd_obj_t));
    if(!m)return -ENOMEM;
    return AnonInstallFd(m, O_RDWR | (flags & O_CLOEXEC));
}

//取 memfd 当前大小(非 memfd 返回 0)
uint64_t AnonMemFdSize(void *obj){
    anon_obj_t *o = (anon_obj_t*)obj;
    if(!o || o->kind != ANON_MEMFD)return 0;
    return ((memfd_obj_t*)o)->size;
}
