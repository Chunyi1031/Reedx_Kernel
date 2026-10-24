// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/vfs/dcache.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 目录项缓存(dcache)
 *
 * 由DeepSeek V4.1 Flash生成
 */
#include <vfs.h>
#include <mm/kslab.h>
#include <print.h>

#define DCACHE_BUCKETS     1024u
#define DCACHE_DEFAULT_MAX 4096u

static struct list_node g_d_hash[DCACHE_BUCKETS];
static struct list_node g_d_lru;
static uint32_t g_nr_dentry;
static uint32_t g_dc_limit = DCACHE_DEFAULT_MAX;
static spinlock_t g_dc_lock = {0};
static int g_dc_ready;
static uint64_t g_st_alloc,g_st_free,g_st_hit,g_st_miss;

#define d_entry(node) ((struct dentry*)((uintptr_t)(node) - __builtin_offsetof(struct dentry, d_hash)))
#define lru_entry(node) ((struct dentry*)((uintptr_t)(node) - __builtin_offsetof(struct dentry, d_lru)))

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
static void dc_init(void){
    if(g_dc_ready)return;
    for(uint32_t i = 0;i < DCACHE_BUCKETS;i ++){
        g_d_hash[i].prev = &g_d_hash[i];
        g_d_hash[i].next = &g_d_hash[i];
    }
    g_d_lru.prev = &g_d_lru;
    g_d_lru.next = &g_d_lru;
    g_nr_dentry = 0;
    g_dc_ready = 1;
}

