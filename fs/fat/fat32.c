#include <fs.h>
#include <drives/disk.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <print.h>

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
} fat_node_priv_t;

static fat32_fs_t g_fat;
static fs_node_ops_t fat_ops;

//读磁盘扇区
static int fat_read_sectors(uint32_t lba, void *buf, uint32_t count){
    return DiskRead(g_fat.disk, lba, count, buf);
}

//读FAT表项
static uint32_t fat_get_entry(uint32_t clu){
    uint32_t fat_off = clu * 4;
    uint32_t fat_lba = g_fat.fat_start + fat_off / g_fat.bps;
    uint32_t off = fat_off % g_fat.bps;
    uint8_t sec[512];
    if(fat_read_sectors(fat_lba, sec, 1)) return 0;
    return *(uint32_t*)(sec + off) & 0x0FFFFFFF;
}

//读一簇到簇缓冲
static int fat_read_cluster(uint32_t clu){
    return fat_read_sectors(g_fat.data_start + (clu - 2) * g_fat.spc,g_fat.cluster_buf, g_fat.spc);
}

//写磁盘扇区
static int fat_write_sectors(uint32_t lba, const void *buf, uint32_t count){
    return DiskWrite(g_fat.disk, lba, count, buf);
}

//簇缓冲写回磁盘
static int fat_write_cluster(uint32_t clu){
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
    return 0;
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

//把节点的起始簇/大小同步回磁盘目录项
static int fat_update_dirent(fat_node_priv_t *p){
    if(!p || !p->dir_clus) return 0; //根节点等无目录项
    if(fat_read_cluster(p->dir_clus)) return -1;
    fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + p->dir_off);
    de->fst_clus_hi = (uint16_t)(p->first_clu >> 16);
    de->fst_clus_lo = (uint16_t)(p->first_clu & 0xFFFF);
    de->file_size = p->size;
    return fat_write_cluster(p->dir_clus);
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
    return n;
}

//名字转8.3
static void to_83(const char *name, char *base, char *ext){
    int i;
    for(i = 0; i < 8; i++)base[i] = ' ';
    for(i = 0; i < 3; i++)ext[i] = ' ';
    i = 0;
    while(name[i] && name[i] != '.' && i < 8){
        base[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
        i++;
    }
    const char *dot = name;
    while(*dot && *dot != '.')dot++;
    if(*dot == '.'){
        dot++;
        for(int j = 0; j < 3 && dot[j]; j++)ext[j] = (dot[j] >= 'a' && dot[j] <= 'z') ? dot[j] - 32 : dot[j];
    }
}

//目录查找
static fs_node_t *fat_lookup(fs_node_t *dir, const char *name){
    if(!g_fat.disk || !dir || !dir->priv) return NULL;
    //转8.3文件名
    char base[8], ext[3];
    to_83(name, base, ext);
    fat_node_priv_t *dp = (fat_node_priv_t*)dir->priv;
    uint32_t clu = dp->first_clu;
    while(clu >= 2 && clu < FAT_CLUSTER_EOF){
        if(fat_read_cluster(clu))return NULL;//读取簇
        //遍历簇中的目录项
        for(uint32_t off = 0; off < g_fat.bpc; off += sizeof(fat_dir_entry_t)){
            fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + off);
            if(de->name[0] == FAT_DIRENT_END)return NULL;//目录项链结束
            if(de->name[0] == FAT_DIRENT_FREE)continue;//已删除
            if(de->attr == FAT_ATTR_LFN)continue;//长文件名条目
            if(de->attr & FAT_ATTR_VOLUME_ID)continue;//卷标
            if(de->name[0] == '.')continue;//.和..
            if(memcmp(de->name, base, 8) != 0 || memcmp(de->ext, ext, 3) != 0)continue;//匹配名称
            //构建节点
            uint32_t first = ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
            uint32_t size = de->file_size;
            int type = (de->attr & FAT_ATTR_DIRECTORY) ? FT_DIR : FT_FILE;
            return fat_new_node(name, type, first, size, clu, off);
        }
        clu = fat_get_entry(clu);//下一簇
    }
    return NULL;
}

