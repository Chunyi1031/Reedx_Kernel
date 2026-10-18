// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/vfs/inode.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * inode 缓存(icache): 键 = (sb, i_ino)
 * 由DeepSeek V4.1 Flash生成
 */
#include <vfs.h>
#include <mm/kslab.h>
#include <print.h>

#define ICACHE_BUCKETS 4096u   //必须是 2 的幂
#define ICACHE_DEFAULT_LIMIT 2048u

static struct list_node g_ino_hash[ICACHE_BUCKETS];
static struct list_node g_ino_lru;          //i_count==0 的 inode
static uint32_t g_nr_inodes;                //当前缓存的 inode 数
static uint32_t g_icache_limit = ICACHE_DEFAULT_LIMIT;
static spinlock_t g_icache_lock = {0};
static int g_ino_ready;

//统计(便于发现"多减/少减"引用)
static uint64_t g_st_alloc, g_st_free, g_st_hit, g_st_evict;

#define ino_entry(node) ((struct inode*)((uintptr_t)(node) - __builtin_offsetof(struct inode, i_hash)))
#define lru_entry(node) ((struct inode*)((uintptr_t)(node) - __builtin_offsetof(struct inode, i_lru)))

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

static void ino_init(void){
    if(g_ino_ready)return;
    for(uint32_t i = 0;i < ICACHE_BUCKETS;i ++){
        g_ino_hash[i].prev = &g_ino_hash[i];
        g_ino_hash[i].next = &g_ino_hash[i];
    }
    g_ino_lru.prev = &g_ino_lru;
    g_ino_lru.next = &g_ino_lru;
    g_nr_inodes = 0;
    g_ino_ready = 1;
}

//哈希: sb 指针参与, 避免不同 FS 相同 ino 撞在一起
static inline uint32_t ino_bucket(struct super_block *sb, uint64_t ino){
    uint64_t h = ino * 2654435761ULL;
    h ^= (uint64_t)(uintptr_t)sb >> 4;
    return (uint32_t)(h & (ICACHE_BUCKETS - 1));
}

//查 icache(调用方持锁)
static struct inode *ino_find_locked(struct super_block *sb, uint64_t ino){
    struct list_node *head = &g_ino_hash[ino_bucket(sb, ino)];
    struct list_node *n;
    for(n = head->next;n != head;n = n->next){
        struct inode *i = ino_entry(n);
        if(i->i_sb == sb && i->i_ino == ino)return i;
    }
    return NULL;
}

//从 FS 的 slab 里分配一个 inode 并清零
static struct inode *ino_alloc(struct super_block *sb){
    if(!sb || !sb->s_inode_cache)return NULL;
    if(sb->s_op && sb->s_op->alloc_inode)return sb->s_op->alloc_inode(sb);
    void *p = KslabZalloc(sb->s_inode_cache);
    if(!p)return NULL;
    struct inode *i = (struct inode*)p;
    if(sb->s_inode_size > sizeof(struct inode))
        i->i_private = (void*)((uint8_t*)p + sizeof(struct inode));
    g_st_alloc ++;
    return i;
}

static void ino_free(struct super_block *sb, struct inode *i){
    if(!sb || !i)return;
    if(sb->s_op && sb->s_op->destroy_inode){
        sb->s_op->destroy_inode(i);
        return;
    }
    KslabFree(sb->s_inode_cache, i);
    g_st_free ++;
}

//把 LRU 上的 inode 回收掉, 直到不超过上限
static void icache_shrink(void){
    for(;;){
        struct inode *victim = NULL;
        uint64_t flags;
        spin_lock_irqsave(&g_icache_lock, flags);
        if(g_nr_inodes <= g_icache_limit){
            spin_unlock_irqrestore(&g_icache_lock, flags);
            return;
        }
        if(g_ino_lru.next != &g_ino_lru){
            struct inode *cand = lru_entry(g_ino_lru.next);
            if(cand->i_state & (I_DIRTY | I_WILL_FREE)){
                //脏 inode 不能丢(会丢元数据), 移到 LRU 尾部下次再说
                l_del(&cand->i_lru);
                l_add_tail(&cand->i_lru, &g_ino_lru);
                spin_unlock_irqrestore(&g_icache_lock, flags);
                continue;
            }
            victim = cand;
            l_del(&victim->i_lru);
            l_del(&victim->i_hash);
            g_nr_inodes --;
            victim->i_state |= I_FREEING;
        }
        spin_unlock_irqrestore(&g_icache_lock, flags);
        if(!victim)return;
        //彻底没人用了才可能到达这里(i_nlink 一定为 0 才会在 iput 时进 LRU 并被回收)
        ino_free(victim->i_sb, victim);
        g_st_evict ++;
    }
}

struct inode *IgetLocked(struct super_block *sb, uint64_t ino){
    ino_init();
    if(!sb || !sb->s_inode_cache)return NULL;
    uint64_t flags;

