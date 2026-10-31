// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/devfs.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * devtmpfs
 * 
 * 由DeepSeek V4.1 Flash生成
 */
#include <vfs.h>
#include <fs.h>
#include <drives/cdev.h>

#define DEVFS_MAX_NODES 32//最多记多少个节点

//一个设备节点
typedef struct devfs_node {
    char     name[NAMELEN_MAX + 1];//节点名
    uint16_t mode;                 //类型位 + 权限位
    uint32_t dev;                  //主/次设备号
    _Bool    used;                 //这个槽位有没有用
} devfs_node_t;

//开机就准备好的节点
static const struct {
    const char *name;
    uint16_t    mode;
    uint32_t    dev;
} g_devfs_predef[] = {
    { "console", S_IFCHR | 0600, MKDEV(5, 1) },
    { "tty",     S_IFCHR | 0666, MKDEV(5, 0) },
    { "null",    S_IFCHR | 0666, MKDEV(1, 3) },
    { "zero",    S_IFCHR | 0666, MKDEV(1, 5) },
    { "ttyS0",   S_IFCHR | 0600, MKDEV(4, 64) },
};
#define DEVFS_NPREDEF ((int)(sizeof(g_devfs_predef) / sizeof(g_devfs_predef[0])))

static devfs_node_t g_devfs_nodes[DEVFS_MAX_NODES];
static int          g_devfs_count;//已经占用的节点数

#define DEVFS_INO_ROOT 1
#define DEVFS_INO(idx) ((uint64_t)((idx) + 2))

//按名字找节点
static int devfs_find(const char *name, uint32_t namelen){
    for(int i = 0;i < g_devfs_count;i ++){
        if(!g_devfs_nodes[i].used)continue;
        const char *n = g_devfs_nodes[i].name;
        if(strlen(n) != namelen)continue;
        if(memcmp(n, name, namelen) == 0)return i;
    }
    return -1;
}

//按节点表建inode
static struct inode *devfs_iget(struct super_block *sb, int idx){
    if(idx < 0 || idx >= g_devfs_count)return NULL;
    devfs_node_t *n = &g_devfs_nodes[idx];
    struct inode *inode = IgetLocked(sb, DEVFS_INO(idx));
    if(!inode)return NULL;
    inode->i_mode  = n->mode;
    inode->i_uid   = 0;
    inode->i_gid   = 0;
    inode->i_nlink = 1;
    inode->i_size  = 0;
    inode->i_rdev  = n->dev;
    inode->i_op    = NULL;//设备节点里没有别的东西可查
    inode->i_fop   = NULL;//打开时按主/次设备号去 cdev 表找驱动
    inode->i_state &= ~I_NEW;
    return inode;
}

//查一个名字: 命中就建出 inode
static struct dentry *devfs_lookup(struct inode *dir, struct dentry *dentry, unsigned flags){
    (void)flags;
    int idx = devfs_find(dentry->d_name, dentry->d_name_len);
    if(idx < 0){
        d_add(dentry, NULL);//负缓存
        return NULL;
    }
    struct inode *inode = devfs_iget(dir->i_sb, idx);
    if(!inode){
        d_add(dentry, NULL);
        return NULL;
    }
    d_add(dentry, inode);
    Iput(inode);//d_add 已经替 dentry 拿了一份引用
    return dentry;
}

//遍历目录: 把节点表里所有名字报上去
static int devfs_iterate(struct file *f, void *ctx,
                         int (*emit)(void *ctx, const char *name, int namelen,
                                     uint64_t ino, int type, int64_t next_off)){
    if(!f || !f->f_inode || !emit)return -EINVAL;
    for(int i = 0;i < g_devfs_count;i ++){
        if(!g_devfs_nodes[i].used)continue;
        const char *n = g_devfs_nodes[i].name;
        int len = (int)strlen(n);
        emit(ctx, n, len, DEVFS_INO(i), (int)(g_devfs_nodes[i].mode & S_IFMT), i);
    }
    return 0;
}

//建一个设备节点
static int devfs_mknod(struct inode *dir, struct dentry *dentry, uint16_t mode, uint64_t rdev){
    if(!dir || !dentry)return -EINVAL;
    if(g_devfs_count >= DEVFS_MAX_NODES)return -ENOSPC;
    int len = (int)dentry->d_name_len;
    if(len <= 0 || len > NAMELEN_MAX)return -ENAMETOOLONG;
    if(devfs_find(dentry->d_name, dentry->d_name_len) >= 0)return -EEXIST;
    int idx = g_devfs_count;
    devfs_node_t *n = &g_devfs_nodes[idx];
    memset(n, 0, sizeof(*n));
    memcpy(n->name, dentry->d_name, (size_t)len);
    n->mode = (uint16_t)(mode & (S_IFMT | 07777));
    n->dev  = (uint32_t)rdev;
    n->used = true;
    g_devfs_count ++;
    struct inode *inode = devfs_iget(dir->i_sb, idx);
    if(!inode)return -EIO;
    d_add(dentry, inode);
    Iput(inode);
    return 0;
}

static const struct inode_operations g_devfs_dir_iops = {
    .lookup = devfs_lookup,
    .mknod  = devfs_mknod,
};

static const struct file_operations g_devfs_dir_fops = {
    .iterate = devfs_iterate,
};

//==================== 挂载 ====================

uint32_t DevFsInodeSize(void){
    return (uint32_t)sizeof(struct inode);
}

const struct super_operations *DevFsSuperOps(void){
    return NULL;//没有需要随卸载回收的东西
}

//建立 devtmpfs 的超级块与根目录
struct dentry *DevFsFillSuper(struct super_block *sb){
    if(!sb)return NULL;
    //先把预置节点填进表里
    g_devfs_count = 0;
    memset(g_devfs_nodes, 0, sizeof(g_devfs_nodes));
    for(int i = 0;i < DEVFS_NPREDEF && i < DEVFS_MAX_NODES;i ++){
        devfs_node_t *n = &g_devfs_nodes[i];
        int len = (int)strlen(g_devfs_predef[i].name);
        memcpy(n->name, g_devfs_predef[i].name, (size_t)len);
        n->mode = g_devfs_predef[i].mode;
        n->dev  = g_devfs_predef[i].dev;
        n->used = true;
        g_devfs_count ++;
    }
    //根目录 inode
    struct inode *root = IgetLocked(sb, DEVFS_INO_ROOT);
    if(!root)return NULL;
    root->i_mode  = (uint16_t)(S_IFDIR | 0755);
    root->i_uid   = 0;
    root->i_gid   = 0;
    root->i_nlink = 2;
    root->i_op    = &g_devfs_dir_iops;
    root->i_fop   = &g_devfs_dir_fops;
    root->i_state &= ~I_NEW;

    struct dentry *d = d_alloc(NULL, "/");
    if(!d)return NULL;
    d_add(d, root);
    Iput(root);//dentry 持有引用
    sb->s_root = d;
    sb->s_root_inode = root;
    return d;
}
