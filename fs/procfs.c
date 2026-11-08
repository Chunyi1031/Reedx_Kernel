// SPDX-License-Identifier: GPL-2.0-only
/**
 * fs/procfs.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 进程与内核状态的伪文件系统
 */
#include <vfs.h>
#include <fs.h>
#include <task.h>
#include <mm/pmm.h>
#include <irq.h>
#include <drives/timer.h>

#define PROC_BUF_MAX 3072//单个伪文件内容上限

//伪文件的种类
enum {
    PROC_K_ROOT = 0,  //根目录
    PROC_K_MEMINFO,   //内存用量
    PROC_K_CPUINFO,   //CPU 信息
    PROC_K_VERSION,   //内核版本
    PROC_K_UPTIME,    //开机时间
    PROC_K_MOUNTS,    //挂载表
    PROC_K_SELF,      //指向自己的符号链接
    PROC_K_PIDDIR,    //进程目录
    PROC_K_STATUS,    //进程状态
    PROC_K_CMDLINE,   //命令行
    PROC_K_STAT,      //进程stat行
    PROC_K_FDDIR,     //进程fd目录
    PROC_K_FDLINK,    //单个fd的符号链接
    PROC_K_ENVIRON,   //进程环境变量块
    PROC_K_COMM,      //进程名
    PROC_K_CGROUP,    //进程所属cgroup
    PROC_K_MOUNTINFO, ///proc/mountinfo
    PROC_K_CMDLINE_ROOT,// /proc/cmdline
    PROC_K_FILESYSTEMS, // /proc/filesystems
    PROC_K_SYSDIR,      // /proc/sys
    PROC_K_SYSKERNELDIR,// /proc/sys/kernel
    PROC_K_SYSCTL,      // /proc/sys/kernel/* 下的单个文件
    PROC_K_CGROUPS,     // /proc/cgroups
    PROC_K_ROOTLINK,    // /proc/<pid>/root 符号链接
};

//每个proc inode的私有信息
typedef struct proc_inode_info {
    uint32_t kind;//本inode的种类(PROC_K_*)
    uint32_t pid; //进程相关文件的pid
    uint32_t fd;  //fd相关文件的描述符
    char     link[256];//符号链接的目标
} proc_inode_info_t;

#define PROC_INFO(ino) ((proc_inode_info_t*)(ino)->i_private)

//inode号
#define PROC_INO_ROOT        1ULL
#define PROC_INO_FIXED(k)    ((uint64_t)(2 + (k)))
#define PROC_INO_PID(p)      ((uint64_t)((uint64_t)(p) * 16 + 8))
#define PROC_INO_PIDFILE(p,k)((uint64_t)((uint64_t)(p) * 16 + 8 + (k)))
#define PROC_INO_FDDIR(p)    ((uint64_t)((uint64_t)(p) * 16 + 23))
#define PROC_INO_FDLINK(p,f) ((uint64_t)(0x10000000ULL + (uint64_t)(p) * 1024 + (uint64_t)(f)))
#define PROC_INO_SYSDIR      ((uint64_t)0x20000001ULL)
#define PROC_INO_SYSKERNELDIR ((uint64_t)0x20000002ULL)
#define PROC_INO_SYSCTL(i)   ((uint64_t)(0x20000100ULL + (uint64_t)(i)))

// /proc/sys/kernel下的可读写参数
typedef struct proc_sysctl_desc {
    const char *name;//文件名
    uint16_t    mode;//权限
    uint32_t    gen; //内容种类
} proc_sysctl_desc_t;

enum {
    PSYS_OSRELEASE = 0,
    PSYS_CAP_LAST_CAP,
    PSYS_PRINTK_DEVKMSG,
    PSYS_CORE_PATTERN,
    PSYS_OSTYPE,
    PSYS_VERSION,
    PSYS_HOSTNAME,
    PSYS_DOMAINNAME,
    PSYS_CONSOLE_LOGLEVEL,
};

