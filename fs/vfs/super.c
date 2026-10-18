// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/vfs/super.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * super_block的分配/注册
 * 由DeepSeek V4.1 Flash生成
 */
#include <vfs.h>
#include <mm/kslab.h>
#include <print.h>

static struct list_node g_sb_list;
static int               g_nr_sb;
static int               g_vfs_ready;

void FsVfsInit(void){
    if(g_vfs_ready)return;
    g_sb_list.prev = &g_sb_list;
    g_sb_list.next = &g_sb_list;
    g_nr_sb = 0;
    KslabInit();
    g_vfs_ready = 1;
}

struct super_block *SuperAlloc(const char *type_name, uint64_t magic,
                               uint32_t inode_size, const struct super_operations *op){
    FsVfsInit();
    if(!inode_size)inode_size = sizeof(struct inode);
    struct kmem_cache *ic = KslabCreate(type_name ? type_name : "inode", inode_size);
    if(!ic){
        printk(PRINTK_ERR"vfs:Unable to create inode cache for%s.", type_name ? type_name : "?");
        return NULL;
    }
    struct kmem_cache *sc = KslabCreate("super_block", sizeof(struct super_block));
    if(!sc)return NULL;
    struct super_block *sb = (struct super_block*)KslabZalloc(sc);
    if(!sb)return NULL;
    int i = 0;
    if(type_name)for(;type_name[i] && i < (int)sizeof(sb->s_type_name) - 1;i ++)
        sb->s_type_name[i] = type_name[i];
    sb->s_magic       = magic;
    sb->s_inode_size  = inode_size;
    sb->s_inode_cache = ic;
    sb->s_op          = op;
    sb->s_blocksize   = 4096;
    sb->s_maxbytes    = 0x7FFFFFFFFFFFFFFFULL;
    sb->s_inodes.prev = &sb->s_inodes;
    sb->s_inodes.next = &sb->s_inodes;
    spin_lock_init(&sb->s_lock);
    sb->s_list.prev = g_sb_list.prev;
    sb->s_list.next = &g_sb_list;
    g_sb_list.prev->next = &sb->s_list;
    g_sb_list.prev = &sb->s_list;
    g_nr_sb ++;
    return sb;
}

void SuperFree(struct super_block *sb){
    if(!sb)return;
    //从全局链上摘下来
    sb->s_list.prev->next = sb->s_list.next;
    sb->s_list.next->prev = sb->s_list.prev;
    if(g_nr_sb)g_nr_sb --;
    struct kmem_cache *sc = KslabCreate("super_block", sizeof(struct super_block));
    if(sc)KslabFree(sc, sb);
}

int SuperCount(void){
    return g_nr_sb;
}
