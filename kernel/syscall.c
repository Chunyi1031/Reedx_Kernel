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
static long sys_read(long fd, long buf, long count){
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
static long sys_write(long fd, long buf, long count){
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
static long sys_open(long path, long flags, long mode){
	(void)mode;
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
static long sys_close(long fd, long b, long c){
	(void)b; (void)c;
	if (!current_task) return -EBADF;
	if (fd < 3 || fd >= MAX_FD || !current_task->files[fd].used) return -EBADF;
	FsClose(&current_task->files[fd]);
	return 0;
}

/*
 * off_t lseek(int fd, off_t offset, int whence)
 * 系统调用:lseek
 * 移动文件指针
 */
static long sys_lseek(long fd, long off, long whence){
	if (!current_task) return -EBADF;
	if (fd < 3 || fd >= MAX_FD || !current_task->files[fd].used) return -EBADF;
	int r = FsSeek(&current_task->files[fd], off, (int)whence);
	if (r < 0) return -EINVAL;
	return r;
}

/*
 * int nanosleep(const struct timespec *req, struct timespec *rem)
 */
static long sys_nanosleep(long req, long rem, long unused){
	(void)unused;
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
		current_task->exit_code = (int)status;//保存退出码
		TaskExit();//退出任务
	}
	SYSTEM_STOP();
}

/*
 * void _exit(int status)
 * 退出当前线程
 */
static long sys_exit(long status, long b, long c){
	(void)b; (void)c;
	do_exit(status);
}

/*
 * void exit_group(int status)
 * 退出整个线程组
 */
static long sys_exit_group(long status, long b, long c){
	(void)b; (void)c;
	do_exit(status);
}

/*
 * pid_t getpid(void)
 * 系统调用:getpid
*/
static long sys_getpid(long a, long b, long c){
	(void)a; (void)b; (void)c;
	if (!current_task) return -1;
	return (long)current_task->pid;
}

/*
 * pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *ru)
 * 等待子进程退出并回收其资源
 */
long sys_waitpid(long pid, long wstatus, long options){
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
typedef long (*syscall_fn)(long, long, long);
static syscall_fn syscall_table[SYSCALL_TABLE_SIZE];

//系统调用分发器
long syscall_dispatch(long num, long a1, long a2, long a3){
	if(num < 0 || num >= SYSCALL_TABLE_SIZE)return -ENOSYS;//检查调用号
    //获取处理函数
	syscall_fn fn = syscall_table[num];
	if(!fn)return -ENOSYS;
	long ret = fn(a1, a2, a3);//调用处理函数
	return ret;
}

void InitSyscall(void){
    //初始化系统调用表
	memset(syscall_table, 0, sizeof(syscall_table));
	syscall_table[SYS_READ]       = sys_read;
	syscall_table[SYS_WRITE]      = sys_write;
	syscall_table[SYS_OPEN]       = sys_open;
	syscall_table[SYS_CLOSE]      = sys_close;
	syscall_table[SYS_LSEEK]      = sys_lseek;
	syscall_table[SYS_NANOSLEEP]  = sys_nanosleep;
	syscall_table[SYS_GETPID]     = sys_getpid;
	syscall_table[SYS_FORK]       = sys_fork;
	syscall_table[SYS_EXIT]       = sys_exit;
	syscall_table[SYS_WAIT4]      = sys_waitpid;
	syscall_table[SYS_EXIT_GROUP] = sys_exit_group;
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
