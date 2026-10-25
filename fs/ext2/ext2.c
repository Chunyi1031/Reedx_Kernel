// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/ext2/ext2.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * ext2文件系统驱动
 *
 * 由DeepSeek-V4.1-Flash生成
 */
#include <vfs.h>
#include <fs.h>
#include <print.h>
#include <drives/blkdev.h>
#include <mm/kslab.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define EXT2_MAGIC       0xEF53u
#define EXT2_ROOT_INO    2u

#define EXT2_INCOMPAT_FILETYPE 0x0002u

#define EXT2_NIDIRECT    12      //i_block 前 12 个是直接块
#define EXT2_BLOCKS      15      //i_block 总共 15 个块号
#define EXT2_LINK_MAX    256     //符号链接目标的最大缓存长度

//磁盘上的超级块(只用到前 136 字节, 但结构必须撑满 1024)
typedef struct __attribute__((packed)) ext2_super {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    uint8_t  s_reserved[888];
} ext2_super_t;

//块组描述符
typedef struct __attribute__((packed)) ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} ext2_group_desc_t;

//磁盘上的 inode
typedef struct __attribute__((packed)) ext2_dinode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT2_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_dinode_t;

//目录项头部: 名字紧跟在 8 字节头后面
typedef struct __attribute__((packed)) ext2_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} ext2_dir_entry_t;

_Static_assert(sizeof(ext2_super_t)      == 1024, "ext2 super must be 1024 bytes");
_Static_assert(sizeof(ext2_group_desc_t) == 32,   "ext2 group desc must be 32 bytes");
_Static_assert(sizeof(ext2_dinode_t)     == 128,  "ext2 inode must be 128 bytes");
_Static_assert(sizeof(ext2_dir_entry_t)  == 8,    "ext2 dirent must be 8 bytes");

//文件系统私有数据
typedef struct ext2_sb_info {
    bdev_t  *bdev;              //ext2 所在的分区块设备
    uint32_t block_size;        //文件系统块大小(1024/2048/4096)
    uint32_t ptrs;              //一个块能放多少个块号
    uint32_t inode_size;        //inode 结构大小(EXT2 标准 128)
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t first_data_block;  //第一个数据块号(1K 块时为 1)
    uint32_t nr_groups;
    uint32_t desc_blk;          //块组描述符表起始块号
    uint32_t incompat;          //incompat 特性字
    uint8_t *meta_buf;          //间接块/元数据临时缓冲
    uint8_t *dir_buf;           //目录块扫描缓冲
} ext2_sb_info_t;

#define EX2SB(sb)  ((ext2_sb_info_t*)(sb)->s_fs_info)
#define EX2I(ino)  ((ext2_inode_info_t*)(ino)->i_private)

//inode 私有数据
typedef struct ext2_inode_info {
    uint32_t i_block[EXT2_BLOCKS];
    char     i_link[EXT2_LINK_MAX];//符号链接目标(NUL 结尾), iget 时就准备好
} ext2_inode_info_t;

static struct kmem_cache *g_ext2_info_cache;

static struct inode_operations g_ext2_iops;
static const struct file_operations g_ext2_file_fops;
static const struct file_operations g_ext2_dir_fops;

//==================== 块读取 ====================

//读一个文件系统块
static int ext2_read_block(ext2_sb_info_t *s, uint32_t blk, void *buf){
    if(!blk)return -EIO;
    if(bdev_read(s->bdev, (uint64_t)blk * s->block_size, buf, s->block_size))return -EIO;
    return 0;
}

//读第 group 个块组描述符
static int ext2_read_group_desc(ext2_sb_info_t *s, uint32_t group, ext2_group_desc_t *out){
    uint64_t off = (uint64_t)s->desc_blk * s->block_size + (uint64_t)group * sizeof(*out);
    if(bdev_read(s->bdev, off, out, sizeof(*out)))return -EIO;
    return 0;
}

