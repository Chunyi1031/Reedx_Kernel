// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/cgroup2.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * cgroup v2 统一层次的最小实现
 *
 * 由DeepSeek V4.1 Flash生成
 */
#include <vfs.h>
#include <mm/kslab.h>
#include <print.h>

//节点种类
enum {
    CG2_K_DIR = 0,  //目录(根目录也是目录)
    CG2_K_FILE,     //伪文件
};

//伪文件编号
enum {
    CG2_F_CONTROLLERS = 0,//可用控制器
    CG2_F_SUBTREE,        //子树启用的控制器
    CG2_F_PROCS,          //进程列表
    CG2_F_THREADS,        //线程列表
    CG2_F_EVENTS,         //populated/frozen
    CG2_F_TYPE,           //domain/threaded
    CG2_F_STAT,           //层级统计
    CG2_F_MAXDEPTH,       //最大深度
    CG2_F_MAXDESC,        //最大后代数
    CG2_F_FREEZE,         //冻结开关
    CG2_F_KILL,           //杀光成员
    CG2_F_PRESSURE,       //PSI(占位)
    CG2_F_PIDS_CURRENT,   //当前进程数
    CG2_F_PIDS_MAX,       //进程数上限
    CG2_F_CPUSET_CPUS,    //允许的CPU
    CG2_F_CPUSET_MEMS,    //允许的内存节点
    CG2_F_CPUSET_CPUS_EF, //生效的CPU
    CG2_F_CPUSET_MEMS_EF, //生效的内存节点
};

//每个cgroup目录/伪文件都有的伪文件表
static const struct {
    const char *name;   //文件名
    uint16_t    mode;   //权限
    _Bool       writable;//是否允许写
} g_cg2_files[] = {
    {"cgroup.controllers",      S_IFREG | 0444, false},
    {"cgroup.subtree_control",  S_IFREG | 0644, true },
    {"cgroup.procs",            S_IFREG | 0644, true },
    {"cgroup.threads",          S_IFREG | 0644, true },
    {"cgroup.events",           S_IFREG | 0444, false},
    {"cgroup.type",             S_IFREG | 0444, false},
    {"cgroup.stat",             S_IFREG | 0444, false},
    {"cgroup.max.depth",        S_IFREG | 0644, true },
    {"cgroup.max.descendants",  S_IFREG | 0644, true },
    {"cgroup.freeze",           S_IFREG | 0644, true },
    {"cgroup.kill",             S_IFREG | 0200, true },
    {"cgroup.pressure",         S_IFREG | 0444, false},
    {"pids.current",            S_IFREG | 0444, false},
    {"pids.max",                S_IFREG | 0644, true },
    {"cpuset.cpus",             S_IFREG | 0644, true },
    {"cpuset.mems",             S_IFREG | 0644, true },
    {"cpuset.cpus.effective",   S_IFREG | 0444, false},
    {"cpuset.mems.effective",   S_IFREG | 0444, false},
};
#define CG2_NFILES ((int)(sizeof(g_cg2_files) / sizeof(g_cg2_files[0])))

typedef struct cg2_inode_info {
    uint32_t         kind;    //CG2_K_*
    uint32_t         fid;     //伪文件编号(CG2_K_FILE时有效)
    struct list_node children;//子目录dentry链(挂在d_alias上)
} cg2_inode_info_t;

static const struct inode_operations g_cg2_dir_iops;//目录操作表
static const struct file_operations  g_cg2_dir_fops;//目录遍历操作表
static const struct file_operations  g_cg2_file_fops;//伪文件读写操作表

#define CG2_INFO(inode) ((cg2_inode_info_t*)(inode)->i_private)

//伪文件的inode号:(所在目录ino << 16) | 0x8000 | 文件编号
#define CG2_INO_FILE(dino, fid) ((uint64_t)(((uint64_t)(dino) << 16) | 0x8000ULL | (uint64_t)(fid)))
#define CG2_INO_IS_FILE(ino)    (((ino) & 0x8000ULL) != 0)

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

