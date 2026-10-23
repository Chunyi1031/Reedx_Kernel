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

//路径
struct path {
    struct vfsmount *mnt;      //从哪个挂载点看见的(决定 ".." 怎么走)
    struct dentry   *dentry;   //具体目录项
};

#define NAMELEN_MAX      255       // 单个名字最大字节数(ext4/FAT 的通用上限)
#define PATHLEN_MAX      4096      // 整条路径最大长度(POSIX PATH_MAX)
#define MAXSYMLINKS      40        // 符号链接最多展开多少层, 超了返回 -ELOOP

//inode 状态位
#define I_NEW        0x00000001u   // 刚分配, 元数据还没从磁盘读上来(由 read_inode 填充)
#define I_DIRTY      0x00000002u   // 元数据改过还没写回
#define I_FREEING    0x00000004u   // 正在被释放(防止别人再拿到它)
#define I_WILL_FREE  0x00000008u   // 即将释放
#define I_DONTCACHE  0x00000010u   // 用完就丢, 不留在 icache 里

//dentry 标志位
#define DCACHE_NEGATIVE 0x00000001u // 负缓存: 这个名字查过了, 确实不存在(d_inode==NULL)
#define DCACHE_MOUNTED  0x00000002u // 这个目录上有挂载点(d_mounted != NULL)
#define DCACHE_NEW      0x00000004u // 已经挂进 dcache 哈希表(方便判断是否重复插入)

//挂载标志
#define MS_RDONLY      0x00000001u // 只读挂载: 任何写操作返回 -EROFS
#define MS_NOSUID      0x00000002u // 忽略 setuid/setgid 位
#define MS_NODEV       0x00000004u // 不允许访问设备节点
#define MS_NOATIME     0x00000008u // 不更新访问时间
#define MS_SYNCHRONOUS 0x00000010u // 同步写: 每次 write 都落盘
#define MS_NOEXEC      0x00000020u // 不允许执行里面的程序

//权限检查掩码
#define MAY_EXEC  0x01             // 需要执行/进入目录的权限(x)
#define MAY_WRITE 0x02             // 需要写权限(w)
#define MAY_READ  0x04             // 需要读权限(r)

//path_lookup()的行为开关
#define LOOKUP_FOLLOW     0x0001   // 末段是符号链接时, 跟进去(open 的默认行为)
#define LOOKUP_DIRECTORY  0x0002   // 末段必须是目录, 否则 -ENOTDIR
#define LOOKUP_PARENT     0x0004   // 只解析到父目录(create/unlink/rename 用)
#define LOOKUP_NOFOLLOW   0x0008   // 末段不跟随符号链接(lstat/readlink 用)
#define LOOKUP_EMPTY      0x0010   // AT_EMPTY_PATH: 空路径表示 dirfd 自己
#define LOOKUP_CREATE     0x0020   // 允许"末段不存在"(配合 LOOKUP_PARENT)

//整个文件系统的生命周期回调
struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);//分配一个inode对象
    void          (*destroy_inode)(struct inode *inode);//释放inode对象
    void          (*read_inode)(struct inode *inode);//从磁盘读取inode的元数据填进inode
    int           (*write_inode)(struct inode *inode, int wait);//把inode元数据写回磁盘
    void          (*dirty_inode)(struct inode *inode);//元数据刚变脏时被调用
    void          (*evict_inode)(struct inode *inode);//i_nlink==0 且最后一个引用消失时调用: 在这里释放数据块
    int           (*sync_fs)(struct super_block *sb, int wait);//卸载前把所有脏数据刷回磁盘
    int           (*statfs)(struct super_block *sb, void *kstatfs);// statfs(2) 用: 填总容量/剩余空间/总 inode 数
};

//被挂载的文件系统实例
struct super_block {
    struct list_node   s_list;         // 挂到全局 sb 链表(所有已挂载的文件系统)
    char               s_type_name[16];// 类型名: "ext4" / "vfat" / "tmpfs", 主要用于日志
    uint64_t           s_magic;        // 魔数: ext4=0xEF53, FAT=0x4D44, 用来交叉校验
    int                s_dev;          // 设备号; 0 表示匿名(如 tmpfs 不占块设备)
    uint32_t           s_blocksize;    // 文件系统块大小(1024/2048/4096), 不是磁盘的 512 扇区
    uint64_t           s_maxbytes;     // 本 FS 允许的最大文件大小
    uint32_t           s_inode_size;   // inode 对象总大小 = sizeof(inode) + FS 私有数据
    struct kmem_cache *s_inode_cache;  // 本 sb 专用的小对象缓存(按 s_inode_size 建)
    struct dentry     *s_root;         // 本 FS 的根目录项(挂载点的 mnt_root 指向它)
    struct inode      *s_root_inode;   // 根目录的 inode(等价于 s_root->d_inode, 省一次解引用)
    const struct super_operations *s_op; // 本 FS 的操作表
    void              *s_fs_info;      // FS 私有数据指针(ext4_sb_info / fat32_fs_t ...)
    struct bdev       *s_bdev;         // 底层块设备; tmpfs 为 NULL
    uint64_t           s_flags;        // MS_* 挂载标志
    uint64_t           s_ino_next;     // 匿名 inode 分配器(tmpfs 这类没有真实 inode 号的 FS 用)
    struct list_node   s_inodes;       // 本 sb 所有 inode 的链(卸载时遍历刷盘)
    spinlock_t         s_lock;         // 只保护本 sb 的 inode 链, 不是全局锁
};

