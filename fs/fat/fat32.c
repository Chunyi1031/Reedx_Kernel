#include <fs.h>
#include <drives/disk.h>
#include <mm/pmm.h>
#include <mm/pagecache.h>
#include <mm/vmm.h>
#include <print.h>
#include <rtc.h>
#include <syscalls.h>

//FAT32盘上数据结构
#define FAT_ATTR_READ_ONLY 0x01 //只读
#define FAT_ATTR_HIDDEN    0x02 //隐藏
#define FAT_ATTR_SYSTEM    0x04 //系统
#define FAT_ATTR_VOLUME_ID 0x08 //卷标
#define FAT_ATTR_DIRECTORY 0x10 //目录
#define FAT_ATTR_ARCHIVE   0x20 //文件
#define FAT_ATTR_LFN       0x0F //长文件名条目

#define FAT_DIRENT_FREE    0xE5 //目录项空闲(已删除)
#define FAT_DIRENT_END     0x00 //目录项链结束标记

#define FAT_CLUSTER_FREE   0x00000000
#define FAT_CLUSTER_BAD    0x0FFFFFF7
#define FAT_CLUSTER_EOF    0x0FFFFFF8

//FAT32引导扇区BPB(偏移已在注释标注,共90字节)
typedef struct __attribute__((packed)) fat32_bpb {
    uint8_t  jmp[3];              //0x00 跳转指令
    uint8_t  oem[8];              //0x03 OEM名称
    uint16_t bytes_per_sector;    //0x0B 每扇区字节数
    uint8_t  sectors_per_cluster; //0x0D 每簇扇区数
    uint16_t reserved_sectors;    //0x0E 保留扇区数
    uint8_t  num_fats;            //0x10 FAT份数
    uint16_t root_entries;        //0x11 FAT32恒为0
    uint16_t total_sectors16;     //0x13 FAT32恒为0
    uint8_t  media;               //0x15 介质描述符
    uint16_t fat_size16;          //0x16 FAT32恒为0
    uint16_t sectors_per_track;   //0x18
    uint16_t num_heads;           //0x1A
    uint32_t hidden_sectors;      //0x1C
    uint32_t total_sectors32;     //0x20
    //--- FAT32扩展字段 ---
    uint32_t fat_size32;          //0x24 每份FAT扇区数
    uint16_t ext_flags;           //0x28
    uint16_t fs_version;          //0x2A
    uint32_t root_cluster;        //0x2C 根目录起始簇
    uint16_t fsinfo_sector;       //0x30
    uint16_t backup_boot_sector;  //0x32
    uint8_t  reserved[12];        //0x34
    uint8_t  drive_number;        //0x40
    uint8_t  nt_flags;            //0x41
    uint8_t  signature;           //0x42
    uint32_t volume_id;           //0x43
    uint8_t  volume_label[11];    //0x47
    uint8_t  fs_type[8];          //0x52
} fat32_bpb_t;

//32字节短目录项
typedef struct __attribute__((packed)) fat_dir_entry {
    uint8_t  name[8];          //0x00 短名(不足用空格填充)
    uint8_t  ext[3];           //0x08 扩展名
    uint8_t  attr;             //0x0B 属性
    uint8_t  nt_res;           //0x0C
    uint8_t  crt_time_tenth;   //0x0D
    uint16_t crt_time;         //0x0E
    uint16_t crt_date;         //0x10
    uint16_t lst_acc_date;     //0x12
    uint16_t fst_clus_hi;      //0x14 起始簇高16位
    uint16_t wrt_time;         //0x16
    uint16_t wrt_date;         //0x18
    uint16_t fst_clus_lo;      //0x1A 起始簇低16位
    uint32_t file_size;        //0x1C
} fat_dir_entry_t;

//32字节长文件名(LFN)条目
typedef struct __attribute__((packed)) fat_lfn_entry {
    uint8_t  order;            //0x00 序号(最高位0x40表示最后一项)
    uint16_t name1[5];         //0x01
    uint8_t  attr;             //0x0B 恒为FAT_ATTR_LFN
    uint8_t  type;             //0x0C
    uint8_t  checksum;         //0x0D
    uint16_t name2[6];         //0x0E
    uint16_t first_cluster;    //0x1A 恒为0
    uint16_t name3[2];         //0x1C
} fat_lfn_entry_t;

//挂载上下文
typedef struct fat32_fs {
    disk_info_t disk_copy; //磁盘信息拷贝
    disk_info_t *disk;     //指向disk_copy
    uint32_t part_lba;    //分区起始LBA
    uint16_t bps;         //每扇区字节(必须512)
    uint8_t  spc;         //每簇扇区数
    uint16_t rsvd;        //保留扇区数
    uint8_t  nfats;       //FAT份数
    uint32_t fat_sz;      //每份FAT扇区数
    uint32_t root_clu;    //根目录起始簇
    uint32_t fat_start;   //FAT区起始LBA
    uint32_t data_start;  //数据区起始LBA
    uint32_t bpc;         //每簇字节数
    uint32_t total_clusters;//数据区总簇数
    uint32_t alloc_hint;  //空闲簇扫描起点
    uint8_t  *cluster_buf;//簇缓冲(虚拟地址)
} fat32_fs_t;

//节点私有数据
typedef struct fat_node_priv {
    uint32_t first_clu;   //起始簇
    uint32_t size;        //文件字节数
    uint32_t dir_clus;    //目录项所在簇
    uint32_t dir_off;     //目录项在簇内偏移
    uint32_t pos_clu;     //顺序读缓存: 下次继续读的簇
    uint64_t pos_off;     //顺序读缓存: 下次继续读的文件偏移
    uint64_t pos_within;  //顺序读缓存: 该簇内的偏移
    uint32_t it_pos;      //目录迭代游标: 已经走到第几个簇
    uint32_t it_clu;      //目录迭代游标: 该簇号
    uint32_t it_valid;    //目录迭代游标是否有效
} fat_node_priv_t;

static fat32_fs_t g_fat;
static fs_node_ops_t fat_ops;

/*DeepSeek-V4.1-Flash*/
//目录项缓存
#define FAT_DC_BUCKETS  1024
#define FAT_DC_ENTRIES  4096
#define FAT_DC_NAME_MAX 48
#define FAT_DC_NIL      0xFFFFFFFFu

typedef struct fat_dcache_ent {
    uint32_t   dir_clu;     //父目录起始簇
    uint32_t   hnext;       //哈希链
    uint64_t   name_hash;   //完整项名哈希
    uint32_t   name_len;    //完整项名长度
    fs_node_t *node;        //命中节点+
    char       name[FAT_DC_NAME_MAX];
} fat_dcache_ent_t;

static fat_dcache_ent_t *g_dc;
static uint32_t *g_dc_hash;
static uint32_t  g_dc_used;//已用条目数
static uint32_t  g_dc_cursor;//环形替换游标
static fs_node_t *g_root_node;//根目录节点

