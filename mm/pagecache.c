//由DeepSeek V4.1 Flash生成
#include <mm/pagecache.h>
#include <mm/pmm.h>
#include <mm/pgtables.h>
#include <mm/vmm.h>
#include <print.h>

//条目上限(缓存页数上限)与默认容量
#define PC_MAX_ENTRIES  8192
#define PC_DEF_ENTRIES  4096            //默认16MB
#define PC_MIN_ENTRIES  256
//哈希桶(2的幂)
#define PC_HASH_BITS    10
#define PC_HASH_SIZE    (1u << PC_HASH_BITS)
//预读窗口(页), 128KB
#define PC_RA_PAGES     32

#define PC_NIL 0xFFFFFFFFu

//缓存条目
typedef struct pc_ent {
    uint64_t ino;       //文件标识
    uint64_t fsize;     //文件大小(键的一部分)
    uint64_t pgoff;     //页号
    void    *page;      //缓存页(物理地址)
    uint32_t hnext;     //哈希链/空闲链
    uint32_t lprev;     //LRU前驱
    uint32_t lnext;     //LRU后继
} pc_ent_t;

static pc_ent_t  *g_pc;             //条目数组
static uint32_t  g_pc_max;          //实际分配到的条目数
static uint32_t *g_pc_hash;         //哈希桶
static uint8_t   *g_pc_scratch;     //预读中转缓冲
static uint32_t  g_pc_free;         //空闲链头
static uint32_t  g_pc_lru_head;     //最近使用
static uint32_t  g_pc_lru_tail;     //最久未用
static uint32_t  g_pc_count;        //已缓存页数
static uint32_t  g_pc_limit = PC_DEF_ENTRIES;
static _Bool     g_pc_inited = false;
static spinlock_t g_pc_lock = {0};

//统计
static uint64_t g_pc_hits;          //命中次数
static uint64_t g_pc_misses;        //未命中次数
static uint64_t g_pc_evict;         //淘汰页数
static uint64_t g_pc_disk_bytes;    //因填充实际读盘的字节数
static uint64_t g_pc_fill_pages;    //填充的页数

static uint32_t pc_hash(uint64_t ino, uint64_t fsize, uint64_t pgoff){
    uint64_t h = ino * 0x9E3779B97F4A7C15ULL;
    h ^= fsize * 0xC2B2AE3D27D4EB4FULL;
    h ^= pgoff * 0x165667B19E3779F9ULL;
    h ^= h >> 29;
    return (uint32_t)(h & (PC_HASH_SIZE - 1));
}

//---- LRU链表(表头=最近使用) ----
static void pc_lru_unlink(uint32_t i){
    uint32_t p = g_pc[i].lprev, n = g_pc[i].lnext;
    if(p != PC_NIL) g_pc[p].lnext = n; else g_pc_lru_head = n;
    if(n != PC_NIL) g_pc[n].lprev = p; else g_pc_lru_tail = p;
}

static void pc_lru_push(uint32_t i){
    g_pc[i].lprev = PC_NIL;
    g_pc[i].lnext = g_pc_lru_head;
    if(g_pc_lru_head != PC_NIL) g_pc[g_pc_lru_head].lprev = i;
    g_pc_lru_head = i;
    if(g_pc_lru_tail == PC_NIL) g_pc_lru_tail = i;
}

static void pc_lru_touch(uint32_t i){
    if(g_pc_lru_head == i) return;
    pc_lru_unlink(i);
    pc_lru_push(i);
}

//---- 查找/淘汰/插入 ----
static uint32_t pc_find(uint64_t ino, uint64_t fsize, uint64_t pgoff){
    uint32_t i = g_pc_hash[pc_hash(ino, fsize, pgoff)];
    while(i != PC_NIL){
        if(g_pc[i].ino == ino && g_pc[i].fsize == fsize && g_pc[i].pgoff == pgoff) return i;
        i = g_pc[i].hnext;
    }
    return PC_NIL;
}

//摘除一个条目并归还物理页
static void pc_drop(uint32_t i){
    uint32_t b = pc_hash(g_pc[i].ino, g_pc[i].fsize, g_pc[i].pgoff);
    uint32_t *pp = &g_pc_hash[b];
    while(*pp != PC_NIL){
        if(*pp == i){ *pp = g_pc[i].hnext; break; }
        pp = &g_pc[*pp].hnext;
    }
    pc_lru_unlink(i);
    if(g_pc[i].page) Pmm_Free(g_pc[i].page, 1);
    g_pc[i].page = NULL;
    g_pc[i].ino = 0;
    g_pc[i].fsize = 0;
    g_pc[i].pgoff = 0;
    g_pc[i].hnext = g_pc_free;
    g_pc_free = i;
    if(g_pc_count) g_pc_count--;
}