//目录的children链用的是 dentry->d_alias 节点
#define alias_dentry(node) ((struct dentry*)((uintptr_t)(node) - __builtin_offsetof(struct dentry, d_alias)))

typedef int (*dir_emit_t)(void *ctx, const char *name, int namelen,
                          uint64_t ino, int type, int64_t next_off);

//==================== 内容生成 ====================
//根目录是"最后一级",可以启用控制器;子目录沿用同样的文本
static int cg2_gen(char *out, int cap, uint32_t fid){
    switch(fid){
        case CG2_F_CONTROLLERS:
            return sprintf(out, "cpuset pids\n");
        case CG2_F_SUBTREE:
            return 0;//初始没有启用任何控制器
        case CG2_F_PROCS:
        case CG2_F_THREADS:
            return 0;//不做真实记账,返回空表
        case CG2_F_EVENTS:
            return sprintf(out, "populated 0\nfrozen 0\n");
        case CG2_F_TYPE:
            return sprintf(out, "domain\n");
        case CG2_F_STAT:
            return sprintf(out, "nr_descendants 0\nnr_dying_descendants 0\n");
        case CG2_F_MAXDEPTH:
        case CG2_F_MAXDESC:
            return sprintf(out, "max\n");
        case CG2_F_FREEZE:
            return sprintf(out, "0\n");
        case CG2_F_KILL:
            return 0;//只写
        case CG2_F_PRESSURE:
            return sprintf(out, "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
        case CG2_F_PIDS_CURRENT:
            return sprintf(out, "1\n");
        case CG2_F_PIDS_MAX:
            return sprintf(out, "max\n");
        case CG2_F_CPUSET_CPUS:
        case CG2_F_CPUSET_MEMS:
        case CG2_F_CPUSET_CPUS_EF:
        case CG2_F_CPUSET_MEMS_EF:
            return sprintf(out, "0\n");
        default:
            return -EINVAL;
    }
    (void)cap;
}

//==================== inode ====================
static struct inode *cg2_iget_dir(struct super_block *sb, uint64_t ino){
    struct inode *inode = IgetLocked(sb, ino);
    if(!inode)return NULL;
    cg2_inode_info_t *ci = CG2_INFO(inode);
    if(!ci)return NULL;
    ci->kind = CG2_K_DIR;
    ci->fid  = 0;
    ci->children.prev = &ci->children;
    ci->children.next = &ci->children;
    inode->i_mode  = (uint16_t)(S_IFDIR | 0755);
    inode->i_uid   = 0;
    inode->i_gid   = 0;
    inode->i_size  = 0;
    inode->i_nlink = 2;
    inode->i_op    = &g_cg2_dir_iops;
    inode->i_fop   = &g_cg2_dir_fops;
    inode->i_state &= ~I_NEW;
    return inode;
}

static struct inode *cg2_iget_file(struct super_block *sb, uint64_t ino, uint32_t fid){
    struct inode *inode = IgetLocked(sb, ino);
    if(!inode)return NULL;
    cg2_inode_info_t *ci = CG2_INFO(inode);
    if(!ci)return NULL;
    ci->kind = CG2_K_FILE;
    ci->fid  = fid;
    ci->children.prev = &ci->children;
    ci->children.next = &ci->children;
    inode->i_mode  = g_cg2_files[fid].mode;
    inode->i_uid   = 0;
    inode->i_gid   = 0;
    inode->i_size  = 0;
    inode->i_nlink = 1;
    inode->i_op    = NULL;
    inode->i_fop   = &g_cg2_file_fops;
    inode->i_state &= ~I_NEW;
    return inode;
}

//==================== 目录操作 ====================
static void cg2_link_child(struct inode *dir, struct dentry *dentry){
    cg2_inode_info_t *ci = CG2_INFO(dir);
    if(!ci)return;
    l_add_tail(&dentry->d_alias, &ci->children);
}

static void cg2_unlink_child(struct inode *dir, struct dentry *dentry){
    (void)dir;
    if(dentry->d_alias.next)l_del(&dentry->d_alias);
}

static int cg2_find_file(const char *name, uint32_t len){
    for(int i = 0;i < CG2_NFILES;i ++){
        if(strlen(g_cg2_files[i].name) != len)continue;
        if(memcmp(g_cg2_files[i].name, name, len) == 0)return i;
    }
    return -1;
}

static struct dentry *cg2_lookup(struct inode *dir, struct dentry *dentry, unsigned flags){
    (void)flags;
    if(!dir || !dentry)return NULL;
    cg2_inode_info_t *ci = CG2_INFO(dir);
    if(!ci){
        d_add(dentry, NULL);
        return NULL;
    }
    if(ci->kind == CG2_K_DIR){
        //先查伪文件
        int fid = cg2_find_file(dentry->d_name, dentry->d_name_len);
        if(fid >= 0){
            struct inode *inode = cg2_iget_file(dir->i_sb, CG2_INO_FILE(dir->i_ino, (uint32_t)fid), (uint32_t)fid);
            if(!inode){
                d_add(dentry, NULL);
                return NULL;
            }
            d_add(dentry, inode);
            Iput(inode);
            return dentry;
        }
        //再查子目录
        struct list_node *n;
        for(n = ci->children.next;n != &ci->children;n = n->next){
            struct dentry *d = alias_dentry(n);
            if(!d->d_inode)continue;
            if(d->d_name_len == dentry->d_name_len &&
               memcmp(d->d_name, dentry->d_name, d->d_name_len) == 0){
                d_add(dentry, d->d_inode);
                return dentry;
            }
        }
    }
    d_add(dentry, NULL);//负缓存
    return NULL;
}

static int cg2_mkdir(struct inode *dir, struct dentry *dentry, uint16_t mode){
    if(!dir || !dentry)return -EINVAL;
    //不许用伪文件的名字建目录
    if(cg2_find_file(dentry->d_name, dentry->d_name_len) >= 0)return -EEXIST;
    struct super_block *sb = dir->i_sb;
    struct inode *inode = cg2_iget_dir(sb, sb->s_ino_next ++);
    if(!inode)return -ENOSPC;
    inode->i_mode = (uint16_t)((inode->i_mode & S_IFMT) | (mode & 07777));
    d_add(dentry, inode);
    Iput(inode);
    cg2_link_child(dir, dentry);
    dir->i_nlink ++;
    return 0;
}

static int cg2_dir_empty(struct inode *dir){
    cg2_inode_info_t *ci = CG2_INFO(dir);
    if(!ci)return 0;
    struct list_node *n;
    for(n = ci->children.next;n != &ci->children;n = n->next){
        if(alias_dentry(n)->d_inode)return 0;
    }
    return 1;
}

static int cg2_rmdir(struct inode *dir, struct dentry *dentry){
    if(!dentry || !dentry->d_inode)return -ENOENT;
    if((dentry->d_inode->i_mode & S_IFMT) != S_IFDIR)return -ENOTDIR;
    if(!cg2_dir_empty(dentry->d_inode))return -ENOTEMPTY;
    cg2_unlink_child(dir, dentry);
    if(dentry->d_inode->i_nlink)dentry->d_inode->i_nlink --;
    if(dir->i_nlink > 1)dir->i_nlink --;
    d_invalidate(dentry);
    return 0;
}

//cgroupfs 不允许用户造普通文件/设备节点/硬链接
static int cg2_create(struct inode *dir, struct dentry *dentry, uint16_t mode){
    (void)dir; (void)dentry; (void)mode;
    return -EPERM;
}

static int cg2_unlink(struct inode *dir, struct dentry *dentry){
    (void)dir; (void)dentry;
    return -EPERM;
}

static int cg2_getattr(const struct path *path, struct stat *st){
    if(!path || !path->dentry || !path->dentry->d_inode)return -ENOENT;
    struct inode *inode = path->dentry->d_inode;
    memset(st, 0, sizeof(*st));
    st->st_ino     = inode->i_ino;
    st->st_mode    = inode->i_mode;
    st->st_nlink   = inode->i_nlink;
    st->st_uid     = inode->i_uid;
    st->st_gid     = inode->i_gid;
    st->st_size    = (int64_t)inode->i_size;
    st->st_blocks  = (int64_t)((inode->i_size + 511) / 512);
    st->st_blksize = 4096;
    st->st_atime   = inode->i_atime_s;
    st->st_mtime   = inode->i_mtime_s;
    st->st_ctime   = inode->i_ctime_s;
    return 0;
}

//==================== 文件操作 ====================
static int64_t cg2_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    if(!f || !f->f_inode || !buf || !pos)return -EINVAL;
    cg2_inode_info_t *ci = CG2_INFO(f->f_inode);
    if(!ci || ci->kind != CG2_K_FILE)return -EISDIR;
    char tmp[128];
    int n = cg2_gen(tmp, (int)sizeof(tmp), ci->fid);
    if(n < 0)return n;
    if(*pos >= (uint64_t)n)return 0;
    uint64_t k = (uint64_t)n - *pos;
    if(k > len)k = len;
    memcpy(buf, tmp + *pos, (size_t)k);
    *pos += k;
    return (int64_t)k;
}

