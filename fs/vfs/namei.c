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
//逐段解析
static int walk(const char *path, const struct path *root, int to_parent,struct path *out, char *last_name, int namesz){
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
        if(cur->d_inode->i_op && cur->d_inode->i_op->permission &&
           cur->d_inode->i_op->permission(cur->d_inode, MAY_EXEC)){
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
    int ec = walk(path,root,0,out,NULL,0);
    if(ec != 0)return ec;
    //负缓存
    if(!out->dentry->d_inode){
        dput(out->dentry);
        out->dentry = NULL;
        return -ENOENT;
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
    int ec = walk(path, root, 1, out, name, namesz);
    if(ec)return ec;
    //父目录必须存在且是目录
    if(!out->dentry->d_inode || ((out->dentry->d_inode->i_mode & S_IFMT) != S_IFDIR)){
        dput(out->dentry);
        out->dentry = NULL;
        return -ENOTDIR;
    }
    return 0;
}
