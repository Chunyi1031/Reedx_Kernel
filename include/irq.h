/*
 * 由Hermes Agent + DeepSeek-V4-Pro生成
 *
 * Copyright (C) 2026 Liu Chunyi
 * 
 * include/irq.h — 硬件中断向量定义与 APIC 初始化接口
 */

#ifndef _INT_IRQ_H_
#define _INT_IRQ_H_

#include <klib.h>
#include <idt.h>
#include <apic.h>

//IRQ 向量基址
#define IRQ_VECTOR_BASE         0x20
#define NUM_IRQ_VECTORS         16

//传统IRQ编号(ISA/Legacy 兼容)
#define IRQ_TIMER          0x00   //定时器/时钟中断 (APIC Timer 替代)
#define IRQ_KEYBOARD       0x01   //键盘中断
#define IRQ_CASCADE        0x02   //级联 (PIC 从片，已废弃)
#define IRQ_COM2           0x03   //COM2/COM4 串口
#define IRQ_COM1           0x04   //COM1/COM3 串口
#define IRQ_LPT2           0x05   //LPT2 并口（或声卡）
#define IRQ_FLOPPY         0x06   //软盘控制器
#define IRQ_LPT1           0x07   //LPT1 并口（或打印机）
#define IRQ_CMOS           0x08   //CMOS 实时时钟
#define IRQ_FREE1          0x09   //空闲（通常未使用）
#define IRQ_FREE2          0x0A   //空闲（通常未使用）
#define IRQ_FREE3          0x0B   //空闲（通常未使用）
#define IRQ_PS2_MOUSE      0x0C   //PS/2 鼠标
#define IRQ_FPU            0x0D   //浮点协处理器
#define IRQ_ATA_PRIMARY    0x0E   //ATA主通道（硬盘）
#define IRQ_ATA_SECONDARY  0x0F   //ATA从通道（硬盘/CD-ROM）

void send_eoi(uint8_t irq);//发送EOI
extern void irq_dispatch(uint32_t vector, uint64_t *frame);//通用硬件中断分发
extern char irq_entries_start[];//IRQ入口桩数组 (kernel/asm/irq_handlers.S)

extern volatile uint64_t SYSTEM_TimerTicks;//系统启动以来的定时器中断计数

void InitAPIC(void);//APIC中断系统初始化

#endif
