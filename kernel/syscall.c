#include <syscalls.h>
#include <desc.h>
#include <serial.h>
#include <print.h>
#include <task.h>
#include <klib.h>
#include <mm/vmm.h>
#include <drives/tty.h>
#include <drives/ps2kbd.h>
#include <idt.h>
#include <fork.h>
#include <fs.h>
#include <futex.h>
#include <rtc.h>
#include <delay.h>

static inline uint64_t rdmsr(uint32_t msr){
	uint32_t low, high;
	__asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}
static inline void wrmsr(uint32_t msr, uint64_t val){
	uint32_t low = (uint32_t)val;
	uint32_t high = (uint32_t)(val >> 32);
	__asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr) : "memory");
}

#define IA32_EFER   0xC0000080
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_FMASK  0xC0000084
#define IA32_FS_BASE 0xC0000100

volatile int cpu_nx_enabled = 0;

//从用户空间复制
uint64_t copy_from_user(void *to, const void *from, uint64_t n){
    uintptr_t uaddr = (uintptr_t)from;
    if (n == 0) return 0;
    if (!to || !from) return n;
    if (uaddr >= USER_VADDR_MAX || uaddr + n > USER_VADDR_MAX) return n;
    memcpy(to, from, n);
    return 0;
}

//复制到用户空间
uint64_t copy_to_user(void *to, const void *from, uint64_t n){
    uintptr_t uaddr = (uintptr_t)to;
    if (n == 0) return 0;
    if (!to || !from) return n;
    if (uaddr >= USER_VADDR_MAX || uaddr + n > USER_VADDR_MAX) return n;
    memcpy(to, from, n);
    return 0;
}

/*
 * ssize_t read(int fd, void *buf, size_t count)
 * 系统调用:read
 * 读取键盘或文件
 */
