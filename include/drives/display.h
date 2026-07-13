#ifndef _DISPLAY_H_
#define _DISPLAY_H_

#include <klib.h>

extern ScreenInfo SYSTEM_ScreenInfo;
extern uint32_t*  SYSTEM_FrameBuffer;

extern uint8_t *kfont_data;//字体数据

//颜色:
#define COLOR_RED 0xFFFF0000
#define COLOR_GREEN 0xFF00FF00
#define COLOR_BLUE 0xFF0000FF
#define COLOR_YELLOW 0xFFFFFF00
#define COLOR_BLACK 0xFF000000
#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_GREY 0xFF222222
#define COLOR_CYAN 0xFF00FFEE
#define COLOR_SKYBLUE 0xFF17B7FF

uint32_t rgb(uint8_t red, uint8_t green, uint8_t blue);//rgb转16进制
void DrawPiexl(uint16_t x,uint16_t y,uint32_t color);//画点
void fillRect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint32_t color);//填充矩形
void DrawChar(char c,int x,int y,uint32_t color);//显示字符
void DrawString(char *s,int x,int y,uint32_t color);//显示字符串

#endif