// SPDX-License-Identifier: GPL-2.0-only
/**
 * drives/bcache.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 块缓存
 */
#include <drives/bcache.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <print.h>

typedef struct bcache_page {
    bdev_t   *bp_bdev;
    uint64_t  bp_index;                 //页号=字节偏移/4096
    uint32_t  bp_count;                 //引用计数
    uint8_t   bp_flags;                 //BP_*
    uint8_t   bp_nblocks;               //每页块数
    uint8_t   bp_vmask;                 //内容已知的块
    uint8_t   bp_dmask;                 //待回写的块
    uint8_t  *bp_data;                  //4KB数据页
    struct bcache_page *bp_hash_next;
    struct bcache_page *bp_free_next;
    struct list_node bp_lru;
} bcache_page_t;

#define lru_entry(node) ((bcache_page_t*)((uintptr_t)(node) - __builtin_offsetof(bcache_page_t, bp_lru)))
#define all_blocks_mask(bp) ((uint8_t)((1u << (bp)->bp_nblocks) - 1u))

static bcache_page_t g_pool[BC_MAX_PAGES];//描述符池
static bcache_page_t *g_hash[BC_HASH_BUCKETS];//哈希桶
static struct list_node g_lru;
static bcache_page_t *g_free;//空闲描述符链(LIFO)
static uint32_t g_nr_used; //已占用的描述符数
static uint64_t g_limit = BC_MAX_PAGES;//页数上限
static int g_ready;//是否已初始化
static spinlock_t g_bc_lock = {0};

//统计
static uint64_t g_st_hit, g_st_miss, g_st_write, g_st_evict, g_st_flush, g_st_drop, g_st_blkread;

//链辅助
static inline void lru_del(bcache_page_t *bp){
    if(!(bp->bp_flags & BP_ONLRU))return;
    bp->bp_lru.prev->next = bp->bp_lru.next;
    bp->bp_lru.next->prev = bp->bp_lru.prev;
    bp->bp_lru.prev = bp->bp_lru.next = NULL;
    bp->bp_flags &= (uint8_t)~BP_ONLRU;
}
static inline void lru_add_tail(bcache_page_t *bp){
    if(bp->bp_flags & BP_ONLRU)return;
    bp->bp_lru.prev = g_lru.prev;
    bp->bp_lru.next = &g_lru;
    g_lru.prev->next = &bp->bp_lru;
    g_lru.prev = &bp->bp_lru;
    bp->bp_flags |= BP_ONLRU;
}

static inline uint32_t bc_hash_of(bdev_t *bd, uint64_t idx){
    return (uint32_t)((idx ^ ((uint64_t)(uintptr_t)bd >> 6)) & (BC_HASH_BUCKETS - 1));
}

static bcache_page_t *bc_hash_find(bdev_t *bd, uint64_t idx){
    bcache_page_t *bp = g_hash[bc_hash_of(bd, idx)];
    while(bp){
        if(bp->bp_bdev == bd && bp->bp_index == idx)return bp;
        bp = bp->bp_hash_next;
    }
    return NULL;
}

static void bc_hash_remove(bcache_page_t *bp){
    if(!bp->bp_bdev)return;
    uint32_t h = bc_hash_of(bp->bp_bdev, bp->bp_index);
    bcache_page_t **pp = &g_hash[h];
    while(*pp){
        if(*pp == bp){
            *pp = bp->bp_hash_next;
            bp->bp_hash_next = NULL;
            return;
        }
        pp = &(*pp)->bp_hash_next;
    }
}

static void bc_hash_insert(bcache_page_t *bp){
    uint32_t h = bc_hash_of(bp->bp_bdev, bp->bp_index);
    bp->bp_hash_next = g_hash[h];
    g_hash[h] = bp;
}

//描述符归还空闲链
static void bc_put_free(bcache_page_t *bp){
    bp->bp_bdev = NULL;
    bp->bp_index = 0;
    bp->bp_count = 0;
    bp->bp_flags = 0;
    bp->bp_vmask = 0;
    bp->bp_dmask = 0;
    bp->bp_hash_next = NULL;
    bp->bp_free_next = g_free;
    g_free = bp;
}