static uint64_t fat_dc_hashname(const char *s){
    uint64_t h = 1469598103934665603ULL;//FNV-1a
    while(*s){ h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}

static void fat_dc_init(void){
    if(g_dc) return;
    int pages = (int)((sizeof(fat_dcache_ent_t) * FAT_DC_ENTRIES + PAGE_SIZE - 1) / PAGE_SIZE);
    void *m = Pmm_Malloc(pages);
    if(!m) return;
    void *hb = Pmm_Malloc((FAT_DC_BUCKETS * (int)sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    if(!hb){ Pmm_Free(m, pages); return; }
    g_dc = (fat_dcache_ent_t*)PHYS_TO_VIRT((uintptr_t)m);
    g_dc_hash = (uint32_t*)PHYS_TO_VIRT((uintptr_t)hb);
    memset(g_dc_hash, 0xFF, FAT_DC_BUCKETS * sizeof(uint32_t));
    g_dc_used = 0;
    g_dc_cursor = 0;
}

//把条目从哈希链上摘下(不回收节点)
static void fat_dc_unlink(uint32_t i){
    uint32_t b = (uint32_t)(g_dc[i].name_hash & (FAT_DC_BUCKETS - 1));
    uint32_t *pp = &g_dc_hash[b];
    while(*pp != FAT_DC_NIL){
        if(*pp == i){ *pp = g_dc[i].hnext; break; }
        pp = &g_dc[*pp].hnext;
    }
    g_dc[i].hnext = FAT_DC_NIL;
}

//查找: hit=1表示缓存命中(返回值可能为NULL, 即负缓存)
static fs_node_t *fat_dc_lookup(uint32_t dir_clu, const char *name, uint64_t h, uint32_t len, int *hit){
    *hit = 0;
    if(!g_dc) return NULL;
    uint32_t b = (uint32_t)(h & (FAT_DC_BUCKETS - 1));
    for(uint32_t i = g_dc_hash[b]; i != FAT_DC_NIL; i = g_dc[i].hnext){
        if(g_dc[i].dir_clu != dir_clu || g_dc[i].name_hash != h || g_dc[i].name_len != len) continue;
        if(memcmp(g_dc[i].name, name, len) != 0) continue;
        *hit = 1;
        return g_dc[i].node;
    }
    return NULL;
}

static void fat_dc_store(uint32_t dir_clu, const char *name, uint64_t h, uint32_t len, fs_node_t *node){
    if(!g_dc || len == 0 || len >= FAT_DC_NAME_MAX) return;
    uint32_t b = (uint32_t)(h & (FAT_DC_BUCKETS - 1));
    //同键已存在则只更新节点(避免同键多条目)
    for(uint32_t i = g_dc_hash[b]; i != FAT_DC_NIL; i = g_dc[i].hnext){
        if(g_dc[i].dir_clu == dir_clu && g_dc[i].name_hash == h && g_dc[i].name_len == len &&
           memcmp(g_dc[i].name, name, len) == 0){ g_dc[i].node = node; return; }
    }
    uint32_t i;
    if(g_dc_used < FAT_DC_ENTRIES){
        i = g_dc_used++;
    }else{
        i = g_dc_cursor;
        if(++g_dc_cursor >= FAT_DC_ENTRIES) g_dc_cursor = 0;
        fat_dc_unlink(i);
    }
    g_dc[i].dir_clu = dir_clu;
    g_dc[i].name_hash = h;
    g_dc[i].name_len = len;
    g_dc[i].node = node;
    for(uint32_t k = 0; k < len; k++) g_dc[i].name[k] = name[k];
    g_dc[i].name[len] = 0;
    g_dc[i].hnext = g_dc_hash[b];
    g_dc_hash[b] = i;
}

//使某目录下的一项失效
static void fat_dc_drop(uint32_t dir_clu, const char *name){
    if(!g_dc || !name || !*name) return;
    uint64_t h = fat_dc_hashname(name);
    uint32_t len = (uint32_t)strlen(name);
    uint32_t b = (uint32_t)(h & (FAT_DC_BUCKETS - 1));
    uint32_t *pp = &g_dc_hash[b];
    while(*pp != FAT_DC_NIL){
        uint32_t i = *pp;
        if(g_dc[i].dir_clu == dir_clu && g_dc[i].name_hash == h && g_dc[i].name_len == len &&
           memcmp(g_dc[i].name, name, len) == 0){ *pp = g_dc[i].hnext; g_dc[i].hnext = FAT_DC_NIL; break; }
        pp = &g_dc[i].hnext;
    }
}

static void fat_dc_clear(void){
    if(!g_dc) return;
    memset(g_dc_hash, 0xFF, FAT_DC_BUCKETS * sizeof(uint32_t));
    g_dc_used = 0;
    g_dc_cursor = 0;
}

//取目录节点的身份(起始簇)
static uint32_t fat_dir_id(fs_node_t *dir){
    if(!dir || !dir->priv) return 0;
    return ((fat_node_priv_t*)dir->priv)->first_clu;
}
/*DeepSeek-V4.1-Flash-END*/

//磁盘块缓存
#define FAT_PAGE_SIZE    4096
#define FAT_PAGE_BUDGET  (2 * 1024 * 1024)  //FAT页缓存上限(2MB)
#define CLU_BUDGET       (256 * 1024)       //簇缓存上限(256KB)
#define CLU_MAX_SLOTS    512

static uint8_t  *g_fatpg_mem;       //FAT页缓存
static uint32_t *g_fatpg_tag;       //页号+1，0=空
static uint32_t  g_fatpg_slots;     //缓存的页数
static uint32_t  g_fatpg_total;     //FAT总页数
static uint8_t  *g_clu_mem;         //簇缓存
static uint32_t *g_clu_tag;         //簇号+1，0=空
static uint32_t  g_clu_slots;

//单扇区FAT缓存
static uint32_t g_fat_cache_lba = ~0u;
static uint8_t  g_fat_cache[512];

//读磁盘扇区
static int fat_read_sectors(uint32_t lba, void *buf, uint32_t count){
    return DiskRead(g_fat.disk, lba, count, buf);
}

//定位FAT页缓存槽位
static uint8_t *fat_page_slot(uint32_t pi){
    uint32_t slot = pi % g_fatpg_slots;
    if(g_fatpg_tag[slot] != pi + 1){
        uint32_t per_page = FAT_PAGE_SIZE / g_fat.bps;
        uint32_t first = pi * per_page;
        uint32_t nsec = per_page;
        if(first >= g_fat.fat_sz) return NULL;
        if(first + nsec > g_fat.fat_sz) nsec = g_fat.fat_sz - first;
        uint8_t *pg = g_fatpg_mem + (uint64_t)slot * FAT_PAGE_SIZE;
        if(fat_read_sectors(g_fat.fat_start + first, pg, nsec)) return NULL;
        g_fatpg_tag[slot] = pi + 1;
    }
    return g_fatpg_mem + (uint64_t)slot * FAT_PAGE_SIZE;
}

//读FAT表项
static uint32_t fat_get_entry(uint32_t clu){
    uint32_t fat_off = clu * 4;
    //有缓存时
    if(g_fatpg_slots){
        uint32_t pi = fat_off / FAT_PAGE_SIZE;
        if(pi >= g_fatpg_total)return 0;
        uint8_t *pg = fat_page_slot(pi);//定位缓存槽位
        if(!pg)return 0;
        return *(uint32_t*)(pg + (fat_off % FAT_PAGE_SIZE)) & 0x0FFFFFFF;
    }
    //缓存不可用，单扇区缓存
    {
        uint32_t fat_lba = g_fat.fat_start + fat_off / g_fat.bps;
        if(fat_lba != g_fat_cache_lba){
            if(fat_read_sectors(fat_lba, g_fat_cache, 1)) return 0;
            g_fat_cache_lba = fat_lba;
        }
        return *(uint32_t*)(g_fat_cache + fat_off % g_fat.bps) & 0x0FFFFFFF;
    }
}

//读一簇到簇缓冲
static int fat_read_cluster(uint32_t clu){
    if(clu < 2) return -1;
    if(g_clu_slots){
        uint32_t slot = clu % g_clu_slots;
        uint8_t *dst = g_clu_mem + (uint64_t)slot * g_fat.bpc;
        if(g_clu_tag[slot] == clu + 1){
            memcpy(g_fat.cluster_buf, dst, g_fat.bpc);
            return 0;
        }
        if(fat_read_sectors(g_fat.data_start + (clu - 2) * g_fat.spc, dst, g_fat.spc)) return -1;
        g_clu_tag[slot] = clu + 1;
        memcpy(g_fat.cluster_buf, dst, g_fat.bpc);
        return 0;
    }
    return fat_read_sectors(g_fat.data_start + (clu - 2) * g_fat.spc,g_fat.cluster_buf, g_fat.spc);
}

//写磁盘扇区
static int fat_write_sectors(uint32_t lba, const void *buf, uint32_t count){
    return DiskWrite(g_fat.disk, lba, count, buf);
}

//簇缓冲写回磁盘
static int fat_write_cluster(uint32_t clu){
    if(g_clu_slots && clu >= 2) g_clu_tag[clu % g_clu_slots] = 0;
    return fat_write_sectors(g_fat.data_start + (clu - 2) * g_fat.spc,g_fat.cluster_buf, g_fat.spc);
}

//写FAT表项
static int fat_set_entry(uint32_t clu, uint32_t value){
    uint32_t fat_off = clu * 4;
    uint32_t fat_lba = g_fat.fat_start + fat_off / g_fat.bps;
    uint32_t off = fat_off % g_fat.bps;
    uint8_t sec[512];
    if(fat_read_sectors(fat_lba, sec, 1)) return -1;
    uint32_t *ent = (uint32_t*)(sec + off);
    *ent = (*ent & 0xF0000000) | (value & 0x0FFFFFFF);
    for(int f = 0; f < g_fat.nfats; f++){
        if(fat_write_sectors(fat_lba + (uint32_t)f * g_fat.fat_sz, sec, 1)) return -1;
    }
    //FAT已修改，使缓存失效
    g_fat_cache_lba = ~0u;
    if(g_fatpg_slots) g_fatpg_tag[(fat_off / FAT_PAGE_SIZE) % g_fatpg_slots] = 0;
    return 0;
}

//分配并初始化块缓存
static void fat_cache_init(void){
    g_fatpg_slots = 0;
    g_fatpg_total = 0;
    g_clu_slots = 0;
    if(g_fat.bps <= FAT_PAGE_SIZE && (FAT_PAGE_SIZE % g_fat.bps) == 0){
        uint64_t bytes = (uint64_t)g_fat.fat_sz * g_fat.bps;
        uint32_t total = (uint32_t)((bytes + FAT_PAGE_SIZE - 1) / FAT_PAGE_SIZE);
        uint32_t slots = total;
        uint32_t cap = FAT_PAGE_BUDGET / FAT_PAGE_SIZE;
        if(slots > cap)slots = cap;
        if(slots){
            int np = (int)(((uint64_t)slots * FAT_PAGE_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
            void *m = Pmm_Malloc(np);
            void *t = m ? Pmm_Malloc((int)((slots * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE)) : NULL;
            if(m && t){
                g_fatpg_mem = (uint8_t*)PHYS_TO_VIRT((uintptr_t)m);
                g_fatpg_tag = (uint32_t*)PHYS_TO_VIRT((uintptr_t)t);
                memset(g_fatpg_tag, 0, slots * sizeof(uint32_t));
                g_fatpg_slots = slots;
                g_fatpg_total = total;
            }else{
                if(m)Pmm_Free(m, np);
            }
        }
    }
    //簇缓存
    {
        uint32_t n = CLU_BUDGET / g_fat.bpc;
        if(n > CLU_MAX_SLOTS) n = CLU_MAX_SLOTS;
        if(n < 8) n = 8;
        int np = (int)(((uint64_t)n * g_fat.bpc + PAGE_SIZE - 1) / PAGE_SIZE);
        void *m = Pmm_Malloc(np);
        void *t = m ? Pmm_Malloc((int)((n * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE)) : NULL;
        if(m && t){
            g_clu_mem = (uint8_t*)PHYS_TO_VIRT((uintptr_t)m);
            g_clu_tag = (uint32_t*)PHYS_TO_VIRT((uintptr_t)t);
            memset(g_clu_tag, 0, n * sizeof(uint32_t));
            g_clu_slots = n;
        }else{
            if(m)Pmm_Free(m, np);
        }
    }
}

//扫描FAT寻找空闲簇
static uint32_t fat_find_free_cluster(void){
    uint32_t total = g_fat.total_clusters + 2;
    uint32_t c = g_fat.alloc_hint;
    if(c < 2 || c >= total) c = 2;
    uint8_t sec[512];
    uint32_t cur_lba = ~0u;
    for(uint32_t n = 0; n < g_fat.total_clusters; n++){
        uint32_t fat_off = c * 4;
        uint32_t lba = g_fat.fat_start + fat_off / g_fat.bps;
        if(lba != cur_lba){ //切换FAT扇区时才重新读入
            if(fat_read_sectors(lba, sec, 1)) return 0;
            cur_lba = lba;
        }
        uint32_t v = *(uint32_t*)(sec + fat_off % g_fat.bps) & 0x0FFFFFFF;
        if(v == FAT_CLUSTER_FREE){
            g_fat.alloc_hint = c + 1;
            return c;
        }
        c++;
        if(c >= total) c = 2;
    }
    return 0;
}

//分配空闲簇并链接到prev_clu之后
static uint32_t fat_alloc_cluster(uint32_t prev_clu){
    uint32_t clu = fat_find_free_cluster();
    if(clu < 2) return 0;
    memset(g_fat.cluster_buf, 0, g_fat.bpc);
    if(fat_write_cluster(clu)) return 0;
    if(fat_set_entry(clu, FAT_CLUSTER_EOF)) return 0;
    if(prev_clu >= 2 && prev_clu < FAT_CLUSTER_EOF){
        if(fat_set_entry(prev_clu, clu)) return 0;
    }
    return clu;
}

//释放整条簇链
static void fat_free_chain(uint32_t clu){
    while(clu >= 2 && clu < FAT_CLUSTER_EOF){
        uint32_t nxt = fat_get_entry(clu);
        fat_set_entry(clu, FAT_CLUSTER_FREE);
        clu = nxt;
    }
}

//把节点的起始簇/大小/修改时间同步回磁盘目录项
static int fat_update_dirent(fat_node_priv_t *p){
    if(!p || !p->dir_clus) return 0; //根节点等无目录项
    if(fat_read_cluster(p->dir_clus)) return -1;
    fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + p->dir_off);
    de->fst_clus_hi = (uint16_t)(p->first_clu >> 16);
    de->fst_clus_lo = (uint16_t)(p->first_clu & 0xFFFF);
    de->file_size = (de->attr & FAT_ATTR_DIRECTORY) ? 0 : p->size;
    //更新最后写入时间
    rtc_time_t tm;
    rtc_get_local(&tm);
    de->wrt_time = (uint16_t)((tm.hour << 11) | (tm.minute << 5) | (tm.second / 2));
    de->wrt_date = (uint16_t)(((tm.year - 1980) << 9) | (tm.month << 5) | tm.day);
    return fat_write_cluster(p->dir_clus);
}

//把当前时间写入目录项的创建/写入/访问字段
static void fat_set_now(fat_dir_entry_t *de){
    rtc_time_t tm;
    rtc_get_local(&tm);
    uint16_t fat_date = (uint16_t)(((tm.year - 1980) << 9) | (tm.month << 5) | tm.day);
    uint16_t fat_time = (uint16_t)((tm.hour << 11) | (tm.minute << 5) | (tm.second / 2));
    de->crt_time = fat_time;
    de->crt_date = fat_date;
    de->crt_time_tenth = 0;
    de->wrt_time = fat_time;
    de->wrt_date = fat_date;
    de->lst_acc_date = fat_date;
}

//解析FAT日期时间字段为epoch
static uint64_t fat_dt_to_epoch(uint16_t fat_date, uint16_t fat_time){
    uint16_t year = 1980 + ((fat_date >> 9) & 0x7F);
    uint8_t month = (uint8_t)((fat_date >> 5) & 0x0F);
    uint8_t day = (uint8_t)(fat_date & 0x1F);
    uint8_t hour = (uint8_t)((fat_time >> 11) & 0x1F);
    uint8_t minute = (uint8_t)((fat_time >> 5) & 0x3F);
    uint8_t second = (uint8_t)((fat_time & 0x1F) * 2);
    return rtc_tm_to_epoch(year, month, day, hour, minute, second);
}

//创建节点
static fs_node_t *fat_new_node(const char *name, int type, uint32_t first_clu, uint32_t size, uint32_t dir_clus, uint32_t dir_off){
    //分配节点与节点私有数据
    void *page = (void*)PHYS_TO_VIRT((uintptr_t)Pmm_Malloc(1));
    if(!page)return NULL;
    memset(page, 0, PAGE_SIZE);
    fs_node_t *n = (fs_node_t*)page;
    fat_node_priv_t *p = (fat_node_priv_t*)((uintptr_t)page + sizeof(fs_node_t));
    //初始化节点结构体
    n->priv = p;
    n->type = type;
    n->ops = &fat_ops;
    n->refs = 1;
    n->size = size;
    n->ino = ((uint64_t)dir_clus << 32) | dir_off;
    int i;
    for(i = 0; i < MAX_NAME - 1 && name[i]; i++)n->name[i] = name[i];
    n->name[i] = 0;
    n->siblings.prev = &n->siblings;
    n->siblings.next = &n->siblings;
    n->children.prev = &n->children;
    n->children.next = &n->children;
    //初始化节点私有数据结构体
    p->first_clu = first_clu;
    p->size = size;
    p->dir_clus = dir_clus;
    p->dir_off = dir_off;
    p->pos_clu = 0;
    p->pos_off = 0;
    p->pos_within = 0;
    //从磁盘目录项解析时间戳(根节点无目录项则用当前时间)
    uint64_t now = rtc_get_epoch();
    n->atime = now;
    n->mtime = now;
    n->ctime = now;
    if(dir_clus >= 2 && dir_off < g_fat.bpc){
        if(!fat_read_cluster(dir_clus)){
            fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + dir_off);
            uint64_t ct = fat_dt_to_epoch(de->crt_date, de->crt_time);
            uint64_t wt = fat_dt_to_epoch(de->wrt_date, de->wrt_time);
            uint64_t at = fat_dt_to_epoch(de->lst_acc_date, 0);
            if(ct){
                n->ctime = ct;
                n->mtime = wt ? wt : ct;
            }else if(wt){
                n->ctime = wt;
                n->mtime = wt;
            }
            if(at)n->atime = at;
        }
    }
    return n;
}

//名字转8.3
static void to_83(const char *name, char *base, char *ext){
    int i;
    for(i = 0; i < 8; i++)base[i] = ' ';
    for(i = 0; i < 3; i++)ext[i] = ' ';
    const char *dot = 0;
    for(const char *p = name; *p; p++){
        if(*p == '.' && p != name)dot = p;
    }
    i = 0;
    for(const char *p = name; *p && p != dot && i < 8; p++, i++){
        base[i] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    }
    if(dot){
        int j = 0;
        for(const char *p = dot + 1; *p && j < 3; p++, j++){
            ext[j] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
        }
    }
}

//计算8.3短名校验和
static uint8_t fat_lfn_checksum(const uint8_t shortname[11]){
    uint8_t sum = 0;
    for(int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + shortname[i]);
    return sum;
}

//判断名字是否需要LFN
static int fat_needs_lfn(const char *name){
    int i = 0, dot = -1, ndots = 0;
    for(; name[i]; i++){
        if(name[i] == '.'){
            //首字符的'.'不当作扩展名分隔符
            if(i > 0){
                dot = i;
                ndots++;
            }
        }else if(name[i] >= 'a' && name[i] <= 'z'){
            return 1;//小写字母需LFN保留原样
        }else if(!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '_' || name[i] == '-' || name[i] == ' ')){
            return 1;//非8.3合法字符
        }
    }
    if(ndots > 1)return 1;//主名里再出现'.'无法用8.3表达
    int base = dot < 0 ? i : dot;
    int ext = dot < 0 ? 0 : i - dot - 1;
    if(base > 8 || ext > 3) return 1;
    return 0;
}

//填充一个LFN条目
static void fat_fill_lfn(fat_lfn_entry_t *lfn, const char *name, int name_len, int block, int total_blocks, uint8_t checksum){
    memset(lfn, 0, sizeof(*lfn));
    lfn->order = (block == total_blocks - 1) ? (uint8_t)(total_blocks | 0x40) : (uint8_t)(block + 1);
    lfn->attr = FAT_ATTR_LFN;
    lfn->type = 0;
    lfn->checksum = checksum;
    //本块13个UTF-16字符
    uint16_t chars[13];
    for(int j = 0; j < 13; j++){
        int ci = block * 13 + j;
        if(ci < name_len) chars[j] = (uint16_t)(uint8_t)name[ci];
        else if(ci == name_len) chars[j] = 0;//字符串终止
        else chars[j] = 0xFFFF;
    }
    for(int j = 0; j < 5; j++) lfn->name1[j] = chars[j];
    for(int j = 0; j < 6; j++) lfn->name2[j] = chars[5 + j];
    lfn->name3[0] = chars[11];
    lfn->name3[1] = chars[12];
}

//比较收集的LFN与名字(ASCII大小写不敏感)
static int fat_lfn_eq(const uint16_t *lfn, int lfn_len, const char *name){
    int i = 0;
    for(;;){
        uint16_t ch = (i < lfn_len) ? lfn[i] : 0;
        if(ch == 0xFFFF || ch == 0)ch = 0;//填充或终止都视为结束
        char q = name[i] ? name[i] : 0;
        if(ch == 0 && q == 0)return 1;
        if(ch == 0 || q == 0)return 0;
        if(ch >= 0x80)return 0;//非ASCII暂不支持
        char c = (char)ch;
        if(c >= 'a' && c <= 'z')c -= 32;
        if(q >= 'a' && q <= 'z')q -= 32;
        if(c != q)return 0;
        i++;
    }
}

//在目录中定位匹配名字的目录项(支持LFN长文件名), 返回所在簇+偏移
static int fat_find_dirent(fs_node_t *dir, const char *name, uint32_t *out_clus, uint32_t *out_off){
    if(!g_fat.disk || !dir || !dir->priv || !name) return -1;
    char base[8], ext[3];
    to_83(name, base, ext);
    fat_node_priv_t *dp = (fat_node_priv_t*)dir->priv;
    uint32_t clu = dp->first_clu;
    uint16_t lfn[256];
    int lfn_len = 0;
    while(clu >= 2 && clu < FAT_CLUSTER_EOF){
        if(fat_read_cluster(clu)) return -1;
        for(uint32_t off = 0; off < g_fat.bpc; off += sizeof(fat_dir_entry_t)){
            fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + off);
            if(de->name[0] == FAT_DIRENT_END) return -1;//目录项链结束
            if(de->name[0] == FAT_DIRENT_FREE){ lfn_len = 0; continue; }//已删除
            if(de->attr == FAT_ATTR_LFN){
                //收集LFN字符(条目倒序存放: 0x40|n为文件名开头块)
                uint8_t order = de->name[0] & 0x3F;
                if(order >= 1 && order <= 20){
                    int pos = (order - 1) * 13;
                    if(pos + 13 <= 256){
                        fat_lfn_entry_t *le = (fat_lfn_entry_t*)de;
                        for(int j = 0; j < 5; j++)lfn[pos + j] = le->name1[j];
                        for(int j = 0; j < 6; j++)lfn[pos + 5 + j] = le->name2[j];
                        for(int j = 0; j < 2; j++)lfn[pos + 11 + j] = le->name3[j];
                        if(order * 13 > lfn_len)lfn_len = order * 13;
                    }
                }
                continue;
            }
            if(de->attr & FAT_ATTR_VOLUME_ID){ lfn_len = 0; continue; }//卷标
            if(de->name[0] == '.'){ lfn_len = 0; continue; }//.和..
            //短条目: 8.3短名匹配优先
            if(memcmp(de->name, base, 8) == 0 && memcmp(de->ext, ext, 3) == 0){
                *out_clus = clu;
                *out_off = off;
                return 0;
            }
            //长名匹配
            if(lfn_len > 0 && fat_lfn_eq(lfn, lfn_len, name)){
                *out_clus = clu;
                *out_off = off;
                return 0;
            }
            lfn_len = 0;
        }
        clu = fat_get_entry(clu);
    }
    return -1;
}

//目录查找
static fs_node_t *fat_lookup(fs_node_t *dir, const char *name){
    if(!dir || !dir->priv || !name || !*name)return NULL;
    uint32_t dir_clu = fat_dir_id(dir);
    uint64_t h = fat_dc_hashname(name);
    uint32_t len = (uint32_t)strlen(name);
    int hit = 0;
    if(dir_clu >= 2){
        fs_node_t *c = fat_dc_lookup(dir_clu, name, h, len, &hit);
        if(hit)return c;
    }
    uint32_t clu, off;
    if(fat_find_dirent(dir, name, &clu, &off) != 0){
        if(dir_clu >= 2)fat_dc_store(dir_clu, name, h, len, NULL);
        return NULL;
    }
    if(fat_read_cluster(clu))return NULL;
    fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + off);
    uint32_t first = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
    uint32_t size = de->file_size;
    int type = (de->attr & FAT_ATTR_DIRECTORY) ? FT_DIR : FT_FILE;
    fs_node_t *n = fat_new_node(name, type, first, size, clu, off);
    if(n && dir_clu >= 2)fat_dc_store(dir_clu, name, h, len, n);
    return n;
}

//文件直接读盘读取
static uint64_t fat_read_direct(fs_node_t *node, uint64_t off, void *buf, uint64_t len){
    if(!node || !node->priv)return 0;
    fat_node_priv_t *p = (fat_node_priv_t*)node->priv;
    if(off >= p->size)return 0;
    if(len > p->size - off)len = p->size - off;
    uint64_t start_off = off;
    uint32_t clu;
    uint64_t within;
    //顺序读缓存
    if(p->pos_clu >= 2 && p->pos_clu < FAT_CLUSTER_EOF && off == p->pos_off){
        clu = p->pos_clu;
        within = p->pos_within;
    }else{
        clu = p->first_clu;
        while(off >= g_fat.bpc){
            clu = fat_get_entry(clu);
            if(clu < 2 || clu >= FAT_CLUSTER_EOF)return 0;
            off -= g_fat.bpc;
        }
        within = off;
    }
    uint64_t done = 0;
    uint8_t *dst = (uint8_t*)buf;
    while(done < len){
        if(clu < 2 || clu >= FAT_CLUSTER_EOF) break;
        //如果簇内非对齐,读单簇到簇缓冲, 拷贝尾部
        if(within > 0){
            if(fat_read_cluster(clu)) break;
            uint64_t c = len - done;
            if(c > g_fat.bpc - within) c = g_fat.bpc - within;
            memcpy(dst + done, g_fat.cluster_buf + within, c);
            done += c;
            within += c;
            if(within >= g_fat.bpc){
                within = 0;
                clu = fat_get_entry(clu);
            }
            continue;
        }
        //探测连续簇长度
        uint32_t run = 1;
        uint32_t probe = clu;
        uint32_t max_run = 512 / g_fat.spc;
        while(run < max_run){
            uint32_t nxt = fat_get_entry(probe);
            if(nxt != probe + 1) break;
            probe = nxt;
            run++;
            if((uint64_t)run * g_fat.bpc >= len - done) break;
        }
        uint64_t avail = (uint64_t)run * g_fat.bpc;
        uint64_t chunk = len - done;
        if(chunk > avail) chunk = avail;
        if(chunk >= g_fat.bpc){
            //批量读整个簇
            uint32_t full_clu = (uint32_t)(chunk / g_fat.bpc);
            uint32_t sectors = full_clu * g_fat.spc;
            if(fat_read_sectors(g_fat.data_start + (clu - 2) * g_fat.spc, dst + done, sectors)) break;
            done += (uint64_t)full_clu * g_fat.bpc;
            for(uint32_t k = 0; k < full_clu && clu >= 2 && clu < FAT_CLUSTER_EOF; k++)clu = fat_get_entry(clu);
            within = 0;
        //不足一个簇，读单簇并拷贝剩余部分
        }else{
            if(fat_read_cluster(clu)) break;
            memcpy(dst + done, g_fat.cluster_buf, chunk);
            done += chunk;
            within = chunk;
        }
    }
    p->pos_off = start_off + done;
    p->pos_clu = clu;
    p->pos_within = within;
    return done;
}

//页缓存填充回调
static uint64_t fat_pcache_fill(void *ctx, uint64_t off, void *buf, uint64_t len){
    return fat_read_direct((fs_node_t*)ctx, off, buf, len);
}

//文件读取
static uint64_t fat_read(fs_node_t *node, uint64_t off, void *buf, uint64_t len){
    if(!node || !node->priv)return 0;
    fat_node_priv_t *p = (fat_node_priv_t*)node->priv;
    if(off >= p->size)return 0;
    //计算页缓存可覆盖部分
    uint64_t want = len;
    if(want > p->size - off)want = p->size - off;
    if(node->type == FT_FILE && PageCacheReady()){
        uint64_t done = PageCacheRead(node->ino, off, buf, want, p->size, fat_pcache_fill, node);
        if(done == want)return done;
        //缓存未能覆盖全部
        uint64_t rest = fat_read_direct(node, off + done, (uint8_t*)buf + done, want - done);
        return done + rest;
    }
    return fat_read_direct(node, off, buf, want);
}

//文件写入
static uint64_t fat_write(fs_node_t *node, uint64_t off, const void *buf, uint64_t len){
    if(!node || !node->priv || !len)return 0;
    fat_node_priv_t *p = (fat_node_priv_t*)node->priv;
    uint64_t base = off;
    uint32_t clu = p->first_clu;
    //定位off所在簇
    while(off >= g_fat.bpc){
        uint32_t nxt = fat_get_entry(clu);
        if(nxt < 2 || nxt >= FAT_CLUSTER_EOF){
            nxt = fat_alloc_cluster(clu);//链尾不足, 追加新簇
            if(nxt < 2) return 0;
        }
        clu = nxt;
        off -= g_fat.bpc;
    }
    //如果文件为空，分配新簇
    if(clu < 2){
        clu = fat_alloc_cluster(FAT_CLUSTER_EOF);
        if(clu < 2)return 0;
        p->first_clu = clu;
    }
    //写入数据
    uint64_t done = 0;
    while(done < len){
        if(fat_read_cluster(clu))break;
        uint64_t chunk = len - done;
        if(chunk > g_fat.bpc - off) chunk = g_fat.bpc - off;
        memcpy(g_fat.cluster_buf + off, (const uint8_t*)buf + done, chunk);
        if(fat_write_cluster(clu))break;
        done += chunk;
        off = 0;
        //如果还有数据，进入下一簇,链尾则分配新簇
        if(done < len){
            uint32_t nxt = fat_get_entry(clu);
            if(nxt < 2 || nxt >= FAT_CLUSTER_EOF){
                nxt = fat_alloc_cluster(clu);
                if(nxt < 2) break;
            }
            clu = nxt;
        }
    }
    //更新文件数据与修改时间(即使大小不变也刷新时间戳)
    if(base + done > p->size){
        p->size = base + done;
        node->size = p->size;
    }
    fat_update_dirent(p);
    if(done)PageCacheInvalidateRange(node->ino, base, done);//写入的区间已失效，丢弃对应缓存页
    return done;
}

//在目录中分配目录项,返回短条目所在簇+偏移
static int fat_alloc_dirent(fs_node_t *dir, const char *name, int type, uint32_t first_clu, uint32_t *out_clus, uint32_t *out_off){
    if(!g_fat.disk || !dir || !dir->priv) return -1;
    char base[8], ext[3];
    to_83(name, base, ext);
    uint8_t shortname[11];
    memcpy(shortname, base, 8);
    memcpy(shortname + 8, ext, 3);
    uint8_t checksum = fat_lfn_checksum(shortname);
    int name_len = strlen(name);
    int n_lfn = fat_needs_lfn(name) ? (name_len + 12) / 13 : 0;//LFN条目数
    int need = n_lfn + 1;//LFN+短条目总槽数
    fat_node_priv_t *dp = (fat_node_priv_t*)dir->priv;
    uint32_t cur = dp->first_clu;
    //目录为空则先分配首簇
    if(cur < 2){
        cur = fat_alloc_cluster(FAT_CLUSTER_EOF);
        if(cur < 2) return -1;
        dp->first_clu = cur;
        fat_update_dirent(dp);
    }
    uint32_t guard = 0;
    while(1){
        if(++guard > g_fat.total_clusters + 2) return -1;
        if(fat_read_cluster(cur)) return -1;
        //找连续need个可用槽
        for(uint32_t off = 0; off + (uint32_t)need * sizeof(fat_dir_entry_t) <= g_fat.bpc; off += sizeof(fat_dir_entry_t)){
            int ok = 1;
            for(int s = 0; s < need; s++){
                uint8_t c = ((fat_dir_entry_t*)(g_fat.cluster_buf + off + s * sizeof(fat_dir_entry_t)))->name[0];
                if(c != FAT_DIRENT_FREE && c != FAT_DIRENT_END){ ok = 0; break; }
            }
            if(!ok) continue;
            //写LFN条目
            for(int k = 0; k < n_lfn; k++){
                fat_lfn_entry_t *lfn = (fat_lfn_entry_t*)(g_fat.cluster_buf + off + (n_lfn - 1 - k) * sizeof(fat_dir_entry_t));
                fat_fill_lfn(lfn, name, name_len, k, n_lfn, checksum);
            }
            //写短条目
            uint32_t short_off = off + (uint32_t)n_lfn * sizeof(fat_dir_entry_t);
            fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + short_off);
            memset(de, 0, sizeof(*de));
            memcpy(de->name, base, 8);
            memcpy(de->ext, ext, 3);
            de->attr = (type == FT_DIR) ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
            de->fst_clus_lo = (uint16_t)(first_clu & 0xFFFF);
            de->fst_clus_hi = (uint16_t)(first_clu >> 16);
            fat_set_now(de);//写入创建/修改/访问时间
            uint32_t after = off + (uint32_t)need * sizeof(fat_dir_entry_t);
            if(fat_write_cluster(cur))return -1;//写回当前簇
            if(after >= g_fat.bpc){
                uint32_t nxt = fat_alloc_cluster(cur);
                if(nxt < 2) return -1;
            }
            *out_clus = cur;
            *out_off = short_off;
            return 0;
        }
        //本簇无连续槽:进入下一簇或扩簇
        uint32_t nxt = fat_get_entry(cur);
        if(nxt < 2 || nxt >= FAT_CLUSTER_EOF){
            nxt = fat_alloc_cluster(cur);
            if(nxt < 2) return -1;
        }
        cur = nxt;
    }
}

//创建文件/目录
static fs_node_t *fat_create(fs_node_t *dir, const char *name, int type){
    if(!g_fat.disk || !dir || !dir->priv)return NULL;
    uint32_t clu, off;
    if(fat_alloc_dirent(dir, name, type, 0, &clu, &off))return NULL;
    PageCacheInvalidate(((uint64_t)clu << 32) | off);//新文件占用复用的槽位，丢弃残留缓存
    fs_node_t *n = fat_new_node(name, type, 0, 0, clu, off);
    //去掉可能的负缓存并登记新节点
    uint32_t dir_clu = fat_dir_id(dir);
    if(dir_clu >= 2){
        fat_dc_drop(dir_clu, name);
        if(n)fat_dc_store(dir_clu, name, fat_dc_hashname(name), (uint32_t)strlen(name), n);
    }
    return n;
}

//截断/扩展文件
static int fat_truncate(fs_node_t *node, uint64_t newsize){
    if(!node || !node->priv) return -1;
    fat_node_priv_t *p = (fat_node_priv_t*)node->priv;
    //如果文件大小为0,释放所有簇
    if(newsize == 0){
        fat_free_chain(p->first_clu);
        p->first_clu = 0;
    }else{
        uint32_t need = (uint32_t)((newsize + g_fat.bpc - 1) / g_fat.bpc);//需保留簇数
        uint32_t clu = p->first_clu, last = 0, have = 0;
        //走到第need个簇
        while(have < need && clu >= 2 && clu < FAT_CLUSTER_EOF){
            last = clu;
            have++;
            clu = fat_get_entry(clu);
        }
        if(have < need){
            //链太短，追加新簇
            uint32_t prev = (have > 0) ? last : FAT_CLUSTER_EOF;
            while(have < need){
                uint32_t n = fat_alloc_cluster(prev);
                if(n < 2) return -1;
                if(p->first_clu < 2) p->first_clu = n;
                last = n;
                prev = n;
                have++;
            }
        }else if(clu >= 2 && clu < FAT_CLUSTER_EOF){
            //有多余簇，断开链尾并释放
            fat_set_entry(last, FAT_CLUSTER_EOF);
            fat_free_chain(clu);
        }
    }
    p->size = (uint32_t)newsize;
    node->size = newsize;
    p->pos_clu = 0;
    p->pos_off = 0;
    p->pos_within = 0;
    fat_update_dirent(p);
    PageCacheInvalidate(node->ino);//大小与内容都可能变化，整文件缓存作废
    return 0;
}

//把节点上的回写到目录项
static int fat_set_times(fs_node_t *node){
    if(!node || !node->priv)return -1;
    fat_node_priv_t *p = (fat_node_priv_t*)node->priv;
    if(!p->dir_clus)return 0;//根节点没有目录项
    if(fat_read_cluster(p->dir_clus))return -1;
    fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + p->dir_off);
    if(node->mtime){
        rtc_time_t tm;
        rtc_epoch_to_utc(node->mtime + (uint64_t)TIMEZONE_OFFSET_HOURS * 3600, &tm);
        if(tm.year >= 1980){
            de->wrt_time = (uint16_t)((tm.hour << 11) | (tm.minute << 5) | (tm.second / 2));
            de->wrt_date = (uint16_t)(((tm.year - 1980) << 9) | (tm.month << 5) | tm.day);
        }
    }
    if(node->atime){
        rtc_time_t tm;
        rtc_epoch_to_utc(node->atime + (uint64_t)TIMEZONE_OFFSET_HOURS * 3600, &tm);
        if(tm.year >= 1980)
            de->lst_acc_date = (uint16_t)(((tm.year - 1980) << 9) | (tm.month << 5) | tm.day);
    }
    return fat_write_cluster(p->dir_clus);
}

