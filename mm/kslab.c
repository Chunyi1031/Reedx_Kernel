// SPDX-License-Identifier: GPL-2.0-only
/**
 * mm/kslab.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 内核小对象分配器(slab)
 */
#include <mm/kslab.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <print.h>

#define KSLAB_MAX_CACHES  16            //最多注册缓存数
#define KSLAB_ALIGN       16            //对象对齐
#define KSLAB_PAGE_MAGIC  0x534C4142u   //"SLAB"

//页内头部，放在页起始处
typedef struct kslab_page {
    uint32_t           kg_magic;   //KSLAB_PAGE_MAGIC
    uint32_t           kg_inuse;   //已分配对象数
    struct kmem_cache *kg_cache;   //所属缓存
    void              *kg_free;    //页内空闲对象链
    struct list_node   kg_node;
} kslab_page_t;
//缓存描述符
struct kmem_cache {
    char             kc_name[24];
    uint32_t         kc_obj_size;       //对象大小
    uint32_t         kc_hdr_size;       //页头
    uint32_t         kc_objs_per_page;  //每页对象数
    uint32_t         kc_total;          //对象总数
    uint32_t         kc_free;           //空闲对象数
    uint32_t         kc_pages;          //页数
    struct list_node kc_partial;        //有空闲对象的页
    struct list_node kc_full;           //全满的页
    struct kmem_cache *kc_next;         //cache注册链
};

static struct kmem_cache g_cache_pool[KSLAB_MAX_CACHES];//缓存池
static struct kmem_cache *g_caches;//缓存注册链头
static int g_nr_caches;//已注册缓存数
static int g_ready;//是否已初始化
static spinlock_t g_kslab_lock = {0};

#define page_of(node) ((kslab_page_t*)((uintptr_t)(node) - __builtin_offsetof(kslab_page_t, kg_node)))

static inline void slab_list_add_tail(struct list_node *n, struct list_node *head){
    n->prev = head->prev;
    n->next = head;
    head->prev->next = n;
    head->prev = n;
}
static inline void slab_list_del(struct list_node *n){
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n->next = NULL;
}

void KslabInit(void){
    if(g_ready)return;
    g_caches = NULL;
    g_nr_caches = 0;
    g_ready = 1;
}

struct kmem_cache *KslabCreate(const char *name, uint32_t obj_size){
    if(!g_ready)KslabInit();//初始化
    //对象大小对齐
    if(!obj_size)obj_size = 1;
    obj_size = (obj_size + KSLAB_ALIGN - 1) & ~(uint32_t)(KSLAB_ALIGN - 1);//按16字节对齐
    uint32_t hdr = (uint32_t)((sizeof(kslab_page_t) + KSLAB_ALIGN - 1) & ~(uint32_t)(KSLAB_ALIGN - 1));
    //已存在同样大小的缓存就复用
    for(struct kmem_cache *c = g_caches;c;c = c->kc_next){
        if(c->kc_obj_size == obj_size)return c;
    }
    //缓存数检查
    if(g_nr_caches >= KSLAB_MAX_CACHES){
        printk(PRINTK_ERR"kslab:The number of caches has reached the maximum(%d)", KSLAB_MAX_CACHES);
        return NULL;
    }
    //对象大小检查
    if(hdr + obj_size > PAGE_SIZE){
        printk(PRINTK_ERR"kslab:Object is too large(%u bytes)", obj_size);
        return NULL;
    }
    struct kmem_cache *c = &g_cache_pool[g_nr_caches++];//从池中取一个缓存
    //初始化缓存描述符
    memset(c, 0, sizeof(*c));
    int i = 0;
    if(name)for(;name[i] && i < (int)sizeof(c->kc_name) - 1;i ++)c->kc_name[i] = name[i];
    c->kc_obj_size = obj_size;
    c->kc_hdr_size = hdr;
    c->kc_objs_per_page = (PAGE_SIZE - hdr) / obj_size;
    if(!c->kc_objs_per_page){
        printk(PRINTK_ERR"kslab:Object is too large(%u bytes)", obj_size);
        g_nr_caches --;
        return NULL;
    }
    c->kc_partial.prev = &c->kc_partial;
    c->kc_partial.next = &c->kc_partial;
    c->kc_full.prev = &c->kc_full;
    c->kc_full.next = &c->kc_full;
    c->kc_next = g_caches;
    g_caches = c;
    return c;
}