//初始化
void BCacheInit(uint64_t max_pages){
    //如果已经初始化，调整大小
    if(g_ready){
        BCacheSetLimit(max_pages);
        return;
    }
    memset(g_hash, 0, sizeof(g_hash));
    //初始化链表
    g_lru.prev = &g_lru;
    g_lru.next = &g_lru;
    g_free = NULL;
    //倒序压栈
    for(int i = BC_MAX_PAGES - 1;i >= 0;i --){
        bcache_page_t *bp = &g_pool[i];
        memset(bp, 0, sizeof(*bp));
        bp->bp_free_next = g_free;
        g_free = bp;
    }
    g_nr_used = 0;
    g_limit = (max_pages == 0 || max_pages > BC_MAX_PAGES) ? BC_MAX_PAGES : max_pages;
    g_ready = 1;
    printk(PRINTK_DEBUG"bcache: Page container =%u bytes maximum =%llu pages (%llu KB)",(uint32_t)BC_PAGE_SIZE, g_limit, g_limit * (BC_PAGE_SIZE / 1024));
}

_Bool BCacheReady(void){
    return g_ready;
}

void BCacheSetLimit(uint64_t max_pages){
    if(max_pages == 0 || max_pages > BC_MAX_PAGES)max_pages = BC_MAX_PAGES;
    g_limit = max_pages;
}

uint64_t BCacheLimit(void){
    return g_limit;
}

//块级I/O
//某块在设备上的有效字节数
static uint32_t bc_block_len(bdev_t *bd, uint64_t page_index, uint32_t bit){
    uint32_t bs = bd->bd_block_size;
    uint64_t start = page_index * BC_PAGE_SIZE + (uint64_t)bit * bs;
    uint64_t total = bd->bd_nr_sectors * 512ULL;
    if(start >= total)return 0;
    uint64_t left = total - start;
    if(left >= bs)return bs;
    left &= ~511ULL;
    return (uint32_t)left;
}
//读取
static int bc_read_block(bcache_page_t *bp, uint32_t bit){
    bdev_t  *bd  = bp->bp_bdev;
    uint32_t bs  = bd->bd_block_size;
    uint32_t len = bc_block_len(bd, bp->bp_index, bit);
    if(!len)return -1;
    uint8_t *p = bp->bp_data + (uint64_t)bit * bs;
    if(len < bs)memset(p + len, 0, bs - len);
    return bdev_read(bd, bp->bp_index * BC_PAGE_SIZE + (uint64_t)bit * bs, p, len);
}
//写入
static int bc_write_block(bcache_page_t *bp, uint32_t bit){
    bdev_t  *bd  = bp->bp_bdev;
    uint32_t bs  = bd->bd_block_size;
    uint32_t len = bc_block_len(bd, bp->bp_index, bit);
    if(!len)return -1;
    return bdev_write(bd, bp->bp_index * BC_PAGE_SIZE + (uint64_t)bit * bs,
                      bp->bp_data + (uint64_t)bit * bs, len);
}
//整页填充
static int bc_fill_page(bcache_page_t *bp){
    bdev_t  *bd    = bp->bp_bdev;
    uint64_t off   = bp->bp_index * BC_PAGE_SIZE;
    uint64_t total = bd->bd_nr_sectors * 512ULL;
    if(off >= total)return -1;
    uint32_t len = BC_PAGE_SIZE;
    if(off + len > total){
        len = (uint32_t)((total - off) & ~511ULL);
        if(!len)return -1;
        memset(bp->bp_data + len, 0, BC_PAGE_SIZE - len);
    }
    if(bdev_read(bd, off, bp->bp_data, len))return -1;
    bp->bp_vmask = all_blocks_mask(bp);
    return 0;
}
//把脏块写回
static int bc_write_dirty(bcache_page_t *bp){
    bdev_t *bd = bp->bp_bdev;
    uint32_t bs = bd->bd_block_size;
    int err = 0;
    if(bp->bp_dmask == all_blocks_mask(bp)){
        uint64_t off = bp->bp_index * BC_PAGE_SIZE;
        uint64_t total = bd->bd_nr_sectors * 512ULL;
        uint32_t len = BC_PAGE_SIZE;
        if(off >= total)return -1;
        if(off + len > total)len = (uint32_t)((total - off) & ~511ULL);
        if(!len)return -1;
        err = bdev_write(bd, off, bp->bp_data, len);
        if(!err)g_st_write ++;
        return err;
    }
    for(uint32_t b = 0;b < bp->bp_nblocks;b ++){
        if(!(bp->bp_dmask & (1u << b)))continue;
        if(bc_write_block(bp, b))err = -1;
        else g_st_write ++;
    }
    (void)bs;
    return err;
}

