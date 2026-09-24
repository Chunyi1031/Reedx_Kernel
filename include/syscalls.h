#ifndef _SYSCALLS_H_
#define _SYSCALLS_H_

#include <types.h>

//POSIX/x86_64系统调用号
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_STAT            4
#define SYS_FSTAT           5
#define SYS_POLL            7
#define SYS_LSEEK           8
#define SYS_MMAP            9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_RT_SIGACTION    13
#define SYS_RT_SIGPROCMASK  14
#define SYS_RT_SIGRETURN    15
#define SYS_IOCTL           16
#define SYS_PREAD64         17
#define SYS_WRITEV          20
#define SYS_ACCESS          21
#define SYS_PIPE            22
#define SYS_DUP             32
#define SYS_DUP2            33
#define SYS_NANOSLEEP       35
#define SYS_GETPID          39
#define SYS_CLONE           56
#define SYS_FORK            57
#define SYS_VFORK           58
#define SYS_EXECVE          59
#define SYS_EXIT            60
#define SYS_WAIT4           61
#define SYS_KILL            62
#define SYS_UNAME           63
#define SYS_FCNTL           72
#define SYS_GETCWD          79
#define SYS_CHDIR           80
#define SYS_RENAME          82
#define SYS_MKDIR           83
#define SYS_RMDIR           84
#define SYS_UNLINK          87
#define SYS_READLINK        89
#define SYS_GETTIMEOFDAY    96
#define SYS_GETUID          102
#define SYS_GETGID          104
#define SYS_GETEUID         107
#define SYS_GETEGID         108
#define SYS_SETPGID         109
#define SYS_GETPPID         110
#define SYS_GETPGRP         111
#define SYS_GETPGID         121
#define SYS_SIGALTSTACK     131
#define SYS_PRCTL           157
#define SYS_ARCH_PRCTL      158
#define SYS_REBOOT          169
#define SYS_GETTID          186
#define SYS_TKILL           200
#define SYS_TIME            201
#define SYS_FUTEX           202
#define SYS_SCHED_GETAFFINITY 204
#define SYS_GETDENTS64      217
#define SYS_SET_TID_ADDRESS 218
#define SYS_CLOCK_GETTIME   228
#define SYS_CLOCK_GETRES    229
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EXIT_GROUP      231
#define SYS_TGKILL          234
#define SYS_OPENAT          257
#define SYS_MKDIRAT         258
#define SYS_UNLINKAT        263
#define SYS_NEWFSTATAT      262
#define SYS_RENAMEAT        264
#define SYS_READLINKAT      267
#define SYS_FACCESSAT       269
#define SYS_SET_ROBUST_LIST 273
#define SYS_DUP3            292
#define SYS_PIPE2           293
#define SYS_PRLIMIT64       302
#define SYS_RENAMEAT2       316
#define SYS_GETRANDOM       318
#define AT_FDCWD            (-100)
#define AT_EMPTY_PATH       0x1000  //path 为空字符串时对 dirfd 自身操作(glibc fstat 用它)

#define AT_REMOVEDIR        0x200
#define RENAME_NOREPLACE    1
#define RENAME_EXCHANGE     2
#define RENAME_WHITEOUT     4

#define SYSCALL_TABLE_SIZE  512

//POSIX open标志
#define O_RDONLY    0       //只读打开
#define O_WRONLY    1       //只写打开
#define O_RDWR      2       //读写打开
#define O_ACCMODE   3       //访问模式掩码
#define O_CREAT     0x40    //文件不存在则创建
#define O_EXCL      0x80    //与O_CREAT同用: 已存在则失败
#define O_TRUNC     0x200   //打开时截断文件长度为0
#define O_APPEND    0x400   //每次写入追加到文件末尾
#define O_NONBLOCK  0x800   //非阻塞
#define O_DIRECTORY 0x10000 //要求路径必须是一个目录
#define O_TMPFILE   (0x400000 | O_DIRECTORY)//匿名临时文件
#define O_CLOEXEC   0x80000 //exec时关闭

//fcntl命令
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define F_DUPFD_CLOEXEC 1030

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

//信号掩码操作
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

//errno值（POSIX）
#define EPERM               1
#define ENOENT              2
#define ESRCH               3
#define EINTR               4
#define EIO                 5
#define E2BIG               7
#define ENOEXEC             8
#define EBADF               9
#define ECHILD              10
#define EAGAIN              11
#define ENOMEM              12
#define EACCES              13
#define EFAULT              14
#define EBUSY               16
#define EEXIST              17
#define ENODEV              19
#define ENOTDIR             20
#define EISDIR              21
#define EINVAL              22
#define ENOTTY              25
#define ENFILE              23
#define ENOSPC              28
#define EPIPE               32
#define ERANGE              34
#define ENOSYS              38
#define EOPNOTSUPP          95
#define ETIMEDOUT           110

#define WNOHANG             1 //waitpid选项

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

//writev用iovec
typedef struct iovec {
    void    *iov_base;
    uint64_t iov_len;
} iovec_t;

//prlimit64用rlimit
typedef struct rlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} rlimit64_t;
#define RLIMIT_STACK 3

//gettimeofday用timeval
typedef struct timeval {
    long tv_sec;
    long tv_usec;
} timeval_t;

//clock_gettime时钟ID
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6

//文件类型/权限位(POSIX)
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_IRWXU  00700
#define S_IRWXG  00070
#define S_IRWXO  00007

//Linux x86_64 struct stat
typedef struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
} stat_t;

extern void syscall_entry(void);//汇编入口（kernel/asm/syscall.S）

void InitSyscall(void);//初始化SYSCALL机制

/**
 * @brief 内核从用户空间拷贝（返回未拷贝字节数，0 = 成功）
 * @param to   用户目标地址
 * @param from 内核源地址
 * @param n    字节数
 */
uint64_t copy_from_user(void *to, const void *from, uint64_t n);
long strncpy_from_user(char *dst, const void *src, long max);
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
