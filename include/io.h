#ifndef _IO_H_
#define _IO_H_

#include <types.h>

void outb(uint16_t port, uint8_t value);//向端口输出1字节
void outw(uint16_t port, uint16_t val);//向端口输出2字节
void outl(uint16_t port, uint32_t val);//向端口输出4字节
uint8_t inb(uint16_t port);//从端口读1字节
uint16_t inw(uint16_t port);//从端口读2字节
uint32_t inl(uint16_t port);//从端口读4字节
void insw(uint16_t port, void *dst, uint32_t count);//从端口读count个16位
void outsw(uint16_t port, const void *src, uint32_t count);//向端口写count个16位
void io_wait(void);//IO延迟

#endif