//槽位分配
static bcache_page_t *bc_get_slot(void){
    bcache_page_t *bp = NULL;
    uint64_t flags;
    //有空槽且未达上限
    spin_lock_irqsave(&g_bc_lock, flags);
    if(g_free && g_nr_used < g_limit){
        bp = g_free;
        g_free = bp->bp_free_next;
        bp->bp_free_next = NULL;
    }
    spin_unlock_irqrestore(&g_bc_lock, flags);
    if(bp)return bp;
    //达到上限
    BCacheFlush();//把脏页全部落盘
    spin_lock_irqsave(&g_bc_lock, flags);
    if(g_free && g_nr_used < g_limit){
        bp = g_free;
        g_free = bp->bp_free_next;
        bp->bp_free_next = NULL;
    }
    if(!bp){
        struct list_node *n;
        for(n = g_lru.next;n != &g_lru;n = n->next){
            bcache_page_t *v = lru_entry(n);
            if(v->bp_count)continue;
            if(v->bp_dmask)continue;
            lru_del(v);
            bc_hash_remove(v);
            if(g_nr_used)g_nr_used --;
            g_st_evict ++;
            v->bp_flags = 0;
            v->bp_bdev = NULL;
            v->bp_index = 0;
            v->bp_vmask = 0;
            v->bp_dmask = 0;
            bp = v;
            break;
        }
    }
    spin_unlock_irqrestore(&g_bc_lock, flags);
    return bp;
}
//确保描述符有数据页，并按当前bd的块大小复位页内状态
static int bc_prepare(bcache_page_t *bp, bdev_t *bd){
    uint32_t bs = bd->bd_block_size;
    if(!bs || bs > BC_PAGE_SIZE || (BC_PAGE_SIZE % bs))return -1;
    if(!bp->bp_data){
        void *pa = Pmm_Malloc(1);
        if(!pa)return -1;
        bp->bp_data = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pa);
        memset(bp->bp_data, 0, BC_PAGE_SIZE);
    }
    bp->bp_nblocks = (uint8_t)(BC_PAGE_SIZE / bs);
    bp->bp_vmask = 0;
    bp->bp_dmask = 0;
    return 0;
}
//定位块所在页、页内块号、页内偏移
static int bc_locate(bdev_t *bd, uint64_t block, uint64_t *idx, uint32_t *bit, uint32_t *off){
    uint32_t bs = bd->bd_block_size;
    if(!bs || bs > BC_PAGE_SIZE || (BC_PAGE_SIZE % bs))return -1;
    uint64_t byte = block * (uint64_t)bs;
    *idx = byte / BC_PAGE_SIZE;
    *off = (uint32_t)(byte % BC_PAGE_SIZE);
    *bit = *off / bs;
    return 0;
}
static void bh_setup(buffer_head_t *bh, bcache_page_t *bp, uint64_t block, uint32_t off){
    bh->b_page = bp;
    bh->b_block = block;
    bh->b_data = bp->bp_data + off;
    bh->b_size = bp->bp_bdev->bd_block_size;
}