//目录/元数据"类操作
struct inode_operations {
    struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry, unsigned flags);//在目录里查一个名字: 找到就 d_add(dentry, inode) 并返回 dentry;
    int  (*create) (struct inode *dir, struct dentry *dentry, uint16_t mode);// 创建普通文件
    int  (*mkdir)  (struct inode *dir, struct dentry *dentry, uint16_t mode);// 创建目录
    int  (*mknod)  (struct inode *dir, struct dentry *dentry, uint16_t mode, uint64_t rdev);// 创建设备节点/FIFO/Socket(mknod), rdev 是设备号
    int  (*symlink)(struct inode *dir, struct dentry *dentry, const char *target);// 创建符号链接
    int  (*link)   (struct dentry *old, struct inode *dir, struct dentry *newd);// 建硬链接(ln)
    int  (*unlink) (struct inode *dir, struct dentry *dentry);// 删除文件
    int  (*rmdir)  (struct inode *dir, struct dentry *dentry);// 删除空目录
    int  (*rename) (struct inode *od, struct dentry *o, struct inode *nd, struct dentry *n);// 改名/移动(同一 FS 内)
    int  (*permission)(struct inode *inode, int mask);// 自定义权限判断; NULL 表示走 VFS 通用的 i_mode + uid/gid 检查
    int  (*getattr)(const struct path *path, struct stat *st);// 填 stat 结构(stat/fstat 用)
    int  (*setattr)(struct dentry *dentry, void *iattr);//修改属性
    int  (*get_link)(struct dentry *dentry, struct inode *inode, const char **target);//读出符号链接的目标内容
};

//文件操作
struct file_operations {
    int64_t (*read)   (struct file *f, char *buf, uint64_t len, uint64_t *pos);//读文件
    int64_t (*write)  (struct file *f, const char *buf, uint64_t len, uint64_t *pos);//写文件
    int     (*open)   (struct inode *inode, struct file *f);
    int     (*release)(struct inode *inode, struct file *f);//最后一个引用关闭时调用
    int     (*iterate)(struct file *f, void *ctx,int (*emit)(void *ctx, const char *name, int namelen,uint64_t ino, int type, int64_t next_off));// 目录遍历
    int     (*fsync)  (struct file *f, int datasync);// 刷盘(fsync/fdatasync)
};

//inode节点
struct inode {
    struct list_node   i_hash;      // icache 哈希链, 键 = (i_sb, i_ino)
    struct list_node   i_lru;       // i_count 减到 0 后挂到 icache 的 LRU 上等待回收
    struct list_node   i_sb_list;   // 挂到 sb->s_inodes
    struct list_node   i_dentry;    // ★ 所有指向本 inode 的 dentry 链(硬链接就有多个)
    struct super_block *i_sb;       // 属于哪个文件系统实例
    uint64_t           i_ino;       // ★ 文件身份号: rename 不变、硬链接共享
    uint32_t           i_count;     // 引用计数: 谁在用这个 inode(0 = 可以回收)
    uint32_t           i_nlink;     // 硬链接数: 目录项指向它的个数。0 = 已删除
    uint16_t           i_mode;      // S_IFMT(文件类型) | 0777 权限位, 如 S_IFREG|0644
    uint32_t           i_uid, i_gid; // 属主/属组
    uint64_t           i_size;      // 文件字节数
    uint64_t           i_blocks;    // 占用的 512 字节块数(stat 的 st_blocks)
    uint32_t           i_blkbits;   // FS 块大小的 log2(4096 → 12), 换算块号用
    uint64_t           i_rdev;      // 设备节点的设备号(S_IFCHR/S_IFBLK 才有意义)
    int64_t            i_atime_s, i_mtime_s, i_ctime_s;  // 访问/修改/状态变更时间(秒)
    uint32_t           i_atime_ns, i_mtime_ns, i_ctime_ns;// 对应的纳秒部分(ext4 需要)
    uint32_t           i_state;     // I_* 状态位
    uint32_t           i_flags;     // 文件属性位(不可变/仅追加等), 预留
    void              *i_mapping;   // struct address_space*: 文件页缓存(Phase 8 接 mmap)
    const struct inode_operations *i_op;  // 目录类操作(lookup/create/unlink...)
    const struct file_operations  *i_fop; // 打开后的读写操作(read/write/iterate)
    void              *i_private;   // ★ FS 私有数据, 紧跟在 struct inode 后面
};

