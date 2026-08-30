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

# 复制execve测试程序
if [ -f "build/app/test.elf" ]; then
    echo "更新测试程序..."
    cp "build/app/test.elf" "$MOUNT_POINT/SYS/TEST.ELF"
fi

# 复制musl静态链接hello程序
if [ -f "build/app/hello.elf" ]; then
    echo "更新musl程序..."
    cp "build/app/hello.elf" "$MOUNT_POINT/SYS/HELLO.ELF"
fi

# 卸载
echo "卸载..."
udisksctl unmount -b "$LOOP"
udisksctl loop-delete -b "$LOOP"

echo "完成！"