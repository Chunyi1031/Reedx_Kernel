// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/ramfs2.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 新VFS上的内存文件系统
 * 
 * 由DeepSeek V4.1 Flash生成
 */
#include <vfs.h>
#include <mm/kslab.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <print.h>

typedef struct ramfs_inode_info {
    uint8_t          *data;      //文件内容
    uint64_t          cap;       //已分配容量
    struct list_node  children;  //目录的子dentry
} ramfs_inode_info_t;

#define RAMFS_INFO(inode) ((ramfs_inode_info_t*)(inode)->i_private)

static inline void l_add_tail(struct list_node *n, struct list_node *head){
    n->prev = head->prev;
    n->next = head;
    head->prev->next = n;
    head->prev = n;
}

static inline void l_del(struct list_node *n){
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n->next = NULL;
}

//★ 目录的 children 链用的是 dentry->d_alias 节点,
//   反查时必顶用 d_alias 的偏移(不是 d_child)
#define alias_dentry(node) ((struct dentry*)((uintptr_t)(node) - __builtin_offsetof(struct dentry, d_alias)))

//==================== 文件操作 ====================
static int64_t ramfs_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    struct inode *inode = f->f_inode;
    ramfs_inode_info_t *ri = RAMFS_INFO(inode);
    if(!ri || !ri->data)return 0;
    if(*pos >= inode->i_size)return 0;
    if(len > inode->i_size - *pos)len = inode->i_size - *pos;
    memcpy(buf, ri->data + *pos, len);
    *pos += len;
    return (int64_t)len;
}

//保证容量足够(按页翻倍), 新区域补零
static int ramfs_reserve(ramfs_inode_info_t *ri, uint64_t need){
    if(need <= ri->cap)return 0;
    uint64_t newcap = ri->cap ? ri->cap : PAGE_SIZE;
    while(newcap < need)newcap <<= 1;
    int np = (int)(newcap / PAGE_SIZE);
    void *pa = Pmm_Malloc(np);
    if(!pa)return -1;
    uint8_t *nd = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pa);
    memset(nd, 0, newcap);
    if(ri->data && ri->cap){
        memcpy(nd, ri->data, ri->cap);
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ri->data), (int)(ri->cap / PAGE_SIZE));
    }
    ri->data = nd;
    ri->cap  = newcap;
    return 0;
}

static int64_t ramfs_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    struct inode *inode = f->f_inode;
    ramfs_inode_info_t *ri = RAMFS_INFO(inode);
    if(!ri)return -EINVAL;
    uint64_t need = *pos + len;
    if(inode->i_size + len < need)return -ENOSPC;   //溢出保护
    if(ramfs_reserve(ri, need))return -ENOSPC;
    memcpy(ri->data + *pos, buf, len);
    *pos += len;
    if(*pos > inode->i_size)inode->i_size = *pos;
    return (int64_t)len;
}

typedef int (*dir_emit_t)(void *ctx, const char *name, int namelen,
                          uint64_t ino, int type, int64_t next_off);

static int ramfs_iterate(struct file *f, void *ctx, dir_emit_t emit){
    struct inode *dir = f->f_inode;
    ramfs_inode_info_t *ri = RAMFS_INFO(dir);
    if(!ri)return -ENOTDIR;
    struct list_node *n;
    int64_t off = 0;
    for(n = ri->children.next;n != &ri->children;n = n->next){
        struct dentry *d = alias_dentry(n);
        if(!d->d_inode)continue;
        int type = S_IFREG;
        if((d->d_inode->i_mode & S_IFMT) == S_IFDIR)type = S_IFDIR;
        else if((d->d_inode->i_mode & S_IFMT) == S_IFLNK)type = S_IFLNK;
        emit(ctx, d->d_name, (int)d->d_name_len, d->d_inode->i_ino, type, off ++);
    }
    return 0;
}

static const struct file_operations g_ramfs_fops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
    .iterate = ramfs_iterate,
};

//==================== inode 操作 ====================
//把新的dentry挂到本FS自己的目录children链上
static void ramfs_link_child(struct inode *dir, struct dentry *dentry){
    ramfs_inode_info_t *ri = RAMFS_INFO(dir);
    if(!ri)return;
    l_add_tail(&dentry->d_alias, &ri->children);
}

static void ramfs_unlink_child(struct inode *dir, struct dentry *dentry){
    (void)dir;
    if(dentry->d_alias.next)l_del(&dentry->d_alias);
}

static struct inode *ramfs_new_inode(struct inode *dir, uint16_t mode){
    struct super_block *sb = dir->i_sb;
    struct inode *inode = IgetLocked(sb, sb->s_ino_next ++);
    if(!inode)return NULL;
    inode->i_mode  = mode;
    inode->i_uid   = CurrentCred()->euid;
    inode->i_gid   = CurrentCred()->egid;
    inode->i_nlink = 1;
    inode->i_op    = dir->i_op;
    inode->i_fop   = &g_ramfs_fops;
    inode->i_state &= ~I_NEW;
    ramfs_inode_info_t *ri = RAMFS_INFO(inode);
    if(ri){
        ri->data = NULL;
        ri->cap  = 0;
        ri->children.prev = &ri->children;
        ri->children.next = &ri->children;
    }
    return inode;
}