static const proc_sysctl_desc_t g_proc_sysctls[] = {
    {"osrelease",           S_IFREG | 0444, PSYS_OSRELEASE},
    {"cap_last_cap",        S_IFREG | 0444, PSYS_CAP_LAST_CAP},
    {"printk_devkmsg",      S_IFREG | 0644, PSYS_PRINTK_DEVKMSG},
    {"core_pattern",        S_IFREG | 0644, PSYS_CORE_PATTERN},
    {"ostype",              S_IFREG | 0444, PSYS_OSTYPE},
    {"version",             S_IFREG | 0444, PSYS_VERSION},
    {"hostname",            S_IFREG | 0644, PSYS_HOSTNAME},
    {"domainname",          S_IFREG | 0644, PSYS_DOMAINNAME},
    {"printk",              S_IFREG | 0644, PSYS_CONSOLE_LOGLEVEL},
};
#define PROC_NSYSCTL ((int)(sizeof(g_proc_sysctls) / sizeof(g_proc_sysctls[0])))

//根目录下的固定文件
static const struct {
    const char *name;//文件名
    uint32_t kind;//文件种类(PROC_K_*)
} g_proc_files[] = {
    {"meminfo",PROC_K_MEMINFO},//内存用量
    {"cpuinfo",PROC_K_CPUINFO},//CPU信息
    {"version",PROC_K_VERSION},//内核版本
    {"uptime",PROC_K_UPTIME},//开机时间
    {"mounts",PROC_K_MOUNTS},//挂载表
    {"mountinfo",PROC_K_MOUNTINFO},//详细挂载表
    {"cmdline",PROC_K_CMDLINE_ROOT},//内核命令行
    {"filesystems",PROC_K_FILESYSTEMS},//已注册的文件系统
    {"cgroups",PROC_K_CGROUPS},//cgroup控制器列表
};
#define PROC_NFILES ((int)(sizeof(g_proc_files) / sizeof(g_proc_files[0])))

//进程目录下的文件
static const struct {
    const char *name;//文件名
    uint32_t kind;//文件种类(PROC_K_*)
} g_proc_pidfiles[] = {
    {"status",PROC_K_STATUS},//进程状态
    {"cmdline",PROC_K_CMDLINE},//命令行
    {"stat",PROC_K_STAT},//进程stat行
    {"environ",PROC_K_ENVIRON},//环境变量块
    {"comm",PROC_K_COMM},//进程名
    {"cgroup",PROC_K_CGROUP},//cgroup归属
    {"root",PROC_K_ROOTLINK},//根目录符号链接
};
#define PROC_NPIDFILES ((int)(sizeof(g_proc_pidfiles) / sizeof(g_proc_pidfiles[0])))

static struct inode_operations g_proc_dir_iops;//目录类操作
static struct inode_operations g_proc_link_iops;//符号链接类操作
static struct file_operations g_proc_dir_fops;//目录类文件操作
static struct file_operations g_proc_file_fops;//普通文件类文件操作

//字符串解析为PID
static int proc_parse_pid(const char *name, uint32_t len){
    if(!len || len > 10)return -1;
    uint32_t v = 0;
    for(uint32_t i = 0;i < len;i ++){
        if(name[i] < '0' || name[i] > '9')return -1;
        v = v * 10 + (uint32_t)(name[i] - '0');
    }
    if(v == 0)return -1;//0号是内核进程,不显示
    return (int)v;
}

//把名字解析成非负十进制数
static int proc_parse_num(const char *name, uint32_t len){
    if(!len || len > 10)return -1;
    uint32_t v = 0;
    for(uint32_t i = 0;i < len;i ++){
        if(name[i] < '0' || name[i] > '9')return -1;
        v = v * 10 + (uint32_t)(name[i] - '0');
    }
    return (int)v;
}

