// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/fs.h
 * 
 * Copyright (C) 2026 Liu Chunyi
 * 
 * Reedx文件系统接口
 */
#ifndef _FS_H_
#define _FS_H_

#include <klib.h>

//文件类型
#define FT_FILE 0
#define FT_DIR  1

#define MAX_FD   256
#define MAX_NAME 256

//目录项类型
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

//目录项迭代结果
typedef struct fs_dirent {
    uint64_t ino;           //inode号
    int      type;          //FT_FILE/FT_DIR
    char     name[MAX_NAME];//目录项名
} fs_dirent_t;

//目录项记录
typedef struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} __attribute__((packed)) linux_dirent64_t;

typedef struct fs_node fs_node_t;
struct file;
struct inode;
struct vfsmount;
struct super_block;
struct dentry;
struct stat;
struct disk_info;//磁盘信息
struct bdev;//块设备

//节点操作集
typedef struct fs_node_ops{
    uint64_t    (*read)(fs_node_t *node, uint64_t off, void *buf, uint64_t len);
    uint64_t    (*write)(fs_node_t *node, uint64_t off, const void *buf, uint64_t len);
    fs_node_t * (*lookup)(fs_node_t *dir, const char *name);
    fs_node_t * (*create)(fs_node_t *dir, const char *name, int type);
    int         (*truncate)(fs_node_t *node, uint64_t newsize);//截断/扩展到指定长度(0成功/-errno失败)
    int         (*mkdir)(fs_node_t *dir, const char *name, int mode);//创建目录
    int         (*unlink)(fs_node_t *dir, const char *name);//删除目录项(文件或目录)
    int         (*rename)(fs_node_t *olddir, const char *oldname, fs_node_t *newdir, const char *newname);//重命名(移动)目录项
    int         (*readdir)(fs_node_t *dir, uint64_t *cookie, fs_dirent_t *de);//目录迭代
    int         (*set_times)(fs_node_t *node);//把节点时间回写到存储介质
} fs_node_ops_t;

//文件系统节点(文件或目录)
struct fs_node{
    char            name[MAX_NAME];
    int             type;           //FT_FILE/FT_DIR
    fs_node_ops_t  *ops;            //节点操作集
    void           *priv;           //具体文件系统私有数据
    uint64_t        size;           //文件字节数/目录项数
    uint64_t        ino;            //inode号
    uint64_t        atime;          //访问时间
    uint64_t        mtime;          //修改时间
    uint64_t        ctime;          //创建时间
    struct list_node siblings;      //父目录内兄弟节点
    struct list_node children;      //子节点(目录用)
    uint32_t        refs;           //引用计数
};

//打开文件描述
typedef struct fs_file{
    struct file     *f;     //共享的打开文件描述
    struct inode    *inode; //新VFS的inode
    struct dentry   *dentry;//新VFS的目录项
    struct vfsmount *mnt;   //解析时经过的挂载点
    void            *pipe;  //管道对象
    void            *epoll; //epoll实例
    int              flags; //打开标志
    _Bool            used;  //是否使用
    _Bool            tty_mark;//控制台fd是否已查询终端
} fs_file_t;

void FsInit(struct disk_info *disk);//初始化文件系统