//读
int sb_bread(bdev_t *bd, uint64_t block, buffer_head_t *bh){
    if(!bh || !bd || !g_ready)return -1;
    bh->b_page = NULL;
    uint64_t idx; uint32_t bit, off;
    if(bc_locate(bd, block, &idx, &bit, &off))return -1;
    if(idx * BC_PAGE_SIZE >= bd->bd_nr_sectors * 512ULL)return -1;
    uint64_t flags;
    for(;;){
        spin_lock_irqsave(&g_bc_lock, flags);
        bcache_page_t *bp = bc_hash_find(bd, idx);
        if(bp && (bp->bp_flags & BP_LOCKED)){
            spin_unlock_irqrestore(&g_bc_lock, flags);
            __asm__ volatile("pause");//别人正在填这一页
            continue;
        }
        if(bp && (bp->bp_vmask & (1u << bit))){
            bp->bp_count ++;
            lru_del(bp);
            g_st_hit ++;
            spin_unlock_irqrestore(&g_bc_lock, flags);
            bh_setup(bh, bp, block, off);
            return 0;
        }
        if(bp){//在缓存里但这一块未知: 只补读这一块, 不碰同页其它块
            bp->bp_flags |= BP_LOCKED;
            bp->bp_count ++;
            lru_del(bp);
            bh_setup(bh, bp, block, off);
            int whole = (bp->bp_vmask == 0 && bp->bp_dmask == 0);
            spin_unlock_irqrestore(&g_bc_lock, flags);

            g_st_miss ++;
            if(!whole)g_st_blkread ++;
            int r = whole ? bc_fill_page(bp) : bc_read_block(bp, bit);

            spin_lock_irqsave(&g_bc_lock, flags);
            if(r == 0 && !whole)bp->bp_vmask |= (uint8_t)(1u << bit);
            bp->bp_flags &= (uint8_t)~BP_LOCKED;
            spin_unlock_irqrestore(&g_bc_lock, flags);
            if(r){
                sb_brelse(bh);
                return -1;
            }
            return 0;
        }
        spin_unlock_irqrestore(&g_bc_lock, flags);
        break;
    }
    //完全未命中: 申请槽位(可能在锁外做 flush/淘汰)
    bcache_page_t *bp = bc_get_slot();
    if(!bp)return -1;
    if(bc_prepare(bp, bd)){
        spin_lock_irqsave(&g_bc_lock, flags);
        bc_put_free(bp);
        spin_unlock_irqrestore(&g_bc_lock, flags);
        printk(PRINTK_ERR"bcache: 无法分配缓存页");
        return -1;
    }

    spin_lock_irqsave(&g_bc_lock, flags);
    bcache_page_t *dup = bc_hash_find(bd, idx);
    if(dup){//并发下别人刚插进来
        bc_put_free(bp);
        spin_unlock_irqrestore(&g_bc_lock, flags);
        return sb_bread(bd, block, bh);
    }
    bp->bp_bdev  = bd;
    bp->bp_index = idx;
    bp->bp_count = 1;
    bp->bp_flags = BP_LOCKED;
    g_nr_used ++;
    bc_hash_insert(bp);
    bh_setup(bh, bp, block, off);
    spin_unlock_irqrestore(&g_bc_lock, flags);
    g_st_miss ++;
    int r = bc_fill_page(bp);
    spin_lock_irqsave(&g_bc_lock, flags);
    bp->bp_flags &= (uint8_t)~BP_LOCKED;
    if(r == 0){
        spin_unlock_irqrestore(&g_bc_lock, flags);
        return 0;
    }
    bc_hash_remove(bp);
    if(g_nr_used)g_nr_used --;
    bc_put_free(bp);
    spin_unlock_irqrestore(&g_bc_lock, flags);
    bh->b_page = NULL;
    return -1;
}