//按(kind, pid)建或取回inode
static struct inode *proc_iget(struct super_block *sb, uint64_t ino, uint32_t kind, uint32_t pid){
    struct inode *inode = IgetLocked(sb, ino);
    if(!inode)return NULL;
    //设置inode的私有信息
    proc_inode_info_t *ii = PROC_INFO(inode);
    if(!ii)return NULL;
    ii->kind = kind;
    ii->pid  = pid;
    ii->fd   = 0;
    ii->link[0] = 0;
    //设置inode的属性
    inode->i_uid = 0;
    inode->i_gid = 0;
    //进程目录的属主取进程自己
    if(kind == PROC_K_PIDDIR || kind == PROC_K_FDDIR){
        task_struct *t = TaskFind((pid_t)pid);
        if(t){
            inode->i_uid = t->cred.euid;
            inode->i_gid = t->cred.egid;
        }
    }
    inode->i_size = 0;
    inode->i_nlink = 1;
    inode->i_fop = &g_proc_file_fops;
    //根目录或或进程目录，设置为目录
    if(kind == PROC_K_ROOT || kind == PROC_K_PIDDIR || kind == PROC_K_FDDIR || kind == PROC_K_SYSDIR || kind == PROC_K_SYSKERNELDIR){
        inode->i_mode = (uint16_t)(S_IFDIR | 0555);
        inode->i_nlink = 2;
        inode->i_op = &g_proc_dir_iops;
        inode->i_fop = &g_proc_dir_fops;
    //符号链接
    }else if(kind == PROC_K_SELF || kind == PROC_K_FDLINK || kind == PROC_K_ROOTLINK){
        inode->i_mode = (uint16_t)(S_IFLNK | 0777);
        inode->i_op = &g_proc_link_iops;
        inode->i_fop = NULL;
    //其他文件
    }else if(kind == PROC_K_SYSCTL){
        uint32_t idx = (uint32_t)(ino - PROC_INO_SYSCTL(0));
        if(idx >= (uint32_t)PROC_NSYSCTL)idx = 0;
        inode->i_mode = g_proc_sysctls[idx].mode;
        inode->i_op = NULL;
    }else{
        inode->i_mode = (uint16_t)(S_IFREG | 0444);
        inode->i_op = NULL;
    }
    inode->i_state &= ~I_NEW;
    return inode;
}

//CPU厂商与型号
static int proc_gen_cpuinfo(char *out, int cap){
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    uint32_t family = (eax >> 8) & 0xF;
    uint32_t model  = (eax >> 4) & 0xF;
    if(family == 0xF)family += (eax >> 20) & 0xFF;
    if(family == 0x6 || family == 0xF)model += ((eax >> 16) & 0xF) << 4;
    return sprintf(out,"vendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\n"
                        "stepping\t: %u\n",
                   vendor, (unsigned)family, (unsigned)model, (unsigned)(eax & 0xF));
}

//内存用量
static int proc_gen_meminfo(char *out, int cap){
    uint64_t total = GetMemoryTotalSize() / 1024;
    uint64_t freeb = PmmFreeSize() / 1024;
    return sprintf(out, "MemTotal:       %8lu kB\nMemFree:        %8lu kB\n"
                        "MemAvailable:   %8lu kB\n",
                   (unsigned long)total, (unsigned long)freeb, (unsigned long)freeb);
}

//内核版本
static int proc_gen_version(char *out, int cap){
    return sprintf(out,"%s version %s (%s) %s\n",system_utsname.sysname,system_utsname.release,system_utsname.version,system_utsname.machine);
}

//开机时长
static int proc_gen_uptime(char *out, int cap){
    uint64_t ticks = SYSTEM_TimerTicks;
    uint64_t sec  = ticks / OS_TICK_HZ;
    uint64_t frac = (ticks % OS_TICK_HZ) * 100 / OS_TICK_HZ;
    return sprintf(out, "%lu.%02lu %lu.%02lu\n",(unsigned long)sec,(unsigned long)frac,(unsigned long)sec,(unsigned long)frac);
}

//挂载表
typedef struct proc_mounts_ctx {
    char *buf;
    int   cap;
    int   len;
} proc_mounts_ctx_t;

