#!/bin/bash

IMAGE="System.img"
KERNEL="kernel.elf"
OFFSET=1048576

# 检查文件
[ -f "$IMAGE" ] || { echo "错误：$IMAGE 不存在"; exit 1; }
[ -f "$KERNEL" ] || { echo "错误：$KERNEL 不存在"; exit 1; }

# 创建 loop 设备
echo "挂载中..."
LOOP=$(udisksctl loop-setup -f "$IMAGE" -o "$OFFSET" | grep -oP '/dev/loop\d+')

# 挂载（自动挂载到 /run/media/用户名/卷标）
udisksctl mount -b "$LOOP"

# 获取实际挂载点
MOUNT_POINT=$(grep "$LOOP" /proc/mounts | awk '{print $2}')

if [ -z "$MOUNT_POINT" ]; then
    echo "错误：无法获取挂载点"
    udisksctl loop-delete -b "$LOOP"
    exit 1
fi

echo "挂载到: $MOUNT_POINT"

# 复制内核
echo "更新内核..."
cp "$KERNEL" "$MOUNT_POINT/SYS/KERNEL.ELF"

# 复制 init 程序(app/init.c → /sbin/init, 内核自动启动); 自动生成 sbin 目录
if [ -f "build/app/init.elf" ]; then
    echo "更新init程序..."
    mkdir -p "$MOUNT_POINT/sbin"
    cp "build/app/init.elf" "$MOUNT_POINT/sbin/init"
fi

# 复制glibc静态链接hello程序
if [ -f "build/app/hello.elf" ]; then
    echo "更新hello程序..."
    cp "build/app/hello.elf" "$MOUNT_POINT/SYS/eshell"
fi

# 复制echo程序(coreutils静态echo → /bin/echo)
if [ -f "build/app/echo.elf" ]; then
    echo "更新echo程序..."
    mkdir -p "$MOUNT_POINT/bin"
    cp "build/app/echo.elf" "$MOUNT_POINT/bin/echo"
fi

# 卸载
echo "卸载..."
udisksctl unmount -b "$LOOP"
udisksctl loop-delete -b "$LOOP"

echo "完成！"