/*
 * fs/vfs.c — 虚拟文件系统层
 * 路径解析 + 打开文件描述操作。后端当前为ramfs。
 */

#include <fs.h>
#include <syscalls.h>

static fs_node_t *fs_root = NULL;

fs_node_t *FsRoot(void){
    return fs_root;
}

/*DeepSeek V4 Pro*/
//解析完整绝对路径,返回节点(文件或目录),不存在返回NULL
static fs_node_t *resolve(const char *path){
    if(!path || path[0] != '/') return NULL;
    fs_node_t *cur = fs_root;
    char comp[MAX_NAME];
    const char *p = path + 1;
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
//解析父目录与末段名:/a/b/c → parent=/a/b, name=c
static int resolve_parent(const char *path, fs_node_t **parent, char name[MAX_NAME]){
    char buf[256];
    int len = strlen(path);
    if(len <= 0 || len >= 256) return -1;
    memcpy(buf, path, len + 1);
    //找最后一个'/'
    char *last = NULL;
    for(char *q = buf; *q; q++) if(*q == '/') last = q;
    if(!last) return -1;
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
    if(!out)return -1;
    fs_node_t *node = resolve(path);//解析文件节点
    //解析父目录与末段名
    if(!node && (flags & O_CREAT)){
        fs_node_t *parent = NULL;
        char name[MAX_NAME];
        if(resolve_parent(path, &parent, name) == 0 && parent->ops->create)node = parent->ops->create(parent, name, FT_FILE);//如果文件不存在则创建
    }
    //检查节点是否存在或非文件
    if(!node)return -1;
    if(node->type != FT_FILE)return -1;
    if(flags & O_TRUNC){
        if(node->ops->truncate)node->ops->truncate(node);
    }
    //填充信息
    node->refs++;
    out->node = node;
    out->flags = flags;
    out->off = (flags & O_APPEND) ? node->size : 0;
    out->used = true;
    return 0;
}

void FsClose(fs_file_t *f){
    if(!f || !f->used)return;//检查是否存在且已打开
    //清理数据
    f->used = false;
    f->node = NULL;
    f->off = 0;
}

uint64_t FsRead(fs_file_t *f, void *buf, uint64_t len){
    if(!f || !f->used || !f->node || !f->node->ops->read) return 0;
    uint64_t n = f->node->ops->read(f->node, f->off, buf, len);
    f->off += n;
    return n;
}

uint64_t FsWrite(fs_file_t *f, const void *buf, uint64_t len){
    if(!f || !f->used || !f->node || !f->node->ops->write) return 0;
    uint64_t n = f->node->ops->write(f->node, f->off, buf, len);
    f->off += n;
    return n;
}

int FsSeek(fs_file_t *f, int64_t off, int whence){
    if(!f || !f->used || !f->node) return -1;
    int64_t base;
    if(whence == SEEK_SET) base = 0;
    else if(whence == SEEK_CUR) base = (int64_t)f->off;
    else if(whence == SEEK_END) base = (int64_t)f->node->size;
    else return -1;
    int64_t npos = base + off;
    if(npos < 0) return -1;
    f->off = (uint64_t)npos;
    return (int)f->off;
}

uint64_t FsSize(fs_file_t *f){
    if(!f || !f->used || !f->node) return 0;
    return f->node->size;
}

void FsInit(void){
    fs_root = ramfs_init();
    //预置一个测试文件
    fs_file_t f;
    if(FsOpen("/hello.txt", O_CREAT | O_RDWR, &f) == 0){
        static const char *s = "Hello from ramfs!\n";
        FsWrite(&f, s, strlen(s));
        FsClose(&f);
    }
}