//读出 ino 对应的磁盘 inode
static int ext2_read_dinode(ext2_sb_info_t *s, uint32_t ino, ext2_dinode_t *out){
    if(ino == 0)return -EINVAL;
    //由 inode 号算出它属于哪个块组、组内第几个
    uint32_t group = (ino - 1) / s->inodes_per_group;
    uint32_t index = (ino - 1) % s->inodes_per_group;
    ext2_group_desc_t gd;
    int r = ext2_read_group_desc(s, group, &gd);
    if(r)return r;
    uint64_t off = (uint64_t)gd.bg_inode_table * s->block_size + (uint64_t)index * s->inode_size;
    if(bdev_read(s->bdev, off, out, sizeof(*out)))return -EIO;
    return 0;
}

//把文件内的逻辑块号映射成磁盘块号, 返回 0 表示空洞
static uint32_t ext2_bmap(ext2_sb_info_t *s, ext2_inode_info_t *ii, uint32_t blkno){
    uint32_t ptrs = s->ptrs;
    if(blkno < EXT2_NIDIRECT)return ii->i_block[blkno];
    blkno -= EXT2_NIDIRECT;
    //一级间接
    if(blkno < ptrs){
        if(ext2_read_block(s, ii->i_block[12], s->meta_buf))return 0;
        return ((uint32_t*)s->meta_buf)[blkno];
    }
    blkno -= ptrs;
    //二级间接
    if(blkno < ptrs * ptrs){
        if(ext2_read_block(s, ii->i_block[13], s->meta_buf))return 0;
        uint32_t lvl1 = blkno / ptrs;
        uint32_t lvl2 = blkno % ptrs;
        uint32_t blk = ((uint32_t*)s->meta_buf)[lvl1];
        if(!blk)return 0;
        if(ext2_read_block(s, blk, s->meta_buf))return 0;
        return ((uint32_t*)s->meta_buf)[lvl2];
    }
    return 0;//不支持三级间接
}

//==================== inode ====================

//读出 ino 并在 icache 里建立对应的 inode
static struct inode *ext2_iget(struct super_block *sb, uint32_t ino){
    ext2_sb_info_t *s = EX2SB(sb);
    struct inode *inode = IgetLocked(sb, ino);
    if(!inode)return NULL;
    if(!(inode->i_state & I_NEW))return inode;
    ext2_inode_info_t *ii = EX2I(inode);
    ext2_dinode_t di;
    if(!ii || ext2_read_dinode(s, ino, &di)){
        inode->i_state &= ~I_NEW;
        Iput(inode);
        return NULL;
    }
    //块映射表和统计信息
    for(int k = 0;k < EXT2_BLOCKS;k ++)ii->i_block[k] = di.i_block[k];
    inode->i_mode   = di.i_mode;
    inode->i_uid    = di.i_uid;
    inode->i_gid    = di.i_gid;
    inode->i_size   = di.i_size;
    inode->i_nlink  = di.i_links_count;
    inode->i_blocks = di.i_blocks;
    inode->i_atime_s = (int64_t)di.i_atime;
    inode->i_mtime_s = (int64_t)di.i_mtime;
    inode->i_ctime_s = (int64_t)di.i_ctime;
    inode->i_op  = &g_ext2_iops;
    //按类型选操作表: 目录要遍历, 普通文件要读, 符号链接两者都不需要
    uint16_t type = (uint16_t)(di.i_mode & S_IFMT);
    if(type == S_IFDIR)     inode->i_fop = &g_ext2_dir_fops;
    else if(type == S_IFREG)inode->i_fop = &g_ext2_file_fops;
    else                    inode->i_fop = NULL;
    //符号链接目标: 不超过 60 字节时直接存在 i_block 里
    if(type == S_IFLNK && di.i_size < EXT2_LINK_MAX){
        if(di.i_size <= 60){
            memcpy(ii->i_link, di.i_block, di.i_size);
        }else{
            uint32_t blk = di.i_block[0];
            if(blk && bdev_read(s->bdev, (uint64_t)blk * s->block_size,
                                ii->i_link, di.i_size) == 0){
                //读成功
            }else{
                ii->i_link[0] = '\0';
            }
        }
        ii->i_link[di.i_size] = '\0';
    }
    inode->i_state &= ~I_NEW;
    return inode;
}

//==================== 目录 ====================

