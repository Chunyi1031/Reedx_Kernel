#ifndef _KERNEL_LIB_H_
#define _KERNEL_LIB_H_

#include <types.h>
#include <boot.h>
#include <serial.h>
#include <kstring.h>
#include <io.h>
#include <spinlock.h>

extern uint64_t SYSTEM_CPU_Fquency;

#define SYSTEM_STOP() do {while (1) __asm__ volatile("hlt");} while(0)

struct list_node {
    struct list_node *prev;//指向前一个节点
    struct list_node *next;//指向下一个节点
};

typedef struct utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
}__attribute__((packed)) utsname_t;

extern utsname_t system_utsname;

#endif