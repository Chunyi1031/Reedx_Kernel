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
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK     10

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
struct disk_info;//磁盘信息(定义在drives/disk.h,此处仅前置声明)

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
    fs_node_t *node;    //文件节点
    void      *pipe;    //管道对象
    uint64_t   off;     //文件指针
    int        flags;   //打开标志
    _Bool      used;    //是否使用
    _Bool      tty_mark;//控制台fd是否已查询终端
} fs_file_t;

void FsInit(struct disk_info *disk);//初始化文件系统
fs_node_t *FsRoot(void);//文件系统根目录节点
fs_node_t *FsResolve(const char *path);//解析绝对路径返回节点
int FsNormalizePath(const char *path, char *out, int outsz);//规范化路径, 输出绝对规范路径
int FsOpen(const char *path, int flags, fs_file_t *out);//打开文件
int FsOpenIn(fs_node_t *dir, const char *name, int flags, fs_file_t *out);//在已打开目录下打开
int FsMkdirIn(fs_node_t *dir, const char *name, int mode);//在已打开目录下建子目录
int FsUnlinkIn(fs_node_t *dir, const char *name);//删除已打开目录下的目录项
int FsRenameIn(fs_node_t *olddir, const char *oldname, fs_node_t *newdir, const char *newname);//两侧均为目录+名字的重命名
int FsExistsIn(fs_node_t *dir, const char *name);//存在性检查
fs_node_t *FsParentOf(const char *path, char *name, int namesz);//取路径的父目录节点与末段名
void FsClose(fs_file_t *f);//关闭文件
uint64_t FsRead(fs_file_t *f, void *buf, uint64_t len);//读取文件
uint64_t FsWrite(fs_file_t *f, const void *buf, uint64_t len);//写入文件
int FsSeek(fs_file_t *f, int64_t off, int whence);//移动文件指针
uint64_t FsSize(fs_file_t *f);//获取文件大小
int FsTruncate(fs_file_t *f, uint64_t len);//按fd截断/扩展文件(ftruncate)
int FsTruncatePath(const char *path, uint64_t len);//按路径截断/扩展文件(truncate)
int FsMkdir(const char *path, int mode);//创建目录
int FsUnlink(const char *path);//删除目录项(文件或目录)
int FsRename(const char *oldpath, const char *newpath);//重命名(移动)目录项
int FsGetdents(fs_file_t *f, void *buf, uint64_t len);//读取目录项到缓冲区

//FAT32
int fat32_mount(struct disk_info *disk);//挂载FAT32分区
fs_node_t *fat32_root(void);//FAT32根目录节点
//RamFS
fs_node_t *ramfs_init(void);//初始化ramfs

#endif