//创建目录
static int fat_mkdir(fs_node_t *dir, const char *name, int mode){
    (void)mode;
    if(!g_fat.disk || !dir || !dir->priv) return -1;
    //分配新目录首簇
    uint32_t newclu = fat_alloc_cluster(FAT_CLUSTER_EOF);
    if(newclu < 2) return -1;
    //在新簇写 . 和 .. 目录项
    memset(g_fat.cluster_buf, 0, g_fat.bpc);
    fat_dir_entry_t *dot = (fat_dir_entry_t*)g_fat.cluster_buf;
    dot->name[0] = '.';
    for(int i = 1; i < 8; i++) dot->name[i] = ' ';
    for(int i = 0; i < 3; i++) dot->ext[i] = ' ';
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->fst_clus_lo = (uint16_t)(newclu & 0xFFFF);
    dot->fst_clus_hi = (uint16_t)(newclu >> 16);
    fat_dir_entry_t *dotdot = (fat_dir_entry_t*)(g_fat.cluster_buf + sizeof(fat_dir_entry_t));
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    for(int i = 2; i < 8; i++) dotdot->name[i] = ' ';
    for(int i = 0; i < 3; i++) dotdot->ext[i] = ' ';
    dotdot->attr = FAT_ATTR_DIRECTORY;
    uint32_t pclu = ((fat_node_priv_t*)dir->priv)->first_clu;
    if(pclu == g_fat.root_clu) pclu = 0;
    dotdot->fst_clus_lo = (uint16_t)(pclu & 0xFFFF);
    dotdot->fst_clus_hi = (uint16_t)(pclu >> 16);
    if(fat_write_cluster(newclu)) return -1;
    //在父目录分配目录项
    uint32_t clu, off;
    int r = fat_alloc_dirent(dir, name, FT_DIR, newclu, &clu, &off);
    if(r == 0)fat_dc_drop(fat_dir_id(dir), name);//新增了目录项，负缓存作废
    return r;
}

