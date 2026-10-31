// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/vfs/mount.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 挂载树(mount tree)
 */
#include <vfs.h>
#include <fs.h>
#include <mm/kslab.h>
#include <print.h>

static struct list_node g_mount_list;
static uint32_t g_nr_mounts;//当前挂载数量
static struct kmem_cache *g_mnt_cache;//挂载结构缓存
static int g_mnt_ready;

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

//初始化挂载子系统
static void mnt_init(void){
    if(g_mnt_ready)return;
    g_mount_list.prev = &g_mount_list;
    g_mount_list.next = &g_mount_list;
    g_nr_mounts = 0;
    g_mnt_cache = KslabCreate("vfsmount", sizeof(struct vfsmount));
    g_mnt_ready = 1;
}

//分配挂载结构
struct vfsmount *MntAlloc(void){
    mnt_init();
    if(!g_mnt_cache)return NULL;
    struct vfsmount *m = (struct vfsmount*)KslabZalloc(g_mnt_cache);
    if(!m)return NULL;
    m->mnt_list.prev = &m->mnt_list;
    m->mnt_list.next = &m->mnt_list;
    m->mnt_child.prev = &m->mnt_child;
    m->mnt_child.next = &m->mnt_child;
    m->mnt_children.prev = &m->mnt_children;
    m->mnt_children.next = &m->mnt_children;
    return m;
}

//释放挂载结构
void MntFree(struct vfsmount *m){
    if(!m || !g_mnt_cache)return;
    KslabFree(g_mnt_cache, m);
}

//内核挂载
struct vfsmount *VfsKernMount(struct super_block *sb, struct dentry *root, uint32_t flags){
    if(!sb)return NULL;
    mnt_init();
    struct vfsmount *m = MntAlloc();
    if(!m)return NULL;
    m->mnt_sb = sb;
    m->mnt_root = root;
    m->mnt_flags = flags;
    m->mnt_parent = NULL;
    m->mnt_mountpoint = NULL;
    l_add_tail(&m->mnt_list, &g_mount_list);
    g_nr_mounts ++;
    return m;
}

//把child挂到parent挂载的mountpoint目录上
int VfsAttachMount(struct vfsmount *parent, struct dentry *mountpoint,struct vfsmount *child){
    if(!parent || !mountpoint || !child)return -EINVAL;
    if(!parent->mnt_sb)return -EINVAL;
    if(!mountpoint->d_inode)return -ENOENT;//挂载点不存在
    if(mountpoint->d_inode->i_sb != parent->mnt_sb)return -EINVAL;
    if(child->mnt_parent)return -EBUSY;//已挂载
    if(mountpoint->d_mounted)return -EBUSY;//该目录已有挂载点
    for(struct vfsmount *p = parent;p;p = p->mnt_parent){
        if(p == child)return -EINVAL;
    }
    child->mnt_parent = parent;
    child->mnt_mountpoint = mountpoint;
    mountpoint->d_mounted = child;
    mountpoint->d_flags |= DCACHE_MOUNTED;
    l_add_tail(&child->mnt_child, &parent->mnt_children);
    return 0;
}

//卸载
int VfsDetachMount(struct vfsmount *m){
    if(!m)return -EINVAL;
    if(m->mnt_children.next != &m->mnt_children)return -EBUSY;//有子挂载
    //从父挂载移除
    if(m->mnt_mountpoint){
        m->mnt_mountpoint->d_mounted = NULL;
        m->mnt_mountpoint->d_flags  &= ~DCACHE_MOUNTED;
        l_del(&m->mnt_child);
        m->mnt_parent     = NULL;
        m->mnt_mountpoint = NULL;
    }
    l_del(&m->mnt_list);
    if(g_nr_mounts)g_nr_mounts --;
    MntFree(m);//释放挂载结构
    return 0;
}

struct dentry *VfsDotDot(struct vfsmount **mnt_io, struct dentry *dentry){
    if(!mnt_io || !dentry)return NULL;
    struct vfsmount *m = *mnt_io;
    while(m && m->mnt_parent && dentry == m->mnt_root){
        dentry = m->mnt_mountpoint;
        m = m->mnt_parent;
        if(!dentry)break;
    }
    *mnt_io = m;
    if(!dentry)return NULL;
    if(!dentry->d_parent)return dentry;
    return dentry->d_parent;
}

//已挂载数
uint32_t VfsMountCount(void){
    return g_nr_mounts;
}

void VfsDumpMounts(void){
    printk(PRINTK_INFO"vfs: %u mount(s)", g_nr_mounts);
    struct list_node *n;
    for(n = g_mount_list.next;n != &g_mount_list;n = n->next){
        struct vfsmount *m = (struct vfsmount*)((uintptr_t)n - __builtin_offsetof(struct vfsmount, mnt_list));
        printk(PRINTK_INFO"  %s%s magic=%llx flags=%x",
               m->mnt_sb ? m->mnt_sb->s_type_name : "?",
               m->mnt_parent ? " (attached)" : " (root)",
               m->mnt_sb ? m->mnt_sb->s_magic : 0, m->mnt_flags);
    }
}

//在mountpoint上挂一个新的内存文件系统
int VfsMountNew(struct vfsmount *parent, struct dentry *mountpoint, const char *type){
    //检查参数与挂载点
    if(!parent || !mountpoint || !type)return -EINVAL;
    if((mountpoint->d_inode->i_mode & S_IFMT) != S_IFDIR)return -ENOTDIR;
    //新建一个超级块,并把它的根目录dentry建出来
    struct super_block *sb = NULL;
    struct dentry *root = NULL;
    if(strcmp(type, "devtmpfs") == 0){
        sb = SuperAlloc("devtmpfs", 0x1373, DevFsInodeSize(), DevFsSuperOps());
        if(!sb)return -ENOMEM;
        root = DevFsFillSuper(sb);
    }else if(strcmp(type, "ramfs") == 0 || strcmp(type, "tmpfs") == 0){
        sb = SuperAlloc("ramfs", 0x01021994, Ramfs2InodeSize(), Ramfs2SuperOps());
        if(!sb)return -ENOMEM;
        root = Ramfs2FillSuper(sb);
    }else{
        return -ENODEV;
    }
    if(!root)return -ENOMEM;
    //先把新文件系统挂成一棵独立的挂载树,再挂到mountpoint下面
    struct vfsmount *m = VfsKernMount(sb, root, 0);
    if(!m)return -ENOMEM;
    int r = VfsAttachMount(parent, mountpoint, m);
    if(r)VfsDetachMount(m);//挂载失败要回收刚建的挂载树
    return r;
}