    //快路径: 缓存命中(含正在被读取的 I_NEW, 直接复用同一个 inode)
    spin_lock_irqsave(&g_icache_lock, flags);
    struct inode *i = ino_find_locked(sb, ino);
    if(i){
        i->i_count ++;
        if(i->i_count == 1 && i->i_lru.next)l_del(&i->i_lru);
        g_st_hit ++;
        spin_unlock_irqrestore(&g_icache_lock, flags);
        return i;
    }
    spin_unlock_irqrestore(&g_icache_lock, flags);

    //未命中: 先在锁外分配
    struct inode *ni = ino_alloc(sb);
    if(!ni)return NULL;
    ni->i_sb    = sb;
    ni->i_ino   = ino;
    ni->i_count = 1;
    ni->i_state = I_NEW;

    spin_lock_irqsave(&g_icache_lock, flags);
    struct inode *dup = ino_find_locked(sb, ino);
    if(dup){//并发下别人抢先插入了: 丢掉自己刚分配的, 改用已有的
        spin_unlock_irqrestore(&g_icache_lock, flags);
        ino_free(sb, ni);
        spin_lock_irqsave(&g_icache_lock, flags);
        dup->i_count ++;
        if(dup->i_count == 1 && dup->i_lru.next)l_del(&dup->i_lru);
        spin_unlock_irqrestore(&g_icache_lock, flags);
        return dup;
    }
    l_add_tail(&ni->i_hash, &g_ino_hash[ino_bucket(sb, ino)]);
    ni->i_lru.prev = ni->i_lru.next = NULL;
    g_nr_inodes ++;
    spin_unlock_irqrestore(&g_icache_lock, flags);

    if(g_nr_inodes > g_icache_limit)icache_shrink();
    return ni;
}

void Iput(struct inode *inode){
    if(!inode)return;
    if(!inode->i_count){
        printk(PRINTK_ERR"vfs:The reference count of inode %llu is already 0.", inode->i_ino);
        return;
    }
    uint64_t flags;
    spin_lock_irqsave(&g_icache_lock, flags);
    inode->i_count --;
    int last = (inode->i_count == 0);
    if(last && inode->i_state & I_FREEING){
        spin_unlock_irqrestore(&g_icache_lock, flags);
        return;
    }
    if(last)l_add_tail(&inode->i_lru, &g_ino_lru);
    spin_unlock_irqrestore(&g_icache_lock, flags);
    //文件已被删除
    if(last && inode->i_nlink == 0){
        struct super_block *sb = inode->i_sb;
        if(sb && sb->s_op && sb->s_op->evict_inode)sb->s_op->evict_inode(inode);
        spin_lock_irqsave(&g_icache_lock, flags);
        if(inode->i_lru.next)l_del(&inode->i_lru);
        if(inode->i_hash.next)l_del(&inode->i_hash);
        if(g_nr_inodes)g_nr_inodes --;
        inode->i_state |= I_FREEING;
        spin_unlock_irqrestore(&g_icache_lock, flags);
        ino_free(sb, inode);
        g_st_evict ++;
    }
}

void InodeMarkDirty(struct inode *inode){
    if(!inode)return;
    inode->i_state |= I_DIRTY;
    struct super_block *sb = inode->i_sb;
    if(sb && sb->s_op && sb->s_op->dirty_inode)sb->s_op->dirty_inode(inode);
}

void InodeSetLimit(uint32_t max){
    g_icache_limit = max ? max : ICACHE_DEFAULT_LIMIT;
}

uint32_t IcacheCount(void){
    return g_nr_inodes;
}

void IcacheStats(void){
    uint32_t lru = 0, used = 0;
    uint64_t flags;
    spin_lock_irqsave(&g_icache_lock, flags);
    struct list_node *n;
    for(n = g_ino_lru.next;n != &g_ino_lru;n = n->next)lru ++;
    used = g_nr_inodes - lru;
    spin_unlock_irqrestore(&g_icache_lock, flags);
    printk(PRINTK_INFO"icache: inode=%u(active =%u idle =%u) Upper limit =%u hit =%llu allocation =%llu release =%llu",
           g_nr_inodes, used, lru, g_icache_limit, g_st_hit, g_st_alloc, g_st_free);
}

//自检
int IcacheSelfTest(void){
    ino_init();
    struct kmem_cache *sc = KslabCreate("super_block", sizeof(struct super_block));
    if(!sc)return -1;
    struct super_block *sb = (struct super_block*)KslabZalloc(sc);
    if(!sb)return -1;
    struct kmem_cache *ic = KslabCreate("icache_test", sizeof(struct inode));
    if(!ic)return -1;
    sb->s_inode_cache = ic;
    sb->s_inode_size  = sizeof(struct inode);
    struct inode *a = IgetLocked(sb, 12345);
    struct inode *b = IgetLocked(sb, 12345);
    int ok = (a && a == b && a->i_count == 2);
    if(!ok){ printk(PRINTK_ERR"vfs:icache self-test failed (the same ino returned a different inode)"); }
    Iput(a);
    Iput(b);
    if(ok && a->i_count != 0){
        printk(PRINTK_ERR"vfs: icache Self-test failed (count after iput =%u)", a->i_count);
        ok = 0;
    }
    KslabFree(ic, a);
    KslabFree(sc, sb);
    return ok ? 0 : -2;
}