//目录项名字长度: 开了 FILETYPE 才是 1 字节
static uint32_t ext2_dirent_namelen(ext2_sb_info_t *s, const uint8_t *e){
    if(s->incompat & EXT2_INCOMPAT_FILETYPE)return e[6];
    return (uint32_t)(e[6] | (e[7] << 8));
}

//ext2 的文件类型码转成 S_IF*
static int ext2_dirent_type(uint8_t ft){
    switch(ft){
    case 1:  return S_IFREG;
    case 2:  return S_IFDIR;
    case 7:  return S_IFLNK;
    default: return S_IFREG;
    }
}

//把目录的第 n 个逻辑块读进 dir_buf
static int ext2_read_dir_block(ext2_sb_info_t *s, struct inode *dir, uint32_t n){
    uint32_t blk = ext2_bmap(s, EX2I(dir), n);
    if(!blk)return -EIO;
    return ext2_read_block(s, blk, s->dir_buf);
}

//目录占几个块
static uint32_t ext2_dir_nblocks(ext2_sb_info_t *s, struct inode *dir){
    return (uint32_t)((dir->i_size + s->block_size - 1) / s->block_size);
}

//在一段目录块里按名字查找, 返回 inode 号(0 = 没找到)
static uint32_t ext2_scan_entry(ext2_sb_info_t *s, const uint8_t *buf,
                                const char *name, uint32_t namelen){
    uint32_t off = 0;
    while(off + sizeof(ext2_dir_entry_t) <= s->block_size){
        const uint8_t *e = buf + off;
        const ext2_dir_entry_t *de = (const ext2_dir_entry_t*)e;
        if(de->rec_len < sizeof(ext2_dir_entry_t) ||
           off + de->rec_len > s->block_size)break;
        uint32_t nlen = ext2_dirent_namelen(s, e);
        if(de->inode && nlen == namelen && memcmp(e + sizeof(*de), name, nlen) == 0)
            return de->inode;
        off += de->rec_len;
    }
    return 0;
}

//在目录里按名字查找, 返回 inode 号(0 = 不存在)
static uint32_t ext2_find_entry(ext2_sb_info_t *s, struct inode *dir,
                                const char *name, uint32_t namelen){
    uint32_t nb = ext2_dir_nblocks(s, dir);
    for(uint32_t n = 0;n < nb;n ++){
        if(ext2_read_dir_block(s, dir, n))return 0;
        uint32_t ino = ext2_scan_entry(s, s->dir_buf, name, namelen);
        if(ino)return ino;
    }
    return 0;
}

//目录查找: 命中就建立 inode, 否则留负缓存
static struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry, unsigned flags){
    (void)flags;
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    uint32_t ino = ext2_find_entry(s, dir, dentry->d_name, dentry->d_name_len);
    if(!ino){
        d_add(dentry, NULL);
        return NULL;
    }
    struct inode *inode = ext2_iget(dir->i_sb, ino);
    if(!inode){
        d_add(dentry, NULL);
        return NULL;
    }
    d_add(dentry, inode);
    Iput(inode);//d_add 已经替 dentry 拿了一份引用
    return dentry;
}

//目录遍历: 只上报真实条目, "." 与 ".." 由 VFS 统一产出
static int ext2_iterate(struct file *f, void *ctx,
                        int (*emit)(void *ctx, const char *name, int namelen,
                                    uint64_t ino, int type, int64_t next_off)){
    if(!f || !f->f_inode || !emit)return -EINVAL;
    struct inode *dir = f->f_inode;
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    uint32_t nb = ext2_dir_nblocks(s, dir);
    int64_t off = 0;
    for(uint32_t n = 0;n < nb;n ++){
        if(ext2_read_dir_block(s, dir, n))return -EIO;
        uint32_t pos = 0;
        while(pos + sizeof(ext2_dir_entry_t) <= s->block_size){
            const uint8_t *e = s->dir_buf + pos;
            const ext2_dir_entry_t *de = (const ext2_dir_entry_t*)e;
            if(de->rec_len < sizeof(ext2_dir_entry_t) ||
               pos + de->rec_len > s->block_size)break;
            uint32_t nlen = ext2_dirent_namelen(s, e);
            const char *nm = (const char*)(e + sizeof(*de));
            //跳过空槽、"." 与 ".."
            if(de->inode && nlen &&
               !(nlen == 1 && nm[0] == '.') &&
               !(nlen == 2 && nm[0] == '.' && nm[1] == '.'))
                emit(ctx, nm, (int)nlen, de->inode, ext2_dirent_type(de->file_type), off ++);
            pos += de->rec_len;
        }
    }
    return 0;
}

