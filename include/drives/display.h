#ifndef _DISPLAY_H_
#define _DISPLAY_H_

#include <klib.h>

extern ScreenInfo SYSTEM_ScreenInfo;
extern uint32_t*  SYSTEM_FrameBuffer;

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
#define COLOR_MAGENTA 0xFFFF00FF //洋红
#define COLOR_PURPLE  0xFFA020F0 //紫
#define COLOR_ORANGE  0xFFFFA500 //橙
#define COLOR_LGREY   0xFFC0C0C0 //亮灰
#define COLOR_DGREY   0xFF808080 //暗灰

#define CHAR_CELL_W 8
#define CHAR_CELL_H 16

uint32_t rgb(uint8_t red, uint8_t green, uint8_t blue);//rgb转16进制
void DrawPiexl(uint16_t x,uint16_t y,uint32_t color);//画点
void fillRect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint32_t color);//填充矩形
void DrawChar(char c,int x,int y,uint32_t color);//显示字符
void DrawString(char *s,int x,int y,uint32_t color);//显示字符串
uint32_t FbReadPixel(int x,int y);//读一个像素
void FbScrollUp(int y0,int y1,int dy,uint32_t bg);//[y0,y1+dy)整体上移dy行, 最后的[y1,y1+dy)填bg
int ScreenFbMappedAt(uintptr_t fb);//检查地址在当前CR3页表中是否已映射
int ScreenFbMapped(void);//检查全局帧缓冲SYSTEM_FrameBuffer是否已映射

#endif
