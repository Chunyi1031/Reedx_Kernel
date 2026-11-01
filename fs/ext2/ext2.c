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
#include <drives/bcache.h>
#include <mm/kslab.h>
#include <mm/pagecache.h>
#include <irq.h>
#include <drives/timer.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <rtc.h>

#define EXT2_MAGIC       0xEF53u
#define EXT2_ROOT_INO    2u

#define EXT2_INCOMPAT_FILETYPE 0x0002u
#define EXT2_COMPAT_DIR_INDEX  0x0020u

#define EXT2_NIDIRECT    12      //i_block 前 12 个是直接块
#define EXT2_BLOCKS      15      //i_block 总共 15 个块号
#define EXT2_LINK_MAX    256     //符号链接目标的最大缓存长度
#define EXT2_READ_RUN    128     //一次连续读最多合并块数

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

//一块位图的缓存(全局只用两块: 块位图 / inode 位图)
typedef struct ext2_bmp {
    uint8_t *buf;      //块内容
    uint32_t blk;      //缓存的是哪个块(0=没有)
    _Bool    dirty;    //改过待回写
} ext2_bmp_t;

//文件系统私有数据
typedef struct ext2_sb_info {
    bdev_t  *bdev;              //ext2 所在的分区块设备
    uint32_t block_size;        //文件系统块大小(1024/2048/4096)
    uint32_t ptrs;              //一个块能放多少个块号
    uint32_t inode_size;        //inode 结构大小(EXT2 标准 128)
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t first_data_block;  //第一个数据块号(1K 块时为 1)
    uint32_t first_ino;         //第一个可分配的 inode 号
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t nr_groups;
    uint32_t desc_blk;          //块组描述符表起始块号
    uint32_t compat;            //compat 特性字
    uint32_t incompat;          //incompat 特性字
    uint32_t blocks_count;      //总块数
    uint32_t inodes_count;      //总 inode 数
    uint8_t *meta_buf;          //间接块临时缓冲
    uint32_t meta_blk;          //meta_buf 里现在缓存的是哪个块(0=没有)
    uint8_t *blk_buf;           //目录块扫描与局部块写入共用
    ext2_bmp_t bbmp;            //块位图缓存
    ext2_bmp_t ibmp;            //inode 位图缓存
    uint8_t *itab_buf;          //inode 表块缓存
    uint32_t itab_blk;          //itab_buf 里是哪个块(0=没有)
    ext2_group_desc_t *gdt;     //块组描述符表的常驻副本
    _Bool gdt_dirty;            //gdt 有改动待回写
    _Bool sb_dirty;             //超级块空闲计数有改动待回写
    uint64_t last_sync;         //上次把脏页刷盘时的tick
} ext2_sb_info_t;

#define EX2SB(sb)  ((ext2_sb_info_t*)(sb)->s_fs_info)
#define EX2I(ino)  ((ext2_inode_info_t*)(ino)->i_private)

//inode 私有数据
typedef struct ext2_inode_info {
    uint32_t i_block[EXT2_BLOCKS];
    uint32_t i_dir_hint;
    char     i_link[EXT2_LINK_MAX];//符号链接目标(NUL 结尾), iget 时就准备好
} ext2_inode_info_t;

static struct kmem_cache *g_ext2_info_cache;

static struct inode_operations g_ext2_iops;
static const struct file_operations g_ext2_file_fops;
static const struct file_operations g_ext2_dir_fops;

//==================== 块读取 ====================

//按字节从块缓存读
static int ext2_bread(ext2_sb_info_t *s, uint64_t off, void *buf, uint32_t len){
    uint8_t *p = (uint8_t*)buf;
    while(len){
        uint64_t blk = off / 512;
        uint32_t inb = (uint32_t)(off % 512);
        uint32_t n   = 512 - inb;
        if(n > len)n = len;
        buffer_head_t bh;
        if(sb_bread(s->bdev, blk, &bh))return -EIO;
        memcpy(p, bh.b_data + inb, n);
        sb_brelse(&bh);
        p += n; off += n; len -= n;
    }
    return 0;
}

//按字节写进块缓存
static int ext2_bwrite(ext2_sb_info_t *s, uint64_t off, const void *buf, uint32_t len){
    const uint8_t *p = (const uint8_t*)buf;
    while(len){
        uint64_t blk = off / 512;
        uint32_t inb = (uint32_t)(off % 512);
        uint32_t n   = 512 - inb;
        if(n > len)n = len;
        buffer_head_t bh;
        if(sb_bread(s->bdev, blk, &bh))return -EIO;
        memcpy(bh.b_data + inb, p, n);
        sb_mark_dirty(&bh);
        sb_brelse(&bh);
        p += n; off += n; len -= n;
    }
    return 0;
}

//读一个文件系统块
static int ext2_read_block(ext2_sb_info_t *s, uint32_t blk, void *buf){
    if(!blk)return -EIO;
    return ext2_bread(s, (uint64_t)blk * s->block_size, buf, s->block_size);
}

//取间接块: 同一块只用读一次, 顺序读文件时省掉大量重复 I/O
static uint8_t *ext2_meta(ext2_sb_info_t *s, uint32_t blk){
    if(!blk)return NULL;
    if(s->meta_blk == blk)return s->meta_buf;
    if(ext2_read_block(s, blk, s->meta_buf))return NULL;
    s->meta_blk = blk;
    return s->meta_buf;
}

//记下 meta_buf 现在装的是哪个块(写完回磁盘后调用)
static void ext2_meta_set(ext2_sb_info_t *s, uint32_t blk){
    s->meta_blk = blk;
}

//读第 group 个块组描述符(走内存副本)
static int ext2_read_group_desc(ext2_sb_info_t *s, uint32_t group, ext2_group_desc_t *out){
    if(s->gdt && group < s->nr_groups){
        *out = s->gdt[group];
        return 0;
    }
    uint64_t off = (uint64_t)s->desc_blk * s->block_size + (uint64_t)group * sizeof(*out);
    if(ext2_bread(s, off, out, sizeof(*out)))return -EIO;
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
    //inode 表按块缓存: 同一目录里的 inode 往往连续, 能省掉大量重复读
    uint64_t byte = (uint64_t)index * s->inode_size;
    uint32_t blk  = (uint32_t)(gd.bg_inode_table + byte / s->block_size);
    uint32_t off  = (uint32_t)(byte % s->block_size);
    if(off + sizeof(*out) > s->block_size)return -EIO;
    if(s->itab_blk != blk){
        if(ext2_read_block(s, blk, s->itab_buf))return -EIO;
        s->itab_blk = blk;
    }
    memcpy(out, s->itab_buf + off, sizeof(*out));
    return 0;
}

