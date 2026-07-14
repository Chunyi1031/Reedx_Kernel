#ifndef _PRINT_H_
#define _PRINT_H_

#include <klib.h>
#include <drives/tty.h>

int early_printk(const char* fmt, ...);
void print_error();
void print_warning();
void print_ok();

#endif