/*DeepSeek V4.1 Flash*/
//判断目录是否为空
static int fat_dir_is_empty(uint32_t first_clu){
    uint32_t c = first_clu;
    //上界必须有限: 目录簇链损坏成环时否则会读盘读到天荒地老(表现为内核卡死)
    for(int guard = 0; c >= 2 && c < FAT_CLUSTER_EOF && guard < 4096; guard++){
        if(fat_read_cluster(c))return 0;//读失败当作非空, 保守处理
        for(uint32_t i = 0; i < g_fat.bpc; i += sizeof(fat_dir_entry_t)){
            fat_dir_entry_t *e = (fat_dir_entry_t*)(g_fat.cluster_buf + i);
            if(e->name[0] == FAT_DIRENT_END)return 1;//结束标记
            if(e->name[0] == FAT_DIRENT_FREE)continue;//已删除
            if(e->name[0] == '.')continue;//. 和 ..
            if(e->attr == FAT_ATTR_LFN)continue;//LFN 残留
            return 0;//还有其它条目
        }
        c = fat_get_entry(c);
    }
    return 1;
}
/*DeepSeek V4.1 Flash-END*/

//删除目录项
static int fat_unlink(fs_node_t *dir, const char *name){
    if(!g_fat.disk || !dir || !dir->priv) return -1;
    uint32_t clu, off;
    if(fat_find_dirent(dir, name, &clu, &off) != 0) return -1;
    if(fat_read_cluster(clu)) return -1;
    fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + off);
    uint32_t first = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
    //目录必须为空
    if((de->attr & FAT_ATTR_DIRECTORY) && first >= 2 && !fat_dir_is_empty(first))return -ENOTEMPTY;
    if(fat_read_cluster(clu)) return -1;//上面可能覆盖了簇缓冲, 重新读入目录项
    de = (fat_dir_entry_t*)(g_fat.cluster_buf + off);
    //释放文件/目录的簇链
    if(first >= 2) fat_free_chain(first);
    PageCacheInvalidate(((uint64_t)clu << 32) | off);//丢弃此标识下的页缓存
    fat_dc_drop(fat_dir_id(dir), name);//目录项缓存作废
    //标记短条目及其前面的LFN条目为已删除
    de->name[0] = FAT_DIRENT_FREE;
    uint32_t lfn_off = off;
    while(lfn_off >= sizeof(fat_dir_entry_t)){
        lfn_off -= sizeof(fat_dir_entry_t);
        fat_dir_entry_t *e = (fat_dir_entry_t*)(g_fat.cluster_buf + lfn_off);
        if(e->attr == FAT_ATTR_LFN) e->name[0] = FAT_DIRENT_FREE;
        else break;
    }
    return fat_write_cluster(clu) ? -1 : 0;
}