//FNV-1a
static uint32_t name_hash(const char *s, uint32_t len){
    uint32_t h = 2166136261u;
    for(uint32_t i = 0;i < len;i ++){
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t d_bucket(struct dentry *parent, const char *name, uint32_t len){
    uint64_t p = (uint64_t)(uintptr_t)parent;
    return (uint32_t)((name_hash(name, len) ^ (p >> 6)) & (DCACHE_BUCKETS - 1));
}

//按名字长度分档(只分5档, 否则名字长度种类一多就会把 slab 缓存表耗尽)
static uint32_t d_name_class(uint32_t len){
    if(len <= 16) return 16;
    if(len <= 32) return 32;
    if(len <= 64) return 64;
    if(len <= 128)return 128;
    return 256;
}

static struct kmem_cache *dentry_cache(uint32_t len){
    uint32_t size = (uint32_t)sizeof(struct dentry) + d_name_class(len) + 1;
    return KslabCreate("dentry", size);
}

struct dentry *d_alloc(struct dentry *parent, const char *name){
    dc_init();
    if(!name)name = "";
    uint32_t len = (uint32_t)strlen(name);
    if(len > NAMELEN_MAX)return NULL;
    struct kmem_cache *c = dentry_cache(len);
    if(!c)return NULL;
    struct dentry *d = (struct dentry*)KslabZalloc(c);
    if(!d)return NULL;
    d->d_name = (char*)(d + 1);
    memcpy(d->d_name, name, len + 1);
    d->d_name_len = len;
    d->d_parent   = parent;
    d->d_count    = 1;
    d->d_subdirs.prev = &d->d_subdirs;
    d->d_subdirs.next = &d->d_subdirs;
    d->d_child.prev = d->d_child.next = NULL;
    d->d_alias.prev = d->d_alias.next = NULL;
    d->d_hash.prev  = d->d_hash.next  = NULL;
    d->d_lru.prev   = d->d_lru.next   = NULL;
    g_st_alloc ++;
    return d;
}

//只在缓存里找, 不触发任何文件系统操作
struct dentry *d_lookup(struct dentry *parent, const char *name){
    dc_init();
    if(!name)return NULL;
    uint32_t len = (uint32_t)strlen(name);
    if(len > NAMELEN_MAX)return NULL;
    uint64_t flags;
    spin_lock_irqsave(&g_dc_lock, flags);
    struct list_node *head = &g_d_hash[d_bucket(parent, name, len)];
    struct list_node *n;
    for(n = head->next;n != head;n = n->next){
        struct dentry *d = d_entry(n);
        if(d->d_parent == parent && d->d_name_len == len &&
           memcmp(d->d_name, name, len) == 0){
            d->d_count ++;
            if(d->d_lru.next)l_del(&d->d_lru);
            g_st_hit ++;
            spin_unlock_irqrestore(&g_dc_lock, flags);
            return d;
        }
    }
    g_st_miss ++;
    spin_unlock_irqrestore(&g_dc_lock, flags);
    return NULL;
}

//把 dentry 与 inode 绑定并放进哈希(d_inode==NULL 就是负缓存)
//★ dentry 持有 inode 的一份引用, 由 d_invalidate()/dc_shrink() 负责释放
void d_add(struct dentry *dentry, struct inode *inode){
    if(!dentry)return;
    dc_init();
    uint64_t flags;
    int added = 0;
    int rebind = 0;
    struct inode *old = NULL;
    spin_lock_irqsave(&g_dc_lock, flags);
    if(!(dentry->d_flags & DCACHE_NEW)){
        dentry->d_inode = inode;
        l_add_tail(&dentry->d_hash, &g_d_hash[d_bucket(dentry->d_parent, dentry->d_name, dentry->d_name_len)]);
        dentry->d_flags |= DCACHE_NEW;
        dentry->d_flags &= ~DCACHE_NEGATIVE;
        g_nr_dentry ++;
        added = 1;
    }else if(dentry->d_inode != inode){
        //负缓存转正(或换了 inode): 旧的归还引用, 新绑定的要占一份
        old = dentry->d_inode;
        dentry->d_inode = inode;
        if(inode)dentry->d_flags &= ~DCACHE_NEGATIVE;
        else     dentry->d_flags |=  DCACHE_NEGATIVE;
        rebind = 1;
    }
    spin_unlock_irqrestore(&g_dc_lock, flags);
    if((added || rebind) && inode)inode->i_count ++;
    if(old && old != inode)Iput(old);
}

//名字失效: 从哈希里摘掉(内容仍被引用着, 等 dput 回收)
void d_drop(struct dentry *dentry){
    if(!dentry)return;
    uint64_t flags;
    spin_lock_irqsave(&g_dc_lock, flags);
    if(dentry->d_flags & DCACHE_NEW){
        l_del(&dentry->d_hash);
        dentry->d_flags &= ~DCACHE_NEW;
        if(g_nr_dentry)g_nr_dentry --;
    }
    spin_unlock_irqrestore(&g_dc_lock, flags);
}

void d_invalidate(struct dentry *dentry){
    if(!dentry)return;
    struct inode *ino;
    uint64_t flags;
    d_drop(dentry);
    spin_lock_irqsave(&g_dc_lock, flags);
    ino = dentry->d_inode;
    dentry->d_inode = NULL;
    dentry->d_flags |= DCACHE_NEGATIVE;
    spin_unlock_irqrestore(&g_dc_lock, flags);
    if(ino)Iput(ino);          //归还 dentry 持有的那份引用
}

struct dentry *dget(struct dentry *dentry){
    if(!dentry)return NULL;
    uint64_t flags;
    spin_lock_irqsave(&g_dc_lock, flags);
    dentry->d_count ++;
    if(dentry->d_lru.next)l_del(&dentry->d_lru);
    spin_unlock_irqrestore(&g_dc_lock, flags);
    return dentry;
}

//从 LRU 回收, 直到不超过上限
static void dc_shrink(void){
    for(;;){
        struct dentry *victim = NULL;
        uint64_t flags;
        spin_lock_irqsave(&g_dc_lock, flags);
        if(g_nr_dentry <= g_dc_limit){
            spin_unlock_irqrestore(&g_dc_lock, flags);
            return;
        }
        struct list_node *n;
        for(n = g_d_lru.next;n != &g_d_lru;n = n->next){
            struct dentry *d = lru_entry(n);
            //挂载点与"有子目录"的 dentry 先留着
            if(d->d_mounted)continue;
            if(d->d_subdirs.next != &d->d_subdirs)continue;
            victim = d;
            break;
        }
        if(!victim){
            spin_unlock_irqrestore(&g_dc_lock, flags);
            return;
        }
        l_del(&victim->d_lru);
        if(victim->d_flags & DCACHE_NEW){
            l_del(&victim->d_hash);
            victim->d_flags &= ~DCACHE_NEW;
        }
        if(g_nr_dentry)g_nr_dentry --;
        uint32_t len = victim->d_name_len;
        struct inode *ino = victim->d_inode;
        spin_unlock_irqrestore(&g_dc_lock, flags);
        KslabFree(dentry_cache(len), victim);
        if(ino)Iput(ino);      //归还 dentry 持有的那份引用
        g_st_free ++;
    }
}

void dput(struct dentry *dentry){
    if(!dentry)return;
    uint64_t flags;
    spin_lock_irqsave(&g_dc_lock, flags);
    if(!dentry->d_count){
        spin_unlock_irqrestore(&g_dc_lock, flags);
        printk(PRINTK_ERR"vfs: dput on dentry '%s' with zero count", dentry->d_name);
        return;
    }
    dentry->d_count --;
    int last = (dentry->d_count == 0);
    if(last && !dentry->d_mounted)l_add_tail(&dentry->d_lru, &g_d_lru);
    spin_unlock_irqrestore(&g_dc_lock, flags);
    if(last)dc_shrink();
}

void DcacheSetLimit(uint32_t max){
    g_dc_limit = max ? max : DCACHE_DEFAULT_MAX;
}

uint32_t DcacheCount(void){ return g_nr_dentry; }

int DcacheSelfTest(void){
    dc_init();
    struct dentry *root = d_alloc(NULL, "/");
    if(!root)return -1;
    //给根节点伪造一个 inode(不需要它真的存在, 只测哈希/引用计数)
    struct dentry *a = d_alloc(root, "abc");
    struct dentry *b = d_alloc(root, "abc2");
    if(!a || !b)return -2;
    int ok = 1;
    d_add(a, NULL);
    d_add(b, NULL);
    //同名查找必须命中同一个节点
    struct dentry *x = d_lookup(root, "abc");
    if(x != a || a->d_count != 2)ok = 0;
    if(x)dput(x);
    //不存在的名字必须 miss
    if(d_lookup(root, "zzz") != NULL)ok = 0;
    //前缀不相同的名字不能误命中
    if(d_lookup(root, "abcd") != NULL)ok = 0;
    d_drop(a);
    if(d_lookup(root, "abc") != NULL)ok = 0;
    dput(a);
    dput(b);
    dput(root);
    if(!ok)printk(PRINTK_ERR"vfs: dcache self-test FAILED");
    return ok ? 0 : -3;
}
