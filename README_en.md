# Reedx Beta 0.0.1 Pre-release

English | [中文](README.md)

## Introduction
- Reedx is a Unix-like macrokernel written entirely in C, strictly following the POSIX standard. It can run basic Unix programs. The current version is a beta release.
- Currently only the `x86-64` architecture is supported. It boots via a fixed UEFI bootloader (`data/BOOTX64.EFI`). The kernel is an ELF file. Currently all files must be placed in the ESP partition, and only the `FAT32` file system is supported.
- Currently supported hardware drivers include: `PS/2 keyboard driver`, `ATA (IDE) driver`, `SATA (AHCI) driver`, `NVMe driver`, `Intel VMD driver`.

## [License](LICENSE)

## API Interface

### System Calls (Syscall)

**Calling convention**: User space enters the kernel via the `syscall` instruction (x86_64).

| Register | Purpose |
|----------|---------|
| `rax` | System call number (return value stored here on return) |
| `rdi` | Argument 1 |
| `rsi` | Argument 2 |
| `rdx` | Argument 3 |
| `r10` | Argument 4 |
| `r8`  | Argument 5 |
| `r9`  | Argument 6 |

- **Return value**: Non-negative on success; `-errno` on failure (e.g., `-ENOENT`, `-EINVAL`)
- **Clobbered registers**: `rcx`, `r11` (hardware behavior of the `syscall` instruction; all other registers are preserved by the kernel)

#### File and I/O

| No. | Name | Description |
|-----|------|-------------|
| 0   | `read` | Read from a file descriptor |
| 1   | `write` | Write to a file descriptor |
| 2   | `open` | Open/create a file |
| 3   | `close` | Close a file descriptor |
| 4   | `stat` | Get file status by path |
| 5   | `fstat` | Get file status by fd |
| 7   | `poll` | Wait for events on fds |
| 8   | `lseek` | Move file read/write offset |
| 16  | `ioctl` | Device control (terminal termios / window size) |
| 17  | `pread64` | Read at a specified offset (without changing file pointer) |
| 19  | `readv` | Scatter read into multiple buffers |
| 20  | `writev` | Gather write from multiple buffers |
| 21  | `access` | Check file accessibility |
| 22  | `pipe` | Create a pipe |
| 23  | `select` | I/O multiplexing (wait for fds to be readable/writable) |
| 32  | `dup` | Duplicate a file descriptor |
| 33  | `dup2` | Duplicate to a specified fd number |
| 72  | `fcntl` | File descriptor control (duplicate / file locks / pipe size) |
| 74  | `fsync` | Synchronize file to storage |
| 75  | `fdatasync` | Synchronize file data to storage |
| 76  | `truncate` | Truncate/extend a file by path |
| 77  | `ftruncate` | Truncate/extend a file by fd |
| 79  | `getcwd` | Get current working directory |
| 80  | `chdir` | Change current working directory |
| 82  | `rename` | Rename/move a file |
| 83  | `mkdir` | Create a directory |
| 84  | `rmdir` | Remove an empty directory |
| 87  | `unlink` | Remove a file directory entry |
| 89  | `readlink` | Read symbolic link target |
| 90  | `chmod` | Change file permissions |
| 91  | `fchmod` | Change file permissions by fd |
| 95  | `umask` | Set permission mask |
| 162 | `sync` | Sync all disks |
| 217 | `getdents64` | Read directory entries |
| 257 | `openat` | Open relative to dirfd |
| 258 | `mkdirat` | Create directory relative to dirfd |
| 262 | `newfstatat` | Get status relative to dirfd |
| 263 | `unlinkat` | Remove directory entry relative to dirfd |
| 264 | `renameat` | Rename relative to dirfd |
| 267 | `readlinkat` | Read link relative to dirfd |
| 268 | `fchmodat` | Change permissions relative to dirfd |
| 269 | `faccessat` | Check permissions relative to dirfd |
| 270 | `pselect6` | I/O multiplexing (with signal mask) |
| 271 | `ppoll` | Wait for fd events (with signal mask, nanosecond timeout) |
| 280 | `utimensat` | Set file timestamps |
| 292 | `dup3` | Duplicate fd (with flags) |
| 293 | `pipe2` | Create pipe (with flags) |
| 316 | `renameat2` | Rename with flags |
| 332 | `statx` | Extended file status query |

