# 由Hermes Agent + DeepSeek-V4-Pro生成
#
# Kernel Makefile
#
# 原理：
#   内核构建系统遵循 Linux 7.1.3 kbuild 的设计模式：
#
#   1. 顶层 Makefile 定义工具链、编译/链接标志
#   2. 每个源码目录的 Makefile 列出 obj-y（要编译的目标文件）
#   3. 使用 quiet_cmd_* / cmd_* 变量对和 $(call cmd,<name>) 宏
#      实现"短输出"（CC / LD / AR）+ 完整命令的双模输出
#   4. V=1 显示完整编译命令，默认显示简短状态行
#
#   构建流程：
#     .c 源文件  ──CC──▶  .o 目标文件  ──LD──▶  built-in.o（每目录）
#                                                  │
#                                          ┌───────┴───────┐
#                                          ▼               ▼
#                                     kernel.elf      kernel.elf
#                                   （ld -T 链接脚本）  （最终 ELF）
#
#   命名约定（与 Linux 7.1.3 一致）：
#     KBUILD_CFLAGS   — 内核 C 编译标志
#     KBUILD_LDFLAGS  — 内核链接标志
#     obj-y           — 子目录中列出要编译的目标文件
#     built-in.o      — 每目录的局部链接产物
#     srctree         — 源码树根目录
#
# 参考：Linux 7.1.3 顶层 Makefile + scripts/Makefile.build

# ========== 版本信息 ==========
VERSION = 0
PATCHLEVEL = 1
SUBLEVEL = 0
EXTRAVERSION =
KERNELRELEASE = $(VERSION).$(PATCHLEVEL).$(SUBLEVEL)$(EXTRAVERSION)

# ========== 架构与工具链 ==========
ARCH      ?= x86_64

CC         = x86_64-linux-gnu-gcc
LD         = x86_64-linux-gnu-ld
AR         = x86_64-linux-gnu-ar
OBJCOPY    = x86_64-linux-gnu-objcopy

# ========== 路径定义 ==========
srctree    := $(CURDIR)
objtree    := $(CURDIR)
build-dir  := build
script-dir := scripts

# ========== 包含路径与编译标志 ==========
# -I 标志确保 #include <klib.h> 能从 $(srctree) 根目录和 $(srctree)/include 找到
KBUILD_CFLAGS := -I$(srctree) -I$(srctree)/include
# 内核基础标志（参考 Linux 7.1.3 arch/x86/Makefile）
KBUILD_CFLAGS += -ffreestanding -fno-stack-protector -mno-red-zone
KBUILD_CFLAGS += -fno-builtin -nostdlib -m64 -g
KBUILD_CFLAGS += -Wall -Wno-unused-function -Wno-unused-variable -O0

# 链接标志（参考 Linux 7.1.3 arch/x86/kernel/vmlinux.lds）
KBUILD_LDFLAGS := -nostdlib -static -T $(srctree)/kernel64.ld
KBUILD_LDFLAGS += -e KernelStart --gc-sections --build-id=none

# ========== 包含配置与构建辅助 ==========
# 生成的内核配置（由 make config 生成）
-include $(srctree)/include/config/auto.conf

# Kbuild 辅助宏（quiet_cmd_*, $(call cmd,...)）
include $(script-dir)/Kbuild.include

# ========== 源码目录列表（链接顺序即遍历顺序）==========
# 注意：init 必须在最前面，因为 KernelStart() 入口点在其中
core-dirs := init kernel lib drives mm fs

# ========== 读取各目录 Makefile 收集目标文件 ==========
# 每个子目录 Makefile 定义 obj-y := file1.o file2.o ...
# 这里通过 include + 前缀转换得到完整路径列表

obj-y :=
include init/Makefile
init-objs := $(addprefix $(build-dir)/init/, $(obj-y))

obj-y :=
include kernel/Makefile
kernel-objs := $(addprefix $(build-dir)/kernel/, $(obj-y))

obj-y :=
include lib/Makefile
lib-objs := $(addprefix $(build-dir)/lib/, $(obj-y))