//写入一律接受但不做任何事(没有真实的资源控制)
static int64_t cg2_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    if(!f || !f->f_inode)return -EINVAL;
    cg2_inode_info_t *ci = CG2_INFO(f->f_inode);
    if(!ci || ci->kind != CG2_K_FILE)return -EISDIR;
    if(!g_cg2_files[ci->fid].writable)return -EPERM;
    if(pos)*pos += len;
    return (int64_t)len;
}

static int cg2_iterate(struct file *f, void *ctx, dir_emit_t emit){
    if(!f || !f->f_inode || !emit)return -EINVAL;
    struct inode *dir = f->f_inode;
    cg2_inode_info_t *ci = CG2_INFO(dir);
    if(!ci)return -ENOTDIR;
    int64_t off = 0;
    //先报伪文件
    for(int i = 0;i < CG2_NFILES;i ++){
        emit(ctx, g_cg2_files[i].name, (int)strlen(g_cg2_files[i].name),
             CG2_INO_FILE(dir->i_ino, (uint32_t)i), (int)(g_cg2_files[i].mode & S_IFMT), off ++);
    }
    //再报子目录
    struct list_node *n;
    for(n = ci->children.next;n != &ci->children;n = n->next){
        struct dentry *d = alias_dentry(n);
        if(!d->d_inode)continue;
        emit(ctx, d->d_name, (int)d->d_name_len, d->d_inode->i_ino, (int)S_IFDIR, off ++);
    }
    return 0;
}

static const struct inode_operations g_cg2_dir_iops = {
    .lookup = cg2_lookup,
    .mkdir  = cg2_mkdir,
    .rmdir  = cg2_rmdir,
    .create = cg2_create,
    .unlink = cg2_unlink,
    .getattr= cg2_getattr,
};

static const struct file_operations g_cg2_dir_fops = {
    .iterate = cg2_iterate,
};

static const struct file_operations g_cg2_file_fops = {
    .read  = cg2_read,
    .write = cg2_write,
};

//==================== 挂载 ====================
uint32_t Cg2InodeSize(void){
    return (uint32_t)(sizeof(struct inode) + sizeof(cg2_inode_info_t));
}

const struct super_operations *Cg2SuperOps(void){ return NULL; }

/**
 * 建立 cgroup2 的超级块与根目录
 * @return 根 dentry(失败返回 NULL)
 */
struct dentry *Cg2FillSuper(struct super_block *sb){
    if(!sb)return NULL;
    sb->s_ino_next = 2;//1 号留给根目录
    struct inode *root = cg2_iget_dir(sb, 1);
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