//遍历挂载点的回调,把每个挂载点写入buf
static void proc_mounts_one(struct vfsmount *m, void *ctx){
    proc_mounts_ctx_t *c = (proc_mounts_ctx_t*)ctx;
    if(!m || (c->len >= (c->cap - 1)))return;
    char path[128];
    struct path p;
    //如果挂载点有父挂载点,就从父挂载点看它的挂载目录,否则就从根目录看它的根目录
    if(m->mnt_parent && m->mnt_mountpoint){
        p.dentry = m->mnt_mountpoint;
        p.mnt = m->mnt_parent;
    }else{
        p.dentry = m->mnt_root;
        p.mnt = NULL;
    }
    if(d_path(&p, path, (int)sizeof(path)) < 0)return;
    const char *type = (m->mnt_sb && m->mnt_sb->s_type_name[0]) ? m->mnt_sb->s_type_name : "unknown";
    int n = sprintf(c->buf + c->len, "%s %s %s rw 0 0\n",path,type,type);
    if(n > 0)c->len += n;
}

//遍历挂载点,把所有挂载点写入buf
static int proc_gen_mounts(char *out, int cap){
    proc_mounts_ctx_t c;
    c.buf = out;
    c.cap = cap;
    c.len = 0;
    VfsMountWalk(proc_mounts_one, &c);
    return c.len;
}

//进程状态
static int proc_gen_status(char *out, int cap, uint32_t pid){
    task_struct *t = TaskFind((pid_t)pid);
    if(!t)return -ESRCH;
    return sprintf(out,
        "Name:\t%s\nPid:\t%d\nPPid:\t%d\nState:\t%c\n"
        "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\n",
        t->name, (int)t->pid, (int)t->parent,
        t->state == TASK_RUNNING ? 'R' : (t->state == TASK_READY ? 'R' : 'S'),
        (unsigned)t->cred.uid, (unsigned)t->cred.euid, (unsigned)t->cred.suid, (unsigned)t->cred.uid,
        (unsigned)t->cred.gid, (unsigned)t->cred.egid, (unsigned)t->cred.sgid, (unsigned)t->cred.gid);
}

//命令行
static int proc_gen_cmdline(char *out, int cap, uint32_t pid){
    task_struct *t = TaskFind((pid_t)pid);
    if(!t)return -ESRCH;
    if(!t->cmdline_len){
        int n = (int)strlen(t->name);
        if(n > cap - 2)n = cap - 2;
        memcpy(out, t->name, (size_t)n);
        out[n] = 0;
        return n + 1;
    }
    uint32_t n = t->cmdline_len;
    if(n > (uint32_t)cap)n = (uint32_t)cap;
    memcpy(out, t->cmdline, n);
    return (int)n;
}

//进程stat行
static int proc_gen_stat(char *out, int cap, uint32_t pid){
    task_struct *t = TaskFind((pid_t)pid);
    if(!t)return -ESRCH;
    char state = (t->state == TASK_RUNNING || t->state == TASK_READY) ? 'R' : 'S';
    int n = sprintf(out,"%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 0",(int)t->pid, t->name, state, (int)t->parent, (int)t->pid, (int)t->pid);
    for(int i = 25;i <= 52 && n < cap - 3;i ++)n += sprintf(out + n, " 0");
    return n;
}

//环境变量块
static int proc_gen_environ(char *out, int cap){
    static const char env[] =
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "HOME=/\0"
        "TERM=linux\0";
    int len = (int)sizeof(env) - 1;
    if(len > cap)len = cap;
    memcpy(out, env, (size_t)len);
    return len;
}

//进程名(带换行)
static int proc_gen_comm(char *out, int cap, uint32_t pid){
    task_struct *t = TaskFind((pid_t)pid);
    if(!t)return -ESRCH;
    return sprintf(out, "%s\n", t->name);
}

//cgroup归属
static int proc_gen_cgroup(char *out, int cap){
    return sprintf(out, "0::/\n");
}