obj-y :=
include drives/Makefile
drives-objs := $(addprefix $(build-dir)/drives/, $(obj-y))

obj-y :=
include mm/Makefile
mm-objs := $(addprefix $(build-dir)/mm/, $(obj-y))

obj-y :=
include fs/Makefile
fs-objs := $(addprefix $(build-dir)/fs/, $(obj-y))

# 所有目标文件
all-objs := $(init-objs) $(kernel-objs) $(lib-objs) $(drives-objs) $(mm-objs) $(fs-objs)

# 每个目录的 built-in.o（局部链接产物）
builtin-all := $(addprefix $(build-dir)/, \
                $(addsuffix /built-in.o, $(core-dirs)))

# ========== 默认目标 ==========
.PHONY: all
all: kernel.elf

# ========== 构建输出宏 ==========
# 每个构建步骤定义一对变量：
#   quiet_cmd_<name> — 短输出中显示的动作名（如 "CC", "LD"）
#   cmd_<name>       — 实际执行的命令
# 通过 $(call cmd,<name>) 调用

# --- C 编译 (.c → .o) ---
quiet_cmd_cc_o_c = CC
      cmd_cc_o_c = $(CC) $(KBUILD_CFLAGS) -c -o $@ $<

# --- 目录局部链接 (所有 .o → built-in.o) ---
quiet_cmd_ld_builtin = LD
      cmd_ld_builtin = $(LD) -r -o $@ $^

# --- 最终内核链接 (built-in.o... → kernel.elf) ---
quiet_cmd_ld_kernel = LD
      cmd_ld_kernel = $(LD) $(KBUILD_LDFLAGS) -o $@ $^

# --- 清理 ---
quiet_cmd_clean = CLEAN
      cmd_clean = rm -rf $(build-dir) kernel.elf

# --- 归档（备用） ---
quiet_cmd_ar = AR
      cmd_ar = rm -f $@; $(AR) cDPrST $@ $^

# ========== 模式规则：C 源文件 → 目标文件 ==========
# 使用静态模式规则，输出到 build/ 目录保持源码目录结构
# 例：init/main.c → build/init/main.o

$(all-objs): | $(build-dir)

$(build-dir)/%.o: $(srctree)/%.c
	@mkdir -p $(dir $@)
	$(call cmd,cc_o_c)

# --- 汇编编译 (.S → .o) ---
quiet_cmd_as_s_S = AS
      cmd_as_s_S = $(CC) $(KBUILD_CFLAGS) -c -o $@ $<

$(build-dir)/%.o: $(srctree)/%.S
	@mkdir -p $(dir $@)
	$(call cmd,as_s_S)

# ========== 目录级链接规则 ==========
# 每个目录的 .o 文件通过 ld -r 合并为单个 built-in.o
# 原理：ld -r（部分链接/可重定位链接）将多个 .o 合并为一个，
#       符号重定位表仍然保留，最终链接时才解析所有符号

$(build-dir)/init/built-in.o: $(init-objs)
	$(call cmd,ld_builtin)

$(build-dir)/kernel/built-in.o: $(kernel-objs)
	$(call cmd,ld_builtin)

$(build-dir)/lib/built-in.o: $(lib-objs)
	$(call cmd,ld_builtin)

$(build-dir)/drives/built-in.o: $(drives-objs)
	$(call cmd,ld_builtin)

$(build-dir)/mm/built-in.o: $(mm-objs)
	$(call cmd,ld_builtin)

$(build-dir)/fs/built-in.o: $(fs-objs)
	$(call cmd,ld_builtin)

# ========== 最终链接：kernel.elf ==========
kernel.elf: $(builtin-all)
	$(call cmd,ld_kernel)

# ========== 确保 build/ 存在 ==========
$(build-dir):
	@mkdir -p $(build-dir)

# ========== make config：生成内核配置 ==========
# 解析 Kconfig 并生成 include/config/auto.conf
.PHONY: config
config:
	@echo "  CONF    include/config/auto.conf"
	@mkdir -p include/config
	@$(srctree)/scripts/kconfig.sh $(srctree) > include/config/auto.conf