#### Memory Management

| No. | Name | Description |
|-----|------|-------------|
| 9  | `mmap` | Create a memory mapping (anonymous/file, `MAP_PRIVATE`/`MAP_SHARED`) |
| 10 | `mprotect` | Change memory protection attributes |
| 11 | `munmap` | Remove a memory mapping |
| 12 | `brk` | Adjust program break |

#### Process and Thread

| No. | Name | Description |
|-----|------|-------------|
| 39  | `getpid` | Get process ID |
| 56  | `clone` | Create a task |
| 57  | `fork` | Duplicate process (copy-on-write) |
| 58  | `vfork` | Duplicate process (parent blocks) |
| 59  | `execve` | Load and execute a new program (ELF, including `PT_INTERP`) |
| 60  | `exit` | Exit the current thread |
| 61  | `wait4` | Wait for a child process and reap it |
| 109 | `setpgid` | Set process group |
| 110 | `getppid` | Get parent process ID |
| 111 | `getpgrp` | Get process group ID |
| 121 | `getpgid` | Get process group ID of a specified process |
| 158 | `arch_prctl` | Architecture-specific (`ARCH_SET_FS`/`ARCH_GET_FS`) |
| 186 | `gettid` | Get thread ID |
| 218 | `set_tid_address` | Set clear_child_tid |
| 231 | `exit_group` | Exit the entire thread group |

#### Signals

| No. | Name | Description |
|-----|------|-------------|
| 13  | `rt_sigaction` | Set signal handling action |
| 14  | `rt_sigprocmask` | Set signal mask |
| 15  | `rt_sigreturn` | Return from signal handler |
| 62  | `kill` | Send a signal |
| 131 | `sigaltstack` | Set alternate signal stack |
| 200 | `tkill` | Send a signal to a single thread |
| 234 | `tgkill` | Send a signal to a specific thread in a thread group |
| 273 | `set_robust_list` | Set robust futex list |

#### Time

| No. | Name | Description |
|-----|------|-------------|
| 35  | `nanosleep` | High-precision sleep |
| 96  | `gettimeofday` | Get wall-clock time (seconds + microseconds) |
| 201 | `time` | Get second-level time |
| 228 | `clock_gettime` | Get time for a specified clock |
| 229 | `clock_getres` | Get clock resolution |
| 230 | `clock_nanosleep` | Timed sleep |

#### System and User Information

| No. | Name | Description |
|-----|------|-------------|
| 63  | `uname` | Get system information |
| 102 | `getuid` | Get user ID |
| 104 | `getgid` | Get group ID |
| 107 | `geteuid` | Get effective user ID |
| 108 | `getegid` | Get effective group ID |
| 118 | `getresuid` | Get real/effective/saved user IDs |
| 120 | `getresgid` | Get real/effective/saved group IDs |
| 157 | `prctl` | Process control |
| 169 | `reboot` | Reboot / power off / halt |
| 202 | `futex` | Fast userspace mutex (`FUTEX_WAIT`/`FUTEX_WAKE`) |
| 204 | `sched_getaffinity` | Get CPU affinity mask |
| 302 | `prlimit64` | Get/set resource limits |
| 318 | `getrandom` | Obtain random bytes |

#### Common Constants

