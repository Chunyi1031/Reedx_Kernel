#ifndef _PS2_KEYBOARD_H_
#define _PS2_KEYBOARD_H_

#include <klib.h>

#define KEYBOARD_BUFFER_SIZE 64 //键盘缓冲区大小
#define PS2_KBD_KEY_PORT    0x60 //键盘按键输入端口
#define PS2_KBD_STATUS_PORT 0x64 //键盘状态端口

// Control 键
#define KEY_LeftCtrl   0x1D  // 左Control键
#define KEY_RightCtrl  0xE01D // 右Control键（扩展扫描码）
// Alt 键  
#define KEY_LeftAlt    0x38  // 左Alt键
#define KEY_RightAlt   0xE038 // 右Alt键（扩展扫描码）
// Shift 键
#define KEY_LeftShift  0x2A  // 左Shift
#define KEY_RightShift 0x36  // 右Shift
// 箭头键
#define KEY_UpArrow    0x48  // 上箭头键
#define KEY_DownArrow  0x50  // 下箭头键  
#define KEY_LeftArrow  0x4B  // 左箭头键
#define KEY_RightArrow 0x4D  // 右箭头键

void KeyboardInit();//键盘初始化
_Bool kbdit();//检查键盘是否可读
char io_getkey();//从io口直接获取按键（阻塞）
char io_getkey_noblock();//从io口直接获取按键（非阻塞）
char GetKey();//获取按键(阻塞)
char GetKey_NoBlock();//获取按键(非阻塞)
int  Kbd_Available(void);//就绪队列中待读字节数
_Bool Kbd_HasLine(void);//队列中是否已有一整行
void Keyboard_IRQ();//键盘中断处理函数

#endif