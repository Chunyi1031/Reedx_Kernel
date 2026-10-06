#include <syscalls.h>
#include <desc.h>
#include <serial.h>
#include <print.h>
#include <task.h>
#include <klib.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <drives/tty.h>
#include <drives/ps2kbd.h>
#include <idt.h>
#include <fork.h>
#include <fs.h>
#include <futex.h>
#include <pipe.h>
#include <signals.h>
#include <rtc.h>
#include <delay.h>
#include <irq.h>
#include <acpi/power.h>

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

//从用户空间拷贝字符串
long strncpy_from_user(char *dst, const void *src, long max){
    uintptr_t uaddr = (uintptr_t)src;
    long i = 0;
    if(!dst || !src) return -1;
    while(i < max - 1){
        if(uaddr >= USER_VADDR_MAX) return -1;
        char c = *(const char*)uaddr;
        dst[i] = c;
        if(c == 0) return i;
        i++;
        uaddr++;
    }
    dst[max - 1] = 0;
    return max - 1;
}

/*
 * int getdents64(unsigned int fd, struct linux_dirent64 *dirp, unsigned int count)
 * 系统调用:getdents64 读取目录项
 */
static long sys_getdents64(long fd, long buf, long count, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!current_task) return -EBADF;
	if(count <= 0) return -EINVAL;
	if(fd < 3 || fd >= MAX_FD || !current_task->files[fd].used) return -EBADF;
	if(count > 32768) count = 32768;
	int pages = (int)((count + 4095) / 4096);
	void *kbuf = (void*)PHYS_TO_VIRT(Pmm_Malloc(pages));
	if(!kbuf) return -ENOMEM;
	int n = FsGetdents(&current_task->files[fd], kbuf, (uint64_t)count);
	if(n < 0){
		Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), pages);
		return n;
	}
	if(n > 0 && copy_to_user((void*)buf, kbuf, (uint64_t)n)){
		Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), pages);
		return -EFAULT;
	}
	Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), pages);
	return n;
}

