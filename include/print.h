/*
 * 由Hermes Agent + DeepSeek-V4-Pro生成
 *
 * include/print.h — 内核日志接口
 * 
 * Copyright (C) 2026 Liu Chunyi
 */

#ifndef _PRINT_H_
#define _PRINT_H_

#include <klib.h>
#include <drives/tty.h>
#include <rtc.h>
#include <serial.h>

int early_printk(const char* fmt, ...);
void print_error(void);
void print_warning(void);
void print_ok(void);

#define PRINTK_TEXT_BUFFER_SIZE  8192   //文本缓冲区大小 (8 KB)
#define PRINTK_COUNT_MAX         512    //最大日志条数

#define PRINTK_EMERG    "<0>"  //系统不可用
#define PRINTK_ALERT    "<1>"  //必须立即处理
#define PRINTK_CRIT     "<2>"  //严重条件
#define PRINTK_ERR      "<3>"  //错误条件
#define PRINTK_WARNING  "<4>"  //警告条件
#define PRINTK_NOTICE   "<5>"  //正常但值得注意
#define PRINTK_INFO     "<6>"  //信息性消息
#define PRINTK_DEBUG    "<7>"  //调试级消息

#define PRINTK_LVL_EMERG    0
#define PRINTK_LVL_ERR      3
#define PRINTK_LVL_WARNING  4
#define PRINTK_LVL_INFO     6
#define PRINTK_LVL_DEBUG    7
#define PRINTK_LVL_NONE     0xFF  //无等级：print_to_console中直接原样输出

/* —— 日志条目元数据 —— */
//原理：每条printk调用生成一条PRINTK_LOG_INFO，记录级别、时间戳和文本缓冲区偏移，
//      print_to_console()遍历此数组将文本格式化输出
typedef struct PRINTK_LOG_INFO {
	uint8_t    level;   //日志级别 (0-7)
	rtc_time_t time;    //记录时刻
	int        start;   //在PRINTK_text_buffer中的字节偏移
	int        length;  //消息长度（不含 '\0'）
} PRINTK_LOG_INFO_t;

extern char*              PRINTK_text_buffer;
extern PRINTK_LOG_INFO_t* PRINTK_log_info;

int  InitPrintk(void);
int  printk(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void print_to_console(int count);
void panic(const char* fmt, ...);

#endif