//详细挂载表
typedef struct proc_mountinfo_ctx {
    char *buf;
    int   cap;
    int   len;
    int   id;
} proc_mountinfo_ctx_t;

static void proc_mountinfo_one(struct vfsmount *m, void *ctx){
    proc_mountinfo_ctx_t *c = (proc_mountinfo_ctx_t*)ctx;
    if(!m || (c->len >= (c->cap - 1)))return;
    char path[128];
    struct path p;
    if(m->mnt_parent && m->mnt_mountpoint){
        p.dentry = m->mnt_mountpoint;
        p.mnt = m->mnt_parent;
    }else{
        p.dentry = m->mnt_root;
        p.mnt = NULL;
    }
    if(d_path(&p, path, (int)sizeof(path)) < 0)return;
    const char *type = (m->mnt_sb && m->mnt_sb->s_type_name[0]) ? m->mnt_sb->s_type_name : "unknown";
    c->id ++;
    int n = sprintf(c->buf + c->len, "%d %d 0:41 / %s rw - %s %s rw\n",c->id, 0, path, type, type);
    if(n > 0)c->len += n;
}

static int proc_gen_mountinfo(char *out, int cap){
    proc_mountinfo_ctx_t c;
    c.buf = out;
    c.cap = cap;
    c.len = 0;
    c.id  = 0;
    VfsMountWalk(proc_mountinfo_one, &c);
    return c.len;
}

//内核命令行
static int proc_gen_cmdline_root(char *out, int cap){
    return sprintf(out, "root=/dev/nvme0n1p2 rw console=ttyS0\n");
}

//cgroup控制器列表
static int proc_gen_cgroups(char *out, int cap){
    return sprintf(out,
        "#subsys_name\thierarchy\tnum_cgroups\tenabled\n"
        "cpuset\t0\t1\t1\n"
        "cpu\t0\t1\t1\n"
        "memory\t0\t1\t1\n"
        "pids\t0\t1\t1\n");
}

//已注册的文件系统列表
static int proc_gen_filesystems(char *out, int cap){
    return sprintf(out,
        "nodev\tsysfs\n"
        "nodev\tproc\n"
        "nodev\ttmpfs\n"
        "nodev\tdevtmpfs\n"
        "nodev\tcgroup2\n"
        "\text2\n"
        "\text4\n"
        "\tvfat\n");
}

// /proc/sys/kernel/* 内容
static int proc_gen_sysctl(char *out, int cap, uint32_t idx){
    if(idx >= (uint32_t)PROC_NSYSCTL)return -EINVAL;
    switch(g_proc_sysctls[idx].gen){
        case PSYS_OSRELEASE:        return sprintf(out, "%s\n", system_utsname.release);
        case PSYS_OSTYPE:           return sprintf(out, "%s\n", system_utsname.sysname);
        case PSYS_VERSION:          return sprintf(out, "%s\n", system_utsname.version);
        case PSYS_CAP_LAST_CAP:     return sprintf(out, "40\n");
        case PSYS_PRINTK_DEVKMSG:   return sprintf(out, "ratelimit\n");
        case PSYS_CORE_PATTERN:     return sprintf(out, "core\n");
        case PSYS_HOSTNAME:         return sprintf(out, "%s\n", system_utsname.nodename);
        case PSYS_DOMAINNAME:       return sprintf(out, "localdomain\n");
        case PSYS_CONSOLE_LOGLEVEL: return sprintf(out, "7\t4\t1\t7\n");
        default:                    return -EINVAL;
    }
}

