# Reedx Beta 0.0.1 Pre-realese

[English](README_en.md) | 中文

## 简介
- Reedx是一个完全使用C语言编写的类`Unix`宏内核,严格按照POSIX标准,可以运行基本的Unix程序,当前版本为测试版
- 目前仅支持`x86-64`架构，使用固定的UEFI引导程序启动(`data/BOOTX64.EFI`)启动，内核为ELF文件,目前必须将所有文件放在ESP分区中，目前仅支持`FAT32`文件系统
- 目前支持的硬件驱动有`PS/2键盘驱动`,`ATA(IDE)驱动`,`SATA(AHCI)驱动`,`NVMe驱动`

## API接口

### 系统调用 (Syscall)

**调用约定**：用户态通过 `syscall` 指令进入内核（x86_64）。

| 寄存器 | 用途 |
|--------|------|
| `rax` | 系统调用号（返回时存放返回值） |
| `rdi` | 参数 1 |
| `rsi` | 参数 2 |
| `rdx` | 参数 3 |
| `r10` | 参数 4 |
| `r8`  | 参数 5 |
| `r9`  | 参数 6 |

- **返回值**：成功为非负值；失败为 `-errno`（例如 `-ENOENT`、`-EINVAL`）
- **被破坏寄存器**：`rcx`、`r11`（`syscall` 指令的硬件行为，其余寄存器由内核原样保留）

#### 文件与 I/O

| 号 | 名称 | 说明 |
|----|------|------|
| 0   | `read` | 从文件描述符读取 |
| 1   | `write` | 向文件描述符写入 |
| 2   | `open` | 打开/创建文件 |
| 3   | `close` | 关闭文件描述符 |
| 4   | `stat` | 按路径获取文件状态 |
| 5   | `fstat` | 按 fd 获取文件状态 |
| 7   | `poll` | 等待 fd 上的事件 |
| 8   | `lseek` | 移动文件读写偏移 |
| 16  | `ioctl` | 设备控制（终端 termios / 窗口尺寸） |
| 17  | `pread64` | 指定偏移读取（不改变文件指针） |
| 19  | `readv` | 分散读取到多个缓冲区 |
| 20  | `writev` | 从多个缓冲区聚集写入 |
| 21  | `access` | 检查文件可访问性 |
| 22  | `pipe` | 创建管道 |
| 32  | `dup` | 复制文件描述符 |
| 33  | `dup2` | 复制到指定 fd 号 |
| 72  | `fcntl` | 文件描述符控制（复制 / 文件锁 / 管道大小） |
| 74  | `fsync` | 同步文件到存储 |
| 75  | `fdatasync` | 同步文件数据到存储 |
| 76  | `truncate` | 按路径截断/扩展文件 |
| 77  | `ftruncate` | 按 fd 截断/扩展文件 |
| 79  | `getcwd` | 获取当前工作目录 |
| 80  | `chdir` | 切换当前工作目录 |
| 82  | `rename` | 重命名/移动文件 |
| 83  | `mkdir` | 创建目录 |
| 84  | `rmdir` | 删除空目录 |
| 87  | `unlink` | 删除文件目录项 |
| 89  | `readlink` | 读取符号链接目标 |
| 90  | `chmod` | 修改文件权限 |
| 91  | `fchmod` | 按 fd 修改文件权限 |
| 95  | `umask` | 设置权限掩码 |
| 162 | `sync` | 全盘同步 |
| 217 | `getdents64` | 读取目录项 |
| 257 | `openat` | 相对 dirfd 打开 |
| 258 | `mkdirat` | 相对 dirfd 创建目录 |
| 262 | `newfstatat` | 相对 dirfd 获取状态 |
| 263 | `unlinkat` | 相对 dirfd 删除目录项 |
| 264 | `renameat` | 相对 dirfd 重命名 |
| 267 | `readlinkat` | 相对 dirfd 读取链接 |
| 268 | `fchmodat` | 相对 dirfd 修改权限 |
| 269 | `faccessat` | 相对 dirfd 检查权限 |
| 280 | `utimensat` | 设置文件时间戳 |
| 292 | `dup3` | 复制 fd（带 flags） |
| 293 | `pipe2` | 创建管道（带 flags） |
| 316 | `renameat2` | 带标志的重命名 |
| 332 | `statx` | 扩展文件状态查询 |

