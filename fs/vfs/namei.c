// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/vfs/namei.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 路径解析
 */
#include <vfs.h>
#include <print.h>

const struct path *VfsRoot(void) __attribute__((weak));

//取下一段名字,返回长度
static int next_component(const char **pp, char *name, int max){
    const char *p = *pp;
    while(*p == '/')p++;
    if(!*p){
        *pp = p;
        return 0;
    }
    int n = 0;
    while(*p && *p != '/'){
        if(n >= max)return -1;
        name[n ++] = *p ++;
    }
    name[n] = 0;
    *pp = p;
    return n;
}

/*由DeepSeek V4.1 Flash*/
#define SYMLINK_MAX_HOP  8    //一条路径里最多连续展开几层符号链接
#define SYMLINK_PATH_MAX 256  //展开后的路径缓冲(受内核栈大小限制)

//逐段解析
static int walk(const char *path, const struct path *root, const struct path *rootpath, int to_parent,struct path *out, char *last_name, int namesz, unsigned flags, int depth){
    if(!path || !root || !root->mnt || !root->dentry)return -EINVAL;
    struct vfsmount *mnt = root->mnt;
    struct dentry   *cur = dget(root->dentry);
    if(!cur)return -ENOENT;

    const char *p = path;
    char name[NAMELEN_MAX + 1];
    int  total = 0;

    for(;;){
        int n = next_component(&p, name, NAMELEN_MAX + 1);
        if(n < 0){ dput(cur); return -ENAMETOOLONG; }
        if(n == 0)break;
        total += n;
        if(total > PATHLEN_MAX){ dput(cur); return -ENAMETOOLONG; }

        //是不是最后一段
        const char *q = p;
        while(*q == '/')q ++;
        if(to_parent && *q == 0){
            while(cur->d_mounted){
                struct vfsmount *sm = cur->d_mounted;
                dput(cur);
                mnt = sm;
                cur = dget(sm->mnt_root);
            }
            if(last_name && namesz > 0){
                int i = 0;
                for(;i < n && i < namesz - 1;i ++)last_name[i] = name[i];
                last_name[i] = 0;
            }
            out->mnt = mnt;
            out->dentry = cur;
            return 0;
        }

        if(n == 1 && name[0] == '.')continue;
        if(n == 2 && name[0] == '.' && name[1] == '.'){
            struct dentry *up = VfsDotDot(&mnt, cur);
            if(up && up != cur){ dput(cur); cur = dget(up); }
            continue;
        }

        //跨过挂载点
        while(cur->d_mounted){
            struct vfsmount *sm = cur->d_mounted;
            dput(cur);
            cur = dget(sm->mnt_root);
            mnt = sm;
            if(!cur)return -ENOENT;
        }
        if(!cur->d_inode){ dput(cur); return -ENOENT; }
        if((cur->d_inode->i_mode & S_IFMT) != S_IFDIR){ dput(cur); return -ENOTDIR; }
        //进目录，检查x位
        if(InodePermission(cur->d_inode, MAY_EXEC) || (cur->d_inode->i_op && cur->d_inode->i_op->permission && cur->d_inode->i_op->permission(cur->d_inode, MAY_EXEC))){
            dput(cur);
            return -EACCES;
        }

        //先查 dcache, 未命中才问文件系统
        struct dentry *next = d_lookup(cur, name);
        if(!next){
            next = d_alloc(cur, name);
            if(!next){ dput(cur); return -ENOMEM; }
            if(!cur->d_inode->i_op || !cur->d_inode->i_op->lookup){
                dput(next);
                dput(cur);
                return -ENOTDIR;
            }
            cur->d_inode->i_op->lookup(cur->d_inode, next, 0);
        }
        if(!next->d_inode){ dput(next); dput(cur); return -ENOENT; }
        //符号链接,读出目标拼上剩余路径,从链接所在目录重新解析
        if((next->d_inode->i_mode & S_IFMT) == S_IFLNK){
            const char *rest = p;
            while(*rest == '/')rest ++;
            if(*rest == 0 && (flags & LOOKUP_NOFOLLOW)){//末段明确要求不跟随, 返回链接本身
                dput(cur);
                cur = next;
                break;
            }
            if(depth >= SYMLINK_MAX_HOP){ dput(next); dput(cur); return -ELOOP; }
            if(!next->d_inode->i_op || !next->d_inode->i_op->get_link){
                dput(next); dput(cur); return -EINVAL;
            }
            const char *target = NULL;
            int lr = next->d_inode->i_op->get_link(next, next->d_inode, &target);
            if(lr || !target){ dput(next); dput(cur); return lr ? lr : -EINVAL; }
            //拼出新路径 = 链接目标 + "/" + 尚未解析的部分
            char npath[SYMLINK_PATH_MAX];
            int tl = (int)strlen(target);
            int rl = (int)strlen(rest);
            if(tl + 1 + rl + 1 > SYMLINK_PATH_MAX){ dput(next); dput(cur); return -ENAMETOOLONG; }
            memcpy(npath, target, (size_t)tl);
            if(rl){ npath[tl] = '/'; memcpy(npath + tl + 1, rest, (size_t)rl); npath[tl + 1 + rl] = 0; }
            else  npath[tl] = 0;
            //绝对目标从进程根解析,相对目标从链接所在目录解析
            struct path nroot;
            if(target[0] == '/' && rootpath){
                nroot.mnt = rootpath->mnt;
                nroot.dentry = dget(rootpath->dentry);
            }else if(target[0] == '/'){
                nroot.mnt = root->mnt;
                nroot.dentry = dget(root->dentry);
            }else{
                nroot.mnt = mnt;
                nroot.dentry = cur;
            }
            dput(next);
            int nr = walk(npath, &nroot, rootpath, to_parent, out, last_name, namesz, flags, depth + 1);
            dput(nroot.dentry);
            return nr;
        }
        dput(cur);
        cur = next;
    }

    out->mnt = mnt;
    out->dentry = cur;
    return 0;
}
/*由DeepSeek V4.1 Flash-END*/

//解析完整路径
int path_lookup(const char *path, const struct path *root, unsigned flags, struct path *out){
    if(!out)return -EINVAL;
    out->dentry = NULL;
    out->mnt = NULL;
    //解析路径
    const struct path *rp = VfsRoot ? VfsRoot() : root;
    int ec = walk(path,root,rp ? rp : root,0,out,NULL,0,flags,0);
    if(ec != 0)return ec;
    //负缓存
    if(!out->dentry->d_inode){
        dput(out->dentry);
        out->dentry = NULL;
        return -ENOENT;
    }
    //末段正好是个挂载点,穿到被挂载文件系统的根
    while(out->dentry->d_mounted){
        struct vfsmount *sm = out->dentry->d_mounted;
        dput(out->dentry);
        out->mnt = sm;
        out->dentry = dget(sm->mnt_root);
        if(!out->dentry)return -ENOENT;
    }
    //如果LOOKUP_DIRECTORY被设置，但解析结果不是目录，返回-ENOTDIR
    if((flags & LOOKUP_DIRECTORY) && ((out->dentry->d_inode->i_mode & S_IFMT) != S_IFDIR)){
        dput(out->dentry);
        out->dentry = NULL;
        return -ENOTDIR;
    }
    return 0;
}

//只解析到父目录,最后一段名字放进name
int path_lookup_parent(const char *path, const struct path *root, struct path *out,char *name, int namesz){
    if(!out || !name || namesz <= 0)return -EINVAL;
    out->mnt = NULL;
    out->dentry = NULL;
    //解析路径到父目录
    const struct path *rp = VfsRoot ? VfsRoot() : root;
    int ec = walk(path, root, rp ? rp : root, 1, out, name, namesz, LOOKUP_FOLLOW, 0);
    if(ec)return ec;
    //父目录必须存在且是目录
    if(!out->dentry->d_inode || ((out->dentry->d_inode->i_mode & S_IFMT) != S_IFDIR)){
        dput(out->dentry);
        out->dentry = NULL;
        return -ENOTDIR;
    }
    return 0;
}

/*
 * 把 dentry 还原成绝对路径(跨挂载点时会接上挂载点所在的目录)
 * 返回字符串长度(不含结尾 NUL), 缓冲不够返回 -ERANGE
 */
int d_path(const struct path *p, char *buf, int size){
    if(!p || !p->dentry || !buf || size <= 0)return -EINVAL;
    const char *names[64];
    int n = 0;
    struct dentry *d = p->dentry;
    const struct vfsmount *m = p->mnt;
    for(;;){
        while(d && d->d_parent && n < 64){
            names[n ++] = d->d_name;
            d = d->d_parent;
        }
        //已经在某个 fs 的根上: 沿挂载链接到父文件系统
        if(!m || !m->mnt_mountpoint)break;
        d = m->mnt_mountpoint;
        m = m->mnt_parent;
    }
    if(n == 0){//就是根
        if(size < 2)return -ERANGE;
        buf[0] = '/';
        buf[1] = 0;
        return 1;
    }
    int pos = 0;
    for(int i = n - 1;i >= 0;i --){
        int len = (int)strlen(names[i]);
        if(pos + 1 + len >= size)return -ERANGE;
        buf[pos ++] = '/';
        memcpy(buf + pos, names[i], (size_t)len);
        pos += len;
    }
    buf[pos] = 0;
    return pos;
}