//文件读取
static int64_t ext2_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    if(!f || !f->f_inode || !buf || !pos)return -EINVAL;
    struct inode *inode = f->f_inode;
    ext2_sb_info_t *s = EX2SB(inode->i_sb);
    ext2_inode_info_t *ii = EX2I(inode);
    uint64_t off   = *pos;
    uint64_t start = off;
    uint64_t left  = len;
    if(off >= inode->i_size)return 0;
    if(off + left > inode->i_size)left = inode->i_size - off;//不越过文件末尾
    while(left){
        uint32_t blkno = (uint32_t)(off / s->block_size);
        uint32_t inblk = (uint32_t)(off % s->block_size);
        uint32_t n     = s->block_size - inblk;
        if((uint64_t)n > left)n = (uint32_t)left;
        uint32_t blk = ext2_bmap(s, ii, blkno);
        if(!blk){
            memset(buf, 0, n);//空洞按 0 处理
        }else if(bdev_read(s->bdev, (uint64_t)blk * s->block_size + inblk, buf, n)){
            return -EIO;
        }
        buf  += n;
        off  += n;
        left -= n;
    }
    *pos = off;
    return (int64_t)(off - start);
}

//符号链接目标
static int ext2_get_link(struct dentry *dentry, struct inode *inode, const char **target){
    (void)dentry;
    ext2_inode_info_t *ii = inode ? EX2I(inode) : NULL;
    if(!ii)return -EINVAL;
    if(inode->i_size >= EXT2_LINK_MAX)return -ENAMETOOLONG;//太长的目标还没缓存
    *target = ii->i_link;
    return 0;
}

//操作表
static struct inode_operations g_ext2_iops = {
    .lookup   = ext2_lookup,
    .get_link = ext2_get_link,
};

static const struct file_operations g_ext2_file_fops = {
    .read = ext2_read,
};

static const struct file_operations g_ext2_dir_fops = {
    .iterate = ext2_iterate,
};

static void ext2_evict_inode(struct inode *inode){
    (void)inode;//只读阶段没有需要释放的私有资源
}

static const struct super_operations g_ext2_sop = {
    .evict_inode = ext2_evict_inode,
};

//挂载
uint32_t Ext2InodeSize(void){
    return (uint32_t)(sizeof(struct inode) + sizeof(ext2_inode_info_t));
}

const struct super_operations *Ext2SuperOps(void){ return &g_ext2_sop; }

/**
 * 读超级块并校验, 建立 ext2 的私有数据
 * @return 0 成功
 */
