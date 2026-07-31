#ifndef _TTY_H_
#define _TTY_H_

#include <klib.h>
#include "display.h"

extern uint16_t TTY_PrintCol;
extern uint16_t TTY_PrintRow;

/* 是否输出到屏幕 —— 由 boot_param->PrintLog 决定
 * 原理：串口输出始终开启（调试用途），屏幕输出仅在引导程序
 * 传入 PrintLog==true 时才执行 DrawChar/DrawString，
 * 避免在静默模式下污染帧缓冲区。 */
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

#endif
