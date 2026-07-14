#ifndef _TTY_H_
#define _TTY_H_

#include <klib.h>
#include "display.h"

extern uint16_t TTY_PrintCol;
extern uint16_t TTY_PrintRow;

typedef struct ConsoleStyle {
    uint32_t TextColor; //文字颜色
    uint32_t BgColor;   //背景颜色
} ConsoleStyle;

extern ConsoleStyle CurrentConsoleStyle;

/**
 * 打印字符
 * @param c 字符
 * @param color 颜色
 */
void TTY_PrintChar(const char c,uint32_t color);
/**
 * 打印字符串
 * @param str 字符串
 * @param color 颜色
 */
void TTY_Print(const char *str,uint32_t color);

void TTY_SetCursor(uint16_t col,uint16_t row);//设置打印位置
void TTY_Clear();//清屏

#endif