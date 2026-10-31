// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/vfs.c
 * 
 * Copyright (C) 2026 Liu Chunyi
 * 
 */
#include <fs.h>
#include <syscalls.h>
#include <task.h>
#include <rtc.h>
#include <print.h>
#include <mm/pagecache.h>
#include <drives/bcache.h>
#include <drives/cdev.h>
#include <vfs.h>

static struct path g_vfs_root;//新VFS的根
static spinlock_t g_fs_lock = {0};//全局文件系统锁

//获取当前VFS的根目录
const struct path *VfsRoot(void){
    return g_vfs_root.dentry ? &g_vfs_root : NULL;
}

//当前工作目录(任务没设置过就是根)
static const struct path *cwd_path(void){
    if(current_task && current_task->fs_cwd.dentry)return &current_task->fs_cwd;
    return VfsRoot();
}

//解析起点
static const struct path *lookup_root(const char *path){
    if(path && path[0] == '/')return VfsRoot();
    const struct path *c = cwd_path();
    return c ? c : VfsRoot();
}

//切换工作目录
int FsChdir(const char *path){
    if(!current_task || !path)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    int r = path_lookup(path, lookup_root(path), LOOKUP_DIRECTORY, &p);
    if(r == 0){
        struct dentry *old = current_task->fs_cwd.dentry;
        current_task->fs_cwd = p;//path_lookup 返回的引用直接归 cwd 所有
        if(old)dput(old);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//取当前工作目录的绝对路径(返回字符串长度, 不含结尾 NUL)
int FsGetcwd(char *buf, int size){
    const struct path *c = cwd_path();
    if(!c || !c->dentry)return -ENOENT;
    return d_path(c, buf, size);
}

//在目录dentry下查一个名字，返回带引用的dentry
static struct dentry *dir_lookup(struct dentry *dir, const char *name){
    if(!dir || !dir->d_inode || !name || !*name)return NULL;
    if((dir->d_inode->i_mode & S_IFMT) != S_IFDIR)return NULL;
    struct dentry *d = d_lookup(dir, name);
    if(d)return d;
    if(!dir->d_inode->i_op || !dir->d_inode->i_op->lookup)return NULL;
    d = d_alloc(dir, name);
    if(!d)return NULL;
    dir->d_inode->i_op->lookup(dir->d_inode, d, 0);
    return d;
}

//在目录 dentry 下创建; 成功返回0, out 得到新 dentry(带引用)
static int dir_create(struct dentry *dir, const char *name, int isdir, uint16_t mode, struct dentry **out){
    if(!dir || !dir->d_inode || !name || !*name)return -ENOTDIR;
    struct inode *din = dir->d_inode;
    int (*fn)(struct inode*, struct dentry*, uint16_t) = NULL;
    if(din->i_op)fn = isdir ? din->i_op->mkdir : din->i_op->create;
    if(!fn)return -EROFS;
    //检查在父目录的写权限和搜索权限
    int pr = InodePermission(din, MAY_WRITE | MAY_EXEC);
    if(pr)return pr;
    struct dentry *d = dir_lookup(dir, name);
    if(!d)return -ENOMEM;
    if(d->d_inode){
        dput(d);
        return -EEXIST;
    }
    //新建文件的权限位要扣掉本进程的umask
    uint16_t m = (uint16_t)(mode & ~CurrentCred()->umask & 07777);
    int r = fn(din, d, m);
    if(r){ dput(d); return r; }
    if(out)*out = d;
    else dput(d);
    return 0;
}

//在目录 dentry 下删除(isdir=1 表示删目录)
static int dir_remove(struct dentry *dir, const char *name, int isdir){
    if(!dir || !dir->d_inode)return -ENOTDIR;
    struct inode *din = dir->d_inode;
    //检查在父目录的写权限和搜索权限
    int pr = InodePermission(din, MAY_WRITE | MAY_EXEC);
    if(pr)return pr;
    struct dentry *d = dir_lookup(dir, name);
    if(!d)return -ENOMEM;
    int r;
    if(!d->d_inode)r = -ENOENT;
    else if(isdir)r = (din->i_op && din->i_op->rmdir) ? din->i_op->rmdir(din, d) : -EISDIR;
    else if((d->d_inode->i_mode & S_IFMT) == S_IFDIR)r = -EISDIR;
    else r = (din->i_op && din->i_op->unlink) ? din->i_op->unlink(din, d) : -EROFS;
    dput(d);
    return r;
}

//改名/移动
static int dir_rename(struct dentry *odir, const char *oname, struct dentry *ndir, const char *nname, int flags){
    if(!odir || !ndir || !odir->d_inode || !ndir->d_inode)return -ENOTDIR;
    struct inode *oi = odir->d_inode;
    struct inode *ni = ndir->d_inode;
    if(oi->i_sb != ni->i_sb)return -EXDEV;
    if(!oi->i_op || !oi->i_op->rename)return -EROFS;
    if(!nname || !*nname)return -ENOENT;
    //检查两边父目录是否都可写
    int pr = InodePermission(oi, MAY_WRITE | MAY_EXEC);
    if(pr)return pr;
    pr = InodePermission(ni, MAY_WRITE | MAY_EXEC);
    if(pr)return pr;
    struct dentry *od = dir_lookup(odir, oname);
    if(!od)return -ENOMEM;
    if(!od->d_inode){ dput(od); return -ENOENT; }
    struct dentry *nd = dir_lookup(ndir, nname);
    if(!nd){ dput(od); return -ENOMEM; }
    int r;
    if((flags & RENAME_NOREPLACE) && nd->d_inode)r = -EEXIST;
    else r = oi->i_op->rename(oi, od, ni, nd);
    dput(nd);
    dput(od);
    return r;
}

//打开标志与inode类型是否匹配
static int open_check(struct inode *ino, int flags){
    uint16_t type = (uint16_t)(ino->i_mode & S_IFMT);
    if((flags & O_DIRECTORY) && type != S_IFDIR)return -ENOTDIR;
    if(type == S_IFDIR && (flags & O_ACCMODE) != O_RDONLY)return -EISDIR;
    return 0;
}

//把 dentry 变成打开文件描述; 成功时接管 d 的引用
static int file_bind(fs_file_t *out, struct dentry *d, struct vfsmount *mnt, int flags){
    struct inode *ino = d->d_inode;
    //打开方式决定要读权限还是写权限
    int acc  = flags & O_ACCMODE;
    int mask = (acc == O_WRONLY) ? MAY_WRITE
             : (acc == O_RDWR) ? (MAY_READ | MAY_WRITE)
             : MAY_READ;
    int r = InodePermission(ino, mask);
    if(r){ dput(d); return r; }
    struct file *file = FileAlloc();
    if(!file){ dput(d); return -ENOMEM; }
    file->f_inode = ino;
    file->f_op = FileOpsOf(ino);
    if(!file->f_op && (ino->i_mode & S_IFMT) == S_IFCHR){
        FilePut(file);
        return -ENXIO;
    }
    file->f_flags = (uint32_t)flags;
    file->f_pos   = (flags & O_APPEND) ? ino->i_size : 0;
    file->f_path.mnt    = mnt;
    file->f_path.dentry = d;
    if(file->f_op && file->f_op->open){
        int r = file->f_op->open(ino, file);
        if(r){ FilePut(file); return r; }//FilePut 会归还目录项
    }
    out->f      = file;
    out->inode  = ino;
    out->dentry = d;
    out->mnt    = mnt;
    out->flags  = flags;
    out->pipe   = NULL;
    out->used   = true;
    return 0;
}

//已解析的 dentry → 打开文件(接管 d 的引用)
static int open_dentry(struct dentry *d, struct vfsmount *mnt, int flags, fs_file_t *out){
    if(!d || !d->d_inode){ dput(d); return -ENOENT; }
    int r = open_check(d->d_inode, flags);
    if(r){ dput(d); return r; }
    //O_TRUNC: 打开时把文件截成 0
    if((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY &&
       (d->d_inode->i_mode & S_IFMT) == S_IFREG){
        struct inode *ino = d->d_inode;
        if(ino->i_op && ino->i_op->setattr){
            struct iattr ia;
            memset(&ia, 0, sizeof(ia));
            ia.ia_valid = ATTR_SIZE;
            ia.ia_size  = 0;
            ino->i_op->setattr(d, &ia);
        }
    }
    return file_bind(out, d, mnt, flags);
}

static int path_create(const char *path, int isdir, uint16_t mode, struct dentry **out){
    const struct path *root = lookup_root(path);
    if(!root)return -ENOENT;
    struct path pdir;
    char name[NAMELEN_MAX + 1];
    int r = path_lookup_parent(path, root, &pdir, name, sizeof(name));
    if(r)return r;
    r = dir_create(pdir.dentry, name, isdir, mode, out);
    dput(pdir.dentry);
    return r;
}

int FsOpen(const char *path, int flags, fs_file_t *out){
    const struct path *root = lookup_root(path);
    if(!out || !root || !path)return -ENOENT;
    if((flags & O_TMPFILE) == O_TMPFILE)return -EOPNOTSUPP;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    int r = path_lookup(path, root, (flags & O_DIRECTORY) ? LOOKUP_DIRECTORY : 0, &p);
    if(r == 0){
        if((flags & O_CREAT) && (flags & O_EXCL)){//已经存在
            dput(p.dentry);
            r = -EEXIST;
        }
    }else if(r == -ENOENT && (flags & O_CREAT)){
        r = path_create(path, 0, 0644, &p.dentry);
        if(r == -EEXIST){//别的任务抢先建好了
            if(flags & O_EXCL)goto done;
            r = path_lookup(path, root, 0, &p);
        }
        p.mnt = root->mnt;
    }
    if(r == 0)r = open_dentry(p.dentry, p.mnt, flags, out);
done:
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//在目录 dentry 下打开(openat 用)
int FsOpenAt(struct dentry *dir, const char *name, int flags, fs_file_t *out){
    if(!out || !dir || !name || !*name)return -EINVAL;
    if((flags & O_TMPFILE) == O_TMPFILE)return -EOPNOTSUPP;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r;
    struct dentry *d = dir_lookup(dir, name);
    if(!d){ r = -ENOMEM; goto done; }
    if(d->d_inode){
        if((flags & O_CREAT) && (flags & O_EXCL)){ dput(d); r = -EEXIST; goto done; }
    }else{
        if(!(flags & O_CREAT)){ dput(d); r = -ENOENT; goto done; }
        dput(d);
        r = dir_create(dir, name, 0, 0644, &d);
        if(r == -EEXIST){
            d = dir_lookup(dir, name);
            r = d ? 0 : -ENOENT;
        }
        if(r || !d){ if(d)dput(d); r = r ? r : -ENOENT; goto done; }
    }
    r = open_dentry(d, NULL, flags, out);
done:
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

void FsClose(fs_file_t *f){
    if(!f || !f->used)return;
    if(f->f){
        if(f->f->f_op && f->f->f_op->release)f->f->f_op->release(f->f->f_inode, f->f);
        FilePut(f->f);          //最后一个引用会归还 f_path 里的目录项
        f->f = NULL;
    }
    f->dentry = NULL;           //借用指针, 生命周期跟着 f
    f->inode  = NULL;
    f->mnt    = NULL;
    f->used   = false;
}

//fd 对应的目录 dentry
struct dentry *FsDirOfFd(int fd){
    if(!current_task || fd < 0 || fd >= MAX_FD)return NULL;
    struct dentry *d = current_task->files[fd].dentry;
    if(!d || !d->d_inode)return NULL;
    if((d->d_inode->i_mode & S_IFMT) != S_IFDIR)return NULL;
    return d;
}

//文件当前长度
static uint64_t fs_file_size(fs_file_t *f){
    if(!f || !f->inode)return 0;
    return f->inode->i_size;
}

uint64_t FsRead(fs_file_t *f, void *buf, uint64_t len){
    if(!f || !f->used || !f->f || !f->inode)return 0;
    if((f->flags & O_ACCMODE) == O_WRONLY)return 0;
    const struct file_operations *op = f->f->f_op ? f->f->f_op : f->inode->i_fop;
    if(!op || !op->read)return 0;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int64_t n = op->read(f->f, (char*)buf, len, &f->f->f_pos);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return (n > 0) ? (uint64_t)n : 0;
}

uint64_t FsWrite(fs_file_t *f, const void *buf, uint64_t len){
    if(!f || !f->used || !f->f || !f->inode)return 0;
    if((f->flags & O_ACCMODE) == O_RDONLY)return 0;
    const struct file_operations *op = f->f->f_op ? f->f->f_op : f->inode->i_fop;
    if(!op || !op->write)return 0;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    if(f->flags & O_APPEND)f->f->f_pos = f->inode->i_size;//追加模式定位到末尾
    int64_t n = op->write(f->f, (const char*)buf, len, &f->f->f_pos);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return (n > 0) ? (uint64_t)n : 0;
}

int FsSeek(fs_file_t *f, int64_t off, int whence){
    if(!f || !f->used || !f->f)return -1;
    int64_t base;
    if(whence == SEEK_SET)base = 0;
    else if(whence == SEEK_CUR)base = (int64_t)f->f->f_pos;
    else if(whence == SEEK_END)base = (int64_t)fs_file_size(f);
    else return -1;
    int64_t npos = base + off;
    if(npos < 0)return -1;
    f->f->f_pos = (uint64_t)npos;
    return (int)f->f->f_pos;
}

uint64_t FsSize(fs_file_t *f){
    return fs_file_size(f);
}

//按fd截断/扩展文件
int FsTruncate(fs_file_t *f, uint64_t len){
    if(!f || !f->used || f->pipe || !f->f || !f->inode)return -EINVAL;
    if((f->flags & O_ACCMODE) == O_RDONLY)return -EINVAL;
    if((f->inode->i_mode & S_IFMT) == S_IFDIR)return -EISDIR;
    if(!f->inode->i_op || !f->inode->i_op->setattr)return -EROFS;
    struct iattr ia;
    memset(&ia, 0, sizeof(ia));
    ia.ia_valid = ATTR_SIZE;
    ia.ia_size  = len;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = f->inode->i_op->setattr(f->dentry, &ia);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//按路径解析出 dentry(带引用)
static int lookup_path(const char *path, struct path *out){
    const struct path *root = lookup_root(path);
    if(!root || !path || !out)return -ENOENT;
    return path_lookup(path, root, 0, out);
}

int FsTruncatePath(const char *path, uint64_t len){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    int r = lookup_path(path, &p);
    if(r)goto done;
    struct inode *ino = p.dentry->d_inode;
    if((ino->i_mode & S_IFMT) == S_IFDIR)r = -EISDIR;
    else if(!ino->i_op || !ino->i_op->setattr)r = -EROFS;
    else{
        struct iattr ia;
        memset(&ia, 0, sizeof(ia));
        ia.ia_valid = ATTR_SIZE;
        ia.ia_size  = len;
        r = ino->i_op->setattr(p.dentry, &ia);
    }
    dput(p.dentry);
done:
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

/*==================== 目录项遍历 ====================*/
typedef struct dents_ctx {
    char    *buf;
    uint64_t len;
    int      written;
    uint64_t skip;   //还要跳过多少个真实条目
    uint64_t seen;   //已经产出过的真实条目数
    uint64_t next;   //下一次从哪个 f_pos 继续
    int      stop;   //缓冲区放不下了
} dents_ctx_t;

//把一条目录项写进缓冲
static int dent_emit(void *ctx, const char *name, int namelen, uint64_t ino, int type, int64_t off){
    (void)off;
    dents_ctx_t *c = (dents_ctx_t*)ctx;
    if(c->stop)return 1;
    c->seen ++;
    if(c->skip){//跳过已读过的
        c->skip --;
        c->next = 2 + c->seen;
        return 0;
    }
    int reclen = (19 + namelen + 1 + 7) & ~7;
    if((uint64_t)c->written + (uint64_t)reclen > c->len){
        c->stop = 1;
        c->seen --;
        return 1;
    }
    linux_dirent64_t *d = (linux_dirent64_t*)(c->buf + c->written);
    d->d_ino    = ino;
    d->d_off    = (int64_t)(2 + c->seen);
    d->d_reclen = (uint16_t)reclen;
    d->d_type   = (uint8_t)((type == S_IFDIR) ? DT_DIR
                          : (type == S_IFREG) ? DT_REG
                          : (type == S_IFLNK) ? DT_LNK
                          : (type == S_IFCHR) ? DT_CHR
                          : (type == S_IFBLK) ? DT_BLK
                          : (type == S_IFIFO) ? DT_FIFO
                          : (type == S_IFSOCK) ? DT_SOCK : DT_UNKNOWN);
    memcpy(d->d_name, name, (size_t)namelen);
    d->d_name[namelen] = 0;
    c->written += reclen;
    c->next     = 2 + c->seen;
    return 0;
}

//往缓冲里塞一条虚拟的 "." 或 ".."
static int dent_put_virtual(dents_ctx_t *c, const char *name, uint64_t ino){
    int namelen = (int)strlen(name);
    int reclen  = (19 + namelen + 1 + 7) & ~7;
    if((uint64_t)c->written + (uint64_t)reclen > c->len)return -1;
    linux_dirent64_t *d = (linux_dirent64_t*)(c->buf + c->written);
    d->d_ino    = ino;
    d->d_off    = (int64_t)(c->next);
    d->d_reclen = (uint16_t)reclen;
    d->d_type   = (uint8_t)DT_DIR;
    memcpy(d->d_name, name, (size_t)namelen + 1);
    c->written += reclen;
    c->next ++;
    return 0;
}

//读目录项到缓冲区(返回写入字节数)
int FsGetdents(fs_file_t *f, void *buf, uint64_t len){
    if(!f || !f->used || !f->f || !buf)return -EINVAL;
    if(!f->inode)return -EBADF;
    if((f->inode->i_mode & S_IFMT) != S_IFDIR)return -ENOTDIR;
    const struct file_operations *op = f->f->f_op ? f->f->f_op : f->inode->i_fop;
    if(!op || !op->iterate)return -ENOSYS;
    dents_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.buf  = (char*)buf;
    c.len  = len;
    c.next = f->f->f_pos;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    if(c.next < 2){
        if(c.next == 0 && dent_put_virtual(&c, ".", f->inode->i_ino))goto finish;
        if(c.next == 1 && dent_put_virtual(&c, "..", f->inode->i_ino))goto finish;
    }
    c.skip = (f->f->f_pos > 2) ? (f->f->f_pos - 2) : 0;
    op->iterate(f->f, &c, dent_emit);
finish:
    f->f->f_pos = c.next;
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return c.written;
}

int FsMkdir(const char *path, int mode){
    if(!lookup_root(path))return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = path_create(path, 1, (uint16_t)mode, NULL);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsMkdirAt(struct dentry *dir, const char *name, int mode){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = dir_create(dir, name, 1, (uint16_t)mode, NULL);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsUnlink(const char *path, int isdir){
    const struct path *root = lookup_root(path);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path pdir;
    char name[NAMELEN_MAX + 1];
    int r = path_lookup_parent(path, root, &pdir, name, sizeof(name));
    if(r == 0){
        r = dir_remove(pdir.dentry, name, isdir);
        dput(pdir.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsUnlinkAt(struct dentry *dir, const char *name, int isdir){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = dir_remove(dir, name, isdir);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsRename(const char *oldpath, const char *newpath, int flags){
    if(!oldpath || !newpath)return -EINVAL;
    const struct path *oroot = lookup_root(oldpath);
    const struct path *nroot = lookup_root(newpath);
    if(!oroot || !nroot)return -EINVAL;
    if(strcmp(oldpath, newpath) == 0)return 0;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path opdir, npdir;
    char oname[NAMELEN_MAX + 1], nname[NAMELEN_MAX + 1];
    int r = path_lookup_parent(oldpath, oroot, &opdir, oname, sizeof(oname));
    if(r)goto done;
    r = path_lookup_parent(newpath, nroot, &npdir, nname, sizeof(nname));
    if(r){ dput(opdir.dentry); goto done; }
    r = dir_rename(opdir.dentry, oname, npdir.dentry, nname, flags);
    dput(npdir.dentry);
    dput(opdir.dentry);
done:
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsRenameAt(struct dentry *odir, const char *oname, struct dentry *ndir, const char *nname, int flags){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = dir_rename(odir, oname, ndir, nname, flags);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//在dir目录下建一个名为name指向target的符号链接
static int symlink_locked(struct dentry *dir, const char *name, const char *target){
    struct inode *din = dir->d_inode;
    if(!din || (din->i_mode & S_IFMT) != S_IFDIR)return -ENOTDIR;
    //检查在父目录的写权限和搜索权限
    int pr = InodePermission(din, MAY_WRITE | MAY_EXEC);
    if(pr)return pr;
    struct dentry *d = dir_lookup(dir, name);
    if(!d)return -ENOMEM;
    int r;
    if(d->d_inode){
        r = -EEXIST;
    }else if(!din->i_op || !din->i_op->symlink){
        r = -EROFS;
    }else{
        r = din->i_op->symlink(din, d, target);
    }
    dput(d);
    return r;
}

//在linkpath处建一个指向target的符号链接
int FsSymlink(const char *target, const char *linkpath){
    if(!target || !linkpath)return -EINVAL;
    //解析起点
    const struct path *root = lookup_root(linkpath);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r;
    //解析出父目录与末段名，再在父目录里建链接
    struct path pdir;
    char name[NAMELEN_MAX + 1];
    r = path_lookup_parent(linkpath,root,&pdir,name,sizeof(name));
    if(r == 0){
        r = symlink_locked(pdir.dentry, name, target);
        dput(pdir.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//在dir目录下建名为name的符号链接
int FsSymlinkAt(struct dentry *dir, const char *name, const char *target){
    if(!dir || !name || !target)return -EINVAL;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = symlink_locked(dir, name, target);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//在dir目录下给old再挂一个名字
static int link_locked(struct dentry *old, struct dentry *dir, const char *name){
    if(!old || !old->d_inode)return -ENOENT;
    struct inode *din = dir->d_inode;
    if(!din || (din->i_mode & S_IFMT) != S_IFDIR)return -ENOTDIR;
    if((old->d_inode->i_mode & S_IFMT) == S_IFDIR)return -EPERM;
    if(old->d_inode->i_sb != din->i_sb)return -EXDEV;
    //检查在父目录的写权限和搜索权限
    int pr = InodePermission(din, MAY_WRITE | MAY_EXEC);
    if(pr)return pr;
    struct dentry *d = dir_lookup(dir, name);
    if(!d)return -ENOMEM;
    int r;
    if(d->d_inode){
        r = -EEXIST;
    }else if(!din->i_op || !din->i_op->link){
        r = -EPERM;
    }else{
        r = din->i_op->link(old, din, d);
    }
    dput(d);
    return r;
}

//在newpath处建一个指向oldpath的硬链接
int FsLink(const char *oldpath, const char *newpath, int follow){
    if(!oldpath || !newpath)return -EINVAL;
    const struct path *oroot = lookup_root(oldpath);
    const struct path *nroot = lookup_root(newpath);
    if(!oroot || !nroot)return -EINVAL;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r;
    struct path src;
    r = path_lookup(oldpath, oroot, follow ? LOOKUP_FOLLOW : LOOKUP_NOFOLLOW, &src);
    if(r == 0){
        //解析出父目录与末段名, 再在父目录里建链接
        struct path pdir;
        char name[NAMELEN_MAX + 1];
        r = path_lookup_parent(newpath,nroot,&pdir,name,sizeof(name));
        if(r == 0){
            r = link_locked(src.dentry,pdir.dentry,name);
            dput(pdir.dentry);
        }
        dput(src.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//在ndir目录下建一个指向odir目录里oname的硬链接
int FsLinkAt(struct dentry *odir, const char *oname, struct dentry *ndir, const char *nname){
    if(!odir || !ndir || !oname || !nname)return -EINVAL;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = -ENOENT;
    struct dentry *src = dir_lookup(odir, oname);
    if(src){
        r = link_locked(src, ndir, nname);
        dput(src);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//读出符号链接的目标，返回目标长度
int FsReadlink(const char *path, char *buf, int size){
    if(!path || !buf || size <= 0)return -EINVAL;
    //解析起点
    const struct path *root = lookup_root(path);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    //解析符号链接路径
    struct path p;
    int r = path_lookup(path,root,LOOKUP_NOFOLLOW,&p);
    if(r == 0){
        struct inode *inode = p.dentry->d_inode;
        const char *target = NULL;
        if((inode->i_mode & S_IFMT) != S_IFLNK)r = -EINVAL;//不是符号链接
        else if(!inode->i_op || !inode->i_op->get_link)r = -EINVAL;
        else r = inode->i_op->get_link(p.dentry, inode, &target);//按inode取出符号链接目标
        if(r == 0){
            //把目标拷贝到输出缓冲区
            int len = (int)strlen(target);
            if(len >= size)len = size - 1;
            memcpy(buf, target, (size_t)len);
            buf[len] = 0;
            r = len;
        }
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//把type类型的文件系统挂到target目录上
int FsMount(const char *type, const char *target){
    if(!type || !target)return -EINVAL;
    const struct path *root = lookup_root(target);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    //解析目标路径拿到挂载点dentry
    struct path p;
    int r = path_lookup(target, root, LOOKUP_DIRECTORY, &p);
    //在挂载点上建立一个新文件系统并挂上去
    if(r == 0){
        r = VfsMountNew(p.mnt, p.dentry, type);
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//把第一个ext2分区挂到target目录上
int FsMountExt2(const char *target){
    if(!target)return -EINVAL;
    const struct path *root = lookup_root(target);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    //解析目标路径拿到挂载点dentry
    struct path p;
    int r = path_lookup(target, root, LOOKUP_DIRECTORY, &p);
    //在该目录上建立 ext2 超级块并挂上去
    if(r == 0){
        r = Ext2MountAt(p.mnt, p.dentry);
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//卸载挂在target目录上的文件系统
int FsUmount(const char *target){
    if(!target)return -EINVAL;
    const struct path *root = lookup_root(target);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    //拆成"父目录 + 末段名"
    struct path pdir;
    char name[NAMELEN_MAX + 1];
    int r = path_lookup_parent(target, root, &pdir, name, sizeof(name));
    if(r){
        spin_unlock_irqrestore(&g_fs_lock, lk);
        return r;
    }
    //在父目录里取出挂载点自己的dentry
    struct dentry *d = dir_lookup(pdir.dentry, name);
    dput(pdir.dentry);
    if(!d){
        spin_unlock_irqrestore(&g_fs_lock, lk);
        return -ENOENT;
    }
    //卸载
    struct vfsmount *m = d->d_mounted;
    if(!m)r = -EINVAL;
    else r = VfsDetachMount(m);
    dput(d);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//inode转stat
static void inode_to_stat(struct inode *i, struct stat *st){
    memset(st, 0, sizeof(*st));
    st->st_dev      = 1;
    st->st_ino      = i->i_ino;
    st->st_nlink    = i->i_nlink ? i->i_nlink : 1;
    st->st_mode     = (i->i_sb && i->i_sb->s_mode_policy) ? i->i_sb->s_mode_policy(i) : i->i_mode;
    st->st_uid      = i->i_uid;
    st->st_gid      = i->i_gid;
    st->st_rdev     = i->i_rdev;
    st->st_size     = (int64_t)i->i_size;
    st->st_blksize  = (i->i_sb && i->i_sb->s_blocksize) ? (int64_t)i->i_sb->s_blocksize : 512;
    st->st_blocks   = (int64_t)((i->i_size + 511) / 512);
    int64_t now = (int64_t)rtc_get_epoch();
    st->st_atime      = i->i_atime_s ? i->i_atime_s : now;
    st->st_mtime      = i->i_mtime_s ? i->i_mtime_s : now;
    st->st_ctime      = i->i_ctime_s ? i->i_ctime_s : now;
    st->st_atime_nsec = i->i_atime_ns;
    st->st_mtime_nsec = i->i_mtime_ns;
    st->st_ctime_nsec = i->i_ctime_ns;
}

int FsStat(const char *path, struct stat *st){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    int r = lookup_path(path, &p);
    if(r == 0){
        inode_to_stat(p.dentry->d_inode, st);
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//不跟随末段符号链接的stat(lstat)
int FsLstat(const char *path, struct stat *st){
    if(!path)return -EINVAL;
    const struct path *root = lookup_root(path);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    int r = path_lookup(path, root, LOOKUP_NOFOLLOW, &p);
    if(r == 0){
        inode_to_stat(p.dentry->d_inode, st);
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsStatAt(struct dentry *dir, const char *name, struct stat *st){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = -ENOENT;
    struct dentry *d = dir_lookup(dir, name);
    if(d){
        if(d->d_inode){
            inode_to_stat(d->d_inode, st);
            r = 0;
        }
        dput(d);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsStatFile(fs_file_t *f, struct stat *st){
    if(!f || !f->used || !f->inode)return -EBADF;
    inode_to_stat(f->inode, st);
    return 0;
}

//atime/mtime
static int dentry_utime(struct dentry *d,int64_t atime,int64_t mtime,int64_t ctime){
    if(!d || !d->d_inode)return -ENOENT;
    struct inode *ino = d->d_inode;
    if(!ino->i_op || !ino->i_op->setattr)return -EROFS;
    struct iattr ia;
    memset(&ia, 0, sizeof(ia));
    if(atime >= 0){ia.ia_valid |= ATTR_ATIME;ia.ia_atime = atime;}
    if(mtime >= 0){ia.ia_valid |= ATTR_MTIME;ia.ia_mtime = mtime;}
    if(ctime >= 0){ia.ia_valid |= ATTR_CTIME;ia.ia_ctime = ctime;}
    if(!ia.ia_valid)return 0;
    return ino->i_op->setattr(d, &ia);
}

int FsUtime(const char *path, int64_t atime, int64_t mtime, int64_t ctime){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    //解析路径
    struct path p;
    int r = lookup_path(path, &p);
    //更新时间
    if(r == 0){
        r = dentry_utime(p.dentry, atime, mtime, ctime);
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsUtimeAt(struct dentry *dir, const char *name, int64_t atime, int64_t mtime, int64_t ctime){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = -ENOENT;
    struct dentry *d = dir_lookup(dir, name);//在目录中查找
    //更新时间
    if(d){
        r = dentry_utime(d, atime, mtime, ctime);
        dput(d);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsUtimeFd(fs_file_t *f, int64_t atime, int64_t mtime, int64_t ctime){
    if(!f || !f->used || !f->dentry)return -EBADF;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = dentry_utime(f->dentry, atime, mtime, ctime);//更新时间
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//存在性/权限检查
int FsAccess(const char *path, int mask){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    int r = lookup_path(path, &p);
    if(r == 0){
        struct inode *ino = p.dentry->d_inode;
        r = InodePermission(ino, mask);
        if(r == 0 && ino->i_op && ino->i_op->permission)r = ino->i_op->permission(ino, mask);
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//检查身份后改属性
static int setattr_checked(struct dentry *d, struct iattr *ia){
    if(!d || !d->d_inode)return -ENOENT;
    struct inode *inode = d->d_inode;
    const cred_t *c = CurrentCred();
    if(ia->ia_valid & (ATTR_UID | ATTR_GID)){
        if(!CredIsRoot(c))return -EPERM;
    }else if(ia->ia_valid & ATTR_MODE){
        if(!CredIsRoot(c) && c->euid != inode->i_uid)return -EPERM;
    }
    if(!inode->i_op || !inode->i_op->setattr)return -EPERM;
    ia->ia_valid |= ATTR_CTIME;
    ia->ia_ctime = (int64_t)rtc_get_epoch();
    return inode->i_op->setattr(d, ia);
}

//构造chown的属性参数
static int chown_iattr(struct iattr *ia, uint32_t uid, uint32_t gid){
    memset(ia, 0, sizeof(*ia));
    if(uid != (uint32_t)-1){
        ia->ia_valid |= ATTR_UID;
        ia->ia_uid = uid;
    }
    if(gid != (uint32_t)-1){
        ia->ia_valid |= ATTR_GID;
        ia->ia_gid = gid;
    }
    if(!ia->ia_valid)return -EINVAL;
    return 0;
}

//按路径改权限位
int FsChmod(const char *path, uint16_t mode){
    //解析起点
    if(!path)return -EINVAL;
    const struct path *root = lookup_root(path);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    //解析路径
    struct path p;
    int r = path_lookup(path, root, LOOKUP_FOLLOW, &p);
    if(r == 0){
        //更改权限位
        struct iattr ia;
        memset(&ia, 0, sizeof(ia));
        ia.ia_valid = ATTR_MODE;
        ia.ia_mode  = (uint16_t)(mode & 07777);
        r = setattr_checked(p.dentry, &ia);
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//按已打开的描述改权限位
int FsChmodFd(int fd, uint16_t mode){
    //获取目录项
    if(!current_task || fd < 0 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
    struct dentry *d = current_task->files[fd].dentry;
    if(!d)return -EBADF;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    //更改权限位
    struct iattr ia;
    memset(&ia, 0, sizeof(ia));
    ia.ia_valid = ATTR_MODE;
    ia.ia_mode  = (uint16_t)(mode & 07777);
    int r = setattr_checked(d, &ia);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//按路径改属主/属组
int FsChown(const char *path, uint32_t uid, uint32_t gid, int follow){
    if(!path)return -EINVAL;
    //构造chown的属性参数
    struct iattr ia;
    int r = chown_iattr(&ia, uid, gid);
    if(r)return r;
    //解析路径
    const struct path *root = lookup_root(path);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    r = path_lookup(path, root, follow ? LOOKUP_FOLLOW : LOOKUP_NOFOLLOW, &p);
    if(r == 0){
        r = setattr_checked(p.dentry, &ia);//生效更改属主/属组
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//按已打开的描述改属主/属组
int FsChownFd(int fd, uint32_t uid, uint32_t gid){
    //构造chown的属性参数
    struct iattr ia;
    int r = chown_iattr(&ia, uid, gid);
    if(r)return r;
    //获取目录项
    if(!current_task || fd < 0 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
    struct dentry *d = current_task->files[fd].dentry;
    if(!d)return -EBADF;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    r = setattr_checked(d, &ia);//生效更改属主/属组
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//在路径处建一个设备节点
int FsMknod(const char *path, uint16_t mode, uint32_t rdev){
    if(!path)return -EINVAL;
    const struct path *root = lookup_root(path);
    if(!root)return -ENOENT;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path pdir;
    char name[NAMELEN_MAX + 1];
    int r = path_lookup_parent(path, root, &pdir, name, sizeof(name));
    if(r == 0){
        struct inode *din = pdir.dentry->d_inode;
        if(!din || (din->i_mode & S_IFMT) != S_IFDIR){
            r = -ENOTDIR;
        }else if(!din->i_op || !din->i_op->mknod){
            r = -EPERM;
        }else{
            r = InodePermission(din, MAY_WRITE | MAY_EXEC);
            if(r == 0){
                struct dentry *d = dir_lookup(pdir.dentry, name);
                if(!d){
                    r = -ENOMEM;
                }else{
                    r = d->d_inode ? -EEXIST : din->i_op->mknod(din, d, mode, rdev);
                    dput(d);
                }
            }
        }
        dput(pdir.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsExistsAt(struct dentry *dir, const char *name){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = -ENOENT;
    struct dentry *d = dir_lookup(dir, name);
    if(d){
        r = d->d_inode ? 0 : -ENOENT;
        dput(d);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//把一个超级块挂成 VFS 根
static int mount_root(const char *type, uint64_t magic, uint32_t inode_size, const struct super_operations *sop, struct dentry *(*fill)(struct super_block *sb)){
    struct super_block *sb = SuperAlloc(type, magic, inode_size, sop);
    if(!sb)return -ENOMEM;
    struct dentry *root = fill(sb);
    if(!root)return -ENOMEM;
    struct vfsmount *mnt = VfsKernMount(sb, root, 0);
    if(!mnt)return -ENOMEM;
    g_vfs_root.mnt    = mnt;
    g_vfs_root.dentry = root;
    return 0;
}

//把第一个ext2分区挂成VFS根
static int mount_root_ext2(void){
    struct bdev *bd = Ext2FindPartition();
    if(!bd)return -ENODEV;
    struct dentry *root = NULL;
    struct super_block *sb = Ext2NewSuper(bd, &root);
    if(!sb)return -ENOMEM;
    struct vfsmount *mnt = VfsKernMount(sb, root, 0);
    if(!mnt)return -ENOMEM;
    g_vfs_root.mnt    = mnt;
    g_vfs_root.dentry = root;
    return 0;
}

void FsInit(struct disk_info *disk){
    FsVfsInit();
    BCacheInit(BC_MAX_PAGES);//块缓存初始化
    PageCacheInit();//初始化文件页缓存
    _Bool ext2_root = (mount_root_ext2() == 0);//先尝试挂载ext2
    //其次尝试FAT32,最后RamFS
    if(!ext2_root && !(disk && (fat32_mount(disk) == 0) && mount_root("vfat", 0x4D44,FatVfsInodeSize(),FatVfsSuperOps(),FatVfsFillSuper) == 0)){
        mount_root("ramfs", 0x01021994, Ramfs2InodeSize(), Ramfs2SuperOps(), Ramfs2FillSuper);
    }
    //挂载tmpfs
    FsMkdir("/tmp", 0755);
    FsMount("tmpfs", "/tmp");
    FsChmod("/tmp", 01777);
    //注册字符设备并挂devtmpfs
    DevicesInit();
    FsMkdir("/dev", 0755);
    if(FsMount("devtmpfs", "/dev") != 0)printk(PRINTK_WARNING"vfs:no /dev");
}
