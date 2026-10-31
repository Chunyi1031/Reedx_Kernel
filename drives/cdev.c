// SPDX-License-Identifier: GPL-2.0-only
/**
 * drives/cdev.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * 字符设备注册表
 */
#include <drives/cdev.h>
#include <drives/tty.h>
#include <serial.h>
#include <vfs.h>

#define CDEV_MAX 16//注册表大小

static cdev_t g_cdev[CDEV_MAX];

//注册一段字符设备
int RegisterChrdev(uint16_t major, uint16_t minor_start, uint16_t count, const char *name, const struct file_operations *fops){
    if(!fops || !count)return -EINVAL;
    for(int i = 0;i < CDEV_MAX;i ++){
        if(g_cdev[i].fops)continue;
        g_cdev[i].major = major;
        g_cdev[i].minor_start = minor_start;
        g_cdev[i].count = count;
        g_cdev[i].name = name;
        g_cdev[i].fops = fops;
        return 0;
    }
    return -ENOSPC;
}

//按设备号找驱动
const struct file_operations *ChrdevLookup(uint32_t dev){
    uint32_t ma = MAJOR(dev), mi = MINOR(dev);
    for(int i = 0;i < CDEV_MAX;i ++){
        if(!g_cdev[i].fops)continue;
        if(g_cdev[i].major != ma)continue;//主设备号不符
        if(mi < g_cdev[i].minor_start)continue;
        if(mi >= (uint32_t)g_cdev[i].minor_start + g_cdev[i].count)continue;
        return g_cdev[i].fops;
    }
    return NULL;
}

//null读
static int64_t null_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    (void)f; (void)buf; (void)len; (void)pos;
    return 0;//读立刻碰到文件尾
}

//null写
static int64_t null_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    (void)f; (void)buf;
    //写进来的内容直接丢掉
    if(pos)*pos += len;
    return (int64_t)len;
}

static const struct file_operations g_null_fops = {
    .read  = null_read,
    .write = null_write,
};

//zero读
static int64_t zero_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    (void)f;
    //要多少给多少,全是0
    uint64_t n = len > 4096 ? 4096 : len;
    memset(buf, 0, (size_t)n);
    if(pos)*pos += n;
    return (int64_t)n;
}

//zero写
static int64_t zero_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    (void)f; (void)buf;
    if(pos)*pos += len;
    return (int64_t)len;
}

static const struct file_operations g_zero_fops = {
    .read  = zero_read,
    .write = zero_write,
};

//控制台读
static int64_t console_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    (void)f; (void)pos;
    if(!len)return 0;
    long n = TTY_TermRead(buf, (long)len);//行规程读取
    return (int64_t)n;
}

//控制台写
static int64_t console_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    (void)f;
    //逐字节打印
    for(uint64_t i = 0;i < len;i ++)TTY_PrintChar(buf[i], CurrentConsoleStyle.TextColor);
    if(pos)*pos += len;
    return (int64_t)len;
}

static const struct file_operations g_console_fops = {
    .read  = console_read,
    .write = console_write,
};

//串口写
static int64_t serial_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    (void)f;
    for(uint64_t i = 0;i < len;i ++)SerialWriteChar(SERIAL_COM1, buf[i]);
    if(pos)*pos += len;
    return (int64_t)len;
}

//串口读
static int64_t serial_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    (void)f; (void)buf; (void)len; (void)pos;
    return 0;
}

static const struct file_operations g_serial_fops = {
    .read  = serial_read,
    .write = serial_write,
};

//注册内置设备
void DevicesInit(void){
    RegisterChrdev(1, 3, 1, "null",  &g_null_fops);
    RegisterChrdev(1, 5, 1, "zero",  &g_zero_fops);
    RegisterChrdev(5, 0, 2, "tty",   &g_console_fops);//(5,0) tty 与 (5,1) console
    RegisterChrdev(4, 64, 1, "ttyS0", &g_serial_fops);
}