# ========== make clean：清理构建产物 ==========
.PHONY: clean
clean:
	$(call cmd,clean)

# ========== 磁盘镜像与部署 ==========
MNT_DIR     := ./mnt
SYSTEM_DISK := ./System.img
SYSTEM_DISK_SIZE := 256

.PHONY: disk
disk:
	dd if=/dev/zero of=$(SYSTEM_DISK) bs=1M count=$(SYSTEM_DISK_SIZE)
	sudo parted $(SYSTEM_DISK) mklabel gpt
	sudo parted $(SYSTEM_DISK) mkpart primary fat32 1MiB 201MiB
	sudo parted $(SYSTEM_DISK) set 1 esp on
	sudo parted $(SYSTEM_DISK) print
	@echo "Disk: $(SYSTEM_DISK) Size: $(SYSTEM_DISK_SIZE)MB"
	@echo "Formatting..."
	sudo losetup -Pf $(SYSTEM_DISK)
	@LOOP_DEV=$$(sudo losetup -j $(SYSTEM_DISK) | cut -d: -f1); \
	sudo mkfs.fat -F 32 $${LOOP_DEV}p1; \
	sudo losetup -d $$LOOP_DEV
	sudo mount -o loop,offset=1048576 $(SYSTEM_DISK) $(MNT_DIR)
	sudo mkdir -p $(MNT_DIR)/EFI/BOOT
	sudo mkdir $(MNT_DIR)/SYS
	sudo cp data/BOOTX64.EFI $(MNT_DIR)/EFI/BOOT/BOOTX64.EFI
	sudo cp data/BOOTCFG.TXT $(MNT_DIR)/EFI/BOOT/BOOTCFG.TXT
	sudo umount $(MNT_DIR)

.PHONY: system
system: # kernel.elf
# 	sudo mount -o loop,offset=1048576 $(SYSTEM_DISK) $(MNT_DIR)
# 	sudo cp kernel.elf $(MNT_DIR)/SYS/KERNEL.ELF
# 	sudo umount $(MNT_DIR)
	bash ./CopyToDisk.sh

update-disk: 
	sudo mount -o loop,offset=1048576 $(SYSTEM_DISK) $(MNT_DIR)
	sudo cp data/BOOTX64.EFI $(MNT_DIR)/EFI/BOOT/BOOTX64.EFI
	sudo cp data/BOOTCFG.TXT $(MNT_DIR)/EFI/BOOT/BOOTCFG.TXT
	sudo cp kernel.elf $(MNT_DIR)/SYS/KERNEL.ELF
	sudo umount $(MNT_DIR)
.PHONY: update-disk

# ========== QEMU 运行 ==========
.PHONY: run
run: system
	qemu-system-x86_64 -m 1G -bios ./OVMF.fd -drive file=$(SYSTEM_DISK),if=ide,format=raw -serial stdio

.PHONY: run-debug
run-debug: system
	qemu-system-x86_64 -D qemu.log -d int -m 1G -bios ./OVMF.fd -drive file=$(SYSTEM_DISK),if=ide,format=raw -s -S -serial stdio

#帮助
.PHONY: help
help:
	@echo 'T001 Kernel build system'
	@echo ''
	@echo 'Targets:'
	@echo '  all          - 构建kernel.elf (默认)'
	@echo '  clean        - 删除build/和kernel.elf'
	@echo '  config       - 由Kconfig生成include/config/auto.conf'
	@echo '  disk         - 创建启动盘镜像'
	@echo '  system       - 复制kernel.elf内核到镜像'
	@echo '  run          - 编译并在QEMU中运行'
	@echo '  run-debug    - 编译并在QEMU中调试'
	@echo ''
	@echo 'Options:'
	@echo '  V=1          - 详细(显示完整的构建命令)'
	@echo '  ARCH=x86_64  - 目标架构(默认: x86_64)'

# 声明伪目标
.PHONY: all config clean distclean disk system run run-debug help FORCE
FORCE:
