// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/sysfs.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 内核对象视图(只读)
 */
#include <vfs.h>
#include <fs.h>
#include <klib.h>
#include <drives/blkdev.h>

#define SYSFS_BUF_MAX  256//单个属性文件的内容上限
#define SYSFS_MAX_BDEV 16 //最多枚举多少个块设备

//节点种类
enum {
    SYS_K_ROOT = 0, // /sys
    SYS_K_BLOCKDIR, // /sys/block
    SYS_K_BLOCKDEV, // /sys/block/<name>
    SYS_K_SIZE,     // 设备容量(512字节扇区数)
    SYS_K_RO,       // 是否只读
    SYS_K_REMOVABLE,// 是否可移动
    SYS_K_DEV,      // 合成的 主:次 设备号
    SYS_K_KERNELDIR,// /sys/kernel
    SYS_K_OSTYPE,   // 系统名
    SYS_K_OSRELEASE,// 内核版本号
    SYS_K_VERSION,  // 编译信息
    SYS_K_DEVICESDIR,// /sys/devices
    SYS_K_CLASSDIR, // /sys/class
    SYS_K_CLASSBLOCK// /sys/class/block
};

//每个sysfs inode的私有信息
typedef struct sysfs_inode_info {
    uint32_t kind;//本inode的种类(SYS_K_*)
    uint32_t idx; //块设备下标(kind与块设备相关时有效)
} sysfs_inode_info_t;

#define SYSFS_INFO(ino) ((sysfs_inode_info_t*)(ino)->i_private)

//inode号
#define SYSFS_INO_ROOT        1ULL
#define SYSFS_INO_BLOCKDIR    2ULL
#define SYSFS_INO_KERNELDIR   3ULL
#define SYSFS_INO_DEVICESDIR  4ULL
#define SYSFS_INO_CLASSDIR    5ULL
#define SYSFS_INO_CLASSBLOCK  6ULL
#define SYSFS_INO_OSTYPE      7ULL
#define SYSFS_INO_OSRELEASE   8ULL
#define SYSFS_INO_VERSION     9ULL
#define SYSFS_INO_BDEV(i)     ((uint64_t)(100 + (uint64_t)(i) * 4))
#define SYSFS_INO_BDEV_F(i,k) ((uint64_t)(100 + (uint64_t)(i) * 4 + 1 + (k)))

//块设备目录下的属性文件
static const struct {
    const char *name;//文件名
    uint32_t    kind;//文件种类
} g_sysfs_bdev_files[] = {
    {"size",      SYS_K_SIZE},
    {"ro",        SYS_K_RO},
    {"removable", SYS_K_REMOVABLE},
    {"dev",       SYS_K_DEV},
};
#define SYSFS_NBDEVFILE ((int)(sizeof(g_sysfs_bdev_files) / sizeof(g_sysfs_bdev_files[0])))

// /sys/kernel 下的属性文件
static const struct {
    const char *name;//文件名
    uint32_t    kind;//文件种类
    uint64_t    ino; //固定inode号
} g_sysfs_kernel_files[] = {
    {"ostype",    SYS_K_OSTYPE,    SYSFS_INO_OSTYPE},
    {"osrelease", SYS_K_OSRELEASE, SYSFS_INO_OSRELEASE},
    {"version",   SYS_K_VERSION,   SYSFS_INO_VERSION},
};
#define SYSFS_NKFILE ((int)(sizeof(g_sysfs_kernel_files) / sizeof(g_sysfs_kernel_files[0])))

static const struct inode_operations g_sysfs_dir_iops;//目录操作表
static const struct inode_operations g_sysfs_file_iops;//文件操作表
static const struct file_operations g_sysfs_dir_fops;//目录遍历操作表
static const struct file_operations g_sysfs_file_fops;//文件读操作表

//当前枚举到的块设备
static struct bdev *g_sysfs_bdev[SYSFS_MAX_BDEV];
static int          g_sysfs_nbdev;