//按种类生成内容
static int proc_gen(char *out, int cap, const proc_inode_info_t *ii){
    switch(ii->kind){
        case PROC_K_MEMINFO: return proc_gen_meminfo(out, cap);
        case PROC_K_CPUINFO: return proc_gen_cpuinfo(out, cap);
        case PROC_K_VERSION: return proc_gen_version(out, cap);
        case PROC_K_UPTIME:  return proc_gen_uptime(out, cap);
        case PROC_K_MOUNTS:  return proc_gen_mounts(out, cap);
        case PROC_K_MOUNTINFO: return proc_gen_mountinfo(out, cap);
        case PROC_K_CMDLINE_ROOT: return proc_gen_cmdline_root(out, cap);
        case PROC_K_FILESYSTEMS:  return proc_gen_filesystems(out, cap);
        case PROC_K_CGROUPS:      return proc_gen_cgroups(out, cap);
        case PROC_K_SYSCTL:       return proc_gen_sysctl(out, cap, ii->fd);
        case PROC_K_STATUS:  return proc_gen_status(out, cap, ii->pid);
        case PROC_K_CMDLINE: return proc_gen_cmdline(out, cap, ii->pid);
        case PROC_K_STAT:    return proc_gen_stat(out, cap, ii->pid);
        case PROC_K_ENVIRON: return proc_gen_environ(out, cap);
        case PROC_K_COMM:    return proc_gen_comm(out, cap, ii->pid);
        case PROC_K_CGROUP:  return proc_gen_cgroup(out, cap);
        default:             return -EINVAL;
    }
}

//读
static int64_t proc_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    if(!f || !f->f_inode || !buf || !pos)return -EINVAL;
    proc_inode_info_t *ii = PROC_INFO(f->f_inode);
    if(!ii)return -EINVAL;
    //生成内容到临时缓冲区
    char tmp[PROC_BUF_MAX];
    int n = proc_gen(tmp, (int)sizeof(tmp), ii);
    if(n < 0)return n;
    //从临时缓冲区拷贝到用户缓冲区
    uint64_t off = *pos;
    if(off >= (uint64_t)n)return 0;
    uint64_t k = (uint64_t)n - off;
    if(k > len)k = len;
    memcpy(buf, tmp + off, (size_t)k);
    *pos = off + k;
    return (int64_t)k;
}

//符号链接的目标
static int proc_get_link(struct dentry *dentry, struct inode *inode, const char **target){
    (void)dentry;
    proc_inode_info_t *ii = inode ? PROC_INFO(inode) : NULL;
    if(!ii || !target)return -EINVAL;
    ii->link[0] = 0;
    if(ii->kind == PROC_K_SELF){
        int pid = current_task ? (int)current_task->pid : 0;
        sprintf(ii->link, "%d", pid);
        *target = ii->link;
        return 0;
    }
    if(ii->kind == PROC_K_ROOTLINK){
        ii->link[0] = '/';
        ii->link[1] = 0;
        *target = ii->link;
        return 0;
    }
    if(ii->kind == PROC_K_FDLINK){
        task_struct *t = TaskFind((pid_t)ii->pid);
        if(!t)return -ESRCH;
        if(ii->fd >= (uint32_t)MAX_FD || !t->files[ii->fd].used)return -ENOENT;
        fs_file_t *ff = &t->files[ii->fd];
        if(ff->dentry){
            struct path p;
            p.dentry = ff->dentry;
            p.mnt    = ff->mnt;
            if(d_path(&p, ii->link, (int)sizeof(ii->link)) < 0)ii->link[0] = 0;
        }
        if(!ii->link[0])sprintf(ii->link, "anon_inode:[%d]", (int)ii->fd);
        *target = ii->link;
        return 0;
    }
    return -EINVAL;
}

//按名字造一个inode并挂到dentry上
static struct dentry *proc_add(struct inode *dir,struct dentry *dentry,uint64_t ino,uint32_t kind,uint32_t pid,uint32_t fd){
    struct inode *inode = proc_iget(dir->i_sb, ino, kind, pid);
    if(!inode){
        d_add(dentry, NULL);
        return NULL;
    }
    proc_inode_info_t *ii = PROC_INFO(inode);
    if(ii)ii->fd = fd;
    d_add(dentry, inode);
    Iput(inode);
    return dentry;
}