//判断fd是否为控制台表项
static inline _Bool fd_is_console(long fd){
	if(!current_task || fd < 0 || fd >= MAX_FD)return false;
	return current_task->files[fd].used && !current_task->files[fd].node && !current_task->files[fd].pipe;
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
	if((fd == 0 && (!current_task || !current_task->files[0].used)) || (fd_is_console(fd) && (current_task->files[fd].flags & O_ACCMODE) != O_WRONLY)){
		if(count == 0)return 0;
		if(current_task && fd >= 0 && fd < MAX_FD){
			if(count > 1)current_task->files[fd].tty_mark = true;
			if(current_task->files[fd].tty_mark){
				char lbuf[TTY_LINE_MAX];
				long n = TTY_TermRead(lbuf, count);
				if(n < 0)return n;//-EINTR(Ctrl+C)
				if(n > 0 && copy_to_user((void*)buf, lbuf, (unsigned long)n))return -EFAULT;
				return n;
			}
		}
		sti();
		char kbuf[KEYBOARD_BUFFER_SIZE];
		long total = 0;
		char k0 = GetKey();//阻塞等待至少一个按键
		if(k0 == '\r' && (g_tty_term.iflag & 0x0100))k0 = '\n';
		kbuf[total++] = k0;
		//一次读完缓冲区中已就绪的多个按键
		while(total < count && total < KEYBOARD_BUFFER_SIZE){
			char c = GetKey_NoBlock();
			if(c == 0)break;
			if(c == '\r' && (g_tty_term.iflag & 0x0100))c = '\n';
			kbuf[total++] = c;
		}
		if(copy_to_user((void*)buf, kbuf, (unsigned long)total))return -EFAULT;//复制到用户空间
		return total;
	}
	//文件读取
	if(!current_task)return -EBADF;
	if(fd < 0 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
	if((current_task->files[fd].flags & O_ACCMODE) == O_WRONLY)return -EBADF;
	long total = 0;
	char kbuf[512];
	while(total < count) {
		long chunk = count - total;
		if(chunk > (long)sizeof(kbuf))chunk = sizeof(kbuf);
		uint64_t n;
		//管道读取
		if(current_task->files[fd].pipe){
			long pr = pipeRead((struct pipe*)current_task->files[fd].pipe, kbuf, (uint64_t)chunk);
			if(pr <= 0){ if(pr == 0)break; return pr; }
			n = (uint64_t)pr;
		//文件读取
		}else{
			n = FsRead(&current_task->files[fd], kbuf, (uint64_t)chunk);//读取内容
			if(n == 0)break;//EOF
		}
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
	//输出到TTY
	_Bool __con_out = fd_is_console(fd) && (current_task->files[fd].flags & O_ACCMODE) != O_RDONLY;
	if(((fd == 1 || fd == 2) && (!current_task || !current_task->files[fd].used)) || __con_out) {
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
	if(fd < 0 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
	if((current_task->files[fd].flags & O_ACCMODE) == O_RDONLY)return -EBADF;
	long total = 0;
	char kbuf[512];
	while(total < count){
		long chunk = count - total;
		if(chunk > (long)sizeof(kbuf))chunk = sizeof(kbuf);
		if(copy_from_user(kbuf, (const char*)buf + total, (unsigned long)chunk))return -EFAULT;//复制到内核空间
		uint64_t n;
		//管道写入
		if(current_task->files[fd].pipe){
			long pw = pipeWrite((struct pipe*)current_task->files[fd].pipe, kbuf, (uint64_t)chunk);
			if(pw == -EPIPE)SignalSend(current_task, SIGPIPE, SI_KERNEL, 0, 0);//写入已断管道，投递SIGPIPE
			if(pw <= 0)return pw < 0 ? pw : -ENOSPC;
			n = (uint64_t)pw;
		//文件写入
		}else{
			n = FsWrite(&current_task->files[fd], kbuf, (uint64_t)chunk);
			if(n == 0)return -ENOSPC;
		}
		total += (long)n;
	}
	return total;
}

/*
 * ssize_t pread64(int fd, void *buf, size_t count, off_t offset)
 * 系统调用:pread64  从offset处读, 不改变文件当前位置
 */
static long sys_pread64(long fd, long buf, long count, long off, long a5, long a6){
	(void)a5; (void)a6;
	if(!current_task)return -EBADF;
	if(count < 0)return -EINVAL;
	if(fd < 3 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
	if((current_task->files[fd].flags & O_ACCMODE) == O_WRONLY)return -EBADF;
	uint64_t old_off = current_task->files[fd].off;
	FsSeek(&current_task->files[fd], (int64_t)off, SEEK_SET);
	long total = 0;
	char kbuf[512];
	while(total < count){
		long chunk = count - total;
		if(chunk > (long)sizeof(kbuf))chunk = sizeof(kbuf);
		uint64_t n = FsRead(&current_task->files[fd], kbuf, (uint64_t)chunk);
		if(n == 0)break;//EOF
		if(copy_to_user((char*)buf + total, kbuf, n)){ total = -EFAULT; break; }
		total += (long)n;
	}
	FsSeek(&current_task->files[fd], (int64_t)old_off, SEEK_SET);//恢复原位置
	return total;
}

/*
 * int rt_sigprocmask(int how, const sigset_t *set, sigset_t *oldset, size_t sigsetsize)
 */
static long sys_rt_sigprocmask(long how, long set, long oldset, long sigsetsize, long a5, long a6){
	(void)a5; (void)a6;
	if(!current_task)return -ENOSYS;
	if(sigsetsize != 8)return -EINVAL;
	uint64_t old = current_task->sigmask;
	if(oldset){
		if(copy_to_user((void*)oldset, &old, 8))return -EFAULT;
	}
	if(set){
		uint64_t mask;
		if(copy_from_user(&mask, (void*)set, 8))return -EFAULT;
		if(how == SIG_BLOCK)current_task->sigmask |= mask;
		else if(how == SIG_UNBLOCK)current_task->sigmask &= ~mask;
		else if(how == SIG_SETMASK)current_task->sigmask = mask;
		else return -EINVAL;
	}
	return 0;
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
	if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
	//分配文件描述符
	int fd;
	for(fd = 3; fd < MAX_FD; fd++) {
		if(!current_task->files[fd].used)break;
	}
	if (fd >= MAX_FD)return -ENFILE;
	fs_file_t *f = &current_task->files[fd];
	int or = FsOpen(kpath, (int)flags, f);//打开文件
	if(or != 0)return or;
	return fd;
}

//关闭单个文件描述符
static void fd_close_one(long fd){
	if(!current_task)return;
	if(fd < 0 || fd >= MAX_FD || !current_task->files[fd].used)return;
	if(current_task->files[fd].pipe){
		pipeCloseEnd((struct pipe*)current_task->files[fd].pipe,(current_task->files[fd].flags & O_ACCMODE) == O_WRONLY);
	}else{
		FsClose(&current_task->files[fd]);
	}
	current_task->files[fd].used = false;
	current_task->files[fd].pipe = NULL;
	current_task->files[fd].node = NULL;
	current_task->files[fd].off = 0;
}

//复制文件描述符表项
static long fd_dup_to(long oldfd, long newfd){
	if(!current_task)return -EBADF;
	if(oldfd < 0 || oldfd >= MAX_FD)return -EBADF;
	if(newfd < 0 || newfd >= MAX_FD)return -EBADF;
	if(oldfd == newfd)return newfd;
	//如果旧fd未使用
	if(!current_task->files[oldfd].used){
		//小于3的虚拟文件描述符，物化为控制台表项
		if(oldfd < 3){
			fd_close_one(newfd);
			memset(&current_task->files[newfd], 0, sizeof(fs_file_t));
			current_task->files[newfd].used = true;
			current_task->files[newfd].flags = (oldfd == 0) ? O_RDONLY : O_WRONLY;
			return newfd;
		}
		return -EBADF;
	}
	fd_close_one(newfd);//目标已占用则先关闭
	current_task->files[newfd] = current_task->files[oldfd];//复制描述符
	//引用计数
	if(current_task->files[newfd].node)current_task->files[newfd].node->refs++;
	if(current_task->files[newfd].pipe)pipeForkRef((struct pipe*)current_task->files[newfd].pipe,(current_task->files[newfd].flags & O_ACCMODE) == O_WRONLY);
	return newfd;
}

/*
 * int close(int fd)
 * 系统调用:close
 * 关闭文件
 */
static long sys_close(long fd, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if (!current_task) return -EBADF;
	if(fd < 0 || fd >= MAX_FD)return -EBADF;
	if(fd < 3 && !current_task->files[fd].used)return 0;
	if(!current_task->files[fd].used)return -EBADF;
	fd_close_one(fd);
	return 0;
}

/*
 * int pipe2(int fds[2], int flags)
 * 创建管道
 */
static long sys_pipe2(long fds, long flags, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOSYS;
	if(!fds)return -EFAULT;
	//分配文件描述符
	int rfd = -1, wfd = -1;
	for(int i = 3; i < MAX_FD; i++){
		if(!current_task->files[i].used){
			if(rfd < 0){
				rfd = i;
			}else{
				wfd = i;
				break;
			}
		}
	}
	if(wfd < 0)return -ENFILE;
	//创建管道
	struct pipe *p = NULL;
	int rc = pipeCreate(&p, (int)flags);
	if(rc != 0)return rc;
	//初始化文件描述符
	current_task->files[rfd].used = true;
	current_task->files[rfd].node = NULL;
	current_task->files[rfd].pipe = p;
	current_task->files[rfd].off = 0;
	current_task->files[rfd].flags = O_RDONLY;
	current_task->files[wfd].used = true;
	current_task->files[wfd].node = NULL;
	current_task->files[wfd].pipe = p;
	current_task->files[wfd].off = 0;
	current_task->files[wfd].flags = O_WRONLY;
	//输出到用户空间
	int out[2] = {rfd, wfd};
	if(copy_to_user((void*)fds, out, sizeof(out)))return -EFAULT;
	return 0;
}

/*
 * int pipe(int fds[2])
 */
static long sys_pipe(long fds, long a2, long a3, long a4, long a5, long a6){
	(void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	return sys_pipe2(fds, 0, 0, 0, 0, 0);
}

/*
 * int dup(int oldfd)
 * 复制描述符到最小空闲fd
 */
static long sys_dup(long oldfd, long a2, long a3, long a4, long a5, long a6){
	(void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -EBADF;
	if(oldfd < 0 || oldfd >= MAX_FD)return -EBADF;
	if(!current_task->files[oldfd].used && oldfd >= 3)return -EBADF;
	for(int i = 3; i < MAX_FD; i++){
		if(!current_task->files[i].used)return fd_dup_to(oldfd, i);
	}
	return -ENFILE;
}

/*
 * int dup2(int oldfd, int newfd)
 * 复制描述符到指定位置(覆盖已有描述符)
 */
static long sys_dup2(long oldfd, long newfd, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	return fd_dup_to(oldfd, newfd);
}

/*
 * int dup3(int oldfd, int newfd, int flags)
 */
static long sys_dup3(long oldfd, long newfd, long flags, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(oldfd == newfd)return -EINVAL;
	if(flags & ~(long)O_CLOEXEC)return -EINVAL;
	return fd_dup_to(oldfd, newfd);
}

/*
 * int mkdir(const char *pathname, mode_t mode)
 */
static long sys_mkdir(long path, long mode, long unused, long a4, long a5, long a6){
	(void)unused; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	char kpath[256];
	if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
	return FsMkdir(kpath, (int)mode) ? -ENOENT : 0;
}

/*
 * int truncate(const char *path, off_t length)
 */
static long sys_truncate(long path, long length, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	if(length < 0)return -EINVAL;
	if(!path)return -EFAULT;
	char kpath[256];
	if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
	return FsTruncatePath(kpath, (uint64_t)length);
}

/*
 * int ftruncate(int fd, off_t length)
 */
static long sys_ftruncate(long fd, long length, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(length < 0)return -EINVAL;
	if(!current_task || fd < 0 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
	//控制台/管道等无存储节点的 fd 不支持截断
	if(!current_task->files[fd].node || current_task->files[fd].pipe)return -EINVAL;
	return FsTruncate(&current_task->files[fd], (uint64_t)length);
}

/*
 * int fsync(int fd) / int fdatasync(int fd)
 * write() 已经把数据直接写到设备(AHCI/ATA 驱动同步等完成), 没有写回缓存,
 * 所以这里校验一下fd就可以直接成功。
 * (nano 存盘时会调它, 不实现就会报 "Function not implemented")
 */
static long sys_fsync(long fd, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if(fd < 0)return -EBADF;
	if(fd >= 3){
		if(!current_task || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
	}
	return 0;
}

/*
 * void sync(void)
 */
static long sys_sync(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	return 0;
}

/*
 * int chmod(const char *path, mode_t mode)
 * int fchmod(int fd, mode_t mode)
 * int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags)
 * FAT32 没有权限位, 当成功处理(否则程序会因 ENOSYS 报错)
 */
static long sys_chmod(long path, long mode, long a3, long a4, long a5, long a6){
	(void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!path)return -EFAULT;
	return 0;
}

static long sys_fchmod(long fd, long mode, long a3, long a4, long a5, long a6){
	(void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
	if(fd < 0)return -EBADF;
	if(fd >= 3){
		if(!current_task || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
	}
	return 0;
}

static long sys_fchmodat(long dirfd, long path, long mode, long flags, long a5, long a6){
	(void)dirfd; (void)path; (void)mode; (void)flags; (void)a5; (void)a6;
	return 0;
}

/*
 * mode_t umask(mode_t mask)
 * 返回旧掩码(glibc 靠返回值实现 umask())
 */
static long sys_umask(long mask, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	static long old = 022;
	long prev = old;
	old = mask & 0777;
	return prev;
}

/*
 * int rmdir(const char *pathname)
 */
static long sys_rmdir(long path, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	char kpath[256];
	if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
	int r = FsUnlink(kpath);
	return (r < 0) ? ((r == -1) ? -ENOENT : r) : 0;
}

/*
 * int unlink(const char *pathname)
 */
static long sys_unlink(long path, long b, long c, long a4, long a5, long a6){
	(void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	char kpath[256];
	if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
	int r = FsUnlink(kpath);
	return (r < 0) ? ((r == -1) ? -ENOENT : r) : 0;
}

/*
 * int rename(const char *oldpath, const char *newpath)
 * 系统调用:rename
 */
static long sys_rename(long oldpath, long newpath, long c, long a4, long a5, long a6){
	(void)c; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	char kold[256], knew[256];
	if(strncpy_from_user(kold, (void*)oldpath, 256) < 0)return -EFAULT;
	if(strncpy_from_user(knew, (void*)newpath, 256) < 0)return -EFAULT;
	return FsRename(kold, knew) ? -ENOENT : 0;
}

//从用户态拷路径到内核缓冲
static long at_read_path(long path, char *kbuf, int bufsz){
	if(!path)return -EFAULT;
	if(strncpy_from_user(kbuf, (void*)path, bufsz) < 0)return -EFAULT;
	if(kbuf[0] == '\0')return -ENOENT;
	return 0;
}
static long at_split(long dirfd, const char *kpath, fs_node_t **dir, char *name, int namesz){
	if((int)dirfd == AT_FDCWD || kpath[0] == '/')return -ENOSYS;
	for(const char *p = kpath; *p; p++)if(*p == '/')return -ENOSYS;//多级相对名无法从fd精确拼接
	if(!current_task || dirfd < 0 || dirfd >= MAX_FD || !current_task->files[dirfd].used)return -EBADF;
	fs_node_t *d = current_task->files[dirfd].node;
	if(!d || d->type != FT_DIR)return -ENOTDIR;
	int i = 0;
	for(; kpath[i] && i < namesz - 1; i++)name[i] = kpath[i];
	name[i] = '\0';
	*dir = d;
	return 0;
}

/*
 * renameat/renameat2共用实现
 */
static long do_renameat(long olddirfd, long oldpath, long newdirfd, long newpath, long flags){
	if(!current_task)return -ENOENT;
	char kold[256], knew[256];
	long pr = at_read_path(oldpath, kold, 256);
	if(pr)return pr;
	pr = at_read_path(newpath, knew, 256);
	if(pr)return pr;
	fs_node_t *odir = NULL, *ndir = NULL;
	char oname[MAX_NAME], nname[MAX_NAME];
	long r = at_split(olddirfd, kold, &odir, oname, MAX_NAME);
	if(r < 0 && r != -ENOSYS)return r;
	if(r == -ENOSYS){
		odir = FsParentOf(kold, oname, MAX_NAME);
		if(!odir)return -ENOENT;
	}
	r = at_split(newdirfd, knew, &ndir, nname, MAX_NAME);
	if(r < 0 && r != -ENOSYS)return r;
	if(r == -ENOSYS){
		ndir = FsParentOf(knew, nname, MAX_NAME);
		if(!ndir)return -ENOENT;
	}
	if((flags & RENAME_NOREPLACE) && FsExistsIn(ndir, nname) == 0)return -EEXIST;
	return FsRenameIn(odir, oname, ndir, nname);
}

/*
 * int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
 */
static long sys_renameat(long olddirfd, long oldpath, long newdirfd, long newpath, long a5, long a6){
	(void)a5; (void)a6;
	return do_renameat(olddirfd, oldpath, newdirfd, newpath, 0);
}

/*
 * int mkdirat(int dirfd, const char *path, mode_t mode)
 */
static long sys_mkdirat(long dirfd, long path, long mode, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!current_task)return -EINVAL;
	char kpath[256];
	long pr = at_read_path(path, kpath, 256);
	if(pr)return pr;
	fs_node_t *dir = NULL;
	char name[MAX_NAME];
	long r = at_split(dirfd, kpath, &dir, name, MAX_NAME);
	if(r < 0 && r != -ENOSYS)return r;
	if(r == -ENOSYS)return sys_mkdir(path, mode, 0, 0, 0, 0);
	return FsMkdirIn(dir, name, (int)mode);
}

/*
 * int unlinkat(int dirfd, const char *path, int flags)
 */
static long sys_unlinkat(long dirfd, long path, long flags, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!current_task)return -EINVAL;
	char kpath[256];
	long pr = at_read_path(path, kpath, 256);
	if(pr)return pr;
	fs_node_t *dir = NULL;
	char name[MAX_NAME];
	long r = at_split(dirfd, kpath, &dir, name, MAX_NAME);
	if(r < 0 && r != -ENOSYS)return r;
	if(r == -ENOSYS){
		if(flags & AT_REMOVEDIR)return sys_rmdir(path, 0, 0, 0, 0, 0);
		return sys_unlink(path, 0, 0, 0, 0, 0);
	}
	int rr = FsUnlinkIn(dir, name);
	return (rr < 0) ? ((rr == -1) ? -ENOENT : rr) : 0;
}

/*
 * int renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags)
 */
static long sys_renameat2(long olddirfd, long oldpath, long newdirfd, long newpath, long flags, long a6){
	(void)a6;
	if(flags & ~RENAME_NOREPLACE)return -ENOSYS;//不支持EXCHANGE/WHITEOUT
	return do_renameat(olddirfd, oldpath, newdirfd, newpath, flags);
}

/*
 * off_t lseek(int fd, off_t offset, int whence)
 * 系统调用:lseek
 * 移动文件指针
 */
static long sys_lseek(long fd, long off, long whence, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if (!current_task) return -EBADF;
	if (fd < 0 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
	int r = FsSeek(&current_task->files[fd], off, (int)whence);
	if (r < 0) return -EINVAL;
	return r;
}
/*DeepSeek V4 Flash*/
//填充 stat 结构
static void fill_stat(stat_t *st, fs_node_t *node){
	memset(st, 0, sizeof(*st));
	st->st_dev = 1;
	st->st_ino = node->ino;
	st->st_nlink = 1;
	st->st_mode = (node->type == FT_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0755);
	st->st_size = (int64_t)node->size;
	st->st_blksize = 512;
	st->st_blocks = ((uint64_t)node->size + 511) / 512;
	uint64_t now = rtc_get_epoch();
	st->st_atime = (int64_t)(node->atime ? node->atime : now);
	st->st_mtime = (int64_t)(node->mtime ? node->mtime : now);
	st->st_ctime = (int64_t)(node->ctime ? node->ctime : now);
}

/*
 * int stat(const char *path, struct stat *buf)
 */
static long sys_newstat(long path, long buf, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -ENOSYS;
	if(!path || !buf)return -EFAULT;
	char kpath[256];
	if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
	fs_node_t *node = FsResolve(kpath);
	if(!node)return -ENOENT;
	stat_t st;
	fill_stat(&st, node);
	if(copy_to_user((void*)buf, &st, sizeof(st)))return -EFAULT;
	return 0;
}

/*
 * int openat(int dirfd, const char *path, int flags, mode_t mode)
 */
static long sys_openat(long dirfd, long path, long flags, long mode, long a5, long a6){
	(void)mode; (void)a5; (void)a6;
	if(!current_task)return -ENOENT;
	char kpath[256];
	long pr = at_read_path(path, kpath, 256);
	if(pr)return pr;
	fs_node_t *dir = NULL;
	char name[MAX_NAME];
	long r = at_split(dirfd, kpath, &dir, name, MAX_NAME);
	if(r < 0 && r != -ENOSYS)return r;
	if(r == -ENOSYS)return sys_open(path, flags, mode, 0, 0, 0);//绝对路径 / 相对CWD
	//dirfd + 单级相对名: 直接在该目录下打开
	int fd;
	for(fd = 3; fd < MAX_FD; fd++){
		if(!current_task->files[fd].used)break;
	}
	if(fd >= MAX_FD)return -ENFILE;
	fs_file_t *f = &current_task->files[fd];
	int or = FsOpenIn(dir, name, (int)flags, f);
	if(or != 0)return or;
	return fd;
}

/*
 * int newfstatat(int dirfd, const char *path, struct stat *buf, int flags)
 */
static long sys_newfstat(long fd, long buf, long a3, long a4, long a5, long a6);//前向声明
static long sys_newfstatat(long dirfd, long path, long buf, long flags, long a5, long a6){
	(void)a5; (void)a6;
	if(!current_task)return -EBADF;
	if(!path || !buf)return -EFAULT;
	char kpath[256];
	if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
	if(kpath[0] == '\0'){
		if(!(flags & AT_EMPTY_PATH))return -ENOENT;
		return sys_newfstat(dirfd, buf, 0, 0, 0, 0);
	}
	if((int)dirfd != AT_FDCWD && kpath[0] != '/'){
		if(dirfd < 0 || dirfd >= MAX_FD || !current_task->files[dirfd].used)return -EBADF;
		fs_node_t *dir = current_task->files[dirfd].node;
		if(!dir || dir->type != FT_DIR)return -ENOTDIR;
		if(!dir->ops || !dir->ops->lookup)return -ENOSYS;
		fs_node_t *child = dir->ops->lookup(dir, kpath);
		if(!child)return -ENOENT;
		stat_t st;
		fill_stat(&st, child);
		if(copy_to_user((void*)buf, &st, sizeof(st)))return -EFAULT;
		return 0;
	}
	return sys_newstat(path, buf, 0, 0, 0, 0);
}

//取fd对应的存储节点
static fs_node_t *fd_node_of(long fd, long *err){
	if(!current_task || fd < 0 || fd >= MAX_FD || !current_task->files[fd].used){
		*err = -EBADF;
		return NULL;
	}
	if(!current_task->files[fd].node){
		*err = -EBADF;
		return NULL;
	}
	return current_task->files[fd].node;
}
//解析dirfd+path
static fs_node_t *resolve_at_node(long dirfd, long path, long flags, long *err){
	*err = -ENOENT;
	if(!path)return fd_node_of(dirfd, err);//如果path为NULL，目标就是dirfd自身
	//复制path
	char kpath[256];
	if(strncpy_from_user(kpath, (void*)path, 256) < 0){
		*err = -EFAULT;
		return NULL;
	}
	//如果path为空字符串，目标就是dirfd自身
	if(kpath[0] == '\0'){
		if(!(flags & AT_EMPTY_PATH)){
			*err = -ENOENT;
			return NULL;
		}
		return fd_node_of(dirfd, err);
	}
	//如果dirfd不是AT_FDCWD且path不是绝对路径，则在dirfd目录下查找
	if((int)dirfd != AT_FDCWD && kpath[0] != '/'){
		if(!current_task || dirfd < 0 || dirfd >= MAX_FD || !current_task->files[dirfd].used){
			*err = -EBADF;
			return NULL;
		}
		fs_node_t *dir = current_task->files[dirfd].node;
		if(!dir || dir->type != FT_DIR){
			*err = -ENOTDIR;
			return NULL;
		}
		if(!dir->ops || !dir->ops->lookup){
			*err = -ENOSYS;
			return NULL;
		}
		fs_node_t *child = dir->ops->lookup(dir, kpath);
		if(!child){
			*err = -ENOENT;
			return NULL;
		}
		return child;
	}
	//否则直接解析绝对路径
	fs_node_t *node = FsResolve(kpath);
	if(!node){
		*err = -ENOENT;
		return NULL;
	}
	return node;
}
//把fd的stat信息填进内核stat_t
static long stat_of_fd(long fd, stat_t *st){
	if(!current_task)return -EBADF;
	if(fd < 0 || fd >= MAX_FD)return -EBADF;
	if((fd < 3 && !current_task->files[fd].used) || fd_is_console(fd)){
		memset(st, 0, sizeof(*st));
		st->st_dev = 1;
		st->st_ino = (uint64_t)(uintptr_t)&current_task->files[fd];
		st->st_nlink = 1;
		st->st_mode = S_IFCHR | 0666;
		st->st_rdev = 1;
		st->st_blksize = 512;
		uint64_t now = rtc_get_epoch();
		st->st_atime = (int64_t)now;
		st->st_mtime = (int64_t)now;
		st->st_ctime = (int64_t)now;
		return 0;
	}
	if(!current_task->files[fd].used)return -EBADF;
	if(current_task->files[fd].pipe){
		memset(st, 0, sizeof(*st));
		st->st_dev = 1;
		st->st_ino = (uint64_t)(uintptr_t)current_task->files[fd].pipe;
		st->st_nlink = 1;
		st->st_mode = S_IFIFO | 0600;
		st->st_blksize = 4096;
		uint64_t now = rtc_get_epoch();
		st->st_atime = (int64_t)now;
		st->st_mtime = (int64_t)now;
		st->st_ctime = (int64_t)now;
		return 0;
	}
	fill_stat(st, current_task->files[fd].node);
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
	long r = stat_of_fd(fd, &st);
	if(r)return r;
	if(copy_to_user((void*)buf, &st, sizeof(st)))return -EFAULT;
	return 0;
}

/*
 * int statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *buf)
 */
static long sys_statx(long dirfd, long path, long flags, long mask, long buf, long a6){
	(void)mask; (void)a6;
	if(!current_task || !current_task->mm)return -ENOSYS;
	if(!buf)return -EFAULT;
	stat_t st;
	//检查目标是否为fd自身
	_Bool self = false;
	if(!path){
		self = true;
	}else{
		char kpath[256];
		if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
		if(kpath[0] == '\0'){
			if(!(flags & AT_EMPTY_PATH))return -ENOENT;
			self = true;
		}
	}
	//如果为fd自身，直接获取fd的stat信息，否则解析dirfd+path
	if(self){
		long r = stat_of_fd(dirfd, &st);
		if(r)return r;
	}else{
		long e = 0;
		fs_node_t *node = resolve_at_node(dirfd, path, flags, &e);
		if(!node)return e;
		fill_stat(&st, node);
	}
	//填充statx
	statx_t sx;
	memset(&sx, 0, sizeof(sx));
	sx.stx_mask = STATX_BASIC_STATS;
	sx.stx_blksize = (uint32_t)st.st_blksize;
	sx.stx_nlink = (uint32_t)st.st_nlink;
	sx.stx_uid = (uint32_t)st.st_uid;
	sx.stx_gid = (uint32_t)st.st_gid;
	sx.stx_mode = (uint16_t)st.st_mode;
	sx.stx_ino = st.st_ino;
	sx.stx_size = (uint64_t)st.st_size;
	sx.stx_blocks = (uint64_t)st.st_blocks;
	sx.stx_atime.tv_sec  = st.st_atime;
	sx.stx_atime.tv_nsec = (uint32_t)st.st_atime_nsec;
	sx.stx_mtime.tv_sec  = st.st_mtime;
	sx.stx_mtime.tv_nsec = (uint32_t)st.st_mtime_nsec;
	sx.stx_ctime.tv_sec  = st.st_ctime;
	sx.stx_ctime.tv_nsec = (uint32_t)st.st_ctime_nsec;
	sx.stx_dev_major = 0;
	sx.stx_dev_minor = 1;
	sx.stx_rdev_major = 0;
	sx.stx_rdev_minor = (uint32_t)st.st_rdev;
	if(copy_to_user((void*)buf, &sx, sizeof(sx)))return -EFAULT;
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
	if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
	if(!FsResolve(kpath))return -ENOENT;
	return 0;
}

/*
 * int faccessat(int dirfd, const char *path, int mode, int flags)
 */
static long sys_faccessat(long dirfd, long path, long mode, long flags, long a5, long a6){
	(void)flags; (void)a5; (void)a6;
	if(!current_task)return -EINVAL;
	char kpath[256];
	long pr = at_read_path(path, kpath, 256);
	if(pr)return pr;
	fs_node_t *dir = NULL;
	char name[MAX_NAME];
	long r = at_split(dirfd, kpath, &dir, name, MAX_NAME);
	if(r < 0 && r != -ENOSYS)return r;
	if(r == -ENOSYS)return sys_access(path, mode, 0, 0, 0, 0);
	return FsExistsIn(dir, name);
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
 * 成功返回字符串长度(含结尾NUL)
 */
static long sys_getcwd(long buf, long size, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -EFAULT;
	if(!buf || size <= 0)return -EFAULT;
	int len = strlen(current_task->cwd);
	if((uint64_t)len + 1 > (uint64_t)size)return -ERANGE;
	if(copy_to_user((void*)buf, current_task->cwd, (uint64_t)len + 1))return -EFAULT;
	return (long)len + 1;//返回写入长度(含NUL)
}

/*
 * int chdir(const char *path)
 */
static long sys_chdir(long path, long a2, long a3, long a4, long a5, long a6){
	(void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -ENOSYS;
	if(!path)return -EFAULT;
	char kpath[256];
	if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
	char norm[512];
	if(FsNormalizePath(kpath, norm, sizeof(norm)) != 0)return -ENOENT;
	fs_node_t *node = FsResolve(kpath);
	if(!node)return -ENOENT;
	if(node->type != FT_DIR)return -ENOTDIR;
	int len = strlen(norm);
	if(len > 255)len = 255;
	memcpy(current_task->cwd, norm, (uint64_t)len);
	current_task->cwd[len] = 0;
	return 0;
}
/*DeepSeek V4 Flash-END*/
/*DeepSeek V4 Pro*/
/*
 * int ioctl(int fd, unsigned long request, ...)
 * 控制台fd支持终端相关请求(termios/窗口/前台组); 其余一律-ENOTTY
 */
static long sys_ioctl(long fd, long request, long arg, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!current_task)return -ENOTTY;
	if(fd < 0 || fd >= MAX_FD)return -ENOTTY;
	//仅控制台fd: 已打开的虚拟控制台 或 未使用的0/1/2
	if(!fd_is_console(fd) && !(fd < 3 && !current_task->files[fd].used))return -ENOTTY;
	switch((unsigned long)request){
	case TTY_TCGETS:{//老式termios
		tty_termios_legacy_t t;
		TTY_TermExportLegacy(&t);
		current_task->files[fd].tty_mark = true;//查询过终端: 之后read走行规程
		if(copy_to_user((void*)arg, &t, sizeof(t)))return -EFAULT;
		return 0;
	}
	case TTY_TCGETS2:{//struct termios2(glibc isatty走这里)
		tty_termios2_t t;
		TTY_TermExport2(&t);
		current_task->files[fd].tty_mark = true;
		if(copy_to_user((void*)arg, &t, sizeof(t)))return -EFAULT;
		return 0;
	}
	case TTY_TCSETS: case TTY_TCSETSW: case TTY_TCSETSF:{
		tty_termios_legacy_t t;
		if(copy_from_user(&t, (void*)arg, sizeof(t)))return -EFAULT;
		TTY_TermImportLegacy(&t);
		current_task->files[fd].tty_mark = true;
		return 0;
	}
	case TTY_TCSETS2: case TTY_TCSETSW2: case TTY_TCSETSF2:{
		tty_termios2_t t;
		if(copy_from_user(&t, (void*)arg, sizeof(t)))return -EFAULT;
		TTY_TermImport2(&t);
		current_task->files[fd].tty_mark = true;
		return 0;
	}
	case TTY_TIOCGPGRP:{//前台进程组: 无进程组概念, 返回自身pid(与getpgid自洽)
		int32_t pgrp = (int32_t)current_task->pid;
		if(copy_to_user((void*)arg, &pgrp, sizeof(pgrp)))return -EFAULT;
		return 0;
	}
	case TTY_TIOCSPGRP:return 0;//作业控制设置: 接受并忽略(假装成功)
	case TTY_TIOCGWINSZ:{
		struct tty_winsize ws;
		ws.ws_row = (uint16_t)(SYSTEM_ScreenInfo.Height / 18);//字符单元: 10x18像素
		ws.ws_col = (uint16_t)(SYSTEM_ScreenInfo.Width / 10);
		ws.ws_xpixel = (uint16_t)SYSTEM_ScreenInfo.Width;
		ws.ws_ypixel = (uint16_t)SYSTEM_ScreenInfo.Height;
		if(copy_to_user((void*)arg, &ws, sizeof(ws)))return -EFAULT;
		return 0;
	}
	case TTY_FIONREAD:{
		int32_t zero = 0;
		if(copy_to_user((void*)arg, &zero, sizeof(zero)))return -EFAULT;
		return 0;
	}
	default:return -ENOTTY;
	}
}

/*
 * pid_t getpgid(pid_t pid) / pid_t getpgrp(void) / int setpgid(pid_t pid, pid_t pgid)
 * 无进程组概念: 查询一律返回自身pid(与TIOCGPGRP自洽, 避免shell作业控制初始化时死循环);
 * 设置假装成功
 */
static long sys_getpgid(long pid, long a2, long a3, long a4, long a5, long a6){
	(void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -ESRCH;
	if(pid != 0 && pid != (long)current_task->pid)return -ESRCH;
	return (long)current_task->pid;
}

static long sys_getpgrp(long a1, long a2, long a3, long a4, long a5, long a6){
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!current_task)return 0;
	return (long)current_task->pid;
}

static long sys_setpgid(long pid, long pgid, long a3, long a4, long a5, long a6){
	(void)pid; (void)pgid; (void)a3; (void)a4; (void)a5; (void)a6;
	return 0;//无进程组: 假装成功(让shell的作业控制初始化通过)
}

/*
 * ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
 * 聚合写(TTY输出或文件写入), 返回写入总字节数
 */
static long sys_writev(long fd, long iov, long iovcnt, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!iov || iovcnt <= 0)return -EINVAL;
	char kbuf[256];
	long total = 0;
	long base_idx = 0;
	while(base_idx < iovcnt){
		long n = iovcnt - base_idx;
		if(n > 32)n = 32;
		struct iovec vec[32];
		if(copy_from_user(vec, (void*)((uintptr_t)iov + (uint64_t)base_idx * sizeof(struct iovec)), (uint64_t)n * sizeof(struct iovec)))return total ? total : -EFAULT;
		for(int i = 0; i < n; i++){
			uint64_t len = vec[i].iov_len;
			if(!len)continue;
			uintptr_t base = (uintptr_t)vec[i].iov_base;
			if(base >= USER_VADDR_MAX || base + len > USER_VADDR_MAX)return total ? total : -EFAULT;
			uint64_t done = 0;
			while(done < len){
				uint64_t chunk = len - done;
				if(chunk > sizeof(kbuf))chunk = sizeof(kbuf);
				if(copy_from_user(kbuf, (const char*)base + done, chunk))return total ? total : -EFAULT;
				_Bool __wc = fd_is_console(fd) && (current_task->files[fd].flags & O_ACCMODE) != O_RDONLY;
				if(((fd == 1 || fd == 2) && (!current_task || !current_task->files[fd].used)) || __wc){
					//TTY输出
					for(uint64_t j = 0; j < chunk; j++)TTY_PrintChar(kbuf[j], CurrentConsoleStyle.TextColor);
				}else{
					//文件/管道写入
					if(!current_task)return -EBADF;
					if(fd < 0 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
					if((current_task->files[fd].flags & O_ACCMODE) == O_RDONLY)return -EBADF;
					if(current_task->files[fd].pipe){
						long pw = pipeWrite((struct pipe*)current_task->files[fd].pipe, kbuf, chunk);
						if(pw == -EPIPE)SignalSend(current_task, SIGPIPE, SI_KERNEL, 0, 0);//写入已断管道
						if(pw <= 0)return total ? total : (pw < 0 ? pw : -ENOSPC);
					}else{
						FsWrite(&current_task->files[fd], kbuf, chunk);
					}
				}
				done += chunk;
				total += (long)chunk;
			}
		}
		base_idx += n;
	}
	return total;
}

/*
 * ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
 */
static long sys_readv(long fd, long iov, long iovcnt, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!iov || iovcnt <= 0)return -EINVAL;
	if(iovcnt > 1024)return -EINVAL;//IOV_MAX
	long total = 0;
	long base_idx = 0;
	while(base_idx < iovcnt){
		long n = iovcnt - base_idx;
		if(n > 32)n = 32;
		struct iovec vec[32];
		if(copy_from_user(vec, (void*)((uintptr_t)iov + (uint64_t)base_idx * sizeof(struct iovec)), (uint64_t)n * sizeof(struct iovec)))return total ? total : -EFAULT;
		for(int i = 0; i < n; i++){
			uint64_t len = vec[i].iov_len;
			if(!len)continue;
			uintptr_t base = (uintptr_t)vec[i].iov_base;
			if(base >= USER_VADDR_MAX)return total ? total : -EFAULT;
			while(len > 0){
				uint64_t chunk = len;
				if(chunk > (uint64_t)(USER_VADDR_MAX - base))chunk = (uint64_t)(USER_VADDR_MAX - base);
				if(!chunk)return total ? total : -EFAULT;
				long r = sys_read(fd, (long)base, (long)chunk, 0, 0, 0);
				if(r < 0)return total ? total : r;
				if(r == 0)return total;
				total += r;
				base += (uintptr_t)r;
				len -= (uint64_t)r;
				if((uint64_t)r < chunk)return total;
			}
		}
		base_idx += n;
	}
	return total;
}

/*
 * int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
 * times==NULL: atime/mtime 都设为当前; tv_nsec==UTIME_NOW 设为当前, UTIME_OMIT 保持原值
 * 无符号链接, AT_SYMLINK_NOFOLLOW 忽略; path 为 NULL/空且带 AT_EMPTY_PATH 时作用于 dirfd 自身
 * (glibc 的 futimens(fd,times) 就是 utimensat(fd, NULL, times, 0))
 */
static long sys_utimensat(long dirfd, long path, long times, long flags, long a5, long a6){
	(void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -ENOSYS;
	long e = 0;
	fs_node_t *node = resolve_at_node(dirfd, path, flags, &e);
	if(!node)return e;
	//解析 times[2](tv_nsec 可能是 UTIME_NOW/UTIME_OMIT 特殊值)
	uint64_t now = rtc_get_epoch();
	uint64_t atime = node->atime, mtime = node->mtime;
	if(!times){
		atime = now;
		mtime = now;
	}else{
		timespec_t ts[2];
		if(copy_from_user(ts, (void*)times, sizeof(ts)))return -EFAULT;
		for(int i = 0; i < 2; i++){
			uint64_t *dst = (i == 0) ? &atime : &mtime;
			if(ts[i].tv_nsec == UTIME_OMIT)continue;
			if(ts[i].tv_nsec == UTIME_NOW){ *dst = now; continue; }
			if(ts[i].tv_nsec < 0 || ts[i].tv_nsec >= 1000000000L || ts[i].tv_sec < 0)return -EINVAL;
			*dst = (uint64_t)ts[i].tv_sec;
		}
	}
	node->atime = atime;
	node->mtime = mtime;
	node->ctime = now;
	//回写到存储介质(FAT32 会更新目录项; ramfs 没有该操作, 仅更新内存节点)
	if(node->ops && node->ops->set_times)node->ops->set_times(node);
	return 0;
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

/*
 * int gettimeofday(struct timeval *tv, void *tz)
 * tz忽略; tv可为NULL(仅查询成功)
 */
static long sys_gettimeofday(long tv, long tz, long a3, long a4, long a5, long a6){
	(void)tz; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!tv)return 0;
	timeval_t t;
	t.tv_sec = (long)rtc_get_epoch();
	t.tv_usec = (long)((SYSTEM_TimerTicks * 10000) % 1000000);//tick=10ms
	if(copy_to_user((void*)tv, &t, sizeof(t)))return -EFAULT;
	return 0;
}

/*
 * time_t time(time_t *tloc)
 * 返回当前UTC时间戳, tloc非NULL时同时写入
 */
static long sys_time(long tloc, long a2, long a3, long a4, long a5, long a6){
	(void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	uint64_t e = rtc_get_epoch();
	if(tloc && copy_to_user((void*)tloc, &e, sizeof(e)))return -EFAULT;
	return (long)e;
}

/*
 * int clock_gettime(clockid_t clk_id, struct timespec *tp)
 * 支持REALTIME/MONOTONIC及COARSE变体
 */
static long sys_clock_gettime(long clk, long tp, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(!tp)return -EFAULT;
	timespec_t ts;
	if(clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE){
		ts.tv_sec = (long)rtc_get_epoch();
		ts.tv_nsec = (long)((SYSTEM_TimerTicks * 10000) % 1000000) * 1000L;
	}else if(clk == CLOCK_MONOTONIC || clk == CLOCK_MONOTONIC_COARSE){
		uint64_t ms = SYSTEM_TimerTicks * 10;//每tick 10ms
		ts.tv_sec = (long)(ms / 1000);
		ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	}else{
		return -EINVAL;
	}
	if(copy_to_user((void*)tp, &ts, sizeof(ts)))return -EFAULT;
	return 0;
}

/*
 * int clock_getres(clockid_t clk_id, struct timespec *res)
 * 分辨率=10ms(OS_TICK_HZ=100)
 */
static long sys_clock_getres(long clk, long tp, long a3, long a4, long a5, long a6){
	(void)a3; (void)a4; (void)a5; (void)a6;
	if(clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC &&
	   clk != CLOCK_REALTIME_COARSE && clk != CLOCK_MONOTONIC_COARSE)return -EINVAL;
	if(tp){
		timespec_t ts = {0, 10000000L};//10ms
		if(copy_to_user((void*)tp, &ts, sizeof(ts)))return -EFAULT;
	}
	return 0;
}

/*
 * int clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)
 * sleep()/nanosleep 最终走这里(glibc)
 */
static long sys_clock_nanosleep(long clk, long flags, long req, long rem, long a5, long a6){
	(void)a5; (void)a6;
	if(!req)return -EFAULT;
	timespec_t ts;
	if(copy_from_user(&ts, (void*)req, sizeof(ts)))return -EFAULT;
	if(ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)return -EINVAL;
	uint64_t ms;
	if(!(flags & 1)){
		//相对时间
		ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
	}else{
		//绝对时间(TIMER_ABSTIME): 相对当前时钟求差值
		uint64_t cur_sec, cur_nsec;
		if(clk == CLOCK_MONOTONIC){
			uint64_t m = SYSTEM_TimerTicks * 10;
			cur_sec = m / 1000;
			cur_nsec = (m % 1000) * 1000000L;
		}else{
			cur_sec = rtc_get_epoch();
			cur_nsec = ((SYSTEM_TimerTicks * 10000) % 1000000) * 1000L;
		}
		int64_t delta = ((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec)
		              - ((int64_t)cur_sec * 1000000000LL + (int64_t)cur_nsec);
		if(delta <= 0)return 0;
		ms = (uint64_t)delta / 1000000;
	}
	msleep(ms);
	if(rem){
		timespec_t zero = {0, 0};
		if(copy_to_user((void*)rem, &zero, sizeof(zero)))return -EFAULT;
	}
	return 0;
}

/*
 * int fcntl(int fd, int cmd, ...)
 * 仅支持 F_GETFD/F_GETFL/F_SETFL, 其余-EINVAL
 * 关键: fd 0/1/2 永远"有效"(F_GETFD返回0), 否则glibc会误判标准输出未打开而丢弃输出
 */
static long sys_fcntl(long fd, long cmd, long arg, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(!current_task)return -EBADF;
	if(fd < 0 || fd >= MAX_FD)return -EBADF;
	_Bool real = current_task->files[fd].used;//是否为真实表项(文件/管道/已重定向或复制的标准fd)
	if(!real && fd >= 3)return -EBADF;
	switch(cmd){
	case F_DUPFD://0: 复制到最小空闲fd(>=arg)
	case F_DUPFD_CLOEXEC://1030
	{
		long minfd = arg;
		if(minfd < 3)minfd = 3;
		for(long i = minfd; i < MAX_FD; i++){
			if(!current_task->files[i].used)return fd_dup_to(fd, i);
		}
		return -EINVAL;
	}
	case F_GETFD://1
		return 0;
	case F_SETFD://2
		return 0;
	case F_GETFL://3
		if(!real)return O_RDWR;
		return current_task->files[fd].flags;
	case F_SETFL://4
		if(!real)return 0;
		current_task->files[fd].flags = (current_task->files[fd].flags & ~O_ACCMODE) | ((int)arg & O_ACCMODE);
		return 0;
	case F_GETLK://5
	{
		if(!arg)return -EINVAL;
		uint16_t unlck = F_UNLCK;
		if(copy_to_user((void*)arg, &unlck, 2))return -EFAULT;
		return 0;
	}
	case F_SETLK://6
	case F_SETLKW://7
		return 0;
	case F_SETOWN://8
		return 0;
	case F_GETOWN://9
		return 0;
	case F_SETPIPE_SZ://1031
		return (long)arg;
	case F_GETPIPE_SZ://1032
		return 65536;
	default:
		return -EINVAL;
	}
}

/*
 * int poll(struct pollfd *fds, nfds_t nfds, int timeout)
 */
#define KPOLL_IN   0x001
#define KPOLL_OUT  0x004
#define KPOLL_NVAL 0x020
#define KPOLL_MAX  64

typedef struct { int fd; short events; short revents; } kpollfd_t;

static long sys_poll(long fds, long nfds, long timeout, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(nfds < 0)return -EINVAL;
	if(nfds == 0){//没有要查的fd: 纯粹当延时用
		if(timeout > 0)msleep((uint64_t)timeout);
		return 0;
	}
	if(!fds)return -EFAULT;
	if(nfds > KPOLL_MAX)nfds = KPOLL_MAX;
	kpollfd_t pfd[KPOLL_MAX];
	unsigned long bytes = (unsigned long)nfds * sizeof(kpollfd_t);
	if(copy_from_user(pfd, (void*)fds, bytes))return -EFAULT;
	uint64_t waited = 0;
	for(;;){
		int ready = 0;
		for(long i = 0; i < nfds; i++){
			short ev = pfd[i].events;
			short rv = 0;
			long fd = pfd[i].fd;
			if(fd >= 0){
				_Bool con_in = false, con_out = false;
				if(fd < 3 && (!current_task || !current_task->files[fd].used)){
					con_in = con_out = true;//标准输入输出默认就是控制台
				}else if(fd_is_console(fd)){
					int acc = current_task->files[fd].flags & O_ACCMODE;
					con_in  = (acc != O_WRONLY);
					con_out = (acc != O_RDONLY);
				}
				if(con_in || con_out){
					if(con_in && TTY_ReadReady())rv |= KPOLL_IN;
					if(con_out)rv |= KPOLL_OUT;
				}else if(current_task && fd < MAX_FD && current_task->files[fd].used){
					int acc = current_task->files[fd].flags & O_ACCMODE;
					if(current_task->files[fd].pipe){
						int pr = pipeReady((struct pipe*)current_task->files[fd].pipe);
						if((pr & 1) && acc != O_WRONLY)rv |= KPOLL_IN;
						if((pr & 2) && acc != O_RDONLY)rv |= KPOLL_OUT;
					}else{//普通文件/设备: 恒就绪
						if(acc != O_WRONLY)rv |= KPOLL_IN;
						if(acc != O_RDONLY)rv |= KPOLL_OUT;
					}
				}else{
					rv |= KPOLL_NVAL;//无效fd
				}
			}
			rv &= (short)(ev | KPOLL_NVAL);//只上报调用者关心的事件(外加NVAL)
			pfd[i].revents = rv;
			if(rv)ready++;
		}
		if(ready > 0){
			if(copy_to_user((void*)fds, pfd, bytes))return -EFAULT;
			return ready;
		}
		if(timeout == 0)return 0;
		if(SignalPending())return -EINTR;//有待投递信号: 交给用户态重试
		if(timeout > 0 && waited >= (uint64_t)timeout)return 0;
		uint64_t step = 2;
		if(timeout > 0){
			uint64_t left = (uint64_t)timeout - waited;
			if(left < step)step = left;
		}
		msleep(step);
		waited += step;
	}
}

/*
 * int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout)
 * int pselect6(int nfds, fd_set *r, fd_set *w, fd_set *e, const struct timespec *ts, const sigset_t *mask)
 * 复用 poll 的就绪判定: 控制台按 TTY_ReadReady 判定, 管道按 pipeReady, 普通文件恒就绪
 */
#define KSEL_BYTES 128//fd_set = 1024bit
#define KSEL_IN    0x1
#define KSEL_OUT   0x2
#define KSEL_EXC   0x4

//返回0表示fd有效, *revents 填入已就绪事件; 返回-1表示fd无效
static int sel_fd_check(long fd, int want, int *revents){
	int rv = 0;
	if(!current_task || fd < 0 || fd >= MAX_FD){
		*revents = 0;
		return -1;
	}
	_Bool con_in = false, con_out = false;
	if(fd < 3 && !current_task->files[fd].used){
		con_in = con_out = true;//标准输入输出默认就是控制台
	}else if(fd_is_console(fd)){
		int acc = current_task->files[fd].flags & O_ACCMODE;
		con_in  = (acc != O_WRONLY);
		con_out = (acc != O_RDONLY);
	}
	if(con_in || con_out){
		if(con_in && TTY_ReadReady())rv |= KSEL_IN;
		if(con_out)rv |= KSEL_OUT;
	}else if(current_task->files[fd].used){
		int acc = current_task->files[fd].flags & O_ACCMODE;
		if(current_task->files[fd].pipe){
			int pr = pipeReady((struct pipe*)current_task->files[fd].pipe);
			if((pr & 1) && acc != O_WRONLY)rv |= KSEL_IN;
			if((pr & 2) && acc != O_RDONLY)rv |= KSEL_OUT;
		}else{//普通文件/设备: 恒就绪
			if(acc != O_WRONLY)rv |= KSEL_IN;
			if(acc != O_RDONLY)rv |= KSEL_OUT;
		}
	}else{
		*revents = 0;
		return -1;
	}
	*revents = rv & want;
	return 0;
}

static long select_common(long nfds, long rfd, long wfd, long efd, uint64_t timeout_ms, _Bool have_timeout){
	if(nfds < 0)return -EINVAL;
	unsigned char rb[KSEL_BYTES], wb[KSEL_BYTES], eb[KSEL_BYTES];
	unsigned char ro[KSEL_BYTES], wo[KSEL_BYTES], eo[KSEL_BYTES];
	memset(rb, 0, sizeof(rb)); memset(wb, 0, sizeof(wb)); memset(eb, 0, sizeof(eb));
	unsigned long nset = ((unsigned long)nfds + 7) / 8;
	if(nset > KSEL_BYTES)nset = KSEL_BYTES;
	if(rfd && copy_from_user(rb, (void*)rfd, nset))return -EFAULT;
	if(wfd && copy_from_user(wb, (void*)wfd, nset))return -EFAULT;
	if(efd && copy_from_user(eb, (void*)efd, nset))return -EFAULT;
	uint64_t waited = 0;
	for(;;){
		memset(ro, 0, sizeof(ro)); memset(wo, 0, sizeof(wo)); memset(eo, 0, sizeof(eo));
		int cnt = 0;
		long lim = nfds < MAX_FD ? nfds : MAX_FD;
		for(long fd = 0; fd < lim; fd++){
			int want = 0;
			if(rfd && (rb[fd >> 3] & (1 << (fd & 7))))want |= KSEL_IN;
			if(wfd && (wb[fd >> 3] & (1 << (fd & 7))))want |= KSEL_OUT;
			if(efd && (eb[fd >> 3] & (1 << (fd & 7))))want |= KSEL_EXC;
			if(!want)continue;
			int rv = 0;
			if(sel_fd_check(fd, want | KSEL_EXC, &rv) < 0)continue;//无效fd: 不置位
			if(rv & KSEL_IN){ ro[fd >> 3] |= (unsigned char)(1 << (fd & 7)); cnt++; }
			if(rv & KSEL_OUT){ wo[fd >> 3] |= (unsigned char)(1 << (fd & 7)); cnt++; }
			if(rv & KSEL_EXC){ eo[fd >> 3] |= (unsigned char)(1 << (fd & 7)); cnt++; }
		}
		if(cnt > 0){
			if(rfd && copy_to_user((void*)rfd, ro, nset))return -EFAULT;
			if(wfd && copy_to_user((void*)wfd, wo, nset))return -EFAULT;
			if(efd && copy_to_user((void*)efd, eo, nset))return -EFAULT;
			return cnt;
		}
		if(have_timeout && timeout_ms == 0)return 0;
		if(SignalPending())return -EINTR;
		uint64_t step = 2;
		if(have_timeout){
			uint64_t left = timeout_ms - waited;
			if(left < step)step = left;
			if(step == 0)step = 1;
		}
		msleep(step);
		waited += step;
		if(have_timeout && waited >= timeout_ms)return 0;
	}
}

static long sys_select(long nfds, long rfd, long wfd, long efd, long timeout, long a6){
	(void)a6;
	uint64_t ms = 0;
	_Bool have = false;
	if(timeout){
		struct { long sec; long usec; } tv;
		if(copy_from_user(&tv, (void*)timeout, sizeof(tv)))return -EFAULT;
		if(tv.sec < 0 || tv.usec < 0)return -EINVAL;
		ms = (uint64_t)tv.sec * 1000 + (uint64_t)(tv.usec / 1000);
		have = true;
	}
	return select_common(nfds, rfd, wfd, efd, ms, have);
}

static long sys_pselect6(long nfds, long rfd, long wfd, long efd, long tsp, long a6){
	(void)a6;//sigmask 暂不支持(不阻塞待投递信号)
	uint64_t ms = 0;
	_Bool have = false;
	if(tsp){
		struct { long sec; long nsec; } ts;
		if(copy_from_user(&ts, (void*)tsp, sizeof(ts)))return -EFAULT;
		if(ts.sec < 0 || ts.nsec < 0)return -EINVAL;
		ms = (uint64_t)ts.sec * 1000 + (uint64_t)(ts.nsec / 1000000);
		have = true;
	}
	return select_common(nfds, rfd, wfd, efd, ms, have);
}

/*
 * int rt_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact, size_t sigsetsize)
 * 存储信号动作表
 */
static long sys_rt_sigaction(long signum, long act, long oldact, long sigsetsize, long a5, long a6){
	(void)a5; (void)a6;
	return SignalDoSigaction(signum, act, oldact, sigsetsize);
}

/*
 * long rt_sigreturn(void)
 * 用户restorer执行, 从信号帧恢复上下文
 */
static long sys_rt_sigreturn(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	return SignalReturn();
}

/*
 * int kill(pid_t pid, int sig)
 */
static long sys_kill(long pid, long sig, long c, long a4, long a5, long a6){
	(void)c; (void)a4; (void)a5; (void)a6;
	if(sig < 0 || sig > 64)return -EINVAL;
	task_struct *t = NULL;
	if(pid > 0)t = TaskFind((pid_t)pid);
	else if(pid == 0)t = current_task;//等同调用者
	else return 0;
	if(!t)return -ESRCH;
	if(sig == 0)return 0;//探测存在性
	SignalSend(t, (int)sig, SI_USER, current_task ? current_task->pid : 0, 0);
	return 0;
}

/*
 * int tgkill(pid_t tgid, pid_t tid, int sig) / int tkill(pid_t tid, int sig)
 */
static long sys_tgkill(long tgid, long tid, long sig, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
	if(tgid <= 0 || tid <= 0)return -EINVAL;
	if(tgid != tid)return -EINVAL;//无线程组: tgid必须等于tid
	if(sig < 0 || sig > 64)return -EINVAL;
	task_struct *t = TaskFind((pid_t)tid);
	if(!t)return -ESRCH;
	if(sig == 0)return 0;
	SignalSend(t, (int)sig, SI_TKILL, current_task ? current_task->pid : 0, 0);
	return 0;
}

static long sys_tkill(long tid, long sig, long c, long a4, long a5, long a6){
	(void)c;
	return sys_tgkill(tid, tid, sig, a4, a5, a6);
}

/*
 * int sigaltstack(const stack_t *ss, stack_t *old_ss)
 * 无信号处理: 接受设置, 清零旧值(stack_t=24字节)
 */
static long sys_sigaltstack(long ss, long old_ss, long a3, long a4, long a5, long a6){
	(void)ss; (void)a3; (void)a4; (void)a5; (void)a6;
	if(old_ss){
		char zero[24];
		memset(zero, 0, sizeof(zero));
		if(copy_to_user((void*)old_ss, zero, sizeof(zero)))return -EFAULT;
	}
	return 0;
}

/*
 * int prctl(int option, ...)
 * 暂不支持, -EINVAL(glibc会回退)
 */
static long sys_prctl(long option, long a2, long a3, long a4, long a5, long a6){
	(void)option; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	return -EINVAL;
}

/*
 * pid_t gettid(void)
 */
static long sys_gettid(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if(!current_task)return -1;
	return current_task->pid;
}

/*
 * int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
 * 单核, 返回1个CPU
 */
static long sys_sched_getaffinity(long pid, long cpusetsize, long mask, long a4, long a5, long a6){
	(void)pid; (void)a4; (void)a5; (void)a6;
	if(!mask || cpusetsize <= 0)return -EFAULT;
	uint8_t one = 0x01;
	if(copy_to_user((void*)mask, &one, 1))return -EFAULT;
	return 1;
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

//用户任务退出
void UserTaskExit(long status){
	if(current_task && current_task->mm){
		if(current_task == init_task && (int)status == -1)panic("Init failed: cannot execute init");
		//通知clear_child_tid(写入0, 供futex/线程库检测退出)
		if(current_task->clear_child_tid){
			uint32_t zero = 0;
			copy_to_user(current_task->clear_child_tid, &zero, sizeof(zero));
		}
		TTY_TermRestoreOnExit(current_task->pid);//改过终端模式就把它恢复回来
		for(int i = 0; i < MAX_FD; i++)fd_close_one(i);//关闭所有打开的文件描述符
		current_task->exit_code = (int)status;//保存退出码
		TaskExit();//退出任务
	}
	SYSTEM_STOP();
}

//进程退出
static long __attribute__((noreturn)) do_exit(long status){
	UserTaskExit(status);
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
 * pid_t getppid(void) / uid_t getuid/geteuid / gid_t getgid/getegid
 * 身份一律为root(0)
 */
static long sys_getppid(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if(!current_task)return 0;
	return (long)current_task->parent;
}

static long sys_getuid(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	return 0;
}

static long sys_geteuid(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	return 0;
}

static long sys_getgid(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	return 0;
}

static long sys_getegid(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	return 0;
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
	//回收僵尸
	int code;
	if(zombie->sig_exit > 0)code = zombie->sig_exit & 0x7F;
	else code = (zombie->exit_code & 0xFF) << 8;
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
	long a4 = *(long*)(user_kernel_stack_top - 128 + 40);
	long ret = fn(a1, a2, a3, a4, a5, a6);//调用处理函数
	SignalDeliverUser(ret);//返回用户前投递待处理信号
	return ret;
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
			if(vmm_mmap(mm, start, end - start, VM_READ | VM_WRITE))return (long)mm->brk;
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
 * 支持匿名私有映射与文件私有映射
 */
//把内核数据写入mm的已映射页(文件映射填充用)
static int mmap_write_mem(mm_struct *mm, uintptr_t vaddr, const void *src, uint64_t len){
    while(len){
        uintptr_t page = vaddr & PAGE_MASK;
        uintptr_t off  = vaddr & (PAGE_SIZE - 1);
        uint64_t chunk = len;
        if(chunk > PAGE_SIZE - off) chunk = PAGE_SIZE - off;
        uintptr_t *pte = (uintptr_t*)get_pte((uintptr_t)mm->pgd, page, 0, 0);
        if(!pte || !pte_is_present(*pte)) return -1;
        memcpy((void*)PHYS_TO_VIRT(pte_get_paddr(*pte) + off), src, chunk);
        vaddr += chunk;
        src = (const uint8_t*)src + chunk;
        len -= chunk;
    }
    return 0;
}

static long sys_mmap(long addr, long length, long prot, long flags, long fd, long offset){
	if(!current_task || !current_task->mm)return -ENOMEM;
	if(length <= 0)return -EINVAL;
	if(offset & (PAGE_SIZE - 1))return -EINVAL;//文件偏移必须页对齐
	if(flags & MAP_SHARED)return -ENODEV;//暂不支持共享映射
	fs_file_t *ff = NULL;
	if((long)fd >= 0 && !(flags & MAP_ANONYMOUS)){
		//校验fd与读权限
		if(fd < 3 || fd >= MAX_FD || !current_task->files[fd].used)return -EBADF;
		if((current_task->files[fd].flags & O_ACCMODE) == O_WRONLY)return -EACCES;
		ff = &current_task->files[fd];
	}
	//权限转换
	uint64_t vm_flags = 0;
	if(prot & PROT_READ) vm_flags |= VM_READ;
	if(prot & PROT_WRITE) vm_flags |= VM_WRITE;
	if(prot & PROT_EXEC) vm_flags |= VM_EXEC;
	//确定映射地址
	uintptr_t vaddr;
	uint64_t bytes = ((uint64_t)length + PAGE_SIZE - 1) & PAGE_MASK;
	if(flags & MAP_FIXED){
		if(addr == 0 || ((uintptr_t)addr & (PAGE_SIZE - 1)))return -EINVAL;
		vaddr = (uintptr_t)addr;
		if(vaddr + bytes > USER_VADDR_MAX)return -EINVAL;
		vmm_munmap(current_task->mm, vaddr, (uint64_t)length);
	}else{
		uintptr_t hint = (addr != 0) ? (uintptr_t)addr : current_task->mm->mmap_hint;
		vaddr = vmm_find_gap(current_task->mm, hint, bytes);
		if(!vaddr && addr != 0)vaddr = vmm_find_gap(current_task->mm, current_task->mm->mmap_hint, bytes);
		if(!vaddr)return -ENOMEM;
		current_task->mm->mmap_hint = vaddr + bytes;
	}
	if(vmm_mmap(current_task->mm, vaddr, (uint64_t)length, vm_flags)) return -ENOMEM;
	//文件映射
	if(ff){
		FsSeek(ff, (int64_t)offset, SEEK_SET);
		uint64_t remain = (uint64_t)length;
		uint64_t off = 0;
		const uint64_t BIG = 32 * PAGE_SIZE;
		void *kbuf = (void*)PHYS_TO_VIRT(Pmm_Malloc(32));
		if(!kbuf) return -ENOMEM;
		while(remain){
			uint64_t chunk = remain > BIG ? BIG : remain;
			uint64_t n = FsRead(ff, kbuf, chunk);
			if(n == 0)break;//EOF，剩余保持零
			if(mmap_write_mem(current_task->mm, vaddr + off, kbuf, n)){
				Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 32);
				return -ENOMEM;
			}
			off += n;
			remain -= n;
		}
		Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 32);
	}
	return (long)vaddr;
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
		cli();
		current_task->fs_base = (uint64_t)addr;
		wrmsr(IA32_FS_BASE, (uint64_t)addr);//直接写入FS基址MSR
		sti();
		return 0;
	}
	if(code == ARCH_GET_FS){
		//把FS基址写到用户指针*addr, 成功返回0
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

/*
 * int uname(struct utsname *buf)
 * 返回系统信息
 */
static long sys_uname(long name, long a2, long a3, long a4, long a5, long a6){
	(void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	if(!name)return -EFAULT;
	if(copy_to_user((void*)name,&system_utsname,sizeof(system_utsname)) != 0)return -EFAULT;
	return 0;
}

/* 
 * int reboot(int magic,int magic2,int cmd, void *arg)
 * 重启/关机/挂起系统
 */
static long sys_reboot(long magic,long magic2,long cmd,long arg,long a5,long a6){
	(void)arg; (void)a5; (void)a6;
	//魔数校验+
	if((uint32_t)magic != (uint32_t)LINUX_REBOOT_MAGIC1)return -EINVAL;
	uint32_t m2 = (uint32_t)magic2;
	if(m2 != (uint32_t)LINUX_REBOOT_MAGIC2 && m2 != (uint32_t)LINUX_REBOOT_MAGIC2A &&
	   m2 != (uint32_t)LINUX_REBOOT_MAGIC2B && m2 != (uint32_t)LINUX_REBOOT_MAGIC2C)return -EINVAL;
	switch((uint32_t)cmd){
		//重启
		case (uint32_t)LINUX_REBOOT_CMD_RESTART:
			SYSTEM_Restart();
			break;
		//关机
		case (uint32_t)LINUX_REBOOT_CMD_POWER_OFF:
			SYSTEM_Shutdown();
			break;
		//停机
		case (uint32_t)LINUX_REBOOT_CMD_HALT:
			SYSTEM_Halt();
			break;
		default:
			return -EINVAL;
	}
	return 0;
}

void InitSyscall(void){
    //初始化系统调用表
	memset(syscall_table, 0, sizeof(syscall_table));
	syscall_table[SYS_READ]       = sys_read;
	syscall_table[SYS_WRITE]      = sys_write;
	syscall_table[SYS_PREAD64]    = sys_pread64;
	syscall_table[SYS_GETDENTS64] = sys_getdents64;
	syscall_table[SYS_OPEN]       = sys_open;
	syscall_table[SYS_CLOSE]      = sys_close;
	syscall_table[SYS_STAT]       = sys_newstat;
	syscall_table[SYS_FSTAT]      = sys_newfstat;
	syscall_table[SYS_OPENAT]     = sys_openat;
	syscall_table[SYS_NEWFSTATAT] = sys_newfstatat;
	syscall_table[SYS_LSEEK]      = sys_lseek;
	syscall_table[SYS_ACCESS]     = sys_access;
	syscall_table[SYS_FACCESSAT]  = sys_faccessat;
	syscall_table[SYS_READLINK]   = sys_readlink;
	syscall_table[SYS_GETCWD]     = sys_getcwd;
	syscall_table[SYS_CHDIR]      = sys_chdir;
	syscall_table[SYS_IOCTL]      = sys_ioctl;
	syscall_table[SYS_PIPE]       = sys_pipe;
	syscall_table[SYS_PIPE2]      = sys_pipe2;
	syscall_table[SYS_DUP]        = sys_dup;
	syscall_table[SYS_DUP2]       = sys_dup2;
	syscall_table[SYS_DUP3]       = sys_dup3;
	syscall_table[SYS_READV]      = sys_readv;
	syscall_table[SYS_WRITEV]     = sys_writev;
	syscall_table[SYS_READLINKAT]      = sys_readlinkat;
	syscall_table[SYS_SET_ROBUST_LIST] = sys_set_robust_list;
	syscall_table[SYS_PRLIMIT64]       = sys_prlimit64;
	syscall_table[SYS_GETRANDOM]       = sys_getrandom;
	syscall_table[SYS_GETTIMEOFDAY]    = sys_gettimeofday;
	syscall_table[SYS_TIME]            = sys_time;
	syscall_table[SYS_CLOCK_GETTIME]   = sys_clock_gettime;
	syscall_table[SYS_CLOCK_GETRES]    = sys_clock_getres;
	syscall_table[SYS_CLOCK_NANOSLEEP] = sys_clock_nanosleep;
	syscall_table[SYS_NANOSLEEP]  = sys_nanosleep;
	syscall_table[SYS_POLL]       = sys_poll;
	syscall_table[SYS_SELECT]     = sys_select;
	syscall_table[SYS_PSELECT6]   = sys_pselect6;
	syscall_table[SYS_RT_SIGACTION]     = sys_rt_sigaction;
	syscall_table[SYS_RT_SIGPROCMASK]   = sys_rt_sigprocmask;
	syscall_table[SYS_RT_SIGRETURN]     = sys_rt_sigreturn;
	syscall_table[SYS_SETPGID]          = sys_setpgid;
	syscall_table[SYS_GETPGRP]          = sys_getpgrp;
	syscall_table[SYS_GETPGID]          = sys_getpgid;
	syscall_table[SYS_KILL]             = sys_kill;
	syscall_table[SYS_TKILL]            = sys_tkill;
	syscall_table[SYS_TGKILL]           = sys_tgkill;
	syscall_table[SYS_SIGALTSTACK]      = sys_sigaltstack;
	syscall_table[SYS_FCNTL]            = sys_fcntl;
	syscall_table[SYS_PRCTL]            = sys_prctl;
	syscall_table[SYS_GETTID]           = sys_gettid;
	syscall_table[SYS_SCHED_GETAFFINITY] = sys_sched_getaffinity;
	syscall_table[SYS_GETPID]     = sys_getpid;
	syscall_table[SYS_GETPPID]    = sys_getppid;
	syscall_table[SYS_GETUID]     = sys_getuid;
	syscall_table[SYS_GETEUID]    = sys_geteuid;
	syscall_table[SYS_GETGID]     = sys_getgid;
	syscall_table[SYS_GETEGID]    = sys_getegid;
	syscall_table[SYS_CLONE]      = sys_clone;
	syscall_table[SYS_FORK]       = sys_fork;
	syscall_table[SYS_VFORK]      = sys_vfork;
	syscall_table[SYS_EXECVE]     = sys_execve;
	syscall_table[SYS_EXIT]       = sys_exit;
	syscall_table[SYS_WAIT4]      = sys_waitpid;
	syscall_table[SYS_EXIT_GROUP] = sys_exit_group;
	syscall_table[SYS_MKDIR]      = sys_mkdir;
	syscall_table[SYS_MKDIRAT]    = sys_mkdirat;
	syscall_table[SYS_RMDIR]      = sys_rmdir;
	syscall_table[SYS_TRUNCATE]   = sys_truncate;
	syscall_table[SYS_FTRUNCATE]  = sys_ftruncate;
	syscall_table[SYS_FSYNC]      = sys_fsync;
	syscall_table[SYS_FDATASYNC]  = sys_fsync;
	syscall_table[SYS_SYNC]       = sys_sync;
	syscall_table[SYS_CHMOD]      = sys_chmod;
	syscall_table[SYS_FCHMOD]     = sys_fchmod;
	syscall_table[SYS_FCHMODAT]   = sys_fchmodat;
	syscall_table[SYS_UMASK]      = sys_umask;
	syscall_table[SYS_UNLINK]     = sys_unlink;
	syscall_table[SYS_UNLINKAT]   = sys_unlinkat;
	syscall_table[SYS_RENAME]     = sys_rename;
	syscall_table[SYS_RENAMEAT]   = sys_renameat;
	syscall_table[SYS_RENAMEAT2]  = sys_renameat2;
	syscall_table[SYS_UTIMENSAT]  = sys_utimensat;
	syscall_table[SYS_STATX]      = sys_statx;
	syscall_table[SYS_BRK]        = sys_brk;
	syscall_table[SYS_ARCH_PRCTL] = sys_arch_prctl;
	syscall_table[SYS_FUTEX]            = sys_futex;
	syscall_table[SYS_SET_TID_ADDRESS]  = sys_set_tid_address;
	syscall_table[SYS_MMAP]       = sys_mmap;
	syscall_table[SYS_MUNMAP]     = sys_munmap;
	syscall_table[SYS_MPROTECT]   = sys_mprotect;
	syscall_table[SYS_UNAME]	  = sys_uname;
	syscall_table[SYS_REBOOT]	  = sys_reboot;
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
