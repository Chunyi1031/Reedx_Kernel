#ifndef _FS_H_
#define _FS_H_

#include <klib.h>

//文件类型
#define FT_FILE 0
#define FT_DIR  1

#define MAX_FD   16
#define MAX_NAME 32

typedef struct fs_node fs_node_t;
struct disk_info;//磁盘信息(定义在drives/disk.h,此处仅前置声明)

//节点操作集
typedef struct fs_node_ops{
    uint64_t    (*read)(fs_node_t *node, uint64_t off, void *buf, uint64_t len);
    uint64_t    (*write)(fs_node_t *node, uint64_t off, const void *buf, uint64_t len);
    fs_node_t * (*lookup)(fs_node_t *dir, const char *name);
    fs_node_t * (*create)(fs_node_t *dir, const char *name, int type);
    void        (*truncate)(fs_node_t *node);
    int         (*mkdir)(fs_node_t *dir, const char *name, int mode);//创建目录
    int         (*unlink)(fs_node_t *dir, const char *name);//删除目录项(文件或目录)
    int         (*rename)(fs_node_t *olddir, const char *oldname, fs_node_t *newdir, const char *newname);//重命名(移动)目录项
} fs_node_ops_t;

//文件系统节点(文件或目录)
struct fs_node{
    char            name[MAX_NAME];
    int             type;           //FT_FILE/FT_DIR
    fs_node_ops_t  *ops;            //节点操作集
    void           *priv;           //具体文件系统私有数据
    uint64_t        size;           //文件字节数/目录项数
    uint64_t        atime;          //访问时间
    uint64_t        mtime;          //修改时间
    uint64_t        ctime;          //创建时间
    struct list_node siblings;      //父目录内兄弟节点
    struct list_node children;      //子节点(目录用)
    uint32_t        refs;           //引用计数
};

//打开文件描述
typedef struct fs_file{
    fs_node_t *node;
    uint64_t   off;
    int        flags;
    _Bool      used;
} fs_file_t;

void FsInit(struct disk_info *disk);//初始化文件系统
fs_node_t *FsRoot(void);//文件系统根目录节点
fs_node_t *FsResolve(const char *path);//解析绝对路径返回节点
int FsOpen(const char *path, int flags, fs_file_t *out);//打开文件
void FsClose(fs_file_t *f);//关闭文件
uint64_t FsRead(fs_file_t *f, void *buf, uint64_t len);//读取文件
uint64_t FsWrite(fs_file_t *f, const void *buf, uint64_t len);//写入文件
int FsSeek(fs_file_t *f, int64_t off, int whence);//移动文件指针
uint64_t FsSize(fs_file_t *f);//获取文件大小
int FsMkdir(const char *path, int mode);//创建目录
int FsUnlink(const char *path);//删除目录项(文件或目录)
int FsRename(const char *oldpath, const char *newpath);//重命名(移动)目录项

//FAT32
int fat32_mount(struct disk_info *disk);//挂载FAT32分区
fs_node_t *fat32_root(void);//FAT32根目录节点
//RamFS
fs_node_t *ramfs_init(void);//初始化ramfs

#endif