//取第idx个块设备
static struct bdev *sysfs_bdev(uint32_t idx){
    if(idx >= (uint32_t)g_sysfs_nbdev)return NULL;
    return g_sysfs_bdev[idx];
}

//按名字找块设备下标
static int sysfs_find_bdev(const char *name, uint32_t len){
    for(int i = 0;i < g_sysfs_nbdev;i ++){
        struct bdev *b = g_sysfs_bdev[i];
        if(!b)continue;
        const char *n = b->bd_name;
        if(strlen(n) != len)continue;
        if(memcmp(n, name, len) == 0)return i;
    }
    return -1;
}

//建一个sysfs inode
static struct inode *sysfs_iget(struct super_block *sb, uint64_t ino, uint32_t kind, uint32_t idx){
    struct inode *inode = IgetLocked(sb, ino);
    if(!inode)return NULL;
    sysfs_inode_info_t *ii = SYSFS_INFO(inode);
    if(!ii)return NULL;
    ii->kind = kind;
    ii->idx  = idx;
    inode->i_uid   = 0;
    inode->i_gid   = 0;
    inode->i_size  = 0;
    inode->i_nlink = 1;
    inode->i_op    = NULL;
    inode->i_fop   = &g_sysfs_file_fops;
    switch(kind){
        case SYS_K_ROOT:
        case SYS_K_BLOCKDIR:
        case SYS_K_KERNELDIR:
        case SYS_K_DEVICESDIR:
        case SYS_K_CLASSDIR:
        case SYS_K_CLASSBLOCK:
            inode->i_mode  = (uint16_t)(S_IFDIR | 0555);
            inode->i_nlink = 2;
            inode->i_op    = &g_sysfs_dir_iops;
            inode->i_fop   = &g_sysfs_dir_fops;
            break;
        case SYS_K_BLOCKDEV:
            inode->i_mode  = (uint16_t)(S_IFDIR | 0555);
            inode->i_nlink = 2;
            inode->i_op    = &g_sysfs_dir_iops;
            inode->i_fop   = &g_sysfs_dir_fops;
            break;
        default:
            inode->i_mode = (uint16_t)(S_IFREG | 0444);
            break;
    }
    inode->i_state &= ~I_NEW;
    return inode;
}

//把一个名字解析成目录项
static struct dentry *sysfs_add(struct inode *dir,struct dentry *dentry,uint64_t ino,uint32_t kind,uint32_t idx){
    struct inode *inode = sysfs_iget(dir->i_sb, ino, kind, idx);
    if(!inode){
        d_add(dentry, NULL);
        return NULL;
    }
    d_add(dentry, inode);
    Iput(inode);
    return dentry;
}