static long sys_read(long fd, long buf, long count, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(count < 0)return -EINVAL;
	if(!buf)return -EFAULT;
	//键盘读取
	if(fd == 0) {
		if(count == 0)return 0;
		sti();
		char c = GetKey();//读取按键
		if(copy_to_user((void*)buf, &c, 1))return -EFAULT;//复制到用户空间
		return 1;
	}
	//文件读取
	if(!current_task)return -EBADF;
	if(fd < 3 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
	if((current_task->files[fd].flags & O_ACCMODE) == O_WRONLY)return -EBADF;
	long total = 0;
	char kbuf[512];
	while(total < count) {
		long chunk = count - total;
		if(chunk > (long)sizeof(kbuf))chunk = sizeof(kbuf);
		uint64_t n = FsRead(&current_task->files[fd], kbuf, (uint64_t)chunk);//读取内容
		if(n == 0)break;//EOF
		if(copy_to_user((char*)buf + total, kbuf, n))return -EFAULT;//复制到用户空间
		total += (long)n;
	}
	return total;
}

/*
 * ssize_t write(int fd, const void *buf, size_t count)
 * 系统调用：write
 * 写入到TTY或文件
 */
static long sys_write(long fd, long buf, long count, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(count < 0) return -EINVAL;
	if(!buf) return -EFAULT;
	//输出到用户缓冲区和TTY
	if((fd == 1) || (fd == 2)) {
		char kbuf[512];
		long written = 0;
		while (written < count) {
			long chunk = count - written;
			if(chunk > (long)sizeof(kbuf))chunk = sizeof(kbuf);
			if(copy_from_user(kbuf, (const char*)buf + written, (unsigned long)chunk))return -EFAULT;
			for(long i = 0; i < chunk; i++){
				TTY_PrintChar(kbuf[i], CurrentConsoleStyle.TextColor);
			}
			written += chunk;
		}
		return written;
	}
	//文件写入
	if(!current_task)return -EBADF;
	if(fd < 3 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
	if((current_task->files[fd].flags & O_ACCMODE) == O_RDONLY)return -EBADF;
	long total = 0;
	char kbuf[512];
	while(total < count){
		long chunk = count - total;
		if(chunk > (long)sizeof(kbuf))chunk = sizeof(kbuf);
		if(copy_from_user(kbuf, (const char*)buf + total, (unsigned long)chunk))return -EFAULT;//复制到内核空间
		uint64_t n = FsWrite(&current_task->files[fd], kbuf, (uint64_t)chunk);//写入文件
		if(n == 0)return -ENOSPC;
		total += (long)n;
	}
	return total;
}

/*
 * int open(const char *pathname, int flags, mode_t mode)
 * 系统调用：open
 * 打开文件
 */
static long sys_open(long path, long flags, long mode, long a4, long a5, long a6){
	(void)mode; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	//复制路径
	char kpath[256];
	if(copy_from_user(kpath, (void*)path, 255))return -EFAULT;
	kpath[255] = 0;
	//分配文件描述符
	int fd;
	for(fd = 3; fd < MAX_FD; fd++) {
		if(!current_task->files[fd].used)break;
	}
	if (fd >= MAX_FD)return -ENFILE;
	fs_file_t *f = &current_task->files[fd];
	if (FsOpen(kpath, (int)flags, f) != 0)return -ENOENT;//打开文件
	return fd;
}

/*
 * int close(int fd)
 * 系统调用:close
 * 关闭文件
 */
static long sys_close(long fd, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if (!current_task) return -EBADF;
	if (fd < 3 || fd >= MAX_FD || !current_task->files[fd].used) return -EBADF;
	FsClose(&current_task->files[fd]);
	return 0;
}

/*
 * int mkdir(const char *pathname, mode_t mode)
 */
static long sys_mkdir(long path, long mode, long unused, long a4, long a5, long a6){
	(void)unused; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	char kpath[256];
	if(copy_from_user(kpath, (void*)path, 255))return -EFAULT;
	kpath[255] = 0;
	return FsMkdir(kpath, (int)mode) ? -ENOENT : 0;
}

/*
 * int rmdir(const char *pathname)
 */
static long sys_rmdir(long path, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	char kpath[256];
	if(copy_from_user(kpath, (void*)path, 255))return -EFAULT;
	kpath[255] = 0;
	return FsUnlink(kpath) ? -ENOENT : 0;
}

/*
 * int unlink(const char *pathname)
 */
static long sys_unlink(long path, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	char kpath[256];
	if(copy_from_user(kpath, (void*)path, 255))return -EFAULT;
	kpath[255] = 0;
	return FsUnlink(kpath) ? -ENOENT : 0;
}

/*
 * off_t lseek(int fd, off_t offset, int whence)
 * 系统调用:lseek
 * 移动文件指针
 */
static long sys_lseek(long fd, long off, long whence, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if (!current_task) return -EBADF;
	if (fd < 3 || fd >= MAX_FD || !current_task->files[fd].used) return -EBADF;
	int r = FsSeek(&current_task->files[fd], off, (int)whence);
	if (r < 0) return -EINVAL;
	return r;
}
/*DeepSeek V4 Flash*/
//填充Linux x86_64 struct stat
static void fill_stat(stat_t *st, fs_node_t *node){
	memset(st, 0, sizeof(*st));
	st->st_dev = 1;
	st->st_ino = (uint64_t)(uintptr_t)node;//节点指针作伪inode
	st->st_nlink = 1;
	st->st_mode = (node->type == FT_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
	st->st_size = (int64_t)node->size;
	st->st_blksize = 512;
	st->st_blocks = ((uint64_t)node->size + 511) / 512;
	uint64_t now = rtc_get_epoch();
	st->st_atime = (int64_t)now;
	st->st_mtime = (int64_t)now;
	st->st_ctime = (int64_t)now;
}

/*
 * int stat(const char *path, struct stat *buf)
 */
static long sys_newstat(long path, long buf, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -ENOSYS;
	if(!path || !buf)return -EFAULT;
	char kpath[256];
	if(copy_from_user(kpath, (void*)path, 255))return -EFAULT;
	kpath[255] = 0;
	fs_node_t *node = FsResolve(kpath);
	if(!node)return -ENOENT;
	stat_t st;
	fill_stat(&st, node);
	if(copy_to_user((void*)buf, &st, sizeof(st)))return -EFAULT;
	return 0;
}

/*
 * int fstat(int fd, struct stat *buf)
 */
static long sys_newfstat(long fd, long buf, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -EBADF;
	if(!buf)return -EFAULT;
	stat_t st;
	if(fd < 3){
		//标准输入/输出/错误: 当作字符设备(glibc据此判断缓冲策略)
		memset(&st, 0, sizeof(st));
		st.st_dev = 1;
		st.st_ino = (uint64_t)(uintptr_t)&current_task->files[fd];
		st.st_nlink = 1;
		st.st_mode = S_IFCHR | 0666;
		st.st_rdev = 1;
		st.st_blksize = 512;
	}else{
		if(fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
		fill_stat(&st, current_task->files[fd].node);
	}
	if(copy_to_user((void*)buf, &st, sizeof(st)))return -EFAULT;
	return 0;
}

/*
 * int access(const char *path, int mode)
 * 权限暂不检查, 只检查存在性(F_OK=0)
 */
static long sys_access(long path, long mode, long a3, long a4, long a5, long a6){
	(void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -ENOSYS;
	if(!path)return -EFAULT;
	char kpath[256];
	if(copy_from_user(kpath, (void*)path, 255))return -EFAULT;
	kpath[255] = 0;
	if(!FsResolve(kpath))return -ENOENT;
	return 0;
}

/*
 * ssize_t readlink(const char *path, char *buf, size_t bufsiz)
 * 无符号链接支持, 一律-EINVAL
 */
static long sys_readlink(long path, long buf, long bufsiz, long a4, long a5, long a6){
	(void)path; (void)buf; (void)bufsiz; (void)a4; (void)a5; (void)a6;
	return -EINVAL;
}

/*
 * char *getcwd(char *buf, size_t size)
 * 成功返回字符串长度(含结尾NUL, Linux内核ABI语义)
 */
static long sys_getcwd(long buf, long size, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -EFAULT;
	if(!buf || size <= 0)return -EFAULT;
	int len = strlen(current_task->cwd);
	if((uint64_t)len + 1 > (uint64_t)size)return -ERANGE;
	if(copy_to_user((void*)buf, current_task->cwd, (uint64_t)len + 1))return -EFAULT;
	return (long)len + 1;//Linux: 返回写入长度(含NUL)
}

/*
 * int chdir(const char *path)
 */
static long sys_chdir(long path, long a2, long a3, long a4, long a5, long a6){
	(void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -ENOSYS;
	if(!path)return -EFAULT;
	char kpath[256];
	if(copy_from_user(kpath, (void*)path, 255))return -EFAULT;
	kpath[255] = 0;
	fs_node_t *node = FsResolve(kpath);
	if(!node)return -ENOENT;
	if(node->type != FT_DIR)return -ENOTDIR;
	int len = strlen(kpath);
	if(len > 255)len = 255;
	memcpy(current_task->cwd, kpath, (uint64_t)len);
	current_task->cwd[len] = 0;
	return 0;
}
/*DeepSeek V4 Flash-END*/
/*DeepSeek V4 Pro*/
/*
 * int ioctl(int fd, unsigned long request, ...)
 * 无终端ioctl支持, 一律-ENOTTY(musl/glibc把ENOTTY当"非终端"处理, 无碍)
 */
static long sys_ioctl(long fd, long request, long arg, long a4, long a5, long a6){
	(void)fd; (void)request; (void)arg; (void)a4; (void)a5; (void)a6;
	return -ENOTTY;
}

/*
 * ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
 * 聚合写(TTY输出或文件写入), 返回写入总字节数
 */
static long sys_writev(long fd, long iov, long iovcnt, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!iov || iovcnt <= 0 || iovcnt > 256)return -EINVAL;
	struct iovec vec[256];
	if(copy_from_user(vec, (void*)iov, (uint64_t)iovcnt * sizeof(struct iovec)))return -EFAULT;
	long total = 0;
	for(int i = 0; i < iovcnt; i++){
		uint64_t len = vec[i].iov_len;
		if(!len)continue;
		uintptr_t base = (uintptr_t)vec[i].iov_base;
		if(base >= USER_VADDR_MAX || base + len > USER_VADDR_MAX)return total ? total : -EFAULT;
		char kbuf[512];
		uint64_t done = 0;
		while(done < len){
			uint64_t chunk = len - done;
			if(chunk > sizeof(kbuf))chunk = sizeof(kbuf);
			if(copy_from_user(kbuf, (const char*)base + done, chunk))return total ? total : -EFAULT;
			if((fd == 1) || (fd == 2)){
				//TTY输出
				for(uint64_t j = 0; j < chunk; j++)TTY_PrintChar(kbuf[j], CurrentConsoleStyle.TextColor);
			}else{
				//文件写入
				if(!current_task)return -EBADF;
				if(fd < 3 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
				if((current_task->files[fd].flags & O_ACCMODE) == O_RDONLY)return -EBADF;
				FsWrite(&current_task->files[fd], kbuf, chunk);
			}
			done += chunk;
			total += (long)chunk;
		}
	}
	return total;
}

/*
 * int set_robust_list(struct robust_list_head *head, size_t len)
 * 单核内核无健壮互斥需求, 忽略指针返回0
 */
static long sys_set_robust_list(long head, long len, long a3, long a4, long a5, long a6){
	(void)head; (void)len; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOSYS;
	return 0;
}

/*
 * int prlimit64(pid_t pid, int resource, const struct rlimit64 *new, struct rlimit64 *old)
 * 仅支持当前进程查询; 栈限制返回8MB/无上限, 其余无上限; 设置忽略
 */
static long sys_prlimit64(long pid, long resource, long newlim, long oldlim, long a5, long a6){
	(void)newlim; (void)a5; (void)a6;
	if(pid != 0)return -EINVAL;//只支持当前进程
	if(oldlim){
		rlimit64_t rl;
		rl.rlim_cur = (resource == RLIMIT_STACK) ? (8ULL * 1024 * 1024) : ~0ULL;
		rl.rlim_max = ~0ULL;
		if(copy_to_user((void*)oldlim, &rl, sizeof(rl)))return -EFAULT;
	}
	return 0;
}

//伪随机数状态(xorshift64, 种子来自TSC)
static uint64_t g_rng_state = 0;

/*
 * ssize_t getrandom(void *buf, size_t len, unsigned flags)
 * 伪随机填充(GRND_NONBLOCK等标志忽略), 返回写入字节数
 */
static long sys_getrandom(long buf, long len, long flags, long a4, long a5, long a6){
	(void)flags; (void)a4; (void)a5; (void)a6;
	if(!buf || len < 0)return -EFAULT;
	if(len == 0)return 0;
	if(!g_rng_state)g_rng_state = rdtsc() | 1;
	uint8_t kbuf[128];
	long total = 0;
	while(total < len){
		long chunk = len - total;
		if(chunk > (long)sizeof(kbuf))chunk = sizeof(kbuf);
		for(long i = 0; i < chunk; i++){
			g_rng_state ^= g_rng_state << 13;
			g_rng_state ^= g_rng_state >> 7;
			g_rng_state ^= g_rng_state << 17;
			kbuf[i] = (uint8_t)g_rng_state;
		}
		if(copy_to_user((char*)buf + total, kbuf, (uint64_t)chunk))return total ? total : -EFAULT;
		total += chunk;
	}
	return total;
}

/*
 * ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz)
 * 无/proc, 一律-ENOENT
 */
static long sys_readlinkat(long dirfd, long path, long buf, long bufsiz, long a5, long a6){
	(void)dirfd; (void)path; (void)buf; (void)bufsiz; (void)a5; (void)a6;
	return -ENOENT;
}
/*DeepSeek V4 Pro-END*/

/*
 * int nanosleep(const struct timespec *req, struct timespec *rem)
 */
static long sys_nanosleep(long req, long rem, long unused, long a4, long a5, long a6){	(void)unused; (void)a4; (void)a5; (void)a6;
	timespec_t ts;
	if (!req) return -EFAULT;
	if (copy_from_user(&ts, (void*)req, sizeof(ts))) return -EFAULT;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -EINVAL;
	uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
	msleep(ms);
	if (rem) {
		timespec_t zero = {0, 0};
		if (copy_to_user((void*)rem, &zero, sizeof(zero))) return -EFAULT;
	}
	return 0;
}

//进程退出
static long __attribute__((noreturn)) do_exit(long status){
	if(current_task && current_task->mm){
		//通知clear_child_tid(写入0, 供futex/线程库检测退出)
		if(current_task->clear_child_tid){
			uint32_t zero = 0;
			copy_to_user(current_task->clear_child_tid, &zero, sizeof(zero));
		}
		current_task->exit_code = (int)status;//保存退出码
		TaskExit();//退出任务
	}
	SYSTEM_STOP();
}

/*
 * void _exit(int status)
 * 退出当前线程
 */
static long sys_exit(long status, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	do_exit(status);
}

/*
 * void exit_group(int status)
 * 退出整个线程组
 */
static long sys_exit_group(long status, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	do_exit(status);
}

/*
 * pid_t getpid(void)
 * 系统调用:getpid
*/
static long sys_getpid(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if (!current_task) return -1;
	return (long)current_task->pid;
}

/*
 * pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *ru)
 * 等待子进程退出并回收其资源
 */
long sys_waitpid(long pid, long wstatus, long options, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -ECHILD;
	pid_t child_pid = (pid_t)pid;
	if(child_pid == 0 || child_pid < -1)return -EINVAL;
	task_struct *zombie;
	for(;;){
		//1.先扫描已退出的僵尸子进程
		zombie = TaskFindZombie(current_task->pid, child_pid);
		if(zombie)break;
		//2.指定子进程不存在则报错
		if(child_pid > 0 && !TaskFindChild(current_task->pid, child_pid))return -ECHILD;
		//3.WNOHANG:不阻塞
		if(options & WNOHANG)return 0;
		//4.挂到当前任务的子进程等待队列
		wait_queue_head_t *wq = &current_task->child_wq;
		current_task->wait_node.prev = wq->prev;
		current_task->wait_node.next = wq;
		wq->prev->next = &current_task->wait_node;
		wq->prev = &current_task->wait_node;
		current_task->state = TASK_BLOCKED;
		//5.双检:挂队列期间子进程可能恰好退出(wake_up已错过)
		zombie = TaskFindZombie(current_task->pid, child_pid);
		if(zombie){
			//自摘除等待队列
			current_task->wait_node.prev->next = current_task->wait_node.next;
			current_task->wait_node.next->prev = current_task->wait_node.prev;
			current_task->wait_node.prev = NULL;
			current_task->wait_node.next = NULL;
			current_task->state = TASK_RUNNING;
			break;
		}
		schedule();//让出CPU,被子进程退出唤醒后重新扫描
	}
	//回收僵尸:先复制退出码再释放资源
	int code = zombie->exit_code;
	pid_t ret = zombie->pid;
	if(wstatus){
		if(copy_to_user((void*)wstatus, &code, sizeof(code)))return -EFAULT;
	}
	TaskKill(zombie);//释放内核栈/mm/task_struct
	return ret;
}

//系统调用表
typedef long (*syscall_fn)(long, long, long, long, long, long);
static syscall_fn syscall_table[SYSCALL_TABLE_SIZE];

//系统调用分发器
long syscall_dispatch(long num, long a1, long a2, long a3, long a5, long a6){
	if(num < 0 || num >= SYSCALL_TABLE_SIZE)return -ENOSYS;//检查调用号
    //获取处理函数
	syscall_fn fn = syscall_table[num];
	if(!fn)return -ENOSYS;
	long a4 = syscall_arg6;//参数4(r10)
	return fn(a1, a2, a3, a4, a5, a6);//调用处理函数
}

/*
 * void *brk(void *addr)
 * 设置/获取program break(堆顶)
 */
static long sys_brk(long addr, long a2, long a3, long a4, long a5, long a6){
	(void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -ENOMEM;
	mm_struct *mm = current_task->mm;
	uintptr_t new_brk = (uintptr_t)addr;
	if(new_brk == 0)return (long)mm->brk;//为0时查询当前brk
	if(new_brk < mm->start_brk)return (long)mm->brk;//不能低于start_brk
	//大于原堆，扩展堆
	if(new_brk > mm->brk){
		//映射新页
		uintptr_t start = (mm->brk + PAGE_SIZE - 1) & PAGE_MASK;
		uintptr_t end = (new_brk + PAGE_SIZE - 1) & PAGE_MASK;
		if(end > start){
			if(!vmm_mmap(mm, start, end - start, VM_READ | VM_WRITE))return (long)mm->brk;
		}
	//小于原堆，收缩堆
	}else if(new_brk < mm->brk){
		//释放页
		uintptr_t start = (new_brk + PAGE_SIZE - 1) & PAGE_MASK;
		uintptr_t end = (mm->brk + PAGE_SIZE - 1) & PAGE_MASK;
		if(end > start)vmm_munmap(mm, start, end - start);
	}
	mm->brk = new_brk;//更新brk
	return (long)new_brk;
}

/*
 * void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
 * 仅支持匿名私有映射(fd=-1或MAP_ANONYMOUS)
 */
static uintptr_t g_mmap_hint = 0x10000000;
static long sys_mmap(long addr, long length, long prot, long flags, long fd, long offset){
	(void)offset;
	if(!current_task || !current_task->mm)return -ENOMEM;
	if(length <= 0)return -EINVAL;
	if((long)fd >= 0 && !(flags & MAP_ANONYMOUS))return -ENODEV;//暂不支持文件映射
	//权限转换
	uint64_t vm_flags = 0;
	if(prot & PROT_READ) vm_flags |= VM_READ;
	if(prot & PROT_WRITE) vm_flags |= VM_WRITE;
	if(prot & PROT_EXEC) vm_flags |= VM_EXEC;
	//确定映射地址
	uintptr_t vaddr;
	if(addr != 0){
		vaddr = (uintptr_t)addr & PAGE_MASK;
	}else{
		vaddr = g_mmap_hint;
		g_mmap_hint += ((uint64_t)length + PAGE_SIZE) & PAGE_MASK;
	}
	void *r = vmm_mmap(current_task->mm, vaddr, (uint64_t)length, vm_flags);
	if(!r) return -ENOMEM;
	return (long)r;
}

/*
 * int munmap(void *addr, size_t length)
 */
static long sys_munmap(long addr, long length, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -EINVAL;
	if(length <= 0)return -EINVAL;
	return vmm_munmap(current_task->mm, (uintptr_t)addr, (uint64_t)length);
}

/*
 * int mprotect(void *addr, size_t len, int prot)
 */
static long sys_mprotect(long addr, long len, long prot, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm) return -ENOMEM;
	if(len <= 0) return -EINVAL;
	uint64_t vm_flags = 0;
	if(prot & PROT_READ) vm_flags |= VM_READ;
	if(prot & PROT_WRITE) vm_flags |= VM_WRITE;
	if(prot & PROT_EXEC) vm_flags |= VM_EXEC;
	return vmm_mprotect(current_task->mm, (uintptr_t)addr, (uint64_t)len, vm_flags);
}

/*
 * int arch_prctl(int code, unsigned long addr)
 * 设置/读取FS段基址(用户态TLS)
 */
static long sys_arch_prctl(long code, long addr, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -EINVAL;
	if(code == ARCH_SET_FS){
		if((uintptr_t)addr >= USER_VADDR_MAX)return -EPERM;//必须是用户态地址
		current_task->fs_base = (uint64_t)addr;
		wrmsr(IA32_FS_BASE, (uint64_t)addr);//直接写入FS基址MSR
		return 0;
	}
	if(code == ARCH_GET_FS){
		//Linux语义: 把FS基址写到用户指针*addr, 成功返回0
		if(!addr)return -EFAULT;
		uint64_t fs = current_task->fs_base;
		if(copy_to_user((void*)addr, &fs, sizeof(fs)))return -EFAULT;
		return 0;
	}
	return -EINVAL;
}

/*
 * int set_tid_address(int *tidptr)
 * 注册clear_child_tid指针, 返回当前PID
 */
static long sys_set_tid_address(long tidptr, long a2, long a3, long a4, long a5, long a6){
	(void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOSYS;
	current_task->clear_child_tid = (int*)tidptr;
	return current_task->pid;
}

void InitSyscall(void){
    //初始化系统调用表
	memset(syscall_table, 0, sizeof(syscall_table));
	syscall_table[SYS_READ]       = sys_read;
	syscall_table[SYS_WRITE]      = sys_write;
	syscall_table[SYS_OPEN]       = sys_open;
	syscall_table[SYS_CLOSE]      = sys_close;
	syscall_table[SYS_STAT]       = sys_newstat;
	syscall_table[SYS_FSTAT]      = sys_newfstat;
	syscall_table[SYS_LSEEK]      = sys_lseek;
	syscall_table[SYS_ACCESS]     = sys_access;
	syscall_table[SYS_READLINK]   = sys_readlink;
	syscall_table[SYS_GETCWD]     = sys_getcwd;
	syscall_table[SYS_CHDIR]      = sys_chdir;
	syscall_table[SYS_IOCTL]      = sys_ioctl;
	syscall_table[SYS_WRITEV]     = sys_writev;
	syscall_table[SYS_READLINKAT]      = sys_readlinkat;
	syscall_table[SYS_SET_ROBUST_LIST] = sys_set_robust_list;
	syscall_table[SYS_PRLIMIT64]       = sys_prlimit64;
	syscall_table[SYS_GETRANDOM]       = sys_getrandom;
	syscall_table[SYS_NANOSLEEP]  = sys_nanosleep;
	syscall_table[SYS_GETPID]     = sys_getpid;
	syscall_table[SYS_FORK]       = sys_fork;
	syscall_table[SYS_EXECVE]     = sys_execve;
	syscall_table[SYS_EXIT]       = sys_exit;
	syscall_table[SYS_WAIT4]      = sys_waitpid;
	syscall_table[SYS_EXIT_GROUP] = sys_exit_group;
	syscall_table[SYS_MKDIR]      = sys_mkdir;
	syscall_table[SYS_RMDIR]      = sys_rmdir;
	syscall_table[SYS_UNLINK]     = sys_unlink;
	syscall_table[SYS_BRK]        = sys_brk;
	syscall_table[SYS_ARCH_PRCTL] = sys_arch_prctl;
	syscall_table[SYS_FUTEX]            = sys_futex;
	syscall_table[SYS_SET_TID_ADDRESS]  = sys_set_tid_address;
	syscall_table[SYS_MMAP]       = sys_mmap;
	syscall_table[SYS_MUNMAP]     = sys_munmap;
	syscall_table[SYS_MPROTECT]   = sys_mprotect;
	//启用SYSCALL/SYSRET
	{
		uint64_t efer = rdmsr(IA32_EFER) | (1ULL << 0);//SCE
		uint32_t eax, ebx, ecx, edx;
		__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
		if (edx & (1U << 20)) {
			efer |= (1ULL << 11);//CPU支持NX则启用NXE
			cpu_nx_enabled = 1;
		}
		wrmsr(IA32_EFER, efer);
	}
	wrmsr(IA32_STAR, ((uint64_t)__USER32_CS << 48) | ((uint64_t)__KERNEL_CS << 32));//设置段选择子
	wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);//入口RIP
	wrmsr(IA32_FMASK, 0x700);//进入内核时清除TF|IF|DF
	printk(PRINTK_INFO"Syscalls ready");
}