//重命名
static int fat_rename(fs_node_t *olddir, const char *oldname, fs_node_t *newdir, const char *newname){
    if(!g_fat.disk || !olddir || !newdir || !olddir->priv || !newdir->priv) return -1;
    //定位旧目录项
    uint32_t old_clus, old_off;
    if(fat_find_dirent(olddir, oldname, &old_clus, &old_off) != 0) return -1;
    if(fat_read_cluster(old_clus)) return -1;
    fat_dir_entry_t *ode = (fat_dir_entry_t*)(g_fat.cluster_buf + old_off);
    //保存旧目录项信息
    uint32_t first = ((uint32_t)ode->fst_clus_hi << 16) | ode->fst_clus_lo;
    uint32_t size = ode->file_size;
    int type = (ode->attr & FAT_ATTR_DIRECTORY) ? FT_DIR : FT_FILE;
    uint16_t crt_time = ode->crt_time, crt_date = ode->crt_date;
    uint8_t crt_tenth = ode->crt_time_tenth;
    uint16_t wrt_time = ode->wrt_time, wrt_date = ode->wrt_date;
    uint16_t lst_acc = ode->lst_acc_date;
    //在新父目录分配新目录项
    uint32_t new_clus, new_off;
    if(fat_alloc_dirent(newdir, newname, type, first, &new_clus, &new_off)) return -1;
    //补写文件大小与原时间戳
    if(fat_read_cluster(new_clus)) return -1;
    fat_dir_entry_t *nde = (fat_dir_entry_t*)(g_fat.cluster_buf + new_off);
    nde->file_size = size;
    nde->crt_time = crt_time;
    nde->crt_date = crt_date;
    nde->crt_time_tenth = crt_tenth;
    nde->wrt_time = wrt_time;
    nde->wrt_date = wrt_date;
    nde->lst_acc_date = lst_acc;
    if(fat_write_cluster(new_clus)) return -1;
    //释放旧目录项
    if(fat_read_cluster(old_clus)) return -1;
    PageCacheInvalidate(((uint64_t)old_clus << 32) | old_off);//丢弃旧标识下的页缓存
    //两侧的键都作废
    fat_dc_drop(fat_dir_id(olddir), oldname);
    fat_dc_drop(fat_dir_id(newdir), newname);
    ode = (fat_dir_entry_t*)(g_fat.cluster_buf + old_off);
    ode->name[0] = FAT_DIRENT_FREE;
    uint32_t lfn_off = old_off;
    while(lfn_off >= sizeof(fat_dir_entry_t)){
        lfn_off -= sizeof(fat_dir_entry_t);
        fat_dir_entry_t *e = (fat_dir_entry_t*)(g_fat.cluster_buf + lfn_off);
        if(e->attr == FAT_ATTR_LFN) e->name[0] = FAT_DIRENT_FREE;
        else break;
    }
    return fat_write_cluster(old_clus) ? -1 : 0;
}