//文件读取
static uint64_t fat_read(fs_node_t *node, uint64_t off, void *buf, uint64_t len){
    if(!node || !node->priv)return 0;
    fat_node_priv_t *p = (fat_node_priv_t*)node->priv;
    if(off >= p->size)return 0;
    if(len > p->size - off)len = p->size - off;
    uint32_t clu = p->first_clu;
    uint64_t done = 0;
    //跳过off之前的数据
    while(off >= g_fat.bpc){
        clu = fat_get_entry(clu);
        if(clu < 2 || clu >= FAT_CLUSTER_EOF)return done;
        off -= g_fat.bpc;
    }
    //读取数据
    while(done < len){
        if(clu < 2 || clu >= FAT_CLUSTER_EOF) break;
        if(fat_read_cluster(clu)) break;
        uint64_t chunk = len - done;
        if(chunk > g_fat.bpc - off) chunk = g_fat.bpc - off;
        memcpy((uint8_t*)buf + done, g_fat.cluster_buf + off, chunk);
        done += chunk;
        off = 0;
        clu = fat_get_entry(clu);
    }
    return done;
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
        if(nxt < 2 || nxt >= FAT_CLUSTER_EOF)return 0;
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
    //更新文件数据
    if(base + done > p->size){
        p->size = base + done;
        node->size = p->size;
        fat_update_dirent(p);
    }
    return done;
}

//创建文件/目录
static fs_node_t *fat_create(fs_node_t *dir, const char *name, int type){
    if(!g_fat.disk || !dir || !dir->priv)return NULL;
    //转8.3文件名
    char base[8], ext[3];
    to_83(name, base, ext);
    fat_node_priv_t *dp = (fat_node_priv_t*)dir->priv;
    uint32_t cur = dp->first_clu;
    //如果目录为空，分配首簇
    if(cur < 2){
        cur = fat_alloc_cluster(FAT_CLUSTER_EOF);
        if(cur < 2)return NULL;
        dp->first_clu = cur;
        fat_update_dirent(dp);
    }
    uint32_t guard = 0;
    while(1){
        if(++guard > g_fat.total_clusters + 2)return NULL;
        if(fat_read_cluster(cur))return NULL;
        for(uint32_t off = 0; off < g_fat.bpc; off += sizeof(fat_dir_entry_t)){
            fat_dir_entry_t *de = (fat_dir_entry_t*)(g_fat.cluster_buf + off);
            if(de->name[0] == FAT_DIRENT_END && off + 2 * sizeof(fat_dir_entry_t) > g_fat.bpc)break;
            if(de->name[0] != FAT_DIRENT_FREE && de->name[0] != FAT_DIRENT_END)continue;
            _Bool was_end = (de->name[0] == FAT_DIRENT_END);
            memset(de, 0, sizeof(fat_dir_entry_t));
            memcpy(de->name, base, 8);
            memcpy(de->ext, ext, 3);
            de->attr = (type == FT_DIR) ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;//标记类型
            //在原结束标记后补新结束标记
            if(was_end){
                memset((uint8_t*)de + sizeof(fat_dir_entry_t), 0, sizeof(fat_dir_entry_t));
                ((fat_dir_entry_t*)((uint8_t*)de + sizeof(fat_dir_entry_t)))->name[0] = FAT_DIRENT_END;
            }
            if(fat_write_cluster(cur)) return NULL;
            return fat_new_node(name, type, 0, 0, cur, off);
        }
        //本簇无空闲项,进入下一簇,链尾则扩簇
        uint32_t nxt = fat_get_entry(cur);
        if(nxt < 2 || nxt >= FAT_CLUSTER_EOF){
            nxt = fat_alloc_cluster(cur);
            if(nxt < 2) return NULL;
        }
        cur = nxt;
    }
}

//截断文件(释放多余簇)
static void fat_truncate(fs_node_t *node){
    if(!node || !node->priv) return;
    fat_node_priv_t *p = (fat_node_priv_t*)node->priv;
    //如果文件大小为0,释放所有簇
    if(p->size == 0){
        fat_free_chain(p->first_clu);
        p->first_clu = 0;
    }else{
        uint32_t keep = (p->size + g_fat.bpc - 1) / g_fat.bpc;//需保留簇数
        uint32_t clu = p->first_clu;
        uint32_t prev = 0;
        //获取要释放的簇链
        for(uint32_t i = 0; i < keep && clu >= 2 && clu < FAT_CLUSTER_EOF; i++){
            prev = clu;
            clu = fat_get_entry(clu);
        }
        //释放
        if(prev >= 2 && prev < FAT_CLUSTER_EOF){
            fat_set_entry(prev, FAT_CLUSTER_EOF);
            fat_free_chain(clu);
        }
    }
    //更新信息
    node->size = p->size;
    fat_update_dirent(p);
}

//FAT32节点操作集
static fs_node_ops_t fat_ops = {
    .read     = fat_read,
    .write    = fat_write,
    .lookup   = fat_lookup,
    .create   = fat_create,
    .truncate = fat_truncate,
};

//挂载FAT32分区
int fat32_mount(struct disk_info *disk){
    if(!disk) return 1;
    memset(&g_fat, 0, sizeof(g_fat));
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
    return 0;
}

//FAT32根目录节点
fs_node_t *fat32_root(void){
    return fat_new_node("/", FT_DIR, g_fat.root_clu, 0, 0, 0);
}