//路径/目录项接口
int FsChdir(const char *path);//切换工作目录
int FsGetcwd(char *buf, int size);//取当前工作目录的绝对路径,返回字符串长度
struct dentry *FsDirOfFd(int fd);//fd对应的目录dentry
int FsOpen(const char *path, int flags, fs_file_t *out);//打开文件
int FsOpenAt(struct dentry *dir, const char *name, int flags, fs_file_t *out);//在目录dentry下打开
int FsMkdir(const char *path, int mode);//创建目录
int FsMkdirAt(struct dentry *dir, const char *name, int mode);//在目录dentry下创建目录
int FsUnlink(const char *path, int isdir);//删除文件/目录
int FsUnlinkAt(struct dentry *dir, const char *name, int isdir);//在目录dentry下删除
int FsRename(const char *oldpath, const char *newpath, int flags);//改名/移动
int FsRenameAt(struct dentry *odir, const char *oname, struct dentry *ndir, const char *nname, int flags);//在目录dentry下改名/移动
int FsSymlink(const char *target, const char *linkpath);//建符号链接
int FsSymlinkAt(struct dentry *dir, const char *name, const char *target);//在dir目录下建符号链接
int FsLink(const char *oldpath, const char *newpath, int follow);//给已有文件再挂一个名字(硬链接)
int FsLinkAt(struct dentry *odir, const char *oname, struct dentry *ndir, const char *nname);//在两个目录dentry之间建硬链接
int FsChmod(const char *path, uint16_t mode);//按路径改权限位
int FsChmodFd(int fd, uint16_t mode);//按文件描述符改权限位
int FsChown(const char *path, uint32_t uid, uint32_t gid, int follow);//按路径改属主/属组
int FsChownFd(int fd, uint32_t uid, uint32_t gid);//按文件描述符改属主/属组
int FsMknod(const char *path, uint16_t mode, uint32_t rdev);//在路径处建设备节点
int FsReadlink(const char *path, char *buf, int size);//读符号链接目标,返回长度
int FsStat(const char *path, struct stat *st);//按路径取属性(跟随符号链接)
int FsLstat(const char *path, struct stat *st);//按路径取属性(不跟随符号链接)
int FsStatAt(struct dentry *dir, const char *name, struct stat *st);//在目录dentry下取属性
int FsStatFile(fs_file_t *f, struct stat *st);//按已打开文件取属性
int FsUtime(const char *path, int64_t atime, int64_t mtime, int64_t ctime);//按路径改时间
int FsUtimeAt(struct dentry *dir, const char *name, int64_t atime, int64_t mtime, int64_t ctime);//在目录 dentry 下改时间
int FsUtimeFd(fs_file_t *f, int64_t atime, int64_t mtime, int64_t ctime);//按已打开文件改时间
int FsAccess(const char *path, int mask);//存在性/权限检查
int FsMount(const char *type, const char *target);//把type类型的文件系统挂到target目录上
int FsMountExt2(const char *target);//把第一个ext2分区挂到target目录上
int FsUmount(const char *target);//卸载挂在target目录上的文件系统
int FsSync(void);//把所有文件系统的脏数据刷回磁盘
int FsExistsAt(struct dentry *dir, const char *name);//存在性检查(0存在,-ENOENT不存在)

void FsClose(fs_file_t *f);//关闭文件
uint64_t FsRead(fs_file_t *f, void *buf, uint64_t len);//读取文件
uint64_t FsWrite(fs_file_t *f, const void *buf, uint64_t len);//写入文件
int FsSeek(fs_file_t *f, int64_t off, int whence);//移动文件指针
uint64_t FsSize(fs_file_t *f);//获取文件大小
int FsTruncate(fs_file_t *f, uint64_t len);//按fd截断/扩展文件(ftruncate)
int FsTruncatePath(const char *path, uint64_t len);//按路径截断/扩展文件(truncate)
int FsGetdents(fs_file_t *f, void *buf, uint64_t len);//读取目录项到缓冲区

//FAT32
int fat32_mount(struct disk_info *disk);//挂载FAT32分区
fs_node_t *fat32_root(void);//FAT32根目录节点
//FAT32在新VFS上的适配
uint32_t FatVfsInodeSize(void);
const struct super_operations *FatVfsSuperOps(void);
struct dentry *FatVfsFillSuper(struct super_block *sb);
//RamFS
uint32_t Ramfs2InodeSize(void);
const struct super_operations *Ramfs2SuperOps(void);
struct dentry *Ramfs2FillSuper(struct super_block *sb);
//ext2
uint32_t Ext2InodeSize(void);//挂载
const struct super_operations *Ext2SuperOps(void);
struct dentry *Ext2FillSuper(struct super_block *sb);
int Ext2ReadSuper(struct super_block *sb, struct bdev *bd);
struct bdev *Ext2FindPartition(void);
struct super_block *Ext2NewSuper(struct bdev *bd, struct dentry **root_out);
int Ext2MountAt(struct vfsmount *parent, struct dentry *mountpoint);
//devtmpfs
uint32_t DevFsInodeSize(void);
const struct super_operations *DevFsSuperOps(void);
struct dentry *DevFsFillSuper(struct super_block *sb);
//procfs
uint32_t ProcFsInodeSize(void);
const struct super_operations *ProcFsSuperOps(void);
struct dentry *ProcFsFillSuper(struct super_block *sb);
//sysfs
uint32_t SysFsInodeSize(void);
const struct super_operations *SysFsSuperOps(void);
struct dentry *SysFsFillSuper(struct super_block *sb);
#endif