#### 内存管理

| 号 | 名称 | 说明 |
|----|------|------|
| 9  | `mmap` | 建立内存映射（匿名/文件，`MAP_PRIVATE`/`MAP_SHARED`） |
| 10 | `mprotect` | 修改内存保护属性 |
| 11 | `munmap` | 解除内存映射 |
| 12 | `brk` | 调整堆顶（program break） |

#### 进程与线程

| 号 | 名称 | 说明 |
|----|------|------|
| 39  | `getpid` | 获取进程 ID |
| 56  | `clone` | 创建任务 |
| 57  | `fork` | 复制进程（写时复制） |
| 58  | `vfork` | 复制进程（父进程阻塞） |
| 59  | `execve` | 加载并执行新程序（ELF，含 `PT_INTERP`） |
| 60  | `exit` | 退出当前线程 |
| 61  | `wait4` | 等待子进程并回收 |
| 109 | `setpgid` | 设置进程组 |
| 110 | `getppid` | 获取父进程 ID |
| 111 | `getpgrp` | 获取进程组 ID |
| 121 | `getpgid` | 获取指定进程的进程组 ID |
| 158 | `arch_prctl` | 架构相关（`ARCH_SET_FS`/`ARCH_GET_FS`） |
| 186 | `gettid` | 获取线程 ID |
| 218 | `set_tid_address` | 设置 clear_child_tid |
| 231 | `exit_group` | 退出整个线程组 |

#### 信号

| 号 | 名称 | 说明 |
|----|------|------|
| 13  | `rt_sigaction` | 设置信号处理动作 |
| 14  | `rt_sigprocmask` | 设置信号掩码 |
| 15  | `rt_sigreturn` | 从信号处理函数返回 |
| 62  | `kill` | 发送信号 |
| 131 | `sigaltstack` | 设置备用信号栈 |
| 200 | `tkill` | 向单个线程发送信号 |
| 234 | `tgkill` | 向线程组内的指定线程发送信号 |
| 273 | `set_robust_list` | 设置 robust futex 链表 |

#### 时间

| 号 | 名称 | 说明 |
|----|------|------|
| 35  | `nanosleep` | 高精度睡眠 |
| 96  | `gettimeofday` | 获取墙上时间（秒 + 微秒） |
| 201 | `time` | 获取秒级时间 |
| 228 | `clock_gettime` | 获取指定时钟时间 |
| 229 | `clock_getres` | 获取时钟精度 |
| 230 | `clock_nanosleep` | 定时睡眠 |

#### 系统与用户信息

| 号 | 名称 | 说明 |
|----|------|------|
| 63  | `uname` | 获取系统信息 |
| 102 | `getuid` | 获取用户 ID |
| 104 | `getgid` | 获取组 ID |
| 107 | `geteuid` | 获取有效用户 ID |
| 108 | `getegid` | 获取有效组 ID |
| 157 | `prctl` | 进程控制 |
| 169 | `reboot` | 重启 / 关机 / 停机 |
| 202 | `futex` | 快速用户态互斥（`FUTEX_WAIT`/`FUTEX_WAKE`） |
| 204 | `sched_getaffinity` | 获取 CPU 亲和性掩码 |
| 302 | `prlimit64` | 获取/设置资源限制 |
| 318 | `getrandom` | 获取随机字节 |

#### 常用常量