static struct dentry *ramfs_lookup(struct inode *dir, struct dentry *dentry, unsigned flags){
    (void)flags;
    ramfs_inode_info_t *ri = RAMFS_INFO(dir);
    if(!ri)return NULL;
    struct list_node *n;
    for(n = ri->children.next;n != &ri->children;n = n->next){
        struct dentry *d = alias_dentry(n);
        if(!d->d_inode)continue;
        if(d->d_name_len == dentry->d_name_len &&
           memcmp(d->d_name, dentry->d_name, d->d_name_len) == 0){
            d_add(dentry, d->d_inode);
            return dentry;
        }
    }
    d_add(dentry, NULL);   //负缓存
    return NULL;
}

//create/mkdir 共用
static int ramfs_create_common(struct inode *dir, struct dentry *dentry, uint16_t mode){
    if(!dir || !dentry)return -EINVAL;
    struct inode *inode = ramfs_new_inode(dir, mode);
    if(!inode)return -ENOSPC;
    d_add(dentry, inode);
    Iput(inode);            //dentry 自己持有一份引用
    ramfs_link_child(dir, dentry);
    if((mode & S_IFMT) == S_IFDIR)dir->i_nlink ++;
    return 0;
}

static int ramfs_create(struct inode *dir, struct dentry *dentry, uint16_t mode){
    return ramfs_create_common(dir, dentry, (uint16_t)(S_IFREG | (mode & 07777)));
}

static int ramfs_mkdir(struct inode *dir, struct dentry *dentry, uint16_t mode){
    return ramfs_create_common(dir, dentry, (uint16_t)(S_IFDIR | (mode & 07777)));
}

//目录是否为空(只剩 "." 与 "..", 它们不在 children 链里)
static int ramfs_dir_empty(struct inode *dir){
    ramfs_inode_info_t *ri = RAMFS_INFO(dir);
    if(!ri)return 0;
    struct list_node *n;
    for(n = ri->children.next;n != &ri->children;n = n->next){
        if(alias_dentry(n)->d_inode)return 0;
    }
    return 1;
}

static int ramfs_unlink(struct inode *dir, struct dentry *dentry){    if(!dentry || !dentry->d_inode)return -ENOENT;
    if((dentry->d_inode->i_mode & S_IFMT) == S_IFDIR)return -EISDIR;
    ramfs_unlink_child(dir, dentry);
    if(dentry->d_inode->i_nlink)dentry->d_inode->i_nlink --;
    d_invalidate(dentry);
    return 0;
}

static int ramfs_rmdir(struct inode *dir, struct dentry *dentry){
    if(!dentry || !dentry->d_inode)return -ENOENT;
    if((dentry->d_inode->i_mode & S_IFMT) != S_IFDIR)return -ENOTDIR;
    if(!ramfs_dir_empty(dentry->d_inode))return -ENOTEMPTY;
    ramfs_unlink_child(dir, dentry);
    if(dentry->d_inode->i_nlink)dentry->d_inode->i_nlink --;
    if(dir->i_nlink > 1)dir->i_nlink --;
    d_invalidate(dentry);
    return 0;
}

//改名
static int ramfs_rename(struct inode *odir, struct dentry *odent, struct inode *ndir, struct dentry *ndent){
    if(!odent || !odent->d_inode)return -ENOENT;
    struct inode *inode = odent->d_inode;
    _Bool isdir = ((inode->i_mode & S_IFMT) == S_IFDIR);
    if(isdir)return -EOPNOTSUPP;
    if(ndent->d_inode){
        if((ndent->d_inode->i_mode & S_IFMT) == S_IFDIR)return -EISDIR;
        ramfs_unlink_child(ndir, ndent);
        if(ndent->d_inode->i_nlink)ndent->d_inode->i_nlink --;
        d_invalidate(ndent);
    }
    ramfs_unlink_child(odir, odent);
    d_invalidate(odent);
    d_add(ndent, inode);
    ramfs_link_child(ndir, ndent);
    return 0;
}

static int ramfs_getattr(const struct path *path, struct stat *st){    if(!path || !path->dentry || !path->dentry->d_inode)return -ENOENT;
    struct inode *inode = path->dentry->d_inode;
    memset(st, 0, sizeof(*st));
    st->st_ino   = inode->i_ino;
    st->st_mode  = inode->i_mode;
    st->st_nlink = inode->i_nlink;
    st->st_uid   = inode->i_uid;
    st->st_gid   = inode->i_gid;
    st->st_size  = (int64_t)inode->i_size;
    st->st_blocks= (int64_t)((inode->i_size + 511) / 512);
    st->st_blksize = 4096;
    st->st_atime = inode->i_atime_s;
    st->st_mtime = inode->i_mtime_s;
    st->st_ctime = inode->i_ctime_s;
    return 0;
}

