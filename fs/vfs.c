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
#include <mm/pagecache.h>
#include <drives/bcache.h>
#include <vfs.h>

static struct path g_vfs_root;//新VFS的根
static spinlock_t g_fs_lock = {0};//保护下面那几个"目录节点+名字"的旧接口

const struct path *VfsRoot(void){
    return g_vfs_root.dentry ? &g_vfs_root : NULL;
}

//规范化路径(相对路径按当前工作目录补全, 处理 . 与 ..)
static int path_normalize(const char *path, char *out, int outsz){
    if(!path) return -1;
    char abs[512];
    const char *src;
    if(path[0] == '/'){
        src = path;
    }else{
        const char *cwd = (current_task && current_task->cwd[0]) ? current_task->cwd : "/";
        int cl = strlen(cwd);
        int pl = strlen(path);
        if(cl + 1 + pl >= (int)sizeof(abs)) return -1;
        memcpy(abs, cwd, cl);
        int off = cl;
        if(off == 0 || abs[off - 1] != '/') abs[off++] = '/';
        memcpy(abs + off, path, pl + 1);
        src = abs;
    }
    //跳过"."、".."弹一级、去重斜杠
    char stack[512];
    int top = 0;
    const char *p = src;
    while(*p){
        while(*p == '/')p++;
        if(!*p) break;
        char comp[MAX_NAME];
        int i = 0;
        while(*p && *p != '/' && i < MAX_NAME - 1)comp[i++] = *p++;
        comp[i] = 0;
        if(i == 1 && comp[0] == '.') continue;//"."跳过
        if(i == 2 && comp[0] == '.' && comp[1] == '.'){//".."弹上一级
            while(top > 0 && stack[top - 1] != '/') top--;
            if(top > 0) top--;
            if(top == 0) stack[top++] = '/';
            continue;
        }
        if(top == 0 || stack[top - 1] != '/'){
            if(top < (int)sizeof(stack) - 1)stack[top++] = '/';
        }
        for(int k = 0; k < i && top < (int)sizeof(stack) - 1; k++)stack[top++] = comp[k];
    }
    if(top == 0) stack[top++] = '/';
    if(top >= outsz) return -1;
    memcpy(out, stack, top);
    out[top] = 0;
    return 0;
}

int FsNormalizePath(const char *path, char *out, int outsz){
    return path_normalize(path, out, outsz);
}

//inode对应的旧节点
static fs_node_t *node_of(struct inode *inode){
    return FatVfsNode(inode);
}

//打开标志与inode类型是否匹配
static int open_check(struct inode *ino, int flags){
    uint16_t type = (uint16_t)(ino->i_mode & S_IFMT);
    if((flags & O_DIRECTORY) && type != S_IFDIR)return -ENOTDIR;
    if(type == S_IFDIR && (flags & O_ACCMODE) != O_RDONLY)return -EISDIR;
    return 0;
}

//在父目录里创建普通文件或目录
static int fs_create_at(const char *norm, int isdir, uint16_t mode, struct path *out){
    const struct path *root = VfsRoot();
    struct path pdir;
    char name[NAMELEN_MAX + 1];
    int r = path_lookup_parent(norm, root, &pdir, name, sizeof(name));
    if(r)return r;
    struct inode *dir = pdir.dentry->d_inode;
    struct dentry *d = d_lookup(pdir.dentry, name);
    if(d && d->d_inode){
        dput(d);
        dput(pdir.dentry);
        return -EEXIST;
    }
    if(!d){
        d = d_alloc(pdir.dentry, name);
        if(!d){ dput(pdir.dentry); return -ENOMEM; }
    }
    int (*fn)(struct inode*, struct dentry*, uint16_t) = NULL;
    if(dir->i_op)fn = isdir ? dir->i_op->mkdir : dir->i_op->create;
    if(!fn){
        dput(d);
        dput(pdir.dentry);
        return -EROFS;
    }
    r = fn(dir, d, mode);
    if(r){
        dput(d);
        dput(pdir.dentry);
        return r;
    }
    if(out){
        out->mnt    = pdir.mnt;
        out->dentry = d;
    }else{
        dput(d);
    }
    dput(pdir.dentry);
    return 0;
}

