// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/fat/fat_vfs.c
 * 
 * Copyright (C) 2026 Liu Chunyi
 * 
 * FAT32文件系统在新VFS框架下的适配层
 * 
 * 由DeepSeek V4.1 Flash生成
 */
#include <vfs.h>
#include <fs.h>
#include <print.h>

//FAT32 在新 VFS 上的适配: 每个 inode 包一个旧路径的 fs_node
typedef struct fat_vfs_inode {
    fs_node_t *node;
} fat_vfs_inode_t;

#define FI(inode) ((fat_vfs_inode_t*)(inode)->i_private)

static uint16_t fat_mode_of(fs_node_t *n){
    return (n->type == FT_DIR) ? (uint16_t)(S_IFDIR | 0755) : (uint16_t)(S_IFREG | 0755);
}

//把旧节点包成 inode(已存在则复用)
static struct inode *fat_wrap(struct inode *dir, fs_node_t *n){
    if(!dir || !n)return NULL;
    struct inode *i = IgetLocked(dir->i_sb, n->ino);
    if(!i)return NULL;
    if(i->i_state & I_NEW){
        fat_vfs_inode_t *fi = FI(i);
        if(!fi){
            Iput(i);
            return NULL;
        }
        fi->node    = n;
        i->i_mode   = fat_mode_of(n);
        i->i_size   = n->size;
        i->i_nlink  = (n->type == FT_DIR) ? 2 : 1;
        i->i_op     = dir->i_op;
        i->i_fop    = dir->i_fop;
        i->i_state &= ~I_NEW;
    }
    return i;
}

//绑定到 dentry(dentry 持有一份 inode 引用)
static int fat_bind(struct inode *dir, struct dentry *dentry, fs_node_t *n){
    struct inode *i = fat_wrap(dir, n);
    if(!i)return -ENOMEM;
    d_add(dentry, i);
    Iput(i);
    return 0;
}

//==================== inode 操作 ====================
static struct dentry *fat_vfs_lookup(struct inode *dir, struct dentry *dentry, unsigned flags){
    (void)flags;
    fat_vfs_inode_t *di = FI(dir);
    if(!di || !di->node || !di->node->ops->lookup)return NULL;
    fs_node_t *n = di->node->ops->lookup(di->node, dentry->d_name);
    if(!n){
        d_add(dentry, NULL);//负缓存
        return NULL;
    }
    if(fat_bind(dir, dentry, n))d_add(dentry, NULL);
    return dentry;
}

static int fat_vfs_create(struct inode *dir, struct dentry *dentry, uint16_t mode){
    (void)mode;
    fat_vfs_inode_t *di = FI(dir);
    if(!di || !di->node || !di->node->ops->create)return -EROFS;
    fs_node_t *n = di->node->ops->create(di->node, dentry->d_name, FT_FILE);
    if(!n)return -ENOSPC;
    return fat_bind(dir, dentry, n);
}

static int fat_vfs_mkdir(struct inode *dir, struct dentry *dentry, uint16_t mode){
    (void)mode;
    fat_vfs_inode_t *di = FI(dir);
    if(!di || !di->node || !di->node->ops->mkdir)return -EROFS;
    int r = di->node->ops->mkdir(di->node, dentry->d_name, 0755);
    if(r)return r;
    fs_node_t *n = di->node->ops->lookup ? di->node->ops->lookup(di->node, dentry->d_name) : NULL;
    if(!n)return -ENOENT;
    return fat_bind(dir, dentry, n);
}

static int fat_vfs_unlink(struct inode *dir, struct dentry *dentry){
    fat_vfs_inode_t *di = FI(dir);
    if(!di || !di->node || !di->node->ops->unlink)return -EROFS;
    if(dentry->d_inode && (dentry->d_inode->i_mode & S_IFMT) == S_IFDIR)return -EISDIR;
    int r = di->node->ops->unlink(di->node, dentry->d_name);
    if(!r)d_invalidate(dentry);
    return r;
}

static int fat_vfs_rmdir(struct inode *dir, struct dentry *dentry){
    fat_vfs_inode_t *di = FI(dir);
    if(!di || !di->node || !di->node->ops->unlink)return -EROFS;
    int r = di->node->ops->unlink(di->node, dentry->d_name);
    if(!r)d_invalidate(dentry);
    return r;
}