//目录项缓存项
struct dentry {
    struct list_node    d_hash;     // dcache 哈希链, 键 = (d_parent, d_name)
    struct list_node    d_lru;      // d_count 减到 0 后挂到 dcache 的 LRU
    struct dentry      *d_parent;   // 父目录项; 只有全局根("/")是 NULL
    struct inode       *d_inode;    // ★指向的 inode; NULL = 负缓存(这个名字不存在)
    char               *d_name;     // 名字(指向本对象尾部, 与 dentry 同一块内存)
    uint32_t            d_name_len; // 名字长度(不用每次 strlen)
    uint32_t            d_count;    // 引用计数: 有几个 path/file 在用它
    uint32_t            d_flags;    // DCACHE_*
    struct list_node    d_subdirs;  // 本目录的**子 dentry 链头**(子项用 d_child 挂进来)
    struct list_node    d_child;    // 作为子项挂在 d_parent->d_subdirs 上的节点
    struct list_node    d_alias;    // 挂在 inode->i_dentry上的节点
    struct vfsmount    *d_mounted;  // 非 NULL 表示这个目录上挂了别的文件系统
};

//打开文件描述符
struct file {
    uint32_t             f_count;//引用计数
    uint32_t             f_flags;//打开标志
    uint64_t             f_pos;//当前读写位置
    struct path          f_path;//路径
    struct inode        *f_inode;//指向inode
    const struct file_operations *f_op;//读写操作表(read/write/iterate)
    void                *f_private;//FS每次打开自己的数据
    void                *f_pipe;// 管道对象
};

//挂载点
struct vfsmount {
    struct list_node   mnt_list;       // 全局挂载链(所有挂载点)
    struct list_node   mnt_child;      // 作为子挂载挂在父的 mnt_children 上
    struct list_node   mnt_children;   // 本挂载点下面又挂了哪些东西
    struct vfsmount   *mnt_parent;     // 父挂载; NULL 表示这是一棵独立的挂载树
    struct dentry     *mnt_mountpoint; //挂在父文件系统的哪个目录上(如 /mnt)
    struct dentry     *mnt_root;       //本文件系统的根 dentry
    struct super_block *mnt_sb;        // 对应的超级块
    uint32_t           mnt_flags;      // MS_* 每挂载点独立的标志(只读等)
};


void FsVfsInit(void);//初始化VFS


struct super_block *SuperAlloc(const char *type_name, uint64_t magic,uint32_t inode_size, const struct super_operations *op);//分配一个super_block
void SuperFree(struct super_block *sb);
int  SuperCount(void);

//inode 缓存(icache)
struct inode *IgetLocked(struct super_block *sb, uint64_t ino);
void          Iput(struct inode *inode);
void          InodeMarkDirty(struct inode *inode);
void          InodeSetLimit(uint32_t max);
uint32_t      IcacheCount(void);
void          IcacheStats(void);
int           IcacheSelfTest(void);

//目录项缓存(dcache)
struct dentry *d_alloc(struct dentry *parent, const char *name);//分配一个dentry
struct dentry *d_lookup(struct dentry *parent, const char *name);
void          d_add(struct dentry *dentry, struct inode *inode);
void          d_drop(struct dentry *dentry);
void          d_invalidate(struct dentry *dentry);
struct dentry *dget(struct dentry *dentry);
void          dput(struct dentry *dentry);
void          DcacheSetLimit(uint32_t max);
uint32_t      DcacheCount(void);
int           DcacheSelfTest(void);

//文件描述
struct file *FileAlloc(void);//新建一个打开文件描述(引用计数=1)
struct file *FileGet(struct file *f);//增加引用计数
void FilePut(struct file *f);//减少引用计数,归零则释放
uint32_t FileCount(void);//已打开文件数

//挂载树
struct vfsmount *MntAlloc(void);
void             MntFree(struct vfsmount *m);
struct vfsmount *VfsKernMount(struct super_block *sb, struct dentry *root, uint32_t flags);//内核挂载
int              VfsAttachMount(struct vfsmount *parent, struct dentry *mountpoint,struct vfsmount *child);//把子挂载挂到目录上
int              VfsDetachMount(struct vfsmount *m);//卸载
struct dentry   *VfsDotDot(struct vfsmount **mnt_io, struct dentry *dentry);
uint32_t         VfsMountCount(void);
void             VfsDumpMounts(void);

#endif