int sb_bget(bdev_t *bd, uint64_t block, buffer_head_t *bh){
    if(!bh || !bd || !g_ready)return -1;
    bh->b_page = NULL;
    uint64_t idx; uint32_t bit, off;
    if(bc_locate(bd, block, &idx, &bit, &off))return -1;
    uint64_t flags;
    spin_lock_irqsave(&g_bc_lock, flags);
    bcache_page_t *bp = bc_hash_find(bd, idx);
    if(bp){
        bp->bp_count ++;
        lru_del(bp);
        spin_unlock_irqrestore(&g_bc_lock, flags);
        bh_setup(bh, bp, block, off);
        return 0;
    }
    spin_unlock_irqrestore(&g_bc_lock, flags);
    bp = bc_get_slot();
    if(!bp)return -1;
    if(bc_prepare(bp, bd)){
        spin_lock_irqsave(&g_bc_lock, flags);
        bc_put_free(bp);
        spin_unlock_irqrestore(&g_bc_lock, flags);
        return -1;
    }
    spin_lock_irqsave(&g_bc_lock, flags);
    bp->bp_bdev  = bd;
    bp->bp_index = idx;
    bp->bp_count = 1;
    bp->bp_flags = 0;
    g_nr_used ++;
    bc_hash_insert(bp);
    bh_setup(bh, bp, block, off);
    spin_unlock_irqrestore(&g_bc_lock, flags);
    return 0;
}

static void bc_release_common(buffer_head_t *bh, int forget){
    if(!bh || !bh->b_page)return;
    bcache_page_t *bp = bh->b_page;
    uint64_t flags;
    spin_lock_irqsave(&g_bc_lock, flags);
    if(bp->bp_count)bp->bp_count --;
    if(bp->bp_count == 0){
        if(forget || (bp->bp_flags & BP_DROP)){
            lru_del(bp);
            bc_hash_remove(bp);
            if(g_nr_used)g_nr_used --;
            g_st_drop ++;
            bc_put_free(bp);
        }else{
            lru_add_tail(bp);
        }
    }
    spin_unlock_irqrestore(&g_bc_lock, flags);
    bh->b_page = NULL;
}

void sb_brelse(buffer_head_t *bh){
    bc_release_common(bh, 0);
}
void sb_bforget(buffer_head_t *bh){
    bc_release_common(bh, 1);
}

void sb_mark_dirty(buffer_head_t *bh){
    if(!bh || !bh->b_page)return;
    bcache_page_t *bp = bh->b_page;
    uint64_t flags;
    spin_lock_irqsave(&g_bc_lock, flags);
    if(bp->bp_bdev){
        uint32_t bs  = bp->bp_bdev->bd_block_size;
        uint32_t bit = (uint32_t)((bh->b_data - bp->bp_data) / bs);
        bp->bp_dmask |= (uint8_t)(1u << bit);
        bp->bp_vmask |= (uint8_t)(1u << bit);
    }
    spin_unlock_irqrestore(&g_bc_lock, flags);
}

void sb_bwrite_dirty(buffer_head_t *bh){
    if(!bh || !bh->b_page)return;
    bcache_page_t *bp = bh->b_page;
    uint64_t flags;
    spin_lock_irqsave(&g_bc_lock, flags);
    uint32_t bs  = bp->bp_bdev->bd_block_size;
    uint32_t bit = (uint32_t)((bh->b_data - bp->bp_data) / bs);
    bp->bp_vmask |= (uint8_t)(1u << bit);
    uint8_t saved = bp->bp_dmask;
    bp->bp_dmask |= (uint8_t)(1u << bit);
    bp->bp_dmask = (uint8_t)(1u << bit);
    spin_unlock_irqrestore(&g_bc_lock, flags);
    int r = bc_write_dirty(bp);
    spin_lock_irqsave(&g_bc_lock, flags);
    if(r == 0)bp->bp_dmask = (uint8_t)(saved & (uint8_t)~(1u << bit));
    else      bp->bp_dmask = (uint8_t)(saved | (1u << bit));
    spin_unlock_irqrestore(&g_bc_lock, flags);
}

static int bc_flush_filter(bdev_t *only){
    int err = 0;
    for(int i = 0;i < BC_MAX_PAGES;i ++){
        bcache_page_t *bp = &g_pool[i];
        uint64_t flags;
        spin_lock_irqsave(&g_bc_lock, flags);
        if(!bp->bp_bdev || !bp->bp_dmask || (bp->bp_flags & BP_LOCKED) ||
           (only && bp->bp_bdev != only)){
            spin_unlock_irqrestore(&g_bc_lock, flags);
            continue;
        }
        bp->bp_count++;
        uint8_t dmask = bp->bp_dmask;
        spin_unlock_irqrestore(&g_bc_lock, flags);
        int r = bc_write_dirty(bp);
        spin_lock_irqsave(&g_bc_lock, flags);
        if(r == 0)bp->bp_dmask &= (uint8_t)~dmask;
        else err = r;
        if(bp->bp_count)bp->bp_count--;
        if(bp->bp_count == 0)lru_add_tail(bp);
        spin_unlock_irqrestore(&g_bc_lock, flags);
    }
    g_st_flush++;
    return err;
}

int BCacheFlush(void){
    if(!g_ready)return 0;
    return bc_flush_filter(NULL);
}

int BCacheSyncBdev(bdev_t *bd){
    if(!g_ready || !bd)return 0;
    return bc_flush_filter(bd);
}

int BCacheInvalidate(bdev_t *bd){
    if(!g_ready || !bd)return -1;
    int dropped = 0, busy = 0;
    uint64_t flags;
    spin_lock_irqsave(&g_bc_lock, flags);
    for(int i = 0;i < BC_MAX_PAGES;i ++){
        bcache_page_t *bp = &g_pool[i];
        if(bp->bp_bdev != bd)continue;
        if(bp->bp_count || bp->bp_dmask){ busy ++; continue; }
        lru_del(bp);
        bc_hash_remove(bp);
        if(g_nr_used)g_nr_used --;
        bc_put_free(bp);
        dropped ++;
    }
    spin_unlock_irqrestore(&g_bc_lock, flags);
    if(busy)printk(PRINTK_DEBUG"bcache:%s %d pages are still occupied or have dirty blocks, which have not expired.",bd->bd_name, busy);
    return dropped;
}

void BCacheStats(void){
    uint32_t dirty = 0, pinned = 0;
    uint64_t flags;
    spin_lock_irqsave(&g_bc_lock, flags);
    for(int i = 0;i < BC_MAX_PAGES;i ++){
        bcache_page_t *bp = &g_pool[i];
        if(!bp->bp_bdev)continue;
        if(bp->bp_dmask)dirty ++;
        if(bp->bp_count)pinned ++;
    }
    spin_unlock_irqrestore(&g_bc_lock, flags);
    uint64_t total = g_st_hit + g_st_miss;
    uint64_t rate  = total ? (g_st_hit * 100 / total) : 0;
    printk(PRINTK_INFO"bcache:Page =%llu/%llu Hit =%llu Miss =%llu(%llu%%) Block Read =%llu Write =%llu Out =%llu Dirty =%u Occupied =%u",
           (uint64_t)g_nr_used, g_limit, g_st_hit, g_st_miss, rate,
           g_st_blkread, g_st_write, g_st_evict, dirty, pinned);
}

int BCacheSelfTest(bdev_t *bd){
    if(!bd)return -1;
    buffer_head_t bh1, bh2;
    memset(&bh1, 0, sizeof(bh1));
    memset(&bh2, 0, sizeof(bh2));
    if(sb_bread(bd, 0, &bh1))return -2;
    uint64_t hit0 = g_st_hit;
    if(sb_bread(bd, 0, &bh2)){
        sb_brelse(&bh1);
        return -3;
    }
    int ok = (g_st_hit == hit0 + 1) && (bh1.b_data == bh2.b_data) && (bh1.b_page == bh2.b_page);
    sb_brelse(&bh1);
    sb_brelse(&bh2);
    return ok ? 0 : -4;
}