int Ext2ReadSuper(struct super_block *sb, bdev_t *bd){
    if(!sb || !bd)return -EINVAL;
    //Pmm_Malloc 给的是物理地址, 访问前要转成高半虚拟地址
    void *pa = Pmm_Malloc(1);
    if(!pa)return -ENOMEM;
    ext2_super_t *es = (ext2_super_t*)PHYS_TO_VIRT((uintptr_t)pa);
    //超级块固定在分区内偏移 1024
    if(bdev_read(bd, 1024, es, sizeof(*es))){
        Pmm_Free(pa, 1);
        return -EIO;
    }
    if(es->s_magic != EXT2_MAGIC){
        Pmm_Free(pa, 1);
        return -ENODEV;
    }
    ext2_sb_info_t *s = (ext2_sb_info_t*)KslabZalloc(g_ext2_info_cache);
    if(!s){
        Pmm_Free(pa, 1);
        return -ENOMEM;
    }
    s->bdev             = bd;
    s->block_size       = 1024u << es->s_log_block_size;
    s->ptrs             = s->block_size / 4;
    s->inode_size       = es->s_rev_level ? es->s_inode_size : 128;
    s->inodes_per_group = es->s_inodes_per_group;
    s->blocks_per_group = es->s_blocks_per_group;
    s->first_data_block = es->s_first_data_block;
    s->incompat         = es->s_feature_incompat;
    if(!s->inode_size)s->inode_size = 128;
    if(s->block_size < 1024 || s->block_size > 4096 ||
       !s->inodes_per_group || !s->blocks_per_group){
        Pmm_Free(pa, 1);
        KslabFree(g_ext2_info_cache, s);
        return -ENODEV;
    }
    //块组数, 以及组描述符表位置(跟着第一个数据块)
    s->nr_groups = (es->s_blocks_count - s->first_data_block + s->blocks_per_group - 1)
                   / s->blocks_per_group;
    s->desc_blk  = s->first_data_block + 1;
    Pmm_Free(pa, 1);
    //两个块缓冲: 一个给间接块/inode, 一个给目录扫描, 避免嵌套覆盖
    void *pm = Pmm_Malloc(1);
    void *pd = Pmm_Malloc(1);
    if(!pm || !pd){
        if(pm)Pmm_Free(pm, 1);
        if(pd)Pmm_Free(pd, 1);
        KslabFree(g_ext2_info_cache, s);
        return -ENOMEM;
    }
    s->meta_buf = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pm);
    s->dir_buf  = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pd);
    sb->s_fs_info   = s;
    sb->s_bdev      = bd;
    sb->s_blocksize = s->block_size;
    return 0;
}

/**
 * 建立 ext2 的超级块与根目录
 * @return 根 dentry(失败返回 NULL)
 */
struct dentry *Ext2FillSuper(struct super_block *sb){
    if(!sb)return NULL;
    struct inode *root = ext2_iget(sb, EXT2_ROOT_INO);
    if(!root)return NULL;
    if((root->i_mode & S_IFMT) != S_IFDIR){
        Iput(root);
        return NULL;
    }
    struct dentry *d = d_alloc(NULL, "/");
    if(!d){
        Iput(root);
        return NULL;
    }
    d_add(d, root);
    Iput(root);//dentry 持有引用
    sb->s_root = d;
    sb->s_root_inode = root;
    return d;
}

/**
 * 扫描块设备, 取第一个 ext2 分区
 * @return 找到的 bdev, 没有返回 NULL
 */
bdev_t *Ext2FindPartition(void){
    int n = BlkBdevCount();
    for(int i = 0;i < n;i ++){
        bdev_t *bd = BlkBdevAt(i);
        if(!bd || bd->bd_part_type == PART_WHOLE)continue;
        //分区表类型要是 Linux 分区, 且探测出来确实是 ext2
        if(strncmp(bd->bd_part_name, "Linux ", 6) != 0)continue;
        if(strcmp(bd->bd_fs_type, "ext2") != 0)continue;
        return bd;
    }
    return NULL;
}

/**
 * 把找到的 ext2 分区挂到 parent 的 mountpoint 上
 * @return 0 成功
 */
int Ext2MountAt(struct vfsmount *parent, struct dentry *mountpoint){
    bdev_t *bd = Ext2FindPartition();
    if(!bd)return -ENODEV;
    if(!g_ext2_info_cache){
        g_ext2_info_cache = KslabCreate("ext2_sb", sizeof(ext2_sb_info_t));
        if(!g_ext2_info_cache)return -ENOMEM;
    }
    struct super_block *sb = SuperAlloc("ext2", EXT2_MAGIC, Ext2InodeSize(), Ext2SuperOps());
    if(!sb)return -ENOMEM;
    //先把超级块读出来并建好根目录, 失败了就当没挂过
    int r = Ext2ReadSuper(sb, bd);
    if(r){
        SuperFree(sb);
        return r;
    }
    struct dentry *root = Ext2FillSuper(sb);
    if(!root){
        SuperFree(sb);
        return -ENOMEM;
    }
    //挂成一棵独立挂载树, 再接到 mountpoint 下面
    struct vfsmount *m = VfsKernMount(sb, root, 0);
    if(!m){
        SuperFree(sb);
        return -ENOMEM;
    }
    r = VfsAttachMount(parent, mountpoint, m);
    if(r)VfsDetachMount(m);
    return r;
}
