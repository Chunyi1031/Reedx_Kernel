// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/vfs.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 由DeepSeek V4.1 Flash生成
 */
#ifndef _VFS_H_
#define _VFS_H_

#include <klib.h>
#include <syscalls.h>

struct super_block;
struct inode;
struct dentry;
struct vfsmount;
struct file;
struct address_space;
struct bdev;

/* ---------------- 通用 ---------------- */
struct path {
    struct vfsmount *mnt;
    struct dentry   *dentry;
};

#define NAMELEN_MAX      255
#define PATHLEN_MAX      4096
#define MAXSYMLINKS      40

/* ---------------- inode 状态位 ---------------- */
#define I_NEW        0x00000001u   //正在从磁盘读取
#define I_DIRTY      0x00000002u   //元数据脏
#define I_FREEING    0x00000004u   //正在被释放
#define I_WILL_FREE  0x00000008u
#define I_DONTCACHE  0x00000010u

/* ---------------- dentry 标志 ---------------- */
#define DCACHE_NEGATIVE 0x00000001u //名字不存在(负缓存)
#define DCACHE_MOUNTED  0x00000002u //此目录上有挂载点
#define DCACHE_NEW      0x00000004u //正在构造

/* ---------------- super_block 标志 ---------------- */
#define MS_RDONLY  0x00000001u
#define MS_NOSUID  0x00000002u
#define MS_NODEV   0x00000004u
#define MS_NOATIME 0x00000008u
#define MS_SYNCHRONOUS 0x00000010u
#define MS_NOEXEC  0x00000020u

/* ---------------- 权限位(namei 用) ---------------- */
#define MAY_EXEC  0x01
#define MAY_WRITE 0x02
#define MAY_READ  0x04

/* ---------------- path_lookup 标志 ---------------- */
#define LOOKUP_FOLLOW     0x0001  //末段跟随符号链接
#define LOOKUP_DIRECTORY  0x0002  //末段必须是目录
#define LOOKUP_PARENT     0x0004  //返回父目录(create/unlink 用)
#define LOOKUP_NOFOLLOW   0x0008
#define LOOKUP_EMPTY      0x0010  //AT_EMPTY_PATH: 空路径取 dirfd 自身
#define LOOKUP_CREATE     0x0020

/* ================= super_block ================= */
struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);
    void          (*destroy_inode)(struct inode *inode);
    void          (*read_inode)(struct inode *inode);
    int           (*write_inode)(struct inode *inode, int wait);
    void          (*dirty_inode)(struct inode *inode);
    void          (*evict_inode)(struct inode *inode);   //i_nlink==0 时释放磁盘资源
    int           (*sync_fs)(struct super_block *sb, int wait);
    int           (*statfs)(struct super_block *sb, void *kstatfs);
};

struct super_block {
    struct list_node   s_list;         //全局 sb 链
    char               s_type_name[16];//"ext4"/"vfat"/"tmpfs"
    uint64_t           s_magic;
    int                s_dev;          //设备号(0=匿名)
    uint32_t           s_blocksize;
    uint64_t           s_maxbytes;
    uint32_t           s_inode_size;   //含 FS 私有数据
    struct kmem_cache *s_inode_cache;  //本 sb 专用 inode slab
    struct dentry     *s_root;
    struct inode      *s_root_inode;
    const struct super_operations *s_op;
    void              *s_fs_info;      //FS 私有(ext4_sb_info / fat32_fs_t ...)
    struct bdev       *s_bdev;
    uint64_t           s_flags;
    uint64_t           s_ino_next;     //匿名 inode(tmpsfs 用)
    struct list_node   s_inodes;
    spinlock_t         s_lock;
};

/* ================= inode ================= */
struct inode_operations {
    struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry, unsigned flags);
    int  (*create) (struct inode *dir, struct dentry *dentry, uint16_t mode);
    int  (*mkdir)  (struct inode *dir, struct dentry *dentry, uint16_t mode);
    int  (*mknod)  (struct inode *dir, struct dentry *dentry, uint16_t mode, uint64_t rdev);
    int  (*symlink)(struct inode *dir, struct dentry *dentry, const char *target);
    int  (*link)   (struct dentry *old, struct inode *dir, struct dentry *newd);
    int  (*unlink) (struct inode *dir, struct dentry *dentry);
    int  (*rmdir)  (struct inode *dir, struct dentry *dentry);
    int  (*rename) (struct inode *od, struct dentry *o, struct inode *nd, struct dentry *n);
    int  (*permission)(struct inode *inode, int mask);
    int  (*getattr)(const struct path *path, struct stat *st);
    int  (*setattr)(struct dentry *dentry, void *iattr);
    int  (*get_link)(struct dentry *dentry, struct inode *inode, const char **target);
};