//新开一页并切成对象
static kslab_page_t *kslab_new_page(struct kmem_cache *c){
    void *pa = Pmm_Malloc(1);
    if(!pa)return NULL;
    uint8_t *base = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pa);
    memset(base, 0, PAGE_SIZE);
    kslab_page_t *pg = (kslab_page_t*)base;
    pg->kg_magic = KSLAB_PAGE_MAGIC;
    pg->kg_cache = c;
    pg->kg_inuse = 0;
    pg->kg_free = NULL;
    for(uint32_t i = c->kc_objs_per_page;i > 0;i --){
        void *obj = base + c->kc_hdr_size + (uint64_t)(i - 1) * c->kc_obj_size;
        *(void**)obj = pg->kg_free;
        pg->kg_free = obj;
    }
    c->kc_total += c->kc_objs_per_page;
    c->kc_free += c->kc_objs_per_page;
    c->kc_pages ++;
    slab_list_add_tail(&pg->kg_node, &c->kc_partial);
    return pg;
}

/*DeepSeek-V4.1-Flash*/
void *KslabAlloc(struct kmem_cache *c){
    if(!c)return NULL;
    uint64_t flags;
    spin_lock_irqsave(&g_kslab_lock, flags);
    kslab_page_t *pg = NULL;
    if(c->kc_partial.next != &c->kc_partial)pg = page_of(c->kc_partial.next);
    if(!pg){
        pg = kslab_new_page(c);
        if(!pg){
            spin_unlock_irqrestore(&g_kslab_lock, flags);
            printk(PRINTK_ERR"kslab:%s Unable to allocate new page.", c->kc_name);
            return NULL;
        }
    }
    void *obj = pg->kg_free;
    if(!obj){//理论上不会发生
        spin_unlock_irqrestore(&g_kslab_lock, flags);
        return NULL;
    }
    pg->kg_free = *(void**)obj;
    pg->kg_inuse ++;
    c->kc_free --;
    if(!pg->kg_free){//这页满了, 挪到 full 链
        slab_list_del(&pg->kg_node);
        slab_list_add_tail(&pg->kg_node, &c->kc_full);
    }
    spin_unlock_irqrestore(&g_kslab_lock, flags);
    return obj;
}

void *KslabZalloc(struct kmem_cache *c){
    void *p = KslabAlloc(c);
    if(p)memset(p, 0, c->kc_obj_size);
    return p;
}

void KslabFree(struct kmem_cache *c, void *obj){
    if(!c || !obj)return;
    kslab_page_t *pg = (kslab_page_t*)((uintptr_t)obj & ~(uintptr_t)(PAGE_SIZE - 1));
    uint64_t flags;
    spin_lock_irqsave(&g_kslab_lock, flags);
    if(pg->kg_magic != KSLAB_PAGE_MAGIC || pg->kg_cache != c){
        spin_unlock_irqrestore(&g_kslab_lock, flags);
        printk(PRINTK_ERR"kslab:Released object %p that does not belong to %s", obj, c->kc_name);
        return;
    }
    int was_full = (pg->kg_free == NULL);
    *(void**)obj = pg->kg_free;
    pg->kg_free = obj;
    if(pg->kg_inuse)pg->kg_inuse --;
    c->kc_free ++;
    if(was_full){//从满变成有空闲
        slab_list_del(&pg->kg_node);
        slab_list_add_tail(&pg->kg_node, &c->kc_partial);
    }
    //整页全空且该 cache 不止一页 → 还给 PMM(保留一页以备下次分配)
    if(pg->kg_inuse == 0 && c->kc_pages > 1){
        slab_list_del(&pg->kg_node);
        c->kc_total -= c->kc_objs_per_page;
        c->kc_free  -= c->kc_objs_per_page;
        c->kc_pages --;
        pg->kg_magic = 0;
        spin_unlock_irqrestore(&g_kslab_lock, flags);
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)pg), 1);
        return;
    }
    spin_unlock_irqrestore(&g_kslab_lock, flags);
}
/*DeepSeek-V4.1-Flash-END*/

uint32_t KslabObjectSize(struct kmem_cache *c){
    return c ? c->kc_obj_size : 0;
}

uint32_t KslabPageCount(struct kmem_cache *c){
    return c ? c->kc_pages : 0;
}

uint32_t KslabInUse(struct kmem_cache *c){
    return c ? (c->kc_total - c->kc_free) : 0;
}

void KslabStats(void){
    printk(PRINTK_INFO"kslab: There are %d object caches", g_nr_caches);
    for(struct kmem_cache *c = g_caches;c;c = c->kc_next){
        printk(PRINTK_INFO"  %s Object =%u Byte Page =%u Used =%u/%u",
               c->kc_name, c->kc_obj_size, c->kc_pages,
               c->kc_total - c->kc_free, c->kc_total);
    }
}