//把文件内的逻辑块号映射成磁盘块号, 返回 0 表示空洞
static uint32_t ext2_bmap(ext2_sb_info_t *s, ext2_inode_info_t *ii, uint32_t blkno){
    uint32_t ptrs = s->ptrs;
    if(blkno < EXT2_NIDIRECT)return ii->i_block[blkno];
    blkno -= EXT2_NIDIRECT;
    //一级间接
    if(blkno < ptrs){
        uint8_t *m = ext2_meta(s, ii->i_block[12]);
        return m ? ((uint32_t*)m)[blkno] : 0;
    }
    blkno -= ptrs;
    //二级间接
    if(blkno < ptrs * ptrs){
        uint8_t *m = ext2_meta(s, ii->i_block[13]);
        if(!m)return 0;
        uint32_t blk = ((uint32_t*)m)[blkno / ptrs];
        if(!blk)return 0;
        m = ext2_meta(s, blk);
        return m ? ((uint32_t*)m)[blkno % ptrs] : 0;
    }
    blkno -= ptrs * ptrs;
    //三级间接
    if(blkno < ptrs * ptrs * ptrs){
        uint8_t *m = ext2_meta(s, ii->i_block[14]);
        if(!m)return 0;
        uint32_t blk = ((uint32_t*)m)[blkno / (ptrs * ptrs)];
        if(!blk)return 0;
        m = ext2_meta(s, blk);
        if(!m)return 0;
        blk = ((uint32_t*)m)[(blkno / ptrs) % ptrs];
        if(!blk)return 0;
        m = ext2_meta(s, blk);
        return m ? ((uint32_t*)m)[blkno % ptrs] : 0;
    }
    return 0;
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
            if(blk && ext2_bread(s, (uint64_t)blk * s->block_size, ii->i_link, di.i_size) == 0){
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

//把目录的第 n 个逻辑块读进 blk_buf
static int ext2_read_dir_block(ext2_sb_info_t *s, struct inode *dir, uint32_t n){
    uint32_t blk = ext2_bmap(s, EX2I(dir), n);
    if(!blk)return -EIO;
    return ext2_read_block(s, blk, s->blk_buf);
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
    if(!nb)return 0;
    //从上次命中的块开始扫, 同目录里相邻的名字(导入探路径)常常挨在一起
    ext2_inode_info_t *ii = EX2I(dir);
    uint32_t start = ii ? ii->i_dir_hint : 0;
    if(start >= nb)start = 0;
    for(uint32_t k = 0;k < nb;k ++){
        uint32_t n = (start + k) % nb;
        if(ext2_read_dir_block(s, dir, n)){
            printk(PRINTK_ERR"ext2: read dir block %u(ino=%llu) failed.", n, (unsigned long long)dir->i_ino);
            return 0;
        }
        uint32_t ino = ext2_scan_entry(s, s->blk_buf, name, namelen);
        if(ino){
            if(ii)ii->i_dir_hint = n;
            return ino;
        }
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
            const uint8_t *e = s->blk_buf + pos;
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
static uint64_t ext2_read_direct(struct inode *inode, uint64_t off, void *buf, uint64_t len){
    ext2_sb_info_t *s = EX2SB(inode->i_sb);
    ext2_inode_info_t *ii = EX2I(inode);
    uint8_t *dst = (uint8_t*)buf;
    uint64_t done = 0;
    while(done < len){
        uint32_t blkno = (uint32_t)(off / s->block_size);
        uint32_t inblk = (uint32_t)(off % s->block_size);
        //整块对齐时把磁盘上连续的块合成一次读
        if(inblk == 0 && PageCacheReady()){
            uint32_t first = ext2_bmap(s, ii, blkno);
            if(first){
                uint32_t maxb = (uint32_t)((len - done + s->block_size - 1) / s->block_size);
                uint32_t run  = 1;
                while(run < maxb && run < EXT2_READ_RUN &&
                      ext2_bmap(s, ii, blkno + run) == first + run)run ++;
                uint32_t bytes = run * s->block_size;
                if((uint64_t)bytes > len - done)bytes = (uint32_t)(len - done);
                if(ext2_bread(s, (uint64_t)first * s->block_size, dst + done, bytes))
                    break;
                done += bytes;
                off  += bytes;
                continue;
            }
        }
        uint32_t n = s->block_size - inblk;
        if((uint64_t)n > len - done)n = (uint32_t)(len - done);
        uint32_t blk = ext2_bmap(s, ii, blkno);
        if(!blk){
            memset(dst + done, 0, n);//空洞按0处理
        }else if(ext2_bread(s, (uint64_t)blk * s->block_size + inblk, dst + done, n)){
            break;
        }
        done += n;
        off  += n;
    }
    return done;
}

//页缓存未命中时的填充回调
static uint64_t ext2_pcache_fill(void *ctx, uint64_t off, void *buf, uint64_t len){
    return ext2_read_direct((struct inode*)ctx, off, buf, len);
}

static int64_t ext2_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    if(!f || !f->f_inode || !buf || !pos)return -EINVAL;
    struct inode *inode = f->f_inode;
    uint64_t off = *pos;
    if(off >= inode->i_size)return 0;
    uint64_t want = len;
    if(want > inode->i_size - off)want = inode->i_size - off;//不越过文件末尾
    //先走页缓存
    if((inode->i_mode & S_IFMT) == S_IFREG && PageCacheReady()){
        uint64_t done = PageCacheRead(inode->i_ino, off, buf, want, inode->i_size, ext2_pcache_fill, inode);
        if(done == want){
            *pos = off + done;
            return (int64_t)done;
        }
        //缓存没能覆盖全部, 剩下的直接读
        done += ext2_read_direct(inode, off + done, buf + done, want - done);
        *pos = off + done;
        return (int64_t)done;
    }
    uint64_t done = ext2_read_direct(inode, off, buf, want);
    *pos = off + done;
    return (int64_t)done;
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

//==================== 元数据回写 ====================

//写一个文件系统块
static int ext2_write_block(ext2_sb_info_t *s, uint32_t blk, const void *buf){
    if(!blk)return -EIO;
    return ext2_bwrite(s, (uint64_t)blk * s->block_size, buf, s->block_size);
}

//回写块组描述符(只改内存副本, 真正落盘交给 ext2_commit)
static int ext2_write_group_desc(ext2_sb_info_t *s, uint32_t group, const ext2_group_desc_t *gd){
    if(s->gdt && group < s->nr_groups){
        s->gdt[group] = *gd;
        s->gdt_dirty = true;
        return 0;
    }
    uint64_t off = (uint64_t)s->desc_blk * s->block_size + (uint64_t)group * sizeof(*gd);
    if(ext2_bwrite(s, off, gd, sizeof(*gd)))return -EIO;
    return 0;
}

//把攒下的元数据(位图 + 块组描述符 + 超级块计数)一次性写回
static void ext2_commit(ext2_sb_info_t *s){
    if(!s)return;
    //位图先落盘: 它记录的是"哪些块被占了", 迟写只会多占不会丢数据
    if(s->bbmp.dirty && s->bbmp.blk){
        if(ext2_write_block(s, s->bbmp.blk, s->bbmp.buf) == 0)s->bbmp.dirty = false;
    }
    if(s->ibmp.dirty && s->ibmp.blk){
        if(ext2_write_block(s, s->ibmp.blk, s->ibmp.buf) == 0)s->ibmp.dirty = false;
    }
    if(s->sb_dirty){
        ext2_super_t sb;
        if(ext2_bread(s, 1024, &sb, sizeof(sb)) == 0){
            sb.s_free_blocks_count = s->free_blocks_count;
            sb.s_free_inodes_count = s->free_inodes_count;
            if(ext2_bwrite(s, 1024, &sb, sizeof(sb)) == 0)s->sb_dirty = false;
        }
    }
    if(s->gdt_dirty && s->gdt){
        uint32_t bytes = s->nr_groups * (uint32_t)sizeof(ext2_group_desc_t);
        if(ext2_bwrite(s, (uint64_t)s->desc_blk * s->block_size, s->gdt, bytes) == 0)
            s->gdt_dirty = false;
    }
    //脏页延迟回写: 每秒最多刷一次盘, 否则每写一次就整盘刷缓存, 写密集的操作
    //(比如 Python 生成 .pyc)会被拖垮。真正卸载时会强制刷(见 ext2_sync_fs)
    uint64_t now = SYSTEM_TimerTicks;
    if(s->last_sync && now - s->last_sync < OS_TICK_HZ)return;
    s->last_sync = now;
    BCacheSyncBdev(s->bdev);
}

//回写超级块里的空闲计数(延迟到 ext2_commit 统一做)
static int ext2_sync_super(ext2_sb_info_t *s){
    s->sb_dirty = true;
    return 0;
}

//inode 表里 ino 对应的字节偏移
static uint64_t ext2_inode_offset(ext2_sb_info_t *s, uint32_t ino, ext2_group_desc_t *gd_out){
    uint32_t group = (ino - 1) / s->inodes_per_group;
    uint32_t index = (ino - 1) % s->inodes_per_group;
    ext2_group_desc_t gd;
    if(ext2_read_group_desc(s, group, &gd))return 0;
    if(gd_out)*gd_out = gd;
    return (uint64_t)gd.bg_inode_table * s->block_size + (uint64_t)index * s->inode_size;
}

//把 VFS inode 组装成磁盘结构写回去
static int ext2_write_dinode(struct inode *inode){
    if(!inode || !inode->i_sb)return -EINVAL;
    ext2_sb_info_t *s = EX2SB(inode->i_sb);
    ext2_inode_info_t *ii = EX2I(inode);
    uint32_t ino = (uint32_t)inode->i_ino;
    if(!ii || !ino)return -EINVAL;
    uint64_t off = ext2_inode_offset(s, ino, NULL);
    if(!off)return -EIO;
    //先读出磁盘上原来的 inode, 只改我们维护的字段
    ext2_dinode_t di;
    if(ext2_bread(s, off, &di, sizeof(di)))return -EIO;
    di.i_mode        = inode->i_mode;
    di.i_uid         = (uint16_t)inode->i_uid;
    di.i_gid         = (uint16_t)inode->i_gid;
    di.i_size        = (uint32_t)inode->i_size;
    di.i_links_count = (uint16_t)inode->i_nlink;
    di.i_blocks      = (uint32_t)inode->i_blocks;
    di.i_atime       = (uint32_t)inode->i_atime_s;
    di.i_mtime       = (uint32_t)inode->i_mtime_s;
    di.i_ctime       = (uint32_t)inode->i_ctime_s;
    for(int k = 0;k < EXT2_BLOCKS;k ++)di.i_block[k] = ii->i_block[k];
    if(ext2_bwrite(s, off, &di, sizeof(di)))return -EIO;
    s->itab_blk = 0;//inode 表块改过了, 缓存作废
    return 0;
}

//==================== 位图与块分配 ====================

//在位图块里找第一个 0 位并置位, 成功返回位号
static int ext2_bitmap_alloc(ext2_sb_info_t *s, ext2_bmp_t *bmp, uint32_t bitmap_blk,
                             uint32_t max_bits, uint32_t *out){
    if(!bitmap_blk || !max_bits)return -EIO;
    if(bmp->blk != bitmap_blk){
        if(ext2_read_block(s, bitmap_blk, bmp->buf))return -EIO;
        bmp->blk   = bitmap_blk;
        bmp->dirty = false;
    }
    for(uint32_t i = 0;i < max_bits;i ++){
        if(!(bmp->buf[i >> 3] & (uint8_t)(1u << (i & 7)))){
            bmp->buf[i >> 3] |= (uint8_t)(1u << (i & 7));
            bmp->dirty = true;
            *out = i;
            return 0;
        }
    }
    return -ENOSPC;
}

//把位图里某一位清 0
static int ext2_bitmap_free(ext2_sb_info_t *s, ext2_bmp_t *bmp, uint32_t bitmap_blk, uint32_t bit){
    if(!bitmap_blk)return -EIO;
    if(bmp->blk != bitmap_blk){
        if(ext2_read_block(s, bitmap_blk, bmp->buf))return -EIO;
        bmp->blk   = bitmap_blk;
        bmp->dirty = false;
    }
    bmp->buf[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    bmp->dirty = true;
    return 0;
}

//分配一个数据块, 从 goal 所在块组开始找; zero=1 时先把块清零
static uint32_t ext2_alloc_block(ext2_sb_info_t *s, uint32_t goal, int zero){
    if(!s->free_blocks_count)return 0;
    uint32_t start = 0;
    if(goal > s->first_data_block)
        start = (goal - s->first_data_block) / s->blocks_per_group;
    if(start >= s->nr_groups)start = 0;
    for(uint32_t i = 0;i < s->nr_groups;i ++){
        uint32_t group = (start + i) % s->nr_groups;
        ext2_group_desc_t gd;
        if(ext2_read_group_desc(s, group, &gd) || !gd.bg_free_blocks_count)continue;
        //最后一块组可能不满, 位图里超出总块数的位不能用
        uint32_t base = s->first_data_block + group * s->blocks_per_group;
        uint32_t maxb = s->blocks_count - base;
        if(maxb > s->blocks_per_group)maxb = s->blocks_per_group;
        uint32_t bit;
        if(ext2_bitmap_alloc(s, &s->bbmp, gd.bg_block_bitmap, maxb, &bit))continue;
        //同步组描述符与超级块的空闲计数
        gd.bg_free_blocks_count --;
        ext2_write_group_desc(s, group, &gd);
        s->free_blocks_count --;
        ext2_sync_super(s);
        uint32_t blk = base + bit;
        //需要清零时把新块写一遍 0, 避免把别的文件的旧数据漏出去
        if(zero){
            memset(s->blk_buf, 0, s->block_size);
            if(ext2_write_block(s, blk, s->blk_buf))return 0;
        }
        return blk;
    }
    return 0;
}

//释放一个数据块
static int ext2_free_block(ext2_sb_info_t *s, uint32_t blk){
    if(blk <= s->first_data_block)return 0;//0 号块与引导块不归位图管
    uint32_t rel = blk - s->first_data_block;
    uint32_t group = rel / s->blocks_per_group;
    uint32_t bit   = rel % s->blocks_per_group;
    if(group >= s->nr_groups)return -EINVAL;
    ext2_group_desc_t gd;
    if(ext2_read_group_desc(s, group, &gd))return -EIO;
    if(ext2_bitmap_free(s, &s->bbmp, gd.bg_block_bitmap, bit))return -EIO;
    gd.bg_free_blocks_count ++;
    ext2_write_group_desc(s, group, &gd);
    s->free_blocks_count ++;
    ext2_sync_super(s);
    return 0;
}

//分配一个 inode, 并把 inode 表里的那一项初始化好
static uint32_t ext2_alloc_inode(struct super_block *sb, uint16_t mode, int isdir){
    ext2_sb_info_t *s = EX2SB(sb);
    if(!s->free_inodes_count)return 0;
    for(uint32_t group = 0;group < s->nr_groups;group ++){
        ext2_group_desc_t gd;
        if(ext2_read_group_desc(s, group, &gd) || !gd.bg_free_inodes_count)continue;
        //最后一个块组可能不满, 超出 inode 总数的位不能用
        uint32_t maxb = s->inodes_count - group * s->inodes_per_group;
        if(maxb > s->inodes_per_group)maxb = s->inodes_per_group;
        uint32_t bit;
        if(ext2_bitmap_alloc(s, &s->ibmp, gd.bg_inode_bitmap, maxb, &bit))continue;
        uint32_t ino = group * s->inodes_per_group + bit + 1;
        gd.bg_free_inodes_count --;
        if(isdir)gd.bg_used_dirs_count ++;
        ext2_write_group_desc(s, group, &gd);
        s->free_inodes_count --;
        ext2_sync_super(s);
        //在 inode 表里写一个空 inode
        ext2_dinode_t di;
        memset(&di, 0, sizeof(di));
        uint64_t now = rtc_get_epoch();
        di.i_mode        = mode;
        di.i_uid         = (uint16_t)CurrentCred()->euid;
        di.i_gid         = (uint16_t)CurrentCred()->egid;
        di.i_links_count = 1;
        di.i_size        = 0;
        di.i_atime       = (uint32_t)now;
        di.i_mtime       = (uint32_t)now;
        di.i_ctime       = (uint32_t)now;
        uint64_t off = ext2_inode_offset(s, ino, NULL);
        if(!off || ext2_bwrite(s, off, &di, sizeof(di)))return 0;
        return ino;
    }
    return 0;
}

//释放一个 inode(位图清位 + 组计数)
static int ext2_free_inode(struct inode *inode){
    ext2_sb_info_t *s = EX2SB(inode->i_sb);
    uint32_t ino = (uint32_t)inode->i_ino;
    if(!ino)return -EINVAL;
    uint32_t group = (ino - 1) / s->inodes_per_group;
    uint32_t bit   = (ino - 1) % s->inodes_per_group;
    ext2_group_desc_t gd;
    if(ext2_read_group_desc(s, group, &gd))return -EIO;
    //先把 inode 清成"已删除"的样子, fsck 靠 dtime 判断它确实是被删的
    uint64_t off = ext2_inode_offset(s, ino, NULL);
    if(off){
        ext2_dinode_t di;
        if(ext2_bread(s, off, &di, sizeof(di)) == 0){
            di.i_dtime       = (uint32_t)rtc_get_epoch();
            di.i_links_count = 0;
            di.i_size        = 0;
            di.i_blocks      = 0;
            for(int k = 0;k < EXT2_BLOCKS;k ++)di.i_block[k] = 0;
            ext2_bwrite(s, off, &di, sizeof(di));
        }
    }
    if(ext2_bitmap_free(s, &s->ibmp, gd.bg_inode_bitmap, bit))return -EIO;
    gd.bg_free_inodes_count ++;
    if((inode->i_mode & S_IFMT) == S_IFDIR && gd.bg_used_dirs_count)gd.bg_used_dirs_count --;
    ext2_write_group_desc(s, group, &gd);
    s->free_inodes_count ++;
    ext2_sync_super(s);
    return 0;
}

//把逻辑块号映射成磁盘块号, 缺块就分配; zero 控制新块是否要先清零
static uint32_t ext2_bmap_alloc(struct inode *inode, uint32_t blkno, int zero){
    ext2_sb_info_t *s = EX2SB(inode->i_sb);
    ext2_inode_info_t *ii = EX2I(inode);
    uint32_t ptrs = s->ptrs;
    uint32_t per  = s->block_size / 512;//一个数据块算几块 512 字节
    if(blkno < EXT2_NIDIRECT){
        if(!ii->i_block[blkno]){
            uint32_t blk = ext2_alloc_block(s, ii->i_block[0] ? ii->i_block[blkno - 1] + 1 : 0, zero);
            if(!blk)return 0;
            ii->i_block[blkno] = blk;
            inode->i_blocks += per;
        }
        return ii->i_block[blkno];
    }
    blkno -= EXT2_NIDIRECT;
    //一级间接
    if(blkno < ptrs){
        if(!ii->i_block[12]){
            uint32_t blk = ext2_alloc_block(s, 0, 1);//间接块必须先清零
            if(!blk)return 0;
            ii->i_block[12] = blk;
            inode->i_blocks += per;
        }
        if(ext2_read_block(s, ii->i_block[12], s->meta_buf))return 0;
        uint32_t *ib = (uint32_t*)s->meta_buf;
        if(!ib[blkno]){
            uint32_t blk = ext2_alloc_block(s, 0, zero);
            if(!blk)return 0;
            ib[blkno] = blk;
            if(ext2_write_block(s, ii->i_block[12], s->meta_buf))return 0;
            ext2_meta_set(s, ii->i_block[12]);
            inode->i_blocks += per;
        }
        return ib[blkno];
    }
    blkno -= ptrs;
    //二级间接
    if(blkno < ptrs * ptrs){
        if(!ii->i_block[13]){
            uint32_t blk = ext2_alloc_block(s, 0, 1);//间接块必须先清零
            if(!blk)return 0;
            ii->i_block[13] = blk;
            inode->i_blocks += per;
        }
        if(ext2_read_block(s, ii->i_block[13], s->meta_buf))return 0;
        uint32_t *l1 = (uint32_t*)s->meta_buf;
        uint32_t i1 = blkno / ptrs, i2 = blkno % ptrs;
        if(!l1[i1]){
            uint32_t blk = ext2_alloc_block(s, 0, 1);//中间层间接块也要清零
            if(!blk)return 0;
            l1[i1] = blk;
            if(ext2_write_block(s, ii->i_block[13], s->meta_buf))return 0;
            ext2_meta_set(s, ii->i_block[13]);
            inode->i_blocks += per;
        }
        uint32_t mid = l1[i1];
        if(ext2_read_block(s, mid, s->meta_buf))return 0;
        uint32_t *l2 = (uint32_t*)s->meta_buf;
        if(!l2[i2]){
            uint32_t blk = ext2_alloc_block(s, 0, zero);
            if(!blk)return 0;
            l2[i2] = blk;
            if(ext2_write_block(s, mid, s->meta_buf))return 0;
            ext2_meta_set(s, mid);
            inode->i_blocks += per;
        }
        return l2[i2];
    }
    blkno -= ptrs * ptrs;
    //三级间接
    if(blkno < ptrs * ptrs * ptrs){
        if(!ii->i_block[14]){
            uint32_t blk = ext2_alloc_block(s, 0, 1);//间接块必须先清零
            if(!blk)return 0;
            ii->i_block[14] = blk;
            inode->i_blocks += per;
        }
        if(ext2_read_block(s, ii->i_block[14], s->meta_buf))return 0;
        uint32_t *l1 = (uint32_t*)s->meta_buf;
        uint32_t i1 = blkno / (ptrs * ptrs);
        if(!l1[i1]){
            uint32_t blk = ext2_alloc_block(s, 0, 1);//中间层间接块也要清零
            if(!blk)return 0;
            l1[i1] = blk;
            if(ext2_write_block(s, ii->i_block[14], s->meta_buf))return 0;
            ext2_meta_set(s, ii->i_block[14]);
            inode->i_blocks += per;
        }
        uint32_t mid1 = l1[i1];
        if(ext2_read_block(s, mid1, s->meta_buf))return 0;
        uint32_t *l2 = (uint32_t*)s->meta_buf;
        uint32_t i2 = (blkno / ptrs) % ptrs;
        if(!l2[i2]){
            uint32_t blk = ext2_alloc_block(s, 0, 1);//再下一层间接块也要清零
            if(!blk)return 0;
            l2[i2] = blk;
            if(ext2_write_block(s, mid1, s->meta_buf))return 0;
            ext2_meta_set(s, mid1);
            inode->i_blocks += per;
        }
        uint32_t mid2 = l2[i2];
        if(ext2_read_block(s, mid2, s->meta_buf))return 0;
        uint32_t *l3 = (uint32_t*)s->meta_buf;
        uint32_t i3 = blkno % ptrs;
        if(!l3[i3]){
            uint32_t blk = ext2_alloc_block(s, 0, zero);
            if(!blk)return 0;
            l3[i3] = blk;
            if(ext2_write_block(s, mid2, s->meta_buf))return 0;
            ext2_meta_set(s, mid2);
            inode->i_blocks += per;
        }
        return l3[i3];
    }
    return 0;
}

//==================== 目录项增删 ====================

//目录项实际占用长度(4 字节对齐)
static uint32_t ext2_dirent_len(uint32_t namelen){
    return (8 + namelen + 3) & ~3u;
}

//在一块目录数据里插入条目, 放不下返回 -ENOSPC
static int ext2_block_add_entry(ext2_sb_info_t *s, uint8_t *blk, const char *name,
                                uint32_t namelen, uint32_t ino, uint8_t ftype){
    uint32_t need = ext2_dirent_len(namelen);
    uint32_t pos  = 0;
    while(pos + sizeof(ext2_dir_entry_t) <= s->block_size){
        ext2_dir_entry_t *de = (ext2_dir_entry_t*)(blk + pos);
        if(de->rec_len < sizeof(ext2_dir_entry_t) ||
           pos + de->rec_len > s->block_size)return -EIO;
        uint32_t used = de->inode ? ext2_dirent_len(ext2_dirent_namelen(s, blk + pos)) : 0;
        uint32_t room = de->rec_len - used;
        if(room >= need){
            if(de->inode){
                //已有条目: 把剩余空间拆成新条目
                de->rec_len = (uint16_t)used;
                ext2_dir_entry_t *nd = (ext2_dir_entry_t*)(blk + pos + used);
                nd->inode     = ino;
                nd->rec_len   = (uint16_t)room;
                nd->name_len  = (uint8_t)namelen;
                nd->file_type = ftype;
                memcpy((uint8_t*)nd + sizeof(*nd), name, namelen);
            }else{
                //整个空闲槽都归新条目
                de->inode     = ino;
                de->name_len  = (uint8_t)namelen;
                de->file_type = ftype;
                memcpy((uint8_t*)de + sizeof(*de), name, namelen);
            }
            return 0;
        }
        pos += de->rec_len;
    }
    return -ENOSPC;
}

//往目录里加一个条目, 必要时给目录追加一块
static int ext2_add_entry(struct inode *dir, const char *name, uint32_t namelen,
                          uint32_t ino, uint8_t ftype){
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    if(!namelen || namelen > NAMELEN_MAX)return -ENAMETOOLONG;
    uint32_t nb = ext2_dir_nblocks(s, dir);
    //已经有多个块且允许目录索引时, 0 号块前面挂着 dx_root, 不能往里插
    _Bool indexed = (s->compat & EXT2_COMPAT_DIR_INDEX) && nb > 1;
    for(uint32_t n = indexed ? 1 : 0;n < nb;n ++){
        if(ext2_read_dir_block(s, dir, n))return -EIO;
        if(ext2_block_add_entry(s, s->blk_buf, name, namelen, ino, ftype) == 0){
            uint32_t blk = ext2_bmap(s, EX2I(dir), n);
            int r = ext2_write_block(s, blk, s->blk_buf);
            if(r)return r;
            return ext2_write_dinode(dir);
        }
    }
    //都放不下: 给目录追加一块
    uint32_t blkno = ext2_bmap_alloc(dir, nb, 0);//整块会被目录项填满
    if(!blkno)return -ENOSPC;
    memset(s->blk_buf, 0, s->block_size);
    ext2_dir_entry_t *de = (ext2_dir_entry_t*)s->blk_buf;
    de->inode     = ino;
    de->rec_len   = (uint16_t)s->block_size;
    de->name_len  = (uint8_t)namelen;
    de->file_type = ftype;
    memcpy(s->blk_buf + sizeof(*de), name, namelen);
    int r = ext2_write_block(s, blkno, s->blk_buf);
    if(r)return r;
    dir->i_size += s->block_size;
    return ext2_write_dinode(dir);
}

//从目录里删掉一个条目, 把空间并进前一个条目
static int ext2_remove_entry(struct inode *dir, const char *name, uint32_t namelen){
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    uint32_t nb = ext2_dir_nblocks(s, dir);
    for(uint32_t n = 0;n < nb;n ++){
        if(ext2_read_dir_block(s, dir, n))return -EIO;
        uint32_t pos = 0, prev_pos = 0;
        _Bool have_prev = false;
        while(pos + sizeof(ext2_dir_entry_t) <= s->block_size){
            ext2_dir_entry_t *de = (ext2_dir_entry_t*)(s->blk_buf + pos);
            if(de->rec_len < sizeof(ext2_dir_entry_t) ||
               pos + de->rec_len > s->block_size)break;
            uint32_t nlen = ext2_dirent_namelen(s, s->blk_buf + pos);
            if(de->inode && nlen == namelen &&
               memcmp(s->blk_buf + pos + sizeof(*de), name, nlen) == 0){
                //删掉: 有前一项就并进去, 否则把这槽标成空闲
                if(have_prev){
                    ext2_dir_entry_t *pv = (ext2_dir_entry_t*)(s->blk_buf + prev_pos);
                    pv->rec_len = (uint16_t)(pv->rec_len + de->rec_len);
                }else{
                    de->inode = 0;
                }
                uint32_t blk = ext2_bmap(s, EX2I(dir), n);
                return ext2_write_block(s, blk, s->blk_buf);
            }
            prev_pos = pos;
            have_prev = (de->inode != 0);
            pos += de->rec_len;
        }
    }
    return -ENOENT;
}

//inode 类型对应的目录项 file_type
static uint8_t ext2_ftype_of(struct inode *inode){
    switch(inode->i_mode & S_IFMT){
    case S_IFDIR: return 2;
    case S_IFLNK: return 7;
    default:      return 1;
    }
}

//把目录里某个条目的 inode 号就地换成另一个(改名覆盖目标时用)
static int ext2_set_entry(struct inode *dir, const char *name, uint32_t namelen,
                          uint32_t ino, uint8_t ftype){
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    uint32_t nb = ext2_dir_nblocks(s, dir);
    for(uint32_t n = 0;n < nb;n ++){
        if(ext2_read_dir_block(s, dir, n))return -EIO;
        uint32_t pos = 0;
        while(pos + sizeof(ext2_dir_entry_t) <= s->block_size){
            ext2_dir_entry_t *de = (ext2_dir_entry_t*)(s->blk_buf + pos);
            if(de->rec_len < sizeof(ext2_dir_entry_t) ||
               pos + de->rec_len > s->block_size)break;
            uint32_t nlen = ext2_dirent_namelen(s, s->blk_buf + pos);
            if(de->inode && nlen == namelen &&
               memcmp(s->blk_buf + pos + sizeof(*de), name, nlen) == 0){
                de->inode     = ino;
                de->file_type = ftype;
                uint32_t blk = ext2_bmap(s, EX2I(dir), n);
                return ext2_write_block(s, blk, s->blk_buf);
            }
            pos += de->rec_len;
        }
    }
    return -ENOENT;
}

//把目录里的 ".." 指到新的父目录(目录搬家时用)
static int ext2_set_dotdot(struct inode *dir, uint32_t parent){
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    if(ext2_read_dir_block(s, dir, 0))return -EIO;
    uint32_t pos = 0;
    while(pos + sizeof(ext2_dir_entry_t) <= s->block_size){
        ext2_dir_entry_t *de = (ext2_dir_entry_t*)(s->blk_buf + pos);
        if(de->rec_len < sizeof(ext2_dir_entry_t) ||
           pos + de->rec_len > s->block_size)break;
        uint32_t nlen = ext2_dirent_namelen(s, s->blk_buf + pos);
        const char *nm = (const char*)(s->blk_buf + pos + sizeof(*de));
        if(de->inode && nlen == 2 && nm[0] == '.' && nm[1] == '.'){
            de->inode = parent;
            uint32_t blk = ext2_bmap(s, EX2I(dir), 0);
            return ext2_write_block(s, blk, s->blk_buf);
        }
        pos += de->rec_len;
    }
    return -ENOENT;
}

//==================== 写文件 ====================

//释放一棵间接子树里 [lo, hi) 这一段叶子块, 返回整棵子树是否已经空了
static _Bool ext2_release_tree(ext2_sb_info_t *s, struct inode *inode,
                               uint32_t blk, uint32_t level, uint32_t lo, uint32_t hi){
    uint32_t ptrs = s->ptrs;
    uint32_t per  = s->block_size / 512;//一个块算几个 512 字节
    if(ext2_read_block(s, blk, s->meta_buf))return false;
    if(level == 1){//叶子层: 指针直接指向数据块
        uint32_t *p = (uint32_t*)s->meta_buf;
        _Bool all_free = true;
        for(uint32_t i = 0;i < ptrs;i ++){
            if(i >= lo && i < hi && p[i]){
                ext2_free_block(s, p[i]);
                p[i] = 0;
                if(inode->i_blocks >= per)inode->i_blocks -= per;
            }
            if(p[i])all_free = false;//还有指针留着
        }
        if(!all_free)ext2_write_block(s, blk, s->meta_buf);//改动写回去
        return all_free;
    }
    uint32_t span = 1;//每个孩子覆盖多少个叶子
    for(uint32_t k = 1;k < level;k ++)span *= ptrs;
    for(uint32_t a = 0;a < ptrs;a ++){
        if(ext2_read_block(s, blk, s->meta_buf))return false;//每轮重读, 递归会用掉 meta_buf
        uint32_t child = ((uint32_t*)s->meta_buf)[a];
        if(!child)continue;
        uint32_t clo = a * span, chi = clo + span;//这个孩子覆盖的范围
        if(chi <= lo || clo >= hi)continue;//和要释放的范围没交集
        uint32_t sub_lo = lo > clo ? lo - clo : 0;
        uint32_t sub_hi = hi < chi ? hi - clo : span;
        if(ext2_release_tree(s, inode, child, level - 1, sub_lo, sub_hi)){
            ext2_free_block(s, child);//孩子空了就一起放掉
            if(inode->i_blocks >= per)inode->i_blocks -= per;
            if(ext2_read_block(s, blk, s->meta_buf))return false;//重读父块再清指针
            ((uint32_t*)s->meta_buf)[a] = 0;
            if(ext2_write_block(s, blk, s->meta_buf))return false;
            ext2_meta_set(s, blk);
        }
    }
    if(ext2_read_block(s, blk, s->meta_buf))return false;
    uint32_t *p = (uint32_t*)s->meta_buf;
    for(uint32_t a = 0;a < ptrs;a ++)if(p[a])return false;//还有孩子就是没空
    return true;
}

//释放 [from, to) 这段逻辑块, 顺手回收空掉的间接块
static void ext2_release_range(struct inode *inode, uint32_t from, uint32_t to){
    ext2_sb_info_t *s = EX2SB(inode->i_sb);
    ext2_inode_info_t *ii = EX2I(inode);
    uint32_t ptrs = s->ptrs;
    uint32_t per  = s->block_size / 512;
    //快速符号链接的目标就存在 i_block 里, 那里的"块号"是字符, 不能当块释放
    if((inode->i_mode & S_IFMT) == S_IFLNK && inode->i_size <= 60)return;
    //直接块
    for(uint32_t i = from;i < to && i < EXT2_NIDIRECT;i ++){
        if(ii->i_block[i]){
            ext2_free_block(s, ii->i_block[i]);
            ii->i_block[i] = 0;
            if(inode->i_blocks >= per)inode->i_blocks -= per;
        }
    }
    //一级间接
    if(to > EXT2_NIDIRECT){
        uint32_t lo = from > EXT2_NIDIRECT ? from - EXT2_NIDIRECT : 0;
        uint32_t hi = to - EXT2_NIDIRECT;
        if(hi > ptrs)hi = ptrs;
        if(ii->i_block[12] && lo < hi &&
           ext2_release_tree(s, inode, ii->i_block[12], 1, lo, hi)){
            ext2_free_block(s, ii->i_block[12]);
            ii->i_block[12] = 0;
            s->meta_blk = 0;
            if(inode->i_blocks >= per)inode->i_blocks -= per;
        }
    }
    //二级间接
    if(to > EXT2_NIDIRECT + ptrs){
        uint32_t base = EXT2_NIDIRECT + ptrs;
        uint32_t lo = from > base ? from - base : 0;
        uint32_t hi = to - base;
        if(hi > ptrs * ptrs)hi = ptrs * ptrs;
        if(ii->i_block[13] && lo < hi &&
           ext2_release_tree(s, inode, ii->i_block[13], 2, lo, hi)){
            ext2_free_block(s, ii->i_block[13]);
            ii->i_block[13] = 0;
            s->meta_blk = 0;
            if(inode->i_blocks >= per)inode->i_blocks -= per;
        }
    }
    //三级间接
    if(to > EXT2_NIDIRECT + ptrs + ptrs * ptrs){
        uint32_t base = EXT2_NIDIRECT + ptrs + ptrs * ptrs;
        uint32_t lo = from > base ? from - base : 0;
        uint32_t hi = to - base;
        if(hi > ptrs * ptrs * ptrs)hi = ptrs * ptrs * ptrs;
        if(ii->i_block[14] && lo < hi &&
           ext2_release_tree(s, inode, ii->i_block[14], 3, lo, hi)){
            ext2_free_block(s, ii->i_block[14]);
            ii->i_block[14] = 0;
            s->meta_blk = 0;
            if(inode->i_blocks >= per)inode->i_blocks -= per;
        }
    }
}

//文件写入
static int64_t ext2_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    if(!f || !f->f_inode || !buf || !pos)return -EINVAL;
    struct inode *inode = f->f_inode;
    if((inode->i_mode & S_IFMT) == S_IFDIR)return -EISDIR;
    ext2_sb_info_t *s = EX2SB(inode->i_sb);
    uint64_t off   = *pos;
    uint64_t start = off;
    uint64_t left  = len;
    while(left){
        uint32_t blkno = (uint32_t)(off / s->block_size);
        uint32_t inblk = (uint32_t)(off % s->block_size);
        uint32_t n     = s->block_size - inblk;
        if((uint64_t)n > left)n = (uint32_t)left;
        //整块要被完整覆盖时, 分配时就不用先把新块清零
        int whole = (inblk == 0 && n == s->block_size);
        uint32_t blk = ext2_bmap_alloc(inode, blkno, whole ? 0 : 1);
        if(!blk){
            if(off > inode->i_size)inode->i_size = off;
            ext2_write_dinode(inode);
            return (int64_t)(off - start);
        }
        if(whole){
            //整块覆盖, 直接写进去
            if(ext2_bwrite(s, (uint64_t)blk * s->block_size, buf, n))return -EIO;
        }else{
            //只改一块里的部分: 先读出来再写回
            if(ext2_read_block(s, blk, s->blk_buf))return -EIO;
            memcpy(s->blk_buf + inblk, buf, n);
            if(ext2_write_block(s, blk, s->blk_buf))return -EIO;
        }
        buf  += n;
        off  += n;
        left -= n;
    }
    //更新大小与时间戳并回写 inode
    if(off > inode->i_size)inode->i_size = off;
    inode->i_mtime_s = inode->i_ctime_s = (int64_t)rtc_get_epoch();
    ext2_write_dinode(inode);
    PageCacheInvalidateRange(inode->i_ino, start, off - start);//写过的区间在页缓存里已经过时
    ext2_commit(s);
    *pos = off;
    return (int64_t)(off - start);
}

//==================== 属性修改 ====================

//按大小截断
static int ext2_truncate(struct inode *inode, uint64_t newsize){
    ext2_sb_info_t *s = EX2SB(inode->i_sb);
    if(newsize >= inode->i_size){
        inode->i_size = newsize;//变大只是改大小, 块写入时再分配
        return ext2_write_dinode(inode);
    }
    uint32_t keep = (uint32_t)((newsize + s->block_size - 1) / s->block_size);
    ext2_release_range(inode, keep, 0xFFFFFFFFu);
    inode->i_size = newsize;
    PageCacheInvalidate(inode->i_ino);//大小变了,整文件缓存作废
    return ext2_write_dinode(inode);
}

static int ext2_setattr(struct dentry *dentry, void *iattr){
    if(!dentry || !dentry->d_inode || !iattr)return -EINVAL;
    struct inode *inode = dentry->d_inode;
    struct iattr *ia = (struct iattr*)iattr;
    if((ia->ia_valid & ATTR_SIZE) && (inode->i_mode & S_IFMT) == S_IFDIR)return -EISDIR;
    if(ia->ia_valid & ATTR_SIZE){
        int r = ext2_truncate(inode, ia->ia_size);
        if(r)return r;
    }
    if(ia->ia_valid & ATTR_MODE)
        inode->i_mode = (uint16_t)((inode->i_mode & S_IFMT) | (ia->ia_mode & 07777));
    if(ia->ia_valid & ATTR_UID)inode->i_uid = ia->ia_uid;
    if(ia->ia_valid & ATTR_GID)inode->i_gid = ia->ia_gid;
    if(ia->ia_valid & ATTR_ATIME){ inode->i_atime_s = ia->ia_atime; inode->i_atime_ns = 0; }
    if(ia->ia_valid & ATTR_MTIME){ inode->i_mtime_s = ia->ia_mtime; inode->i_mtime_ns = 0; }
    if(ia->ia_valid & ATTR_CTIME){ inode->i_ctime_s = ia->ia_ctime; inode->i_ctime_ns = 0; }
    int r = ext2_write_dinode(inode);
    ext2_commit(EX2SB(inode->i_sb));
    return r;
}

//==================== 创建与删除 ====================

//create/mkdir 共用: 分配 inode, 目录再补上空块和 "." ".."
static int ext2_new_node(struct inode *dir, struct dentry *dentry, uint16_t mode, int isdir){
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    uint16_t full = (uint16_t)((isdir ? S_IFDIR : S_IFREG) | (mode & 07777));
    uint32_t ino = ext2_alloc_inode(dir->i_sb, full, isdir);
    if(!ino)return -ENOSPC;
    uint32_t parent_ino = (uint32_t)dir->i_ino;
    if(isdir){
        //新目录的第一块里放 "." 和 ".."
        uint32_t blk = ext2_alloc_block(s, 0, 0);
        if(!blk)return -ENOSPC;
        memset(s->blk_buf, 0, s->block_size);
        ext2_dir_entry_t *dot = (ext2_dir_entry_t*)s->blk_buf;
        dot->inode = ino;
        dot->rec_len = 12;
        dot->name_len = 1;
        dot->file_type = 2;
        s->blk_buf[sizeof(*dot)] = '.';
        ext2_dir_entry_t *dotdot = (ext2_dir_entry_t*)(s->blk_buf + 12);
        dotdot->inode = parent_ino;
        dotdot->rec_len = (uint16_t)(s->block_size - 12);
        dotdot->name_len = 2;
        dotdot->file_type = 2;
        s->blk_buf[12 + sizeof(*dotdot)] = '.';
        s->blk_buf[12 + sizeof(*dotdot) + 1] = '.';
        if(ext2_write_block(s, blk, s->blk_buf))return -EIO;
        //把这一块记到新 inode 上
        ext2_group_desc_t gd;
        uint64_t off = ext2_inode_offset(s, ino, &gd);
        ext2_dinode_t di;
        if(!off || ext2_bread(s, off, &di, sizeof(di)))return -EIO;
        di.i_block[0] = blk;
        di.i_size     = s->block_size;
        di.i_blocks   = s->block_size / 512;
        di.i_links_count = 2;//"." 与父目录各算一个
        if(ext2_bwrite(s, off, &di, sizeof(di)))return -EIO;
    }
    //在父目录里登记名字
    int r = ext2_add_entry(dir, dentry->d_name, dentry->d_name_len, ino, isdir ? 2 : 1);
    if(r)return r;
    if(isdir){
        dir->i_nlink ++;
        dir->i_mtime_s = dir->i_ctime_s = (int64_t)rtc_get_epoch();
        ext2_write_dinode(dir);
    }
    //读回新 inode 挂到 dentry 上
    struct inode *inode = ext2_iget(dir->i_sb, ino);
    if(!inode)return -EIO;
    d_add(dentry, inode);
    Iput(inode);
    ext2_commit(s);
    return 0;
}

static int ext2_create(struct inode *dir, struct dentry *dentry, uint16_t mode){
    return ext2_new_node(dir, dentry, mode, 0);
}

static int ext2_mkdir(struct inode *dir, struct dentry *dentry, uint16_t mode){
    return ext2_new_node(dir, dentry, mode, 1);
}

//建符号链接
static int ext2_symlink(struct inode *dir, struct dentry *dentry, const char *target){
    if(!dir || !dentry || !target)return -EINVAL;
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    int len = (int)strlen(target);
    if(len == 0 || len >= EXT2_LINK_MAX)return -ENAMETOOLONG;
    uint32_t ino = ext2_alloc_inode(dir->i_sb, (uint16_t)(S_IFLNK | 0777), 0);
    if(!ino)return -ENOSPC;
    uint64_t off = ext2_inode_offset(s, ino, NULL);
    ext2_dinode_t di;
    if(!off || ext2_bread(s, off, &di, sizeof(di)))return -EIO;
    di.i_size        = (uint32_t)len;
    di.i_links_count = 1;
    if(len <= 60){
        //快速符号链接: 目标放进 i_block, 不占数据块
        memset(di.i_block, 0, sizeof(di.i_block));
        memcpy(di.i_block, target, (size_t)len);
        di.i_blocks = 0;
    }else{
        //长目标: 单独给一个块来存
        uint32_t blk = ext2_alloc_block(s, 0, 0);
        if(!blk)return -ENOSPC;
        memset(s->blk_buf, 0, s->block_size);
        memcpy(s->blk_buf, target, (size_t)len);
        if(ext2_write_block(s, blk, s->blk_buf))return -EIO;
        di.i_block[0] = blk;
        di.i_blocks   = s->block_size / 512;
    }
    if(ext2_bwrite(s, off, &di, sizeof(di)))return -EIO;
    //在父目录里登记名字(file_type = 7 表示符号链接)
    int r = ext2_add_entry(dir, dentry->d_name, dentry->d_name_len, ino, 7);
    if(r)return r;
    struct inode *inode = ext2_iget(dir->i_sb, ino);
    if(!inode)return -EIO;
    d_add(dentry, inode);
    Iput(inode);
    ext2_commit(s);
    return 0;
}

//删除一个文件所在的 inode: 链接数归零就释放块和 inode
static int ext2_drop_inode(struct inode *inode){
    if(inode->i_nlink > 1){
        inode->i_nlink --;
        return ext2_write_dinode(inode);
    }
    ext2_release_range(inode, 0, 0xFFFFFFFFu);
    inode->i_nlink = 0;
    inode->i_size  = 0;
    inode->i_ctime_s = (int64_t)rtc_get_epoch();
    ext2_write_dinode(inode);
    return ext2_free_inode(inode);
}

//给已有的 inode 再挂一个名字(硬链接)
static int ext2_link(struct dentry *old, struct inode *dir, struct dentry *newd){
    if(!old || !old->d_inode || !newd)return -ENOENT;
    struct inode *inode = old->d_inode;
    if((inode->i_mode & S_IFMT) == S_IFDIR)return -EPERM;
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    //在目标目录里登记这个名字
    int r = ext2_add_entry(dir, newd->d_name, newd->d_name_len,
                           (uint32_t)inode->i_ino, ext2_ftype_of(inode));
    if(r)return r;
    //链接数加一, 两个名字共用同一份数据
    int64_t now = (int64_t)rtc_get_epoch();
    inode->i_nlink ++;
    inode->i_ctime_s = now;
    ext2_write_dinode(inode);
    dir->i_mtime_s = dir->i_ctime_s = now;
    ext2_write_dinode(dir);
    d_add(newd, inode);
    ext2_commit(s);
    return 0;
}

static int ext2_unlink(struct inode *dir, struct dentry *dentry){
    if(!dentry || !dentry->d_inode)return -ENOENT;
    struct inode *inode = dentry->d_inode;
    if((inode->i_mode & S_IFMT) == S_IFDIR)return -EISDIR;
    int r = ext2_remove_entry(dir, dentry->d_name, dentry->d_name_len);
    if(r)return r;
    dir->i_mtime_s = dir->i_ctime_s = (int64_t)rtc_get_epoch();
    ext2_write_dinode(dir);
    r = ext2_drop_inode(inode);
    d_invalidate(dentry);
    PageCacheInvalidate(inode->i_ino);
    ext2_commit(EX2SB(dir->i_sb));
    return r;
}

//目录是否只剩 "." 和 ".."
static int ext2_dir_empty(struct inode *dir){
    ext2_sb_info_t *s = EX2SB(dir->i_sb);
    uint32_t nb = ext2_dir_nblocks(s, dir);
    for(uint32_t n = 0;n < nb;n ++){
        if(ext2_read_dir_block(s, dir, n))return 0;
        uint32_t pos = 0;
        while(pos + sizeof(ext2_dir_entry_t) <= s->block_size){
            ext2_dir_entry_t *de = (ext2_dir_entry_t*)(s->blk_buf + pos);
            if(de->rec_len < sizeof(ext2_dir_entry_t) ||
               pos + de->rec_len > s->block_size)break;
            uint32_t nlen = ext2_dirent_namelen(s, s->blk_buf + pos);
            const char *nm = (const char*)(s->blk_buf + pos + sizeof(*de));
            if(de->inode && nlen &&
               !(nlen == 1 && nm[0] == '.') &&
               !(nlen == 2 && nm[0] == '.' && nm[1] == '.'))return 0;
            pos += de->rec_len;
        }
    }
    return 1;
}

static int ext2_rmdir(struct inode *dir, struct dentry *dentry){
    if(!dentry || !dentry->d_inode)return -ENOENT;
    struct inode *inode = dentry->d_inode;
    if((inode->i_mode & S_IFMT) != S_IFDIR)return -ENOTDIR;
    if(!ext2_dir_empty(inode))return -ENOTEMPTY;
    int r = ext2_remove_entry(dir, dentry->d_name, dentry->d_name_len);
    if(r)return r;
    if(dir->i_nlink > 2)dir->i_nlink --;
    dir->i_mtime_s = dir->i_ctime_s = (int64_t)rtc_get_epoch();
    ext2_write_dinode(dir);
    inode->i_nlink = 0;
    ext2_release_range(inode, 0, 0xFFFFFFFFu);
    ext2_write_dinode(inode);
    ext2_free_inode(inode);
    d_invalidate(dentry);
    PageCacheInvalidate(inode->i_ino);
    ext2_commit(EX2SB(dir->i_sb));
    return 0;
}

//改名/移动: 同目录改名, 或挪进另一个目录; 目标可以是已有文件或空目录
static int ext2_rename(struct inode *odir, struct dentry *odent,
                       struct inode *ndir, struct dentry *ndent){
    if(!odent || !odent->d_inode || !ndent)return -ENOENT;
    struct inode *inode = odent->d_inode;
    _Bool isdir = ((inode->i_mode & S_IFMT) == S_IFDIR);
    //不能把目录搬进它自己的子树, 否则目录树会成环
    if(isdir){
        for(struct dentry *p = ndent->d_parent;p;p = p->d_parent)
            if(p->d_inode == inode)return -EINVAL;
    }
    //目标已存在: 类型要相符, 目录还得是空的
    struct inode *target = ndent->d_inode;
    if(target == inode)target = NULL;
    _Bool tdir = false;
    if(target){
        tdir = ((target->i_mode & S_IFMT) == S_IFDIR);
        if(tdir && !isdir)return -EISDIR;
        if(isdir && !tdir)return -ENOTDIR;
        if(tdir && !ext2_dir_empty(target))return -ENOTEMPTY;
    }
    uint8_t ftype = ext2_ftype_of(inode);
    int r;
    //新名字先落地: 有目标就改它那一条的 inode 号, 否则新增一条
    if(target)r = ext2_set_entry(ndir, ndent->d_name, ndent->d_name_len,
                                 (uint32_t)inode->i_ino, ftype);
    else       r = ext2_add_entry(ndir, ndent->d_name, ndent->d_name_len,
                                 (uint32_t)inode->i_ino, ftype);
    if(r)return r;
    //再撤掉旧名字
    r = ext2_remove_entry(odir, odent->d_name, odent->d_name_len);
    if(r)return r;
    //目录换了父目录: 改它自己的 "..", 两个父目录的链接数一减一加
    if(isdir && odir != ndir){
        if(ext2_set_dotdot(inode, (uint32_t)ndir->i_ino) == 0){
            if(odir->i_nlink > 2)odir->i_nlink --;
            ndir->i_nlink ++;
        }
    }
    //被覆盖的目标此刻没人引用, 回收它占的块和 inode
    if(target){
        if(tdir){
            if(ndir->i_nlink > 2)ndir->i_nlink --;
            target->i_nlink = 0;
            ext2_release_range(target, 0, 0xFFFFFFFFu);
            ext2_write_dinode(target);
            ext2_free_inode(target);
        }else{
            ext2_drop_inode(target);
        }
        PageCacheInvalidate(target->i_ino);
    }
    int64_t now = (int64_t)rtc_get_epoch();
    inode->i_ctime_s = now;
    odir->i_mtime_s  = odir->i_ctime_s = now;
    ext2_write_dinode(inode);
    ext2_write_dinode(odir);
    if(ndir != odir){
        ndir->i_mtime_s = ndir->i_ctime_s = now;
        ext2_write_dinode(ndir);
    }
    //dcache 跟着改: 新名字绑上 inode, 旧名字失效
    d_add(ndent, inode);
    if(odent != ndent)d_invalidate(odent);
    ext2_commit(EX2SB(ndir->i_sb));
    return 0;
}

//操作表
static struct inode_operations g_ext2_iops = {
    .lookup   = ext2_lookup,
    .create   = ext2_create,
    .mkdir    = ext2_mkdir,
    .symlink  = ext2_symlink,
    .link     = ext2_link,
    .unlink   = ext2_unlink,
    .rmdir    = ext2_rmdir,
    .rename   = ext2_rename,
    .get_link = ext2_get_link,
    .setattr  = ext2_setattr,
};

static const struct file_operations g_ext2_file_fops = {
    .read  = ext2_read,
    .write = ext2_write,
};

static const struct file_operations g_ext2_dir_fops = {
    .iterate = ext2_iterate,
};

static void ext2_evict_inode(struct inode *inode){
    (void)inode;//没有需要在这里释放的私有资源
}

//卸载/同步时把攒下的元数据刷盘
static int ext2_sync_fs(struct super_block *sb, int wait){
    (void)wait;
    ext2_sb_info_t *s = EX2SB(sb);
    ext2_commit(s);
    BCacheSyncBdev(s->bdev);//同步点必须真的落盘
    return 0;
}

static const struct super_operations g_ext2_sop = {
    .evict_inode = ext2_evict_inode,
    .sync_fs     = ext2_sync_fs,
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
    s->compat           = es->s_feature_compat;
    s->blocks_count     = es->s_blocks_count;
    s->inodes_count     = es->s_inodes_count;
    s->first_ino        = es->s_rev_level ? es->s_first_ino : 11;
    s->free_blocks_count = es->s_free_blocks_count;
    s->free_inodes_count = es->s_free_inodes_count;
    //写路径要靠这 1 字节的 name_len/file_type, 老镜像不支持
    if(!(s->incompat & EXT2_INCOMPAT_FILETYPE)){
        Pmm_Free(pa, 1);
        KslabFree(g_ext2_info_cache, s);
        return -ENODEV;
    }
    if(s->incompat & ~EXT2_INCOMPAT_FILETYPE){
        printk(PRINTK_WARNING"ext2: unsupported incompat features %p (extents=0x40 64bit=0x80 meta_bg=0x10 flex_bg=0x200), read may be wrong",
               (void*)(uintptr_t)(s->incompat & ~EXT2_INCOMPAT_FILETYPE));
    }
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
    void *pb = Pmm_Malloc(1);
    void *pi = Pmm_Malloc(1);
    void *pt = Pmm_Malloc(1);
    if(!pm || !pd || !pb || !pi || !pt){
        if(pm)Pmm_Free(pm, 1);
        if(pd)Pmm_Free(pd, 1);
        if(pb)Pmm_Free(pb, 1);
        if(pi)Pmm_Free(pi, 1);
        if(pt)Pmm_Free(pt, 1);
        KslabFree(g_ext2_info_cache, s);
        return -ENOMEM;
    }
    s->meta_buf = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pm);
    s->blk_buf  = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pd);
    s->bbmp.buf = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pb);
    s->ibmp.buf = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pi);
    s->itab_buf = (uint8_t*)PHYS_TO_VIRT((uintptr_t)pt);
    //块组描述符表很小, 直接整体读进内存, 之后查组信息不用再发 I/O
    uint32_t gdt_bytes = s->nr_groups * (uint32_t)sizeof(ext2_group_desc_t);
    uint32_t gdt_pages = (gdt_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void *pg = Pmm_Malloc((int)gdt_pages);
    if(!pg){
        KslabFree(g_ext2_info_cache, s);
        return -ENOMEM;
    }
    s->gdt = (ext2_group_desc_t*)PHYS_TO_VIRT((uintptr_t)pg);
    if(bdev_read(bd, (uint64_t)s->desc_blk * s->block_size, s->gdt, gdt_bytes)){
        s->gdt = NULL;
        KslabFree(g_ext2_info_cache, s);
        return -EIO;
    }
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
 * 新建一个 ext2 超级块并建好根目录
 * @param bd       ext2 所在的分区块设备
 * @param root_out 成功时写入根 dentry
 * @return 超级块, 失败返回 NULL
 */
struct super_block *Ext2NewSuper(struct bdev *bd, struct dentry **root_out){
    if(!bd)return NULL;
    if(!g_ext2_info_cache){
        g_ext2_info_cache = KslabCreate("ext2_sb", sizeof(ext2_sb_info_t));
        if(!g_ext2_info_cache)return NULL;
    }
    struct super_block *sb = SuperAlloc("ext2", EXT2_MAGIC, Ext2InodeSize(), Ext2SuperOps());
    if(!sb)return NULL;
    //先把超级块读出来并建好根目录, 失败了就当没挂过
    if(Ext2ReadSuper(sb, (bdev_t*)bd)){
        SuperFree(sb);
        return NULL;
    }
    struct dentry *root = Ext2FillSuper(sb);
    if(!root){
        SuperFree(sb);
        return NULL;
    }
    if(root_out)*root_out = root;
    return sb;
}

/**
 * 把找到的 ext2 分区挂到 parent 的 mountpoint 上
 * @return 0 成功
 */
int Ext2MountAt(struct vfsmount *parent, struct dentry *mountpoint){
    bdev_t *bd = Ext2FindPartition();
    if(!bd)return -ENODEV;
    struct dentry *root = NULL;
    struct super_block *sb = Ext2NewSuper(bd, &root);
    if(!sb)return -ENOMEM;
    //挂成一棵独立挂载树, 再接到 mountpoint 下面
    struct vfsmount *m = VfsKernMount(sb, root, 0);
    if(!m){
        SuperFree(sb);
        return -ENOMEM;
    }
    int r = VfsAttachMount(parent, mountpoint, m);
    if(r)VfsDetachMount(m);
    return r;
}