struct file_operations {
    int64_t (*read)   (struct file *f, char *buf, uint64_t len, uint64_t *pos);
    int64_t (*write)  (struct file *f, const char *buf, uint64_t len, uint64_t *pos);
    int     (*open)   (struct inode *inode, struct file *f);
    int     (*release)(struct inode *inode, struct file *f);
    int     (*iterate)(struct file *f, void *ctx, int (*emit)(void *ctx, const char *name, int namelen, uint64_t ino, int type, int64_t next_off));
    int     (*fsync)  (struct file *f, int datasync);
};

struct inode {
    struct list_node   i_hash;      //icache 哈希链
    struct list_node   i_lru;       //未使用 inode 的 LRU 链
    struct list_node   i_sb_list;   //sb->s_inodes
    struct list_node   i_dentry;    //指向本 inode 的所有 dentry
    struct super_block *i_sb;
    uint64_t           i_ino;       //★ 稳定标识(rename 不变)
    uint32_t           i_count;     //引用计数(0 = 可回收)
    uint32_t           i_nlink;     //硬链接数
    uint16_t           i_mode;      //S_IFMT | 权限位
    uint32_t           i_uid, i_gid;
    uint64_t           i_size;
    uint64_t           i_blocks;    //占用的 512B 块数(stat 用)
    uint32_t           i_blkbits;   //FS 块大小 log2
    uint64_t           i_rdev;      //设备节点
    int64_t            i_atime_s, i_mtime_s, i_ctime_s;
    uint32_t           i_atime_ns, i_mtime_ns, i_ctime_ns;
    uint32_t           i_state;     //I_*
    uint32_t           i_flags;     //S_* (不可变/仅追加, 预留)
    void              *i_mapping;   //struct address_space*(Phase 8 接通 mmap)
    const struct inode_operations *i_op;
    const struct file_operations  *i_fop;
    void              *i_private;   //FS 私有(内嵌在 slab 对象尾部)
};

/* ================= dentry ================= */
struct dentry {
    struct list_node    d_hash;     //dcache 哈希链
    struct dentry      *d_parent;
    struct inode       *d_inode;    //NULL = 负缓存
    char               *d_name;     //变长名字(与 dentry 同块内存)
    uint32_t            d_name_len;
    uint32_t            d_count;
    uint32_t            d_flags;    //DCACHE_*
    struct list_node    d_subdirs;
    struct list_node    d_child;
    struct list_node    d_alias;    //挂在 inode->i_dentry
    struct vfsmount    *d_mounted;
};

/* ================= file ================= */
struct file {
    uint32_t             f_count;   //★ dup/fork 共享的关键
    uint32_t             f_flags;
    uint64_t             f_pos;     //★ 多个 fd 共享同一个 f_pos
    struct path          f_path;
    struct inode        *f_inode;
    const struct file_operations *f_op;
    void                *f_private;
    void                *f_pipe;    //管道对象(复用现有 pipe)
};

/* ================= vfsmount ================= */
struct vfsmount {
    struct list_node   mnt_list;       //全局挂载链
    struct list_node   mnt_child;      //父的 mnt_children
    struct list_node   mnt_children;
    struct vfsmount   *mnt_parent;
    struct dentry     *mnt_mountpoint; //挂在父的哪个目录上
    struct dentry     *mnt_root;       //本 FS 的根
    struct super_block *mnt_sb;
    uint32_t           mnt_flags;
};

/* ================= 接口 ================= */
void FsVfsInit(void);//初始化sb链/icache(幂等)

struct super_block *SuperAlloc(const char *type_name, uint64_t magic,uint32_t inode_size, const struct super_operations *op);
void SuperFree(struct super_block *sb);
int  SuperCount(void);

struct inode *IgetLocked(struct super_block *sb, uint64_t ino);
void          Iput(struct inode *inode);
void          InodeMarkDirty(struct inode *inode);
void          InodeSetLimit(uint32_t max);
uint32_t      IcacheCount(void);
void          IcacheStats(void);
int           IcacheSelfTest(void);

#endif
