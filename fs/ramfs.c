/*
 * fs/ramfs.c — 内存文件系统
 * 纯内存实现:节点与文件内容都分配自物理页,不依赖磁盘驱动。
 * 作为VFS的第一个后端,打通open/read/write/close/lseek完整链路。
 * 由DeepSeek V4 Pro生成
 */

#include <fs.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

//ramfs节点私有数据:文件内容
typedef struct ramfs_priv {
    uint8_t *data;   //内容(虚拟地址,高半区可访问)
    uint64_t cap;    //已分配容量(字节)
} ramfs_priv_t;

#define container_of(ptr, type, member) ((type*)((char*)(ptr) - (size_t)&(((type*)0)->member)))

static const fs_node_ops_t g_ramfs_ops;

static uint64_t ramfs_read(fs_node_t *node, uint64_t off, void *buf, uint64_t len){
    ramfs_priv_t *p = (ramfs_priv_t*)node->priv;
    if(off >= node->size) return 0;
    if(len > node->size - off) len = node->size - off;
    memcpy(buf, p->data + off, len);
    return len;
}

static uint64_t ramfs_write(fs_node_t *node, uint64_t off, const void *buf, uint64_t len){
    ramfs_priv_t *p = (ramfs_priv_t*)node->priv;
    uint64_t need = off + len;
    //容量不足则翻倍扩容
    if(need > p->cap){
        uint64_t newcap = p->cap ? p->cap : PAGE_SIZE;
        while(newcap < need) newcap <<= 1;
        void *nd = (void*)PHYS_TO_VIRT((uintptr_t)Pmm_Malloc((int)(newcap / PAGE_SIZE)));
        if(!nd) return 0;
        if(p->data && node->size) memcpy(nd, p->data, node->size);
        if(p->data) Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)p->data), (int)(p->cap / PAGE_SIZE));
        p->data = (uint8_t*)nd;
        p->cap = newcap;
    }
    memcpy(p->data + off, buf, len);
    if(need > node->size) node->size = need;
    return len;
}

static fs_node_t *ramfs_lookup(fs_node_t *dir, const char *name){
    struct list_node *pos;
    fs_node_t *c;
    for(pos = dir->children.next; pos != &dir->children; pos = pos->next){
        c = container_of(pos, fs_node_t, siblings);
        if(strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static void ramfs_truncate(fs_node_t *node){
    node->size = 0;
}

//分配并初始化一个节点(节点结构+priv内嵌在同一物理页)
static fs_node_t *ramfs_alloc_node(fs_node_t *dir, const char *name, int type){
    void *page = (void*)PHYS_TO_VIRT((uintptr_t)Pmm_Malloc(1));
    if(!page) return NULL;
    memset(page, 0, PAGE_SIZE);
    fs_node_t *n = (fs_node_t*)page;
    ramfs_priv_t *p = (ramfs_priv_t*)((uintptr_t)page + sizeof(fs_node_t));
    n->priv = p;
    n->type = type;
    n->ops = (fs_node_ops_t*)&g_ramfs_ops;
    n->refs = 1;
    //截断名字到MAX_NAME-1
    int i;
    for(i = 0; i < MAX_NAME - 1 && name[i]; i++) n->name[i] = name[i];
    n->name[i] = 0;
    //初始化链表(自环)
    n->siblings.prev = &n->siblings;
    n->siblings.next = &n->siblings;
    n->children.prev = &n->children;
    n->children.next = &n->children;
    //挂到父目录
    if(dir){
        n->siblings.prev = dir->children.prev;
        n->siblings.next = &dir->children;
        dir->children.prev->next = &n->siblings;
        dir->children.prev = &n->siblings;
        dir->size++;
    }
    return n;
}

static fs_node_t *ramfs_create(fs_node_t *dir, const char *name, int type){
    return ramfs_alloc_node(dir, name, type);
}

static const fs_node_ops_t g_ramfs_ops = {
    .read     = ramfs_read,
    .write    = ramfs_write,
    .lookup   = ramfs_lookup,
    .create   = ramfs_create,
    .truncate = ramfs_truncate,
};

//创建根目录节点
fs_node_t *ramfs_init(void){
    return ramfs_alloc_node(NULL, "/", FT_DIR);
}
