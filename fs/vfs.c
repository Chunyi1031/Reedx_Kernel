#include <fs.h>
#include <syscalls.h>
#include <task.h>

static fs_node_t *fs_root = NULL;
static spinlock_t g_fs_lock = {0};//全局文件系统锁

fs_node_t *FsRoot(void){
    return fs_root;
}

/*DeepSeek V4 Pro*/
//规范化路径
static int path_normalize(const char *path, char *out, int outsz){
    if(!path) return -1;
    char abs[512];
    const char *src;
    if(path[0] == '/'){
        src = path;
    }else{
        const char *cwd = (current_task && current_task->cwd[0]) ? current_task->cwd : "/";
        int cl = strlen(cwd);
        int pl = strlen(path);
        if(cl + 1 + pl >= (int)sizeof(abs)) return -1;
        memcpy(abs, cwd, cl);
        int off = cl;
        if(off == 0 || abs[off - 1] != '/') abs[off++] = '/';
        memcpy(abs + off, path, pl + 1);
        src = abs;
    }
    //跳过"."、".."弹一级、去重斜杠
    char stack[512];
    int top = 0;
    const char *p = src;
    while(*p){
        while(*p == '/')p++;
        if(!*p) break;
        char comp[MAX_NAME];
        int i = 0;
        while(*p && *p != '/' && i < MAX_NAME - 1)comp[i++] = *p++;
        comp[i] = 0;
        if(i == 1 && comp[0] == '.') continue;//"."跳过
        if(i == 2 && comp[0] == '.' && comp[1] == '.'){//".."弹上一级
            while(top > 0 && stack[top - 1] != '/') top--;
            if(top > 0) top--;
            if(top == 0) stack[top++] = '/';
            continue;
        }
        if(top == 0 || stack[top - 1] != '/'){
            if(top < (int)sizeof(stack) - 1)stack[top++] = '/';
        }
        for(int k = 0; k < i && top < (int)sizeof(stack) - 1; k++)stack[top++] = comp[k];
    }
    if(top == 0) stack[top++] = '/';
    if(top >= outsz) return -1;
    memcpy(out, stack, top);
    out[top] = 0;
    return 0;
}

//解析路径,返回节点(文件或目录),不存在返回NULL
static fs_node_t *resolve(const char *path){
    char norm[512];
    if(!fs_root) return NULL;//文件系统尚未初始化
    if(path_normalize(path, norm, sizeof(norm)) != 0) return NULL;
    fs_node_t *cur = fs_root;
    char comp[MAX_NAME];
    const char *p = norm + 1;
    while(*p){
        int i = 0;
        while(*p && *p != '/' && i < MAX_NAME - 1) comp[i++] = *p++;
        comp[i] = 0;
        if(*p == '/') p++;
        if(i == 0) continue;
        if(cur->type != FT_DIR || !cur->ops->lookup) return NULL;
        cur = cur->ops->lookup(cur, comp);
        if(!cur) return NULL;
    }
    return cur;
}
//公开的路径解析
fs_node_t *FsResolve(const char *path){
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    fs_node_t *n = resolve(path);
    spin_unlock_irqrestore(&g_fs_lock, flags);
    return n;
}
//规范化路径
int FsNormalizePath(const char *path, char *out, int outsz){
    return path_normalize(path, out, outsz);
}
//解析父目录与末段名:/a/b/c → parent=/a/b, name=c
static int resolve_parent(const char *path, fs_node_t **parent, char name[MAX_NAME]){
    char buf[256];
    int len = strlen(path);
    if(len <= 0 || len >= 256) return -1;
    memcpy(buf, path, len + 1);
    //找最后一个'/'
    char *last = NULL;
    for(char *q = buf; *q; q++) if(*q == '/') last = q;
    if(!last){
        int i;
        for(i = 0; i < MAX_NAME - 1 && path[i]; i++)name[i] = path[i];
        name[i] = 0;
        fs_node_t *p = resolve(".");
        if(!p || p->type != FT_DIR)return -1;
        *parent = p;
        return 0;
    }
    *last = 0;
    int i;
    for(i = 0; i < MAX_NAME - 1 && last[1 + i]; i++) name[i] = last[1 + i];
    name[i] = 0;
    fs_node_t *p = resolve(buf[0] ? buf : "/");
    if(!p || p->type != FT_DIR) return -1;
    *parent = p;
    return 0;
}
/*DeepSeek V4 Pro-END*/

int FsOpen(const char *path, int flags, fs_file_t *out){
    uint64_t iflags;
    spin_lock_irqsave(&g_fs_lock, iflags);
    int r = -1;
    if(!out)goto done;
    fs_node_t *node = resolve(path);//解析文件节点
    //解析父目录与末段名
    if(!node && (flags & O_CREAT)){
        fs_node_t *parent = NULL;
        char name[MAX_NAME];
        if(resolve_parent(path, &parent, name) == 0 && parent->ops->create)node = parent->ops->create(parent, name, FT_FILE);//如果文件不存在则创建
    }
    //检查节点是否存在或非文件
    if(!node)goto done;
    if(node->type != FT_FILE && node->type != FT_DIR)goto done;
    if(flags & O_TRUNC){
        if(node->ops->truncate)node->ops->truncate(node);
    }
    //填充信息
    node->refs++;
    out->node = node;
    out->flags = flags;
    out->off = (flags & O_APPEND) ? node->size : 0;
    out->used = true;
    r = 0;
done:
    spin_unlock_irqrestore(&g_fs_lock, iflags);
    return r;
}

void FsClose(fs_file_t *f){
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    if(f && f->used){
        //清理数据
        f->used = false;
        f->node = NULL;
        f->off = 0;
    }
    spin_unlock_irqrestore(&g_fs_lock, flags);
}