//查一个名字
static struct dentry *sysfs_lookup(struct inode *dir, struct dentry *dentry, unsigned flags){
    (void)flags;
    if(!dir || !dentry)return NULL;
    sysfs_inode_info_t *ii = SYSFS_INFO(dir);
    if(!ii){
        d_add(dentry, NULL);
        return NULL;
    }
    const char *name = dentry->d_name;
    uint32_t len = dentry->d_name_len;
    if(ii->kind == SYS_K_ROOT){
        if((len == 5) && (memcmp(name, "block", 5) == 0))return sysfs_add(dir, dentry, SYSFS_INO_BLOCKDIR, SYS_K_BLOCKDIR, 0);
        if((len == 6) && (memcmp(name, "kernel", 6) == 0))return sysfs_add(dir, dentry, SYSFS_INO_KERNELDIR, SYS_K_KERNELDIR, 0);
        if((len == 7) && (memcmp(name, "devices", 7) == 0))return sysfs_add(dir, dentry, SYSFS_INO_DEVICESDIR, SYS_K_DEVICESDIR, 0);
        if((len == 5) && (memcmp(name, "class", 5) == 0))return sysfs_add(dir, dentry, SYSFS_INO_CLASSDIR, SYS_K_CLASSDIR, 0);
    }else if(ii->kind == SYS_K_BLOCKDIR || ii->kind == SYS_K_CLASSBLOCK){
        //块设备名
        int idx = sysfs_find_bdev(name, len);
        if(idx >= 0)return sysfs_add(dir, dentry, SYSFS_INO_BDEV(idx), SYS_K_BLOCKDEV, (uint32_t)idx);
    }else if(ii->kind == SYS_K_BLOCKDEV){
        //设备属性文件
        struct bdev *b = sysfs_bdev(ii->idx);
        if(b){
            for(int k = 0;k < SYSFS_NBDEVFILE;k ++){
                const char *n = g_sysfs_bdev_files[k].name;
                if(strlen(n) != len || memcmp(n, name, len) != 0)continue;
                return sysfs_add(dir, dentry, SYSFS_INO_BDEV_F(ii->idx, k),g_sysfs_bdev_files[k].kind, ii->idx);
            }
        }
    }else if(ii->kind == SYS_K_KERNELDIR){
        for(int k = 0;k < SYSFS_NKFILE;k ++){
            const char *n = g_sysfs_kernel_files[k].name;
            if(strlen(n) != len || memcmp(n, name, len) != 0)continue;
            return sysfs_add(dir, dentry, g_sysfs_kernel_files[k].ino,g_sysfs_kernel_files[k].kind, 0);
        }
    }else if(ii->kind == SYS_K_CLASSDIR){
        if((len == 5) && (memcmp(name, "block", 5) == 0))
            return sysfs_add(dir, dentry, SYSFS_INO_CLASSBLOCK, SYS_K_CLASSBLOCK, 0);
    }
    d_add(dentry, NULL);
    return NULL;
}

//把块设备的目录项报给上层
static int sysfs_emit_bdevs(struct file *f, void *ctx,int (*emit)(void *ctx, const char *name, int namelen,uint64_t ino, int type, int64_t next_off)){
    for(int i = 0;i < g_sysfs_nbdev;i ++){
        struct bdev *b = g_sysfs_bdev[i];
        if(!b)continue;
        int len = (int)strlen(b->bd_name);
        emit(ctx, b->bd_name, len, SYSFS_INO_BDEV(i), (int)S_IFDIR, i);
    }
    (void)f;
    return 0;
}

//目录遍历
static int sysfs_iterate(struct file *f, void *ctx,int (*emit)(void *ctx, const char *name, int namelen,uint64_t ino, int type, int64_t next_off)){
    if(!f || !f->f_inode || !emit)return -EINVAL;
    sysfs_inode_info_t *ii = SYSFS_INFO(f->f_inode);
    if(!ii)return -EINVAL;
    switch(ii->kind){
        case SYS_K_ROOT:
            emit(ctx, "block",   5, SYSFS_INO_BLOCKDIR,   (int)S_IFDIR, 0);
            emit(ctx, "kernel",  6, SYSFS_INO_KERNELDIR,  (int)S_IFDIR, 1);
            emit(ctx, "devices", 7, SYSFS_INO_DEVICESDIR, (int)S_IFDIR, 2);
            emit(ctx, "class",   5, SYSFS_INO_CLASSDIR,   (int)S_IFDIR, 3);
            return 0;
        case SYS_K_BLOCKDIR:
        case SYS_K_CLASSBLOCK:
            return sysfs_emit_bdevs(f, ctx, emit);
        case SYS_K_BLOCKDEV: {
            struct bdev *b = sysfs_bdev(ii->idx);
            if(!b)return 0;
            for(int k = 0;k < SYSFS_NBDEVFILE;k ++){
                const char *n = g_sysfs_bdev_files[k].name;
                emit(ctx, n, (int)strlen(n), SYSFS_INO_BDEV_F(ii->idx, k), (int)S_IFREG, k);
            }
            return 0;
        }
        case SYS_K_KERNELDIR:
            for(int k = 0;k < SYSFS_NKFILE;k ++){
                const char *n = g_sysfs_kernel_files[k].name;
                emit(ctx, n, (int)strlen(n), g_sysfs_kernel_files[k].ino, (int)S_IFREG, k);
            }
            return 0;
        case SYS_K_CLASSDIR:
            emit(ctx, "block", 5, SYSFS_INO_CLASSBLOCK, (int)S_IFDIR, 0);
            return 0;
        default:
            return 0;
    }
}

