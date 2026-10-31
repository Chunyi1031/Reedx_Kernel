// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/drives/cdev.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * Reedx字符设备注册表
 */
#ifndef _CDEV_H_
#define _CDEV_H_

#include <klib.h>

#define CDEV_MAJOR_SHIFT 20
#define MKDEV(ma, mi)  (((uint32_t)(ma) << CDEV_MAJOR_SHIFT) | ((uint32_t)(mi) & 0xFFFFFu))
#define MAJOR(dev)     ((uint32_t)(dev) >> CDEV_MAJOR_SHIFT)
#define MINOR(dev)     ((uint32_t)(dev) & 0xFFFFFu)

struct file_operations;

//一次注册覆盖=这一段次设备号
typedef struct cdev {
    uint16_t major;       //主设备号
    uint16_t minor_start; //起始次设备号
    uint16_t count;       //覆盖的次设备号个数
    const char *name;     //设备名
    const struct file_operations *fops;//读/写操作
} cdev_t;

int RegisterChrdev(uint16_t major, uint16_t minor_start, uint16_t count,const char *name, const struct file_operations *fops);//注册一段字符设备
const struct file_operations *ChrdevLookup(uint32_t dev);//按设备号找驱动
void DevicesInit(void);//注册内置设备

#endif