//把一个已解析的 path 变成打开文件描述; 成功时接管 p->dentry 的引用
static int file_bind(fs_file_t *out, struct path *p, int flags){
    struct inode *ino = p->dentry->d_inode;
    struct file *file = FileAlloc();
    if(!file){ dput(p->dentry); return -ENOMEM; }
    file->f_inode = ino;
    file->f_op    = ino->i_fop;
    file->f_flags = (uint32_t)flags;
    file->f_pos   = (flags & O_APPEND) ? ino->i_size : 0;
    file->f_path  = *p;
    if(file->f_op && file->f_op->open){
        int r = file->f_op->open(ino, file);
        if(r){ FilePut(file); return r; }
    }
    out->f      = file;
    out->inode  = ino;
    out->dentry = p->dentry;
    out->mnt    = p->mnt;
    out->node   = node_of(ino);
    out->flags  = flags;
    out->pipe   = NULL;
    out->used   = true;
    return 0;
}

int FsOpen(const char *path, int flags, fs_file_t *out){
    const struct path *root = VfsRoot();
    if(!out || !root)return -ENOENT;
    if((flags & O_TMPFILE) == O_TMPFILE)return -EOPNOTSUPP;
    char norm[PATHLEN_MAX];
    if(path_normalize(path, norm, sizeof(norm)))return -ENAMETOOLONG;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    int r = path_lookup(norm, root, (flags & O_DIRECTORY) ? LOOKUP_DIRECTORY : 0, &p);
    if(r == 0){
        //O_CREAT|O_EXCL: 已经存在就失败
        if((flags & O_CREAT) && (flags & O_EXCL)){
            dput(p.dentry);
            r = -EEXIST;
            goto done;
        }
    }else if(r == -ENOENT && (flags & O_CREAT)){
        r = fs_create_at(norm, 0, 0644, &p);
        if(r == -EEXIST){//别的任务抢先建好了
            if(flags & O_EXCL)goto done;
            r = path_lookup(norm, root, 0, &p);
        }
    }
    if(r)goto done;
    r = open_check(p.dentry->d_inode, flags);
    if(r){ dput(p.dentry); goto done; }
    //O_TRUNC: 打开时把文件截成 0
    if((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY &&
       (p.dentry->d_inode->i_mode & S_IFMT) == S_IFREG){
        struct inode *ino = p.dentry->d_inode;
        if(ino->i_op && ino->i_op->setattr){
            struct iattr ia;
            memset(&ia, 0, sizeof(ia));
            ia.ia_valid = ATTR_SIZE;
            ia.ia_size  = 0;
            ino->i_op->setattr(p.dentry, &ia);
        }
    }
    r = file_bind(out, &p, flags);
done:
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

void FsClose(fs_file_t *f){
    if(!f || !f->used)return;
    if(f->f){
        if(f->f->f_op && f->f->f_op->release)f->f->f_op->release(f->f->f_inode, f->f);
        FilePut(f->f);
        f->f = NULL;
    }
    f->dentry = NULL;
    f->inode = NULL;
    f->mnt   = NULL;
    f->node  = NULL;
    f->used  = false;
}

//文件当前长度
static uint64_t fs_file_size(fs_file_t *f){
    if(!f)return 0;
    if(f->inode)return f->inode->i_size;
    return f->node ? f->node->size : 0;
}

uint64_t FsRead(fs_file_t *f, void *buf, uint64_t len){
    if(!f || !f->used || !f->f)return 0;
    if((f->flags & O_ACCMODE) == O_WRONLY)return 0;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    uint64_t r = 0;
    if(f->inode){
        if(f->inode->i_fop && f->inode->i_fop->read){
            int64_t n = f->inode->i_fop->read(f->f, (char*)buf, len, &f->f->f_pos);
            r = (n > 0) ? (uint64_t)n : 0;
        }
    }else if(f->node && f->node->ops->read){
        r = f->node->ops->read(f->node, f->f->f_pos, buf, len);
        f->f->f_pos += r;
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

uint64_t FsWrite(fs_file_t *f, const void *buf, uint64_t len){
    if(!f || !f->used || !f->f)return 0;
    if((f->flags & O_ACCMODE) == O_RDONLY)return 0;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    //追加模式每次写都定位到末尾
    if(f->flags & O_APPEND)f->f->f_pos = fs_file_size(f);
    uint64_t r = 0;
    if(f->inode){
        if(f->inode->i_fop && f->inode->i_fop->write){
            int64_t n = f->inode->i_fop->write(f->f, (const char*)buf, len, &f->f->f_pos);
            r = (n > 0) ? (uint64_t)n : 0;
        }
    }else if(f->node && f->node->ops->write){
        r = f->node->ops->write(f->node, f->f->f_pos, buf, len);
        f->f->f_pos += r;
        if(f->f->f_pos > f->node->size)f->node->size = f->f->f_pos;
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
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
    if(!f || !f->used || f->pipe || !f->f)return -EINVAL;
    if((f->flags & O_ACCMODE) == O_RDONLY)return -EINVAL;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = -EINVAL;
    if(f->inode){
        if((f->inode->i_mode & S_IFMT) == S_IFDIR){
            r = -EISDIR;
        }else if(!f->inode->i_op || !f->inode->i_op->setattr){
            r = -EROFS;
        }else{
            struct iattr ia;
            memset(&ia, 0, sizeof(ia));
            ia.ia_valid = ATTR_SIZE;
            ia.ia_size  = len;
            r = f->inode->i_op->setattr(f->dentry, &ia);
        }
    }else if(f->node){
        if(f->node->type == FT_DIR)r = -EISDIR;
        else if(f->node->ops && f->node->ops->truncate)r = f->node->ops->truncate(f->node, len);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//按路径解析出 inode(带 dentry 引用), 失败返回负 errno
static int lookup_path(const char *path, struct path *out){
    const struct path *root = VfsRoot();
    if(!root || !path || !out)return -ENOENT;
    char norm[PATHLEN_MAX];
    if(path_normalize(path, norm, sizeof(norm)))return -ENAMETOOLONG;
    return path_lookup(norm, root, 0, out);
}

//按路径截断/扩展文件
int FsTruncatePath(const char *path, uint64_t len){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    int r = lookup_path(path, &p);
    if(r)goto done;
    struct inode *ino = p.dentry->d_inode;
    if((ino->i_mode & S_IFMT) == S_IFDIR){ r = -EISDIR; }
    else if(!ino->i_op || !ino->i_op->setattr){ r = -EROFS; }
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

/*==================== 新VFS: 目录项遍历 ====================*/
typedef struct dents_ctx {
    char    *buf;
    uint64_t len;
    int      written;
    uint64_t skip;   //还要跳过多少个真实条目
    uint64_t seen;   //已经产出过的真实条目数
    uint64_t next;   //下一次从哪个 f_pos 继续
    int      stop;   //缓冲区放不下了
} dents_ctx_t;

//把一条目录项写进用户缓冲
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
    d->d_type   = (uint8_t)((type == S_IFDIR) ? DT_DIR : ((type == S_IFREG) ? DT_REG : DT_UNKNOWN));
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

//读目录项到缓冲区
static int getdents_locked(fs_file_t *f, void *buf, uint64_t len);

//读目录项到缓冲区
int FsGetdents(fs_file_t *f, void *buf, uint64_t len){
    if(!f || !f->used || !f->f || !buf)return -EINVAL;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int written = getdents_locked(f, buf, len);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return written;
}

static int getdents_locked(fs_file_t *f, void *buf, uint64_t len){
    //走i_fop->iterate
    if(f->inode){
        if((f->inode->i_mode & S_IFMT) != S_IFDIR)return -ENOTDIR;
        if(!f->inode->i_fop || !f->inode->i_fop->iterate)return -ENOSYS;
        dents_ctx_t c;
        memset(&c, 0, sizeof(c));
        c.buf  = (char*)buf;
        c.len  = len;
        c.next = f->f->f_pos;
        if(c.next < 2){
            if(c.next == 0 && dent_put_virtual(&c, ".", f->inode->i_ino))goto finish;
            if(c.next == 1 && dent_put_virtual(&c, "..", f->inode->i_ino))goto finish;
        }
        c.skip = (f->f->f_pos > 2) ? (f->f->f_pos - 2) : 0;
        f->inode->i_fop->iterate(f->f, &c, dent_emit);
finish:
        f->f->f_pos = c.next;
        return c.written;
    }
    //旧节点
    if(f->node && f->node->type == FT_DIR && f->node->ops && f->node->ops->readdir){
        int written = 0;
        uint64_t pos = f->f->f_pos;
        char *p = (char*)buf;
        while((uint64_t)written < len){
            fs_dirent_t de;
            uint64_t save = pos;
            memset(&de, 0, sizeof(de));
            if(pos < 2){
                strcpy(de.name, pos == 0 ? "." : "..");
                de.ino  = f->node->ino;
                de.type = FT_DIR;
                pos++;
            }else{
                uint64_t cookie = pos - 2;
                int rr = f->node->ops->readdir(f->node, &cookie, &de);
                if(rr != 0){ written = (rr < 0) ? rr : written; break; }
                pos = cookie + 2;
            }
            int nlen = strlen(de.name);
            uint16_t reclen = (uint16_t)(19 + nlen + 1);
            reclen = (uint16_t)((reclen + 7) & ~7);
            if((uint64_t)written + reclen > len){ pos = save; break; }
            linux_dirent64_t *d = (linux_dirent64_t*)(p + written);
            d->d_ino    = de.ino;
            d->d_off    = (int64_t)pos;
            d->d_reclen = reclen;
            d->d_type   = (uint8_t)((de.type == FT_DIR) ? DT_DIR : ((de.type == FT_FILE) ? DT_REG : DT_UNKNOWN));
            strcpy(d->d_name, de.name);
            written += reclen;
        }
        f->f->f_pos = pos;
        return written;
    }
    return -ENOTDIR;
}

/*==================== 新VFS: 目录项增删 ====================*/
//创建目录
int FsMkdir(const char *path, int mode){
    const struct path *root = VfsRoot();
    if(!root)return -ENOENT;
    char norm[PATHLEN_MAX];
    if(path_normalize(path, norm, sizeof(norm)))return -ENAMETOOLONG;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    int r = fs_create_at(norm, 1, (uint16_t)mode, NULL);
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//删除目录项(文件或目录)
int FsUnlink(const char *path){
    const struct path *root = VfsRoot();
    if(!root)return -ENOENT;
    char norm[PATHLEN_MAX];
    if(path_normalize(path, norm, sizeof(norm)))return -ENAMETOOLONG;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path pdir;
    char name[NAMELEN_MAX + 1];
    int r = path_lookup_parent(norm, root, &pdir, name, sizeof(name));
    if(r)goto done;
    struct inode *dir = pdir.dentry->d_inode;
    struct dentry *d = d_lookup(pdir.dentry, name);
    if(!d){
        d = d_alloc(pdir.dentry, name);
        if(!d){ r = -ENOMEM; dput(pdir.dentry); goto done; }
        if(dir->i_op && dir->i_op->lookup)dir->i_op->lookup(dir, d, 0);
    }
    if(!d->d_inode){ r = -ENOENT; }
    else if((d->d_inode->i_mode & S_IFMT) == S_IFDIR){
        r = (dir->i_op && dir->i_op->rmdir) ? dir->i_op->rmdir(dir, d) : -EISDIR;
    }else{
        r = (dir->i_op && dir->i_op->unlink) ? dir->i_op->unlink(dir, d) : -EROFS;
    }
    dput(d);
    dput(pdir.dentry);
done:
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

int FsRename(const char *oldpath, const char *newpath){
    const struct path *root = VfsRoot();
    if(!root || !oldpath || !newpath)return -EINVAL;
    char onorm[PATHLEN_MAX], nnorm[PATHLEN_MAX];
    if(path_normalize(oldpath, onorm, sizeof(onorm)))return -ENAMETOOLONG;
    if(path_normalize(newpath, nnorm, sizeof(nnorm)))return -ENAMETOOLONG;
    if(strcmp(onorm, nnorm) == 0)return 0;
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path opdir, npdir;
    char oname[NAMELEN_MAX + 1], nname[NAMELEN_MAX + 1];
    int r = path_lookup_parent(onorm, root, &opdir, oname, sizeof(oname));
    if(r)goto done;
    r = path_lookup_parent(nnorm, root, &npdir, nname, sizeof(nname));
    if(r){ dput(opdir.dentry); goto done; }
    struct inode *odir = opdir.dentry->d_inode;
    struct inode *ndir = npdir.dentry->d_inode;
    if(!odir->i_op || !odir->i_op->rename){ r = -EROFS; goto put; }
    struct dentry *od = d_lookup(opdir.dentry, oname);
    if(!od){
        od = d_alloc(opdir.dentry, oname);
        if(!od){ r = -ENOMEM; goto put; }
        if(odir->i_op->lookup)odir->i_op->lookup(odir, od, 0);
    }
    if(!od->d_inode){
        r = -ENOENT;
    }else{
        struct dentry *nd = d_lookup(npdir.dentry, nname);
        if(!nd)nd = d_alloc(npdir.dentry, nname);
        if(!nd){
            r = -ENOMEM;
        }else{
            r = odir->i_op->rename(odir, od, ndir, nd);
            dput(nd);
        }
    }
    dput(od);
put:
    dput(npdir.dentry);
    dput(opdir.dentry);
done:
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return r;
}

//解析路径返回节点
fs_node_t *FsResolve(const char *path){
    uint64_t lk;
    spin_lock_irqsave(&g_fs_lock, lk);
    struct path p;
    fs_node_t *n = NULL;
    if(lookup_path(path, &p) == 0){
        n = node_of(p.dentry->d_inode);
        dput(p.dentry);
    }
    spin_unlock_irqrestore(&g_fs_lock, lk);
    return n;
}

//取路径的父目录节点与末段名
fs_node_t *FsParentOf(const char *path, char *name, int namesz){
    const struct path *root = VfsRoot();
    if(!root || !path || !name || namesz <= 0)return NULL;
    char norm[PATHLEN_MAX];
    if(path_normalize(path, norm, sizeof(norm)))return NULL;
    struct path pdir;
    if(path_lookup_parent(norm, root, &pdir, name, namesz))return NULL;
    fs_node_t *n = node_of(pdir.dentry->d_inode);
    dput(pdir.dentry);
    return n;
}

//用已解析好的旧节点完成"打开"的收尾(仅 dirfd 相对打开用)
static int legacy_open_node(fs_node_t *node, int flags, fs_file_t *out){
    if(!out || !node)return -EINVAL;
    if(node->type != FT_FILE && node->type != FT_DIR)return -EINVAL;
    if((flags & O_DIRECTORY) && node->type != FT_DIR)return -ENOTDIR;
    if(node->type == FT_DIR && (flags & O_ACCMODE) != O_RDONLY)return -EISDIR;
    if(flags & O_TRUNC){
        if(node->ops->truncate)node->ops->truncate(node, 0);
    }
    node->refs++;
    struct file *file = FileAlloc();
    if(!file){
        node->refs--;
        return -ENOMEM;
    }
    out->node  = node;
    out->inode = NULL;
    out->dentry= NULL;
    out->mnt   = NULL;
    out->flags = flags;
    out->f     = file;
    file->f_flags = (uint32_t)flags;
    file->f_pos = (flags & O_APPEND) ? node->size : 0;
    out->used = true;
    return 0;
}

//在已打开的目录节点下打开(openat 用)
int FsOpenIn(fs_node_t *dir, const char *name, int flags, fs_file_t *out){
    uint64_t iflags;
    spin_lock_irqsave(&g_fs_lock, iflags);
    int r = -EINVAL;
    if(!out)goto done;
    if((flags & O_TMPFILE) == O_TMPFILE){ r = -EOPNOTSUPP; goto done; }
    if(!name || !*name){ r = -ENOENT; goto done; }
    if(!dir || dir->type != FT_DIR || !dir->ops || !dir->ops->lookup){ r = -ENOTDIR; goto done; }
    fs_node_t *node = dir->ops->lookup(dir, name);
    if(!node && (flags & O_CREAT) && dir->ops->create)node = dir->ops->create(dir, name, FT_FILE);
    if(!node){ r = -ENOENT; goto done; }
    r = legacy_open_node(node, flags, out);
done:
    spin_unlock_irqrestore(&g_fs_lock, iflags);
    return r;
}

//在已打开的目录节点下创建子目录
int FsMkdirIn(fs_node_t *dir, const char *name, int mode){
    uint64_t iflags;
    spin_lock_irqsave(&g_fs_lock, iflags);
    int r = -ENOTDIR;
    if(dir && dir->type == FT_DIR && dir->ops && dir->ops->mkdir && name && *name)
        r = dir->ops->mkdir(dir, name, mode);
    spin_unlock_irqrestore(&g_fs_lock, iflags);
    return r;
}

//删除已打开目录下的目录项
int FsUnlinkIn(fs_node_t *dir, const char *name){
    uint64_t iflags;
    spin_lock_irqsave(&g_fs_lock, iflags);
    int r = -ENOTDIR;
    if(dir && dir->type == FT_DIR && dir->ops && dir->ops->unlink && name && *name)
        r = dir->ops->unlink(dir, name);
    spin_unlock_irqrestore(&g_fs_lock, iflags);
    return r;
}

//两侧都是"目录节点+名字"的重命名
int FsRenameIn(fs_node_t *olddir, const char *oldname, fs_node_t *newdir, const char *newname){
    uint64_t iflags;
    spin_lock_irqsave(&g_fs_lock, iflags);
    int r = -ENOTDIR;
    do{
        if(!olddir || !newdir || !oldname || !newname || !*oldname || !*newname)break;
        if(olddir->type != FT_DIR || newdir->type != FT_DIR)break;
        if(!olddir->ops || !olddir->ops->rename)break;
        if(olddir == newdir && strcmp(oldname, newname) == 0){ r = 0; break; }
        if(newdir->ops && newdir->ops->lookup && newdir->ops->unlink && newdir->ops->lookup(newdir, newname)){
            if(newdir->ops->unlink(newdir, newname) != 0)break;
        }
        r = olddir->ops->rename(olddir, oldname, newdir, newname);
    }while(0);
    spin_unlock_irqrestore(&g_fs_lock, iflags);
    return r;
}

//存在性检查(存在返回0, 不存在返回-ENOENT)
int FsExistsIn(fs_node_t *dir, const char *name){
    uint64_t iflags;
    spin_lock_irqsave(&g_fs_lock, iflags);
    int r = -ENOTDIR;
    if(dir && dir->type == FT_DIR && dir->ops && dir->ops->lookup && name && *name)
        r = dir->ops->lookup(dir, name) ? 0 : -ENOENT;
    spin_unlock_irqrestore(&g_fs_lock, iflags);
    return r;
}

/*==================== 初始化 ====================*/
//把一个超级块挂成 VFS 根
static int mount_root(const char *type, uint64_t magic, uint32_t inode_size,
                      const struct super_operations *sop,
                      struct dentry *(*fill)(struct super_block *sb)){
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

void FsInit(struct disk_info *disk){
    FsVfsInit();
    BCacheInit(BC_MAX_PAGES);//块缓存初始化
    PageCacheInit();//初始化文件页缓存
    //优先挂载 FAT32, 失败则退回内存文件系统
    if(disk && (fat32_mount(disk) == 0) &&
       mount_root("vfat", 0x4D44, FatVfsInodeSize(), FatVfsSuperOps(), FatVfsFillSuper) == 0)
        return;
    mount_root("ramfs", 0x01021994, Ramfs2InodeSize(), Ramfs2SuperOps(), Ramfs2FillSuper);
}