//按线性目录项索引定位
static int fat_dir_entry_at(fat_node_priv_t *dp, uint32_t idx, uint32_t *clu, uint32_t *off){
    uint32_t epp = g_fat.bpc / sizeof(fat_dir_entry_t);//每簇目录项数
    uint32_t cluster_idx = idx / epp;
    uint32_t within = idx % epp;
    uint32_t c, steps;
    if(dp->it_valid && dp->it_pos <= cluster_idx && cluster_idx - dp->it_pos < 4096){
        c = dp->it_clu;
        steps = cluster_idx - dp->it_pos;
    }else{
        c = dp->first_clu;
        steps = cluster_idx;
    }
    while(steps--){
        uint32_t nxt = fat_get_entry(c);
        if(nxt < 2 || nxt >= FAT_CLUSTER_EOF){
            dp->it_valid = 0;
            return -1;
        }
        c = nxt;
    }
    if(c < 2 || c >= FAT_CLUSTER_EOF){ dp->it_valid = 0; return -1; }
    dp->it_pos = cluster_idx;
    dp->it_clu = c;
    dp->it_valid = 1;
    *clu = c;
    *off = within * sizeof(fat_dir_entry_t);
    return 0;
}

//由8.3短名生成字符串
static void fat_short_name(fat_dir_entry_t *de, char *name){
    int b = 7; while(b >= 0 && de->name[b] == ' ')b--;
    int e = 2; while(e >= 0 && de->ext[e] == ' ')e--;
    int lo_base = (de->nt_res & 0x08) ? 1 : 0;
    int lo_ext  = (de->nt_res & 0x10) ? 1 : 0;
    int j = 0;
    for(int i = 0; i <= b; i++){
        char c = (char)de->name[i];
        if(lo_base && c >= 'A' && c <= 'Z') c += 32;
        name[j++] = c;
    }
    if(e >= 0){
        name[j++] = '.';
        for(int i = 0; i <= e; i++){
            char c = (char)de->ext[i];
            if(lo_ext && c >= 'A' && c <= 'Z') c += 32;
            name[j++] = c;
        }
    }
    name[j] = 0;
}