static int ramfs_setattr(struct dentry *dentry, void *iattr){
    if(!dentry || !dentry->d_inode || !iattr)return -EINVAL;
    struct inode *inode = dentry->d_inode;
    struct iattr *ia = (struct iattr*)iattr;
    if(ia->ia_valid & ATTR_MODE)inode->i_mode = (uint16_t)((inode->i_mode & S_IFMT) | (ia->ia_mode & 07777));
    if(ia->ia_valid & ATTR_UID)inode->i_uid = ia->ia_uid;
    if(ia->ia_valid & ATTR_GID)inode->i_gid = ia->ia_gid;
    if(ia->ia_valid & ATTR_CTIME)inode->i_ctime_s = ia->ia_ctime;
    return 0;
}

//建符号链接
static int ramfs_symlink(struct inode *dir, struct dentry *dentry, const char *target){
    if(!dir || !dentry || !target)return -EINVAL;
    int len = (int)strlen(target);
    if(len == 0 || len >= NAMELEN_MAX + 1)return -ENAMETOOLONG;
    //创建符号链接inode
    struct inode *inode = ramfs_new_inode(dir, (uint16_t)(S_IFLNK | 0777));
    if(!inode)return -ENOMEM;
    //为符号链接分配空间
    ramfs_inode_info_t *ri = RAMFS_INFO(inode);
    if(!ri || ramfs_reserve(ri,(uint64_t)len + 1)){
        Iput(inode);
        return -ENOMEM;
    }
    memcpy(ri->data, target, (size_t)len + 1);//拷贝符号链接目标
    inode->i_size = (uint64_t)len;//符号链接的大小是目标字符串长度
    inode->i_fop  = NULL;//符号链接不支持读写
    ramfs_link_child(dir, dentry);//挂到目录的children链上
    d_add(dentry, inode);
    Iput(inode);
    return 0;
}

//读出符号链接的目标
static int ramfs_get_link(struct dentry *dentry, struct inode *inode, const char **target){
    (void)dentry;
    ramfs_inode_info_t *ri = inode ? RAMFS_INFO(inode) : NULL;
    if(!ri || !ri->data)return -EINVAL;
    *target = (const char*)ri->data;
    return 0;
}

static const struct inode_operations g_ramfs_iops = {
    .lookup   = ramfs_lookup,
    .create   = ramfs_create,
    .mkdir    = ramfs_mkdir,
    .unlink   = ramfs_unlink,
    .rmdir    = ramfs_rmdir,
    .symlink  = ramfs_symlink,
    .rename   = ramfs_rename,
    .get_link = ramfs_get_link,
    .getattr  = ramfs_getattr,
    .setattr  = ramfs_setattr,
};

//i_nlink 归零: 释放数据
static void ramfs_evict_inode(struct inode *inode){
    ramfs_inode_info_t *ri = RAMFS_INFO(inode);
    if(!ri)return;
    if(ri->data && ri->cap){
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ri->data), (int)(ri->cap / PAGE_SIZE));
        ri->data = NULL;
        ri->cap  = 0;
    }
}

static const struct super_operations g_ramfs_sop = {
    .evict_inode = ramfs_evict_inode,
};

//==================== 挂载 ====================
uint32_t Ramfs2InodeSize(void){
    return (uint32_t)(sizeof(struct inode) + sizeof(ramfs_inode_info_t));
}

const struct super_operations *Ramfs2SuperOps(void){ return &g_ramfs_sop; }

/**
 * 建立 ramfs 的超级块与根目录
 * @return 根 dentry(失败返回 NULL)
 */
struct dentry *Ramfs2FillSuper(struct super_block *sb){
    if(!sb)return NULL;
    sb->s_ino_next = 2;      //1 号留给根目录
    struct inode *root = IgetLocked(sb, 1);
    if(!root)return NULL;
    root->i_mode  = (uint16_t)(S_IFDIR | 0755);
    root->i_uid   = 0;
    root->i_gid   = 0;
    root->i_nlink = 2;       //"." 与父目录的引用
    root->i_op    = &g_ramfs_iops;
    root->i_fop   = &g_ramfs_fops;
    root->i_state &= ~I_NEW;
    ramfs_inode_info_t *ri = RAMFS_INFO(root);
    if(!ri)return NULL;
    ri->data = NULL;
    ri->cap  = 0;
    ri->children.prev = &ri->children;
    ri->children.next = &ri->children;

    struct dentry *d = d_alloc(NULL, "/");
    if(!d)return NULL;
    d_add(d, root);
    Iput(root);                        //dentry 持有引用
    sb->s_root = d;
    sb->s_root_inode = root;
    return d;
}