| Category | Constants |
|----------|-----------|
| `open` flags | `O_RDONLY` `O_WRONLY` `O_RDWR` `O_CREAT` `O_EXCL` `O_TRUNC` `O_APPEND` `O_NONBLOCK` `O_DIRECTORY` `O_CLOEXEC` |
| `mmap` protections | `PROT_NONE` `PROT_READ` `PROT_WRITE` `PROT_EXEC` |
| `mmap` flags | `MAP_SHARED` `MAP_PRIVATE` `MAP_FIXED` `MAP_ANONYMOUS` |
| `lseek` | `SEEK_SET` `SEEK_CUR` `SEEK_END` |
| `*at` family | `AT_FDCWD` `AT_EMPTY_PATH` `AT_SYMLINK_NOFOLLOW` `AT_REMOVEDIR` |
| Timestamps | `UTIME_NOW` `UTIME_OMIT` |
| `fcntl` | `F_DUPFD` `F_GETFD` `F_SETFD` `F_GETFL` `F_SETFL` `F_GETLK` `F_SETLK` `F_SETLKW` `F_DUPFD_CLOEXEC` `F_SETPIPE_SZ` `F_GETPIPE_SZ` |
| `futex` operations | `FUTEX_WAIT` `FUTEX_WAKE` `FUTEX_REQUEUE` `FUTEX_CMP_REQUEUE` `FUTEX_WAIT_BITSET` `FUTEX_WAKE_BITSET` |
| `clockid` | `CLOCK_REALTIME` `CLOCK_MONOTONIC` `CLOCK_PROCESS_CPUTIME_ID` `CLOCK_REALTIME_COARSE` `CLOCK_MONOTONIC_COARSE` |
| `wait` options | `WNOHANG` |
| `errno` | `EPERM` `ENOENT` `ESRCH` `EINTR` `EIO` `EBADF` `ECHILD` `EAGAIN` `ENOMEM` `EACCES` `EFAULT` `EEXIST` `ENODEV` `ENOTDIR` `EISDIR` `EINVAL` `ENFILE` `ENOSPC` `EPIPE` `ERANGE` `ENOSYS` `ENOTEMPTY` `EOPNOTSUPP` `ETIMEDOUT` |

### Kernel Boot Parameters
```c
typedef struct {
  UINT32                  Type;
  EFI_PHYSICAL_ADDRESS    PhysicalStart;
  EFI_VIRTUAL_ADDRESS     VirtualStart;
  UINT64                  NumberOfPages;
  UINT64                  Attribute;
} EFI_MEMORY_DESCRIPTOR;

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
    memory_info_t MemoryInfo;//Memory information
    screen_info_t ScreenInfo;//Screen information
    disk_info_t DiskInfo;//Disk information
    VOID* RSDP;//RSDP address
    SMBIOS_TABLE_ENTRY_POINT *SMBIOS;//SMBIOS entry point address
    EFI_RUNTIME_SERVICES *RuntimeServices;//Runtime services
    UINTN RandomSeed;//Random seed
    UINTN KernelAddress;//Kernel address
    UINTN KernelSize;//Kernel size
    UINTN KernelStackAddress;//Kernel stack address
    UINTN KernelStackSize;//Kernel stack size
    _Bool PrintLog;//Whether to display logs on screen
}__attribute__((packed)) boot_param_t;
```

## Compilation, Installation, and Running

- All components of this system must be installed in the ESP partition, and the ESP partition must be in FAT32 format.
- Note: This kernel is still in the development stage and is not recommended as the kernel of a primary operating system.

### ESP Partition Directory Structure (Using the Default Bootloader)

```text
/
├─EFI
| └─BOOT
|   ├─BOOTX64.EFI
|   └─BOOT.CFG
└─SYS
  └─KERNEL.ELF
```

### `init` Loading Order

1. `/sbin/init`

2. `/bin/init`

### Compilation

Required packages:

- `build-essential`
- `binutils`
- `gcc-x86-64-linux-gnu`

For build commands, see `make help`.

### Virtual Machine Testing (Optional)

Required packages:

- `coreutils`
- `parted`
- `util-linux`
- `dosfstools`
- `util-linux`
- `udisks2`
- `qemu-system-x86_64`
- Linux kernel `kvm` module
- `OVMF.fd` firmware, which must be placed in the source code root directory

## Participants:
- Liu Chunyi(67%)
- DeepSeek V4(33%)
