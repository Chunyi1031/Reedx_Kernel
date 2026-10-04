/**
 * include/drives/tty.h
 * 
 * Copyright (C) 2026 Liu Chunyi
 * 
 * Reedx TTY接口
 */
#ifndef _TTY_H_
#define _TTY_H_

#include <klib.h>
#include "display.h"

extern uint16_t TTY_PrintCol;
extern uint16_t TTY_PrintRow;

extern _Bool TTY_ScreenEnabled;

typedef struct ConsoleStyle {
    uint32_t TextColor; //文字颜色
    uint32_t BgColor;   //背景颜色
} ConsoleStyle;

extern ConsoleStyle CurrentConsoleStyle;

void TTY_PrintChar(const char c,uint32_t color);
void TTY_Print(const char *str,uint32_t color);
void TTY_SetCursor(uint16_t col,uint16_t row);
void TTY_Clear();

/*DeepSeek-V4.1-Flash*/
void TTY_PrintColor(const char *str,uint32_t fg,uint32_t bg);
_Bool TTY_ReadReady(void);//终端输入是否就绪(供poll判定POLLIN)

typedef struct {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line;
    uint8_t  cc[32];
    uint32_t ispeed, ospeed;
} ktermios_t;

#define TTY_ISIG    0x0001
#define TTY_ICANON  0x0002
#define TTY_ECHO    0x0008
#define TTY_ECHOE   0x0010
#define TTY_ECHOK   0x0020
#define TTY_ECHOCTL 0x0200
#define TTY_ECHOKE  0x0800
#define TTY_IEXTEN  0x8000

//termios相关请求号
#define TTY_TCGETS     0x5401UL
#define TTY_TCSETS     0x5402UL
#define TTY_TCSETSW    0x5403UL
#define TTY_TCSETSF    0x5404UL
#define TTY_TCGETS2    0x802C542AUL
#define TTY_TCSETS2    0x402C542BUL
#define TTY_TCSETSW2   0x402C542CUL
#define TTY_TCSETSF2   0x402C542DUL
#define TTY_TIOCGPGRP  0x540FUL
#define TTY_TIOCSPGRP  0x5410UL
#define TTY_TIOCGWINSZ 0x5413UL
#define TTY_FIONREAD   0x541BUL
//窗口尺寸查询结构
struct tty_winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

#define TTY_LINE_MAX 256

//用户侧结构布局(与glibc一致; packed避免对齐填充差异)
typedef struct __attribute__((packed)) { uint32_t iflag,oflag,cflag,lflag; uint8_t line; uint8_t cc[19]; uint32_t ispeed,ospeed; } tty_termios2_t;//44字节(TCGETS2)
typedef struct __attribute__((packed)) { uint32_t iflag,oflag,cflag,lflag; uint8_t line; uint8_t cc[32]; uint8_t pad[3]; uint32_t ispeed,ospeed; } tty_termios_legacy_t;//60字节(TCGETS)

extern ktermios_t g_tty_term;
long TTY_TermRead(char *kbuf, long count);//行规程读取: 返回字节数; Ctrl+D空行返回0; Ctrl+C返回-EINTR
void TTY_TermExportLegacy(void *dst);
void TTY_TermExport2(void *dst);
void TTY_TermImportLegacy(const void *src);
void TTY_TermImport2(const void *src);
void TTY_TermRestoreOnExit(pid_t pid);//任务退出时恢复被它改过的终端模式
_Bool TTY_KeyInput(char c);//键盘IRQ调用: 无读者时的Ctrl+C → 消费该键并标记待投递(返回true)
void TTY_IntrCheck(void);//定时器IRQ调用(安全点): 向前台任务及其子孙投递挂起的SIGINT
/*DeepSeek-V4.1-Flash-END*/

#endif