//回看短条目前面的LFN条目,重建长文件名,有LFN返回1，否则0
#define LFN_MAX_BLOCKS 20
static int fat_reconstruct_lfn(fat_node_priv_t *dp, uint32_t idx, char *name){
    uint16_t buf[LFN_MAX_BLOCKS * 13];
    uint8_t  seen[LFN_MAX_BLOCKS + 1];
    int maxo = 0, cnt = 0;
    uint8_t chain_chk = 0;
    memset(seen, 0, sizeof(seen));
    for(int i = 0; i < LFN_MAX_BLOCKS * 13; i++)buf[i] = 0xFFFF;
    for(uint32_t j = 1; j <= idx; j++){
        uint32_t clu, eoff;
        if(fat_dir_entry_at(dp, idx - j, &clu, &eoff))break;
        if(fat_read_cluster(clu))return 0;
        fat_lfn_entry_t *le = (fat_lfn_entry_t*)(g_fat.cluster_buf + eoff);
        if(le->attr != FAT_ATTR_LFN)break;
        if(le->order == FAT_DIRENT_FREE)break;//已删除的LFN槽(0xE5)不参与拼接
        if(j == 1){
            chain_chk = le->checksum;
        }else if(le->checksum != chain_chk){
            break;//校验和变了说明不是同一条链
        }
        uint8_t order = le->order & 0x3F;
        if(order < 1 || order > LFN_MAX_BLOCKS)break;
        if(seen[order])break;//序号重复,链条到此为止
        seen[order] = 1;
        cnt++;
        if(order > maxo)maxo = order;
        int pos = (order - 1) * 13;
        for(int k = 0; k < 5; k++)buf[pos + k] = le->name1[k];
        for(int k = 0; k < 6; k++)buf[pos + 5 + k] = le->name2[k];
        for(int k = 0; k < 2; k++)buf[pos + 11 + k] = le->name3[k];
    }
    if(cnt == 0)return 0;
    for(int i = 1; i <= maxo; i++){
        if(!seen[i])return 0;//缺块
    }
    int total = 0;
    for(int k = 0; k < LFN_MAX_BLOCKS * 13 && k < MAX_NAME - 1; k++){
        uint16_t ch = buf[k];
        if(ch == 0)break;//字符串终止
        if(ch == 0xFFFF)continue;//块尾填充
        name[k] = (ch < 0x80) ? (char)ch : '?';
        total = k + 1;
    }
    if(total <= 0)return 0;
    name[total] = 0;
    return 1;
}

