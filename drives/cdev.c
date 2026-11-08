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
#include <print.h>

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

//kmsg读
static int64_t kmsg_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    (void)f; (void)buf; (void)len; (void)pos;
    return -EAGAIN;
}

//kmsg写
static int64_t kmsg_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    (void)f;
    if(len > 512)len = 512;
    char tmp[513];
    memcpy(tmp, buf, (size_t)len);
    tmp[len] = 0;
    char *msg = tmp;
    int level = PRINTK_LVL_INFO;//没有级别前缀时按INFO处理
    //解析 <PRI>前缀,PRI = facility * 8 + level
    if(msg[0] == '<'){
        int pri = 0;
        char *p = msg + 1;
        while(*p >= '0' && *p <= '9' && pri <= 255)pri = pri * 10 + (*p ++ - '0');
        if(*p == '>' && p != msg + 1){
            level = pri & 7;
            msg = p + 1;
        }
    }
    //去掉行尾换行,上屏时统一补一个
    int n = (int)strlen(msg);
    while(n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))msg[-- n] = 0;
    level = (level & 0x7);
    if(n > 0)printk_log(level, msg);
    if(pos)*pos += len;
    return (int64_t)len;
}

static const struct file_operations g_kmsg_fops = {
    .read  = kmsg_read,
    .write = kmsg_write,
};

//随机字节源
static uint64_t g_rand_state = 0x9E3779B97F4A7C15ULL;

//splitmix64收尾混合，把低熵种子打散到全部比特
static uint64_t rand_mix64(uint64_t x){
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

//用引导程序传入的8字节随机种子初始化随机源
void RandInit(uint64_t seed){
    if(!seed)seed = 0x9E3779B97F4A7C15ULL;
    g_rand_state = rand_mix64(seed);
    if(!g_rand_state)g_rand_state = 0x9E3779B97F4A7C15ULL;
}

static uint32_t rand_next(void){
    uint64_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rand_state = x;
    return (uint32_t)(x >> 32);
}

static int64_t rand_read(struct file *f, char *buf, uint64_t len, uint64_t *pos){
    (void)f;
    uint64_t n = len > 4096 ? 4096 : len;
    for(uint64_t i = 0;i < n;i += 4){
        uint32_t v = rand_next();
        uint64_t k = n - i < 4 ? n - i : 4;
        memcpy(buf + i, &v, (size_t)k);
    }
    if(pos)*pos += n;
    return (int64_t)n;
}

static int64_t rand_write(struct file *f, const char *buf, uint64_t len, uint64_t *pos){
    (void)f;
    for(uint64_t i = 0;i < len;i ++)g_rand_state = (g_rand_state ^ (uint8_t)buf[i]) * 1099511628211ULL;
    if(pos)*pos += len;
    return (int64_t)len;
}

static const struct file_operations g_rand_fops = {
    .read  = rand_read,
    .write = rand_write,
};

//注册内置设备
void DevicesInit(void){
    RegisterChrdev(1, 3, 1, "null",  &g_null_fops);
    RegisterChrdev(1, 5, 1, "zero",  &g_zero_fops);
    RegisterChrdev(5, 0, 2, "tty",   &g_console_fops);//(5,0) tty 与 (5,1) console
    RegisterChrdev(4, 64, 1, "ttyS0", &g_serial_fops);
    RegisterChrdev(1, 11, 1, "kmsg", &g_kmsg_fops);
    RegisterChrdev(1, 8, 1, "random", &g_rand_fops);
    RegisterChrdev(1, 9, 1, "urandom", &g_rand_fops);
}
