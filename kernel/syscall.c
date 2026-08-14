#include <syscalls.h>
#include <desc.h>
#include <serial.h>
#include <print.h>
#include <task.h>
#include <klib.h>

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

/*
 * ssize_t write(int fd, const void *buf, size_t count)
 * fd=1(stdout)/fd=2(stderr) 输出到串口，其余返回 -EBADF
 */
static long sys_write(long fd, long buf, long count){
	if ((fd != 1) && (fd != 2)) return -EBADF;
	if (!buf || count <= 0) return -EINVAL;
	const char *s = (const char*)buf;
	for (long i = 0; i < count; i++) SerialWriteChar(SERIAL_COM1, s[i]);
	return count;
}

/*
    ssize_t read(int fd, void *buf, size_t count)
    系统调用:read
*/
static long sys_read(long fd, long buf, long count){
	(void)fd; (void)buf; (void)count;
	return -ENOSYS;
}

/*
    pid_t getpid(void)
    系统调用:getpid
*/
static long sys_getpid(long a, long b, long c){
	(void)a; (void)b; (void)c;
	if (!current_task) return -1;
	return (long)current_task->pid;
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
	syscall_table[SYS_READ]   = sys_read;
	syscall_table[SYS_WRITE]  = sys_write;
	syscall_table[SYS_GETPID] = sys_getpid;
	wrmsr(IA32_EFER, rdmsr(IA32_EFER) | (1ULL << 0));//启用SYSCALL/SYSRET
	wrmsr(IA32_STAR, ((uint64_t)__USER32_CS << 48) | ((uint64_t)__KERNEL_CS << 32));//设置段选择子
	wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);//入口RIP
	wrmsr(IA32_FMASK, 0x700);//进入内核时清除TF|IF|DF
	printk(PRINTK_INFO"Syscalls ready");
}
