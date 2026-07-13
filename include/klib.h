#ifndef _KERNEL_LIB_H_
#define _KERNEL_LIB_H_

#include <types.h>
#include <boot.h>
#include <serial.h>
#include <kstring.h>

extern uint64_t SYSTEM_CPU_Fquency;

#define SYSTEM_STOP() do {while (1) __asm__ volatile("hlt");} while(0)

#endif