//按名字查找子文件或子目录
static struct dentry *proc_lookup(struct inode *dir, struct dentry *dentry, unsigned flags){
    (void)flags;
    proc_inode_info_t *di = PROC_INFO(dir);
    if(!di){
        d_add(dentry, NULL);
        return NULL;
    }
    const char *name = dentry->d_name;
    uint32_t len = dentry->d_name_len;
    //根目录下的文件或目录
    if(di->kind == PROC_K_ROOT){
        if(len == 4 && memcmp(name, "self", 4) == 0)return proc_add(dir, dentry, PROC_INO_FIXED(PROC_NFILES), PROC_K_SELF, 0, 0);
        if(len == 3 && memcmp(name, "sys", 3) == 0)return proc_add(dir, dentry, PROC_INO_SYSDIR, PROC_K_SYSDIR, 0, 0);
        //固定文件
        for(int i = 0;i < PROC_NFILES;i ++){
            if((strlen(g_proc_files[i].name) == len) && (memcmp(g_proc_files[i].name, name, len) == 0))return proc_add(dir, dentry, PROC_INO_FIXED(i), g_proc_files[i].kind, 0, 0);
        }
        //进程目录
        int pid = proc_parse_pid(name, len);
        if(pid > 0 && TaskFind((pid_t)pid))return proc_add(dir, dentry, PROC_INO_PID(pid), PROC_K_PIDDIR, (uint32_t)pid, 0);
    //进程目录下的文件
    }else if(di->kind == PROC_K_PIDDIR){
        for(int i = 0;i < PROC_NPIDFILES;i ++){
            if((strlen(g_proc_pidfiles[i].name) == len) && (memcmp(g_proc_pidfiles[i].name, name, len) == 0)){
                if(!TaskFind((pid_t)di->pid))break;
                return (proc_add(dir, dentry, PROC_INO_PIDFILE(di->pid, i + 1),g_proc_pidfiles[i].kind, di->pid, 0));
            }
        }
        if(len == 2 && memcmp(name, "fd", 2) == 0 && TaskFind((pid_t)di->pid))return proc_add(dir, dentry, PROC_INO_FDDIR(di->pid), PROC_K_FDDIR, di->pid, 0);//fd目录
    }else if(di->kind == PROC_K_SYSDIR){
        if(len == 6 && memcmp(name, "kernel", 6) == 0)return proc_add(dir, dentry, PROC_INO_SYSKERNELDIR, PROC_K_SYSKERNELDIR, 0, 0);
    }else if(di->kind == PROC_K_SYSKERNELDIR){
        for(int i = 0;i < PROC_NSYSCTL;i ++){
            if((strlen(g_proc_sysctls[i].name) == len) && (memcmp(g_proc_sysctls[i].name, name, len) == 0))
                return proc_add(dir, dentry, PROC_INO_SYSCTL(i), PROC_K_SYSCTL, 0, (uint32_t)i);
        }
    //fd目录下的描述符
    }else if(di->kind == PROC_K_FDDIR){
        task_struct *t = TaskFind((pid_t)di->pid);
        int fd = proc_parse_num(name, len);
        if(t && fd >= 0 && fd < MAX_FD && t->files[fd].used)
            return proc_add(dir, dentry, PROC_INO_FDLINK(di->pid, fd), PROC_K_FDLINK, di->pid, (uint32_t)fd);
    }
    d_add(dentry, NULL);//负缓存
    return NULL;
}

//列进程目录时把pid报上去
static int proc_emit_pid(task_struct *t, void *ctx){
    void **args = (void**)ctx;
    int (*emit)(void*, const char*, int, uint64_t, int, int64_t) = args[0];
    void *c = args[1];
    int64_t *off = (int64_t*)args[2];
    if(!t || t->pid <= 0)return 0;
    char name[12];
    int n = sprintf(name, "%d", (int)t->pid);
    emit(c, name, n, PROC_INO_PID(t->pid), S_IFDIR, (*off) ++);
    return 0;
}

//遍历目录
static int proc_iterate(struct file *f,void *ctx,int (*emit)(void *ctx,const char *name,int namelen,uint64_t ino,int type,int64_t next_off)){
    if(!f || !f->f_inode || !emit)return -EINVAL;
    proc_inode_info_t *ii = PROC_INFO(f->f_inode);
    if(!ii)return -EINVAL;
    int64_t off = 0;
    if(ii->kind == PROC_K_ROOT){
        emit(ctx, "self", 4, PROC_INO_FIXED(PROC_NFILES), S_IFLNK, off ++);
        for(int i = 0;i < PROC_NFILES;i ++)emit(ctx, g_proc_files[i].name, (int)strlen(g_proc_files[i].name),PROC_INO_FIXED(i), S_IFREG, off ++);
        void *args[3];
        args[0] = (void*)emit;
        args[1] = ctx;
        args[2] = &off;
        TaskForEach(proc_emit_pid, args);
    }else if(ii->kind == PROC_K_PIDDIR){
        for(int i = 0;i < PROC_NPIDFILES;i ++)emit(ctx, g_proc_pidfiles[i].name,(int)strlen(g_proc_pidfiles[i].name),PROC_INO_PIDFILE(ii->pid, i + 1), S_IFREG, off ++);
        emit(ctx, "fd", 2, PROC_INO_FDDIR(ii->pid), S_IFDIR, off ++);
    }else if(ii->kind == PROC_K_SYSDIR){
        emit(ctx, "kernel", 6, PROC_INO_SYSKERNELDIR, (int)S_IFDIR, 0);
    }else if(ii->kind == PROC_K_SYSKERNELDIR){
        for(int i = 0;i < PROC_NSYSCTL;i ++)
            emit(ctx, g_proc_sysctls[i].name, (int)strlen(g_proc_sysctls[i].name),
                 PROC_INO_SYSCTL(i), (int)S_IFREG, off ++);
    }else if(ii->kind == PROC_K_FDDIR){
        task_struct *t = TaskFind((pid_t)ii->pid);
        if(t){
            for(int fd = 0;fd < MAX_FD;fd ++){
                if(!t->files[fd].used)continue;
                char name[12];
                int n = sprintf(name, "%d", fd);
                emit(ctx, name, n, PROC_INO_FDLINK(ii->pid, fd), S_IFLNK, off ++);
            }
        }
    }
    return 0;
}

static struct inode_operations g_proc_dir_iops = {
    .lookup = proc_lookup,
};

static struct inode_operations g_proc_link_iops = {
    .get_link = proc_get_link,
};

static struct file_operations g_proc_dir_fops = {
    .iterate = proc_iterate,
};

static int64_t proc_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    if(!f || !f->f_inode)return -EINVAL;
    proc_inode_info_t *ii = PROC_INFO(f->f_inode);
    if(!ii)return -EINVAL;
    if(ii->kind == PROC_K_SYSCTL){
        if((g_proc_sysctls[ii->fd].mode & 0222) == 0)return -EPERM;
    }else if(ii->kind != PROC_K_CMDLINE_ROOT && ii->kind != PROC_K_FILESYSTEMS){
        return -EPERM;
    }
    if(pos)*pos += len;
    return (int64_t)len;
}

static struct file_operations g_proc_file_fops = {
    .read  = proc_read,
    .write = proc_write,
};

uint32_t ProcFsInodeSize(void){
    return (uint32_t)(sizeof(struct inode) + sizeof(proc_inode_info_t));
}

const struct super_operations *ProcFsSuperOps(void){
    return NULL;
}

//建立procfs的超级块与根目录
struct dentry *ProcFsFillSuper(struct super_block *sb){
    if(!sb)return NULL;
    struct inode *root = proc_iget(sb, PROC_INO_ROOT, PROC_K_ROOT, 0);
    if(!root)return NULL;
    struct dentry *d = d_alloc(NULL, "/");
    if(!d)return NULL;
    d_add(d, root);
    Iput(root);
    sb->s_root = d;
    sb->s_root_inode = root;
    return d;
}
