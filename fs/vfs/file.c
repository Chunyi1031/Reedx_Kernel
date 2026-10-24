// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/vfs/file.c
 * 
 * Copyright (C) 2026 Liu Chunyi
 * 
 * 打开文件描述符操作
 */
#include <vfs.h>
#include <mm/kslab.h>
#include <print.h>

static struct kmem_cache *g_file_cache;//文件对象缓存
static spinlock_t g_file_lock = {0};
static uint32_t g_nr_files;//当前打开文件数量

static void file_init(void){
    if(!g_file_cache)g_file_cache = KslabCreate("file", sizeof(struct file));
}

//新建一个打开文件描述
struct file *FileAlloc(void){
    file_init();
    if(!g_file_cache)return NULL;
    struct file *f = (struct file*)KslabZalloc(g_file_cache);//分配描述符
    if(!f)return NULL;
    f->f_count = 1;
    uint64_t flags;
    spin_lock_irqsave(&g_file_lock, flags);
    g_nr_files ++;
    spin_unlock_irqrestore(&g_file_lock, flags);
    return f;
}

//增加引用计数
struct file *FileGet(struct file *f){
    if(!f)return NULL;
    uint64_t flags;
    spin_lock_irqsave(&g_file_lock, flags);
    f->f_count ++;
    spin_unlock_irqrestore(&g_file_lock, flags);
    return f;
}

//减少引用计数,归零则释放
void FilePut(struct file *f){
    if(!f)return;
    uint64_t flags;
    spin_lock_irqsave(&g_file_lock, flags);
    if(!f->f_count){
        spin_unlock_irqrestore(&g_file_lock, flags);
        printk(PRINTK_ERR"vfs: FilePut on a file with zero count");
        return;
    }
    f->f_count --;//减少
    int dead = (f->f_count == 0);
    if(dead && g_nr_files)g_nr_files --;
    spin_unlock_irqrestore(&g_file_lock, flags);
    if(!dead)return;
    //最后一个引用消失，归还打开时占用的目录项
    if(f->f_path.dentry){
        struct dentry *d = f->f_path.dentry;
        f->f_path.dentry = NULL;
        dput(d);
    }
    file_init();
    if(g_file_cache)KslabFree(g_file_cache, f);
}

uint32_t FileCount(void){
    return g_nr_files;
}
