#ifndef _SYSCALLS_H_
#define _SYSCALLS_H_

#include <types.h>

//POSIX/x86_64系统调用号
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_LSEEK           8
#define SYS_MMAP            9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_NANOSLEEP       35
#define SYS_GETPID          39
#define SYS_FORK            57
#define SYS_EXECVE          59
#define SYS_EXIT            60
#define SYS_WAIT4           61
#define SYS_MKDIR           83
#define SYS_RMDIR           84
#define SYS_UNLINK          87
#define SYS_ARCH_PRCTL      158
#define SYS_FUTEX           202
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP      231
#define SYSCALL_TABLE_SIZE  256

//POSIX open标志
#define O_RDONLY    0       //只读打开
#define O_WRONLY    1       //只写打开
#define O_RDWR      2       //读写打开
#define O_ACCMODE   3       //访问模式掩码
#define O_CREAT     0x40    //文件不存在则创建
#define O_TRUNC     0x200   //打开时截断文件长度为0
#define O_APPEND    0x400   //每次写入追加到文件末尾

//lseek whence
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

//mmap保护位
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

//mmap标志位
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

//arch_prctl代码
#define ARCH_SET_FS  0x1002 //设置FS段基址
#define ARCH_GET_FS  0x1003 //读取FS段基址

//errno值（POSIX）
#define EPERM               1
#define ENOENT              2
#define EINTR               4
#define EIO                 5
#define EBADF               9
#define EAGAIN              11
#define ENOMEM              12
#define EACCES              13
#define EFAULT              14
#define EBUSY               16
#define EEXIST              17
#define ECHILD             10
#define EINVAL             22
#define ENOEXEC             8
#define E2BIG               7
#define ENOSYS             38
#define ENFILE             23
#define ENOSPC             28
#define ENODEV             19
#define ETIMEDOUT         110
//waitpid选项
#define WNOHANG             1//不阻塞,没有已退出的子进程时立即返回0

//futex操作
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256

//POSIX timespec
typedef struct timespec {
    long tv_sec;//秒
    long tv_nsec;//纳秒（0 ~ 999999999）
} timespec_t;

extern void syscall_entry(void);//汇编入口（kernel/asm/syscall.S）

void InitSyscall(void);//初始化SYSCALL机制

/**
 * @brief 内核从用户空间拷贝（返回未拷贝字节数，0 = 成功）
 * @param to   用户目标地址
 * @param from 内核源地址
 * @param n    字节数
 */
uint64_t copy_from_user(void *to, const void *from, uint64_t n);
/**
 * @brief 用户从内核拷贝（返回未拷贝字节数，0 = 成功）
 * @param to   内核目标地址
 * @param from 用户源地址
 * @param n    字节数
 */
uint64_t copy_to_user(void *to, const void *from, uint64_t n);

/**
 * @brief 系统调用分发器（由汇编入口调用）
 * @param num 调用号
 * @param a1/a2/a3 参数 1~3
 * @param a5/a6 参数 5~6(参数4在syscall_arg6全局变量中)
 * @return 系统调用返回值（负数 = -errno）
 */
long syscall_dispatch(long num, long a1, long a2, long a3, long a5, long a6);
extern long syscall_arg6;//参数4(r10)保存区

/**
 * @brief 系统调用wait4处理函数
 * @param pid 子进程PID(<0=任意子进程)
 * @param wstatus 输出退出状态(原始退出码)
 * @param options WNOHANG等选项
 * @return 回收的子进程PID,失败返回-errno
 * @author DeepSeek V4 Pro
 */
long sys_waitpid(long pid, long wstatus, long options, long a4, long a5, long a6);

/**
 * @brief 系统调用execve处理函数:加载并执行新程序
 * @param path 程序路径(用户指针)
 * @param argv 参数指针数组(用户指针,以NULL结尾)
 * @param envp 环境变量指针数组(用户指针,可为NULL)
 * @return 成功不返回,失败返回-errno
 */
long sys_execve(long path, long argv, long envp, long a4, long a5, long a6);

/**
 * @brief 系统调用
 * @param num 调用号
 * @param arg1/arg2/arg3 参数 1~3
 * @return 系统调用返回值
 */
#define syscall(num, arg1, arg2, arg3) ({ \
    long __ret; \
    __asm__ volatile( \
        "syscall" \
        : "=a" (__ret) \
        : "a" (num), \
          "D" (arg1), \
          "S" (arg2), \
          "d" (arg3) \
        : "rcx", "r11", "memory" \
    ); \
    __ret; \
})


#endif