uint64_t FsRead(fs_file_t *f, void *buf, uint64_t len){
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    uint64_t n = 0;
    if(f && f->used && f->node && f->node->ops->read && (f->flags & O_ACCMODE) != O_WRONLY){//只写打开的文件不可读
        n = f->node->ops->read(f->node, f->off, buf, len);
        f->off += n;
    }
    spin_unlock_irqrestore(&g_fs_lock, flags);
    return n;
}

uint64_t FsWrite(fs_file_t *f, const void *buf, uint64_t len){
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    uint64_t n = 0;
    if(f && f->used && f->node && f->node->ops->write && (f->flags & O_ACCMODE) != O_RDONLY){//只读打开的文件不可写
        if(f->flags & O_APPEND) f->off = f->node->size;//追加模式每次写强制写到文件末尾
        n = f->node->ops->write(f->node, f->off, buf, len);
        f->off += n;
    }
    spin_unlock_irqrestore(&g_fs_lock, flags);
    return n;
}

int FsSeek(fs_file_t *f, int64_t off, int whence){
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    int r = -1;
    if(f && f->used && f->node){
        int64_t base;
        if(whence == SEEK_SET) base = 0;
        else if(whence == SEEK_CUR) base = (int64_t)f->off;
        else if(whence == SEEK_END) base = (int64_t)f->node->size;
        else goto done;
        int64_t npos = base + off;
        if(npos < 0)goto done;
        f->off = (uint64_t)npos;
        r = (int)f->off;
    }
done:
    spin_unlock_irqrestore(&g_fs_lock, flags);
    return r;
}

uint64_t FsSize(fs_file_t *f){
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    uint64_t r = (f && f->used && f->node) ? f->node->size : 0;
    spin_unlock_irqrestore(&g_fs_lock, flags);
    return r;
}

//读取目录项到缓冲区, 返回写入字节数
int FsGetdents(fs_file_t *f, void *buf, uint64_t len){
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    int written = 0;
    if(f && f->used && f->node && f->node->type == FT_DIR && f->node->ops->readdir && buf){
        uint64_t cookie = f->off;
        char *p = (char*)buf;
        while((uint64_t)written < len){
            fs_dirent_t de;
            uint64_t save = cookie;
            int rr = f->node->ops->readdir(f->node, &cookie, &de);
            if(rr != 0){ written = (rr < 0) ? rr : written; break; }
            int nlen = strlen(de.name);
            uint16_t reclen = (uint16_t)(19 + nlen + 1);
            reclen = (uint16_t)((reclen + 7) & ~7);//8字节对齐
            if((uint64_t)written + reclen > len){ cookie = save; break; }//放不下则回退
            linux_dirent64_t *d = (linux_dirent64_t*)(p + written);
            d->d_ino = de.ino;
            d->d_off = (int64_t)cookie;
            d->d_reclen = reclen;
            d->d_type = (uint8_t)((de.type == FT_DIR) ? DT_DIR : ((de.type == FT_FILE) ? DT_REG : DT_UNKNOWN));
            strcpy(d->d_name, de.name);
            written += reclen;
        }
        f->off = cookie;
    }
    spin_unlock_irqrestore(&g_fs_lock, flags);
    return written;
}

int FsMkdir(const char *path, int mode){
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    int r = -1;
    if(path){
        fs_node_t *parent = NULL;
        char name[MAX_NAME];
        if(resolve_parent(path, &parent, name) == 0 && parent->ops->mkdir)
            r = parent->ops->mkdir(parent, name, mode);
    }
    spin_unlock_irqrestore(&g_fs_lock, flags);
    return r;
}

int FsUnlink(const char *path){
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    int r = -1;
    if(path){
        fs_node_t *parent = NULL;
        char name[MAX_NAME];
        if(resolve_parent(path, &parent, name) == 0 && parent->ops->unlink)
            r = parent->ops->unlink(parent, name);
    }
    spin_unlock_irqrestore(&g_fs_lock, flags);
    return r;
}

int FsRename(const char *oldpath, const char *newpath){
    if(!oldpath || !newpath) return -1;
    uint64_t flags;
    spin_lock_irqsave(&g_fs_lock, flags);
    int r = -1;
    do{
        if(strcmp(oldpath, newpath) == 0){ r = 0; break; }//同一路径无需操作
        //目标已存在则先删除
        if(resolve(newpath)){
            fs_node_t *tparent = NULL;
            char tname[MAX_NAME];
            if(resolve_parent(newpath, &tparent, tname) != 0 || !tparent->ops->unlink) break;
            if(tparent->ops->unlink(tparent, tname) != 0) break;
        }
        fs_node_t *oldparent = NULL;
        char oldname[MAX_NAME];
        fs_node_t *newparent = NULL;
        char newname[MAX_NAME];
        if(resolve_parent(oldpath, &oldparent, oldname) != 0) break;
        if(resolve_parent(newpath, &newparent, newname) != 0) break;
        if(!oldparent->ops->rename) break;
        r = oldparent->ops->rename(oldparent, oldname, newparent, newname);
    }while(0);
    spin_unlock_irqrestore(&g_fs_lock, flags);
    return r;
}

void FsInit(struct disk_info *disk){
    //优先挂载FAT32,失败则回退ramfs
    if(disk && (fat32_mount(disk) == 0)){
        fs_root = fat32_root();
    }else{
        fs_root = ramfs_init();
        fs_file_t f;
        if(FsOpen("/hello.txt", O_CREAT | O_RDWR, &f) == 0){
            static const char *s = "Hello from ramfs!\n";
            FsWrite(&f, s, strlen(s));
            FsClose(&f);
        }
    }
}
