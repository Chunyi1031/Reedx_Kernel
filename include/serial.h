/**
 * include/klib.h
 * 
 * Copyright (C) 2026 Liu Chunyi
 * 
 */
#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <types.h>

#define SERIAL_COM1 0x3F8
#define SERIAL_COM2 0x2F8
#define SERIAL_COM3 0x3E8
#define SERIAL_COM4 0x2E8
#define SERIAL_COM5 0x5F8
#define SERIAL_COM6 0x4F8
#define SERIAL_COM7 0x5E8
#define SERIAL_COM8 0x4E8

extern _Bool SerialIsInited;

/**
 * @brief 初始化串口
 * @param PORT 端口
 * @return 0=成功 1=失败
 */
int InitSerial(uint16_t PORT);

/**
 * @brief 检查串口是否准备好发送数据
 * @param port 端口
 */
_Bool IsTransmitEmpty(uint16_t port);

/**
 * @brief 串口发送一个字符
 * @param port 串口端口
 * @param c 要发送的字符
 */
void SerialWriteChar(uint16_t port, char c);

/**
 * @brief 串口发送一个字符串
 * @param port 串口端口
 * @param str 以 null 结尾的字符串
 */
void SerialWriteString(uint16_t port, const char* str);

#endif