//取一个可用槽位(必要时淘汰最久未用的页)
static uint32_t pc_alloc_slot(void){
    while(g_pc_count >= g_pc_limit || g_pc_free == PC_NIL){
        if(g_pc_lru_tail == PC_NIL) return PC_NIL;//既无空槽又无可淘汰项
        pc_drop(g_pc_lru_tail);
        g_pc_evict++;
    }
    uint32_t i = g_pc_free;
    g_pc_free = g_pc[i].hnext;
    return i;
}

//把槽位还回空闲链(尚未挂载内容时使用)
static void pc_put_slot(uint32_t i){
    g_pc[i].hnext = g_pc_free;
    g_pc_free = i;
}

static void pc_insert(uint32_t i, uint64_t ino, uint64_t fsize, uint64_t pgoff, void *page){
    uint32_t b = pc_hash(ino, fsize, pgoff);
    g_pc[i].ino = ino;
    g_pc[i].fsize = fsize;
    g_pc[i].pgoff = pgoff;
    g_pc[i].page = page;
    g_pc[i].hnext = g_pc_hash[b];
    g_pc_hash[b] = i;
    pc_lru_push(i);
    g_pc_count++;
}

/*
 * 填充以pgoff所在页为目标的预读窗口
 * 窗口按PC_RA_PAGES对齐; 一次fill读入整窗后拆成页存入缓存。返回1成功。
 */
static int pc_fill_window(uint64_t ino, uint64_t fsize, uint64_t pgoff,
                          pcache_fill_t fill, void *ctx){
    g_pc_misses++;
    uint64_t base = pgoff & ~((uint64_t)PC_RA_PAGES - 1);
    uint64_t off  = base << PAGE_SHIFT;
    if(off >= fsize) return 0;
    uint64_t want = (uint64_t)PC_RA_PAGES << PAGE_SHIFT;
    if(want > fsize - off) want = fsize - off;
    uint64_t n = fill(ctx, off, g_pc_scratch, want);
    if(!n) return 0;
    g_pc_disk_bytes += n;
    uint64_t pages = (n + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if(pages > PC_RA_PAGES) pages = PC_RA_PAGES;
    for(uint64_t k = 0; k < pages; k++){
        uint64_t po = base + k;
        if((po << PAGE_SHIFT) >= fsize) break;//窗口末页超出文件尾部
        if(pc_find(ino, fsize, po) != PC_NIL) continue;
        uint32_t slot = pc_alloc_slot();
        if(slot == PC_NIL) break;//容量耗尽
        void *pg = Pmm_Malloc(1);
        if(!pg){ pc_put_slot(slot); break; }//内存不足, 保留已填充的页
        memcpy((void*)PHYS_TO_VIRT((uintptr_t)pg), g_pc_scratch + (k << PAGE_SHIFT), PAGE_SIZE);
        pc_insert(slot, ino, fsize, po, pg);
        g_pc_fill_pages++;
    }
    return pc_find(ino, fsize, pgoff) != PC_NIL;
}

//---------------------------------------------------------------------------
void PageCacheInit(void){
    if(g_pc_inited) return;
    //条目数组: 内存不足时逐级减半, 尽量把缓存建起来
    for(uint32_t n = PC_MAX_ENTRIES; n >= PC_MIN_ENTRIES; n >>= 1){
        int pages = (int)((sizeof(pc_ent_t) * n + PAGE_SIZE - 1) / PAGE_SIZE);
        void *m = Pmm_Malloc(pages);
        if(m){ g_pc = (pc_ent_t*)PHYS_TO_VIRT((uintptr_t)m); g_pc_max = n; break; }
    }
    if(!g_pc) return;//彻底没内存, 缓存保持关闭
    //哈希桶
    int hpages = (int)((PC_HASH_SIZE * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    void *h = Pmm_Malloc(hpages);
    if(!h){ Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)g_pc), (int)((sizeof(pc_ent_t) * g_pc_max + PAGE_SIZE - 1) / PAGE_SIZE)); g_pc = NULL; return; }
    //预读中转缓冲
    void *s = Pmm_Malloc(PC_RA_PAGES * PAGE_SIZE / PAGE_SIZE);
    if(!s){
        Pmm_Free(h, hpages);
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)g_pc), (int)((sizeof(pc_ent_t) * g_pc_max + PAGE_SIZE - 1) / PAGE_SIZE));
        g_pc = NULL;
        return;
    }
    g_pc_hash = (uint32_t*)PHYS_TO_VIRT((uintptr_t)h);
    g_pc_scratch = (uint8_t*)PHYS_TO_VIRT((uintptr_t)s);
    memset(g_pc_hash, 0xFF, PC_HASH_SIZE * sizeof(uint32_t));//桶为空=PC_NIL
    for(uint32_t i = 0; i < g_pc_max; i++){
        g_pc[i].page = NULL;
        g_pc[i].ino = 0;
        g_pc[i].fsize = 0;
        g_pc[i].pgoff = 0;
        g_pc[i].lprev = PC_NIL;
        g_pc[i].lnext = PC_NIL;
        g_pc[i].hnext = (i + 1 < g_pc_max) ? (i + 1) : PC_NIL;
    }
    g_pc_free = 0;
    g_pc_lru_head = PC_NIL;
    g_pc_lru_tail = PC_NIL;
    g_pc_count = 0;
    g_pc_limit = (g_pc_max < PC_DEF_ENTRIES) ? g_pc_max : PC_DEF_ENTRIES;
    g_pc_inited = true;
}

_Bool PageCacheReady(void){
    return g_pc_inited;
}

void PageCacheSetLimit(uint64_t pages){
    if(!g_pc_inited) return;
    if(pages == 0 || pages > g_pc_max) pages = g_pc_max;
    uint64_t f;
    spin_lock_irqsave(&g_pc_lock, f);
    g_pc_limit = (uint32_t)pages;
    //容量调小后按需淘汰
    while(g_pc_count > g_pc_limit && g_pc_lru_tail != PC_NIL){
        pc_drop(g_pc_lru_tail);
        g_pc_evict++;
    }
    spin_unlock_irqrestore(&g_pc_lock, f);
}

uint64_t PageCacheLimit(void){
    return g_pc_inited ? g_pc_limit : 0;
}

void PageCacheFlush(void){
    if(!g_pc_inited) return;
    uint64_t f;
    spin_lock_irqsave(&g_pc_lock, f);
    while(g_pc_lru_tail != PC_NIL) pc_drop(g_pc_lru_tail);
    spin_unlock_irqrestore(&g_pc_lock, f);
}

void PageCacheInvalidate(uint64_t ino){
    if(!g_pc_inited) return;
    uint64_t f;
    spin_lock_irqsave(&g_pc_lock, f);
    for(uint32_t i = 0; i < g_pc_max; i++){
        if(g_pc[i].page && g_pc[i].ino == ino) pc_drop(i);
    }
    spin_unlock_irqrestore(&g_pc_lock, f);
}

void PageCacheInvalidateRange(uint64_t ino, uint64_t off, uint64_t len){
    if(!g_pc_inited || !len) return;
    uint64_t first = off >> PAGE_SHIFT;
    uint64_t last  = (off + len - 1) >> PAGE_SHIFT;
    uint64_t f;
    spin_lock_irqsave(&g_pc_lock, f);
    for(uint32_t i = 0; i < g_pc_max; i++){
        if(!g_pc[i].page || g_pc[i].ino != ino) continue;
        if(g_pc[i].pgoff >= first && g_pc[i].pgoff <= last) pc_drop(i);
    }
    spin_unlock_irqrestore(&g_pc_lock, f);
}

uint64_t PageCacheRead(uint64_t ino, uint64_t off, void *buf, uint64_t len,
                       uint64_t fsize, pcache_fill_t fill, void *ctx){
    if(!g_pc_inited || !fill || !len) return 0;
    if(off >= fsize) return 0;
    if(len > fsize - off) len = fsize - off;
    uint64_t f;
    /*
     * 整个读取都在锁内完成: 拷出的页不会被并发淘汰。
     * 本函数仅由文件系统在文件系统锁内调用, 实际不存在并发压力, 锁开销可忽略。
     */
    spin_lock_irqsave(&g_pc_lock, f);
    uint64_t done = 0;
    while(done < len){
        uint64_t foff = off + done;
        uint64_t pgoff = foff >> PAGE_SHIFT;
        uint64_t poff  = foff & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - poff;
        if(chunk > len - done) chunk = len - done;
        uint32_t i = pc_find(ino, fsize, pgoff);
        if(i == PC_NIL){
            if(!pc_fill_window(ino, fsize, pgoff, fill, ctx)) break;
            i = pc_find(ino, fsize, pgoff);
            if(i == PC_NIL) break;
        }else{
            g_pc_hits++;
            pc_lru_touch(i);
        }
        memcpy((uint8_t*)buf + done,
               (const uint8_t*)PHYS_TO_VIRT((uintptr_t)g_pc[i].page) + poff, chunk);
        done += chunk;
    }
    spin_unlock_irqrestore(&g_pc_lock, f);
    return done;
}

void PageCacheStats(void){
    if(!g_pc_inited){
        early_printk("[PCACHE] disabled\n");
        return;
    }
    early_printk("[PCACHE] pages=%lu/%lu hit=%lu miss=%lu evict=%lu filled=%lu disk=%luKB\n",
                 (uint64_t)g_pc_count, (uint64_t)g_pc_limit,
                 g_pc_hits, g_pc_misses, g_pc_evict, g_pc_fill_pages,
                 g_pc_disk_bytes / 1024);
}