static int fat_vfs_getattr(const struct path *path, struct stat *st){
    if(!path || !path->dentry || !path->dentry->d_inode)return -ENOENT;
    struct inode *i = path->dentry->d_inode;
    fat_vfs_inode_t *fi = FI(i);
    if(!fi || !fi->node)return -ENOENT;
    memset(st, 0, sizeof(*st));
    st->st_ino   = i->i_ino;
    st->st_mode  = i->i_mode;
    st->st_nlink = i->i_nlink;
    st->st_uid   = i->i_uid;
    st->st_gid   = i->i_gid;
    st->st_size  = (int64_t)fi->node->size;
    st->st_blocks= (int64_t)((fi->node->size + 511) / 512);
    st->st_blksize = 4096;
    st->st_atime = (int64_t)fi->node->atime;
    st->st_mtime = (int64_t)fi->node->mtime;
    st->st_ctime = (int64_t)fi->node->ctime;
    return 0;
}

static const struct inode_operations g_fat_iops = {
    .lookup  = fat_vfs_lookup,
    .create  = fat_vfs_create,
    .mkdir   = fat_vfs_mkdir,
    .unlink  = fat_vfs_unlink,
    .rmdir   = fat_vfs_rmdir,
    .getattr = fat_vfs_getattr,
};

//==================== 文件操作 ====================
static int64_t fat_vfs_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    fat_vfs_inode_t *fi = f ? FI(f->f_inode) : NULL;
    if(!fi || !fi->node || !fi->node->ops->read)return -EIO;
    uint64_t n = fi->node->ops->read(fi->node, *pos, buf, len);
    *pos += n;
    return (int64_t)n;
}

static int64_t fat_vfs_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    fat_vfs_inode_t *fi = f ? FI(f->f_inode) : NULL;
    if(!fi || !fi->node || !fi->node->ops->write)return -EIO;
    uint64_t n = fi->node->ops->write(fi->node, *pos, buf, len);
    *pos += n;
    fi->node->size = (*pos > fi->node->size) ? *pos : fi->node->size;
    f->f_inode->i_size = fi->node->size;
    return (int64_t)n;
}

typedef int (*dir_emit_t)(void *ctx, const char *name, int namelen,
                          uint64_t ino, int type, int64_t next_off);

static int fat_vfs_iterate(struct file *f, void *ctx, dir_emit_t emit){
    fat_vfs_inode_t *fi = f ? FI(f->f_inode) : NULL;
    if(!fi || !fi->node || !fi->node->ops->readdir)return -ENOTDIR;
    uint64_t cookie = 0;
    fs_dirent_t de;
    while(1){
        memset(&de, 0, sizeof(de));
        if(fi->node->ops->readdir(fi->node, &cookie, &de) != 0)break;
        emit(ctx, de.name, (int)strlen(de.name), de.ino,
             de.type == FT_DIR ? S_IFDIR : S_IFREG, (int64_t)cookie);
    }
    return 0;
}

static const struct file_operations g_fat_fops = {
    .read    = fat_vfs_read,
    .write   = fat_vfs_write,
    .iterate = fat_vfs_iterate,
};

//==================== 超级块 ====================
static void fat_vfs_evict_inode(struct inode *inode){
    fat_vfs_inode_t *fi = FI(inode);
    if(fi)fi->node = NULL;   //节点由 FAT 自己的缓存管理
}

static const struct super_operations g_fat_sop = {
    .evict_inode = fat_vfs_evict_inode,
};

uint32_t FatVfsInodeSize(void){
    return (uint32_t)(sizeof(struct inode) + sizeof(fat_vfs_inode_t));
}

const struct super_operations *FatVfsSuperOps(void){ return &g_fat_sop; }

//建立 FAT32 的根 inode 与根 dentry
struct dentry *FatVfsFillSuper(struct super_block *sb){
    if(!sb)return NULL;
    fs_node_t *rn = fat32_root();
    if(!rn)return NULL;
    struct inode *root = IgetLocked(sb, rn->ino);
    if(!root)return NULL;
    fat_vfs_inode_t *fi = FI(root);
    if(!fi){ Iput(root); return NULL; }
    fi->node      = rn;
    root->i_mode  = fat_mode_of(rn);
    root->i_size  = rn->size;
    root->i_nlink = 2;
    root->i_op    = &g_fat_iops;
    root->i_fop   = &g_fat_fops;
    root->i_state &= ~I_NEW;

    struct dentry *d = d_alloc(NULL, "/");
    if(!d){ Iput(root); return NULL; }
    d_add(d, root);
    Iput(root);
    sb->s_root = d;
    sb->s_root_inode = root;
    return d;
}