//目录迭代
static int fat_readdir(fs_node_t *dir, uint64_t *cookie, fs_dirent_t *out){
    if(!g_fat.disk || !dir || !dir->priv)return -1;
    fat_node_priv_t *dp = (fat_node_priv_t*)dir->priv;
    uint32_t idx = (uint32_t)(*cookie / sizeof(fat_dir_entry_t));
    for(;;){
        uint32_t clu, eoff;
        if(fat_dir_entry_at(dp, idx, &clu, &eoff))return 1;
        if(fat_read_cluster(clu))return -1;
        fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + eoff);
        uint8_t c = de->name[0];
        if(c == FAT_DIRENT_END)return 1;
        if(c == FAT_DIRENT_FREE || de->attr == FAT_ATTR_LFN || (de->attr & FAT_ATTR_VOLUME_ID) || c == '.'){
            idx++;
            continue;
        }
        fat_dir_entry_t saved = *de;
        if(!fat_reconstruct_lfn(dp, idx, out->name)){
            fat_short_name(&saved, out->name);
        }
        out->ino = ((uint64_t)clu << 32) | eoff;
        out->type = (saved.attr & FAT_ATTR_DIRECTORY) ? FT_DIR : FT_FILE;
        *cookie = (uint64_t)(idx + 1) * sizeof(fat_dir_entry_t);
        return 0;
    }
}

//FAT32节点操作集
static fs_node_ops_t fat_ops = {
    .read     = fat_read,
    .write    = fat_write,
    .lookup   = fat_lookup,
    .create   = fat_create,
    .truncate = fat_truncate,
    .mkdir    = fat_mkdir,
    .unlink   = fat_unlink,
    .rename   = fat_rename,
    .readdir  = fat_readdir,
    .set_times= fat_set_times,
};

//挂载FAT32分区
int fat32_mount(struct disk_info *disk){
    if(!disk) return 1;
    memset(&g_fat, 0, sizeof(g_fat));
    g_root_node = NULL;
    fat_dc_init();
    fat_dc_clear();
    //拷贝磁盘信息到自身
    g_fat.disk_copy = *disk;
    g_fat.disk = &g_fat.disk_copy;
    g_fat.part_lba = (uint32_t)disk->partition_start_lba;//分区起始LBA
    //读取并解析BPB
    uint8_t boot[512];
    if(fat_read_sectors(g_fat.part_lba, boot, 1))return 2;
    fat32_bpb_t *bpb = (fat32_bpb_t*)boot;
    g_fat.bps = bpb->bytes_per_sector;
    g_fat.spc = bpb->sectors_per_cluster;
    g_fat.rsvd = bpb->reserved_sectors;
    g_fat.nfats = bpb->num_fats;
    g_fat.fat_sz = bpb->fat_size32;
    g_fat.root_clu = bpb->root_cluster;
    if(g_fat.bps != 512 || g_fat.spc == 0 || g_fat.fat_sz == 0)return 3;
    if(g_fat.root_clu < 2)return 3;
    g_fat.fat_start = g_fat.part_lba + g_fat.rsvd;
    g_fat.data_start = g_fat.fat_start + (uint32_t)g_fat.nfats * g_fat.fat_sz;
    g_fat.bpc = (uint32_t)g_fat.spc * g_fat.bps;
    g_fat.total_clusters = (bpb->total_sectors32 - g_fat.rsvd -(uint32_t)g_fat.nfats * g_fat.fat_sz) / g_fat.spc;
    g_fat.alloc_hint = 2;
    //分配簇缓冲
    g_fat.cluster_buf = (uint8_t*)PHYS_TO_VIRT((uintptr_t)Pmm_Malloc((int)((g_fat.bpc + 4095) / 4096)));
    if(!g_fat.cluster_buf)return 4;
    fat_cache_init();//分配FAT页缓存与簇缓存
    return 0;
}

//FAT32根目录节点
fs_node_t *fat32_root(void){
    if(!g_root_node)g_root_node = fat_new_node("/", FT_DIR, g_fat.root_clu, 0, 0, 0);
    return g_root_node;
}