//按种类生成属性内容
static int sysfs_gen(char *out, int cap, const sysfs_inode_info_t *ii){
    switch(ii->kind){
        case SYS_K_OSTYPE:
            return sprintf(out, "%s\n", system_utsname.sysname);
        case SYS_K_OSRELEASE:
            return sprintf(out, "%s\n", system_utsname.release);
        case SYS_K_VERSION:
            return sprintf(out, "%s %s\n", system_utsname.version, system_utsname.machine);
        case SYS_K_SIZE: {
            struct bdev *b = sysfs_bdev(ii->idx);
            if(!b)return -ENODEV;
            return sprintf(out, "%lu\n", (unsigned long)b->bd_nr_sectors);
        }
        case SYS_K_RO:
            return sprintf(out, "0\n");
        case SYS_K_REMOVABLE:
            return sprintf(out, "0\n");
        case SYS_K_DEV: {
            if(!sysfs_bdev(ii->idx))return -ENODEV;
            return sprintf(out, "259:%u\n", (unsigned)ii->idx);
        }
        default:
            return -EINVAL;
    }
}

//读属性文件
static int64_t sysfs_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    if(!f || !f->f_inode || !buf || !pos)return -EINVAL;
    sysfs_inode_info_t *ii = SYSFS_INFO(f->f_inode);
    if(!ii)return -EINVAL;
    char tmp[SYSFS_BUF_MAX];
    int n = sysfs_gen(tmp, (int)sizeof(tmp), ii);
    if(n < 0)return n;
    uint64_t off = *pos;
    if(off >= (uint64_t)n)return 0;
    uint64_t k = (uint64_t)n - off;
    if(k > len)k = len;
    memcpy(buf, tmp + off, (size_t)k);
    *pos = off + k;
    return (int64_t)k;
}

static const struct inode_operations g_sysfs_dir_iops = {
    .lookup = sysfs_lookup,
};

static const struct file_operations g_sysfs_dir_fops = {
    .iterate = sysfs_iterate,
};

static const struct file_operations g_sysfs_file_fops = {
    .read = sysfs_read,
};

uint32_t SysFsInodeSize(void){
    return (uint32_t)(sizeof(struct inode) + sizeof(sysfs_inode_info_t));
}

const struct super_operations *SysFsSuperOps(void){
    return NULL;
}

//建立sysfs的超级块与根目录
struct dentry *SysFsFillSuper(struct super_block *sb){
    if(!sb)return NULL;
    g_sysfs_nbdev = 0;
    memset(g_sysfs_bdev, 0, sizeof(g_sysfs_bdev));
    int n = BlkBdevCount();
    for(int i = 0;i < n && g_sysfs_nbdev < SYSFS_MAX_BDEV;i ++){
        struct bdev *b = BlkBdevAt(i);
        if(!b)continue;
        g_sysfs_bdev[g_sysfs_nbdev ++] = b;
    }
    struct inode *root = sysfs_iget(sb, SYSFS_INO_ROOT, SYS_K_ROOT, 0);
    if(!root)return NULL;
    struct dentry *d = d_alloc(NULL, "/");
    if(!d){
        Iput(root);
        return NULL;
    }
    d_add(d, root);
    Iput(root);
    sb->s_root = d;
    sb->s_root_inode = root;
    return d;
}