| 类别 | 常量 |
|------|------|
| `open` 标志 | `O_RDONLY` `O_WRONLY` `O_RDWR` `O_CREAT` `O_EXCL` `O_TRUNC` `O_APPEND` `O_NONBLOCK` `O_DIRECTORY` `O_CLOEXEC` |
| `mmap` 保护 | `PROT_NONE` `PROT_READ` `PROT_WRITE` `PROT_EXEC` |
| `mmap` 标志 | `MAP_SHARED` `MAP_PRIVATE` `MAP_FIXED` `MAP_ANONYMOUS` |
| `lseek` | `SEEK_SET` `SEEK_CUR` `SEEK_END` |
| `*at` 系列 | `AT_FDCWD` `AT_EMPTY_PATH` `AT_SYMLINK_NOFOLLOW` `AT_REMOVEDIR` |
| 时间戳 | `UTIME_NOW` `UTIME_OMIT` |
| `fcntl` | `F_DUPFD` `F_GETFD` `F_SETFD` `F_GETFL` `F_SETFL` `F_GETLK` `F_SETLK` `F_SETLKW` `F_DUPFD_CLOEXEC` `F_SETPIPE_SZ` `F_GETPIPE_SZ` |
| `futex` 操作 | `FUTEX_WAIT` `FUTEX_WAKE` `FUTEX_REQUEUE` `FUTEX_CMP_REQUEUE` `FUTEX_WAIT_BITSET` `FUTEX_WAKE_BITSET` |
| `clockid` | `CLOCK_REALTIME` `CLOCK_MONOTONIC` `CLOCK_PROCESS_CPUTIME_ID` `CLOCK_REALTIME_COARSE` `CLOCK_MONOTONIC_COARSE` |
| `wait` 选项 | `WNOHANG` |
| `errno` | `EPERM` `ENOENT` `ESRCH` `EINTR` `EIO` `EBADF` `ECHILD` `EAGAIN` `ENOMEM` `EACCES` `EFAULT` `EEXIST` `ENODEV` `ENOTDIR` `EISDIR` `EINVAL` `ENFILE` `ENOSPC` `EPIPE` `ERANGE` `ENOSYS` `ENOTEMPTY` `EOPNOTSUPP` `ETIMEDOUT` |

### 内核启动参数
```c
typedef struct memory_info {
    UINT64 MapSize;
    UINT64 DescriptorSize;
    UINT32 DescriptorVersion;
    EFI_MEMORY_DESCRIPTOR *Buffer;
} __attribute__((packed)) memory_info_t;

typedef struct screen_info {
    _Bool       IsAvailable;
    UINT32*     FrameBuffer;
    UINTN       FrameBuffer_Size;
    UINT32      Hieght;
    UINT32      Width;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
}__attribute__((packed)) screen_info_t;

typedef struct disk_info {
    _Bool IsGpt;
    EFI_GUID PartitionGuid;
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;
}__attribute__((packed)) disk_info_t;

typedef struct boot_param {
    memory_info_t MemoryInfo;//内存信息
    screen_info_t ScreenInfo;//屏幕信息
    disk_info_t DiskInfo;//磁盘信息
    VOID* RSDP;//RSDP地址
    SMBIOS_TABLE_ENTRY_POINT *SMBIOS;//SMBIOS节点地址
    EFI_RUNTIME_SERVICES *RuntimeServices;//运行时服务
    UINTN RandomSeed;//随机数种子
    UINTN KernelAddress;//内核地址
    UINTN KernelSize;//内核大小
    UINTN KernelStackAddress;//内核栈地址
    UINTN KernelStackSize;//内核栈大小
    _Bool PrintLog;//是否显示日志到屏幕
}__attribute__((packed)) boot_param_t;
```

## 构建
|项目|要求|
|-------|---------|
|操作系统|Linux发行版|
|构建工具|`gcc-x86-64-linux-gnu`,`GNU-Make`|

- 构建命令见`make help`

## 参与开发:
- Liu Chunyi(67%)
- DeepSeek V4(33%)