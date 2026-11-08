// SPDX-License-Identifier: GPL-2.0-only
/**
 * kernel/print/print.c
 * 
 * Copyright (C) 2026 Liu Chunyi
 * 
 */
#include <print.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <idt.h>
#include <task.h>

int early_printk(const char* fmt, ...){
	char buf[256];
	__builtin_va_list args;
	int ret;
	__builtin_va_start(args, fmt);
	ret = vsprintf(buf, fmt, args);
	__builtin_va_end(args);
	TTY_Print(buf, CurrentConsoleStyle.TextColor);
	return ret;
}

void print_error(){
	TTY_PrintChar('[',COLOR_WHITE);
	TTY_Print("ERROR",COLOR_RED);
	TTY_PrintChar(']',COLOR_WHITE);
}
void print_warning(){
	TTY_PrintChar('[',COLOR_WHITE);
	TTY_Print("WARNING",COLOR_YELLOW);
	TTY_PrintChar(']',COLOR_WHITE);
}
void print_ok(){
	TTY_PrintChar('[',COLOR_WHITE);
	TTY_Print("OK",COLOR_GREEN);
	TTY_PrintChar(']',COLOR_WHITE);
}


char* PRINTK_text_buffer = NULL;
PRINTK_LOG_INFO_t* PRINTK_log_info    = NULL;
volatile int PRINTK_in_irq = 0;//中断上下文嵌套计数

//保存中断标志
static inline uint64_t printk_save_irq(void){
	uint64_t f;
	__asm__ volatile("pushfq\n\tpopq %0" : "=r"(f) : : "memory");
	return f;
}

//按保存的标志恢复中断状态
static inline void printk_restore_irq(uint64_t f){
	__asm__ volatile("pushq %0\n\tpopfq" : : "r"(f) : "memory");
}
static int text_pos = 0;//文本缓冲区当前写入位置
static int info_head = 0;//日志信息数组写入索引
static int info_count = 0;//已记录日志条数
static int console_read = 0;//print_to_console已读取条数（从最旧条目起算的偏移）

//日志级别字符串
static const char *level_strings[] = {
	[PRINTK_LVL_EMERG]     = "EMERG",
	[1]                    = "ALERT",
	[2]                    = "CRIT",
	[PRINTK_LVL_ERR]       = "ERR",
	[PRINTK_LVL_WARNING]   = "WARN",
	[5]                    = "NOTICE",
	[PRINTK_LVL_INFO]      = "INFO",
	[PRINTK_LVL_DEBUG]     = "DEBUG",
};

void task_putlog(){
	while(1){
		print_to_console(8);
		msleep(10);
	}
}

int InitPrintk(){
	int text_pages = (PRINTK_TEXT_BUFFER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
	int info_pages = (int)((sizeof(PRINTK_LOG_INFO_t) * PRINTK_COUNT_MAX + PAGE_SIZE - 1) / PAGE_SIZE);
	PRINTK_text_buffer = (char*)PHYS_TO_VIRT(Pmm_Malloc(text_pages));
	PRINTK_log_info = (PRINTK_LOG_INFO_t*)PHYS_TO_VIRT(Pmm_Malloc(info_pages));
	if(!PRINTK_text_buffer || !PRINTK_log_info)return 1;
	memset(PRINTK_text_buffer, 0, PRINTK_TEXT_BUFFER_SIZE);
	memset(PRINTK_log_info, 0, sizeof(PRINTK_LOG_INFO_t) * PRINTK_COUNT_MAX);
	text_pos = 0;
	info_head = 0;
	info_count = 0;
	console_read = 0;
	CreateKernelThread(task_putlog,8192,"Kernel Log");
	return 0;
}

//把一条已格式化的文本按给定级别写入环形缓冲区
static void printk_buffer_write(int level, int length, const char *msg){
	int len, idx;
	PRINTK_LOG_INFO_t *info;
	uint64_t iflags;
	if(!msg || length < 0)return;
	len = length + 1;
	iflags = printk_save_irq();
	cli();
	if (text_pos + len > PRINTK_TEXT_BUFFER_SIZE)text_pos = 0;//文本缓冲区不够时回绕
	//日志数组已满，新条目顶替最旧条目,头部与未打印游标一起前移
	if (info_count >= PRINTK_COUNT_MAX) {
		idx = info_head;
		info_head = (info_head + 1) % PRINTK_COUNT_MAX;
		if (console_read > 0)console_read --;
	}else{
		idx = (info_head + info_count) % PRINTK_COUNT_MAX;
		info_count ++;
	}
	memcpy(&PRINTK_text_buffer[text_pos], msg, (size_t)len);
	info = &PRINTK_log_info[idx];
	info->level = (uint8_t)level;
	rtc_get_local(&info->time);
	info->start = text_pos;
	info->length = length;
	text_pos += len;
	printk_restore_irq(iflags);
}

int printk(const char* fmt, ...){
	char buf[256];
	__builtin_va_list args;
	int level = PRINTK_LVL_NONE;
	const char *msg = fmt;
	int ret;
	if (!PRINTK_text_buffer || !PRINTK_log_info)goto fallback;
	//解析,级别前缀
	if (fmt[0] == '<' && fmt[1] >= '0' && fmt[1] <= '7' && fmt[2] == '>') {
		level = fmt[1] - '0';
		msg = fmt + 3;
	}
    //格式化字符
	__builtin_va_start(args, fmt);
	ret = vsprintf(buf, msg, args);
	__builtin_va_end(args);
	printk_buffer_write(level, ret, buf);
	return ret;
fallback:
    //未初始化时回退到无级别解析的直出
	__builtin_va_start(args, fmt);
	ret = vsprintf(buf, fmt, args);
	__builtin_va_end(args);
	TTY_Print(buf, CurrentConsoleStyle.TextColor);
	return ret;
}

//按指定级别写入一段已格式化的文本
void printk_log(int level, const char *msg){
	if(!msg)return;
	if(!PRINTK_text_buffer || !PRINTK_log_info){
		TTY_Print(msg, CurrentConsoleStyle.TextColor);
		TTY_PrintChar('\n', CurrentConsoleStyle.TextColor);
		return;
	}
	if(level < 0 || (level > PRINTK_LVL_DEBUG && level != PRINTK_LVL_NONE))level = PRINTK_LVL_INFO;
	printk_buffer_write(level, (int)strlen(msg), msg);
}

void print_to_console(int count){
	uint64_t iflags = printk_save_irq();
	cli();
	int i, idx, to_print;
	char buf[320];
	PRINTK_LOG_INFO_t *info;
	const char *lvl;
	uint32_t lvl_color;
	if (!PRINTK_text_buffer || !PRINTK_log_info || info_count == 0){printk_restore_irq(iflags);return;}
	//环形缓冲区满时旧条目被覆盖，游标可能失效，兵底复位
	if (console_read > info_count)console_read = 0;
	to_print = info_count - console_read;
	if (to_print <= 0){
		printk_restore_irq(iflags);
		return;
	}
	if (count < to_print)to_print = count;
	idx = (info_head + console_read) % PRINTK_COUNT_MAX;
	for (i = 0; i < to_print; i++) {
		info = &PRINTK_log_info[(idx + i) % PRINTK_COUNT_MAX];
		if (info->start < 0 || info->length < 0 || info->start + info->length >= PRINTK_TEXT_BUFFER_SIZE)continue;//文本已被覆盖的陈旧条目
		if (info->level > PRINTK_LVL_INFO)continue;//DEBUG 级只进环形缓冲区，不上屏
		//无等级→原样直出，不套任何格式
		if (info->level == PRINTK_LVL_NONE) {
			int show_len = (info->length < 256) ? info->length : 256;
			memcpy(buf, &PRINTK_text_buffer[info->start], (size_t)show_len);
			buf[show_len] = '\0';
			TTY_Print(buf, CurrentConsoleStyle.TextColor);
			continue;
		}
		lvl = (info->level <= PRINTK_LVL_DEBUG) ? level_strings[info->level] : "?";
		if (info->level <= PRINTK_LVL_ERR)lvl_color = COLOR_RED;
		else if (info->level <= PRINTK_LVL_WARNING)lvl_color = COLOR_YELLOW;
		else if (info->level == PRINTK_LVL_INFO)lvl_color = 0xFFAAE696;
		else lvl_color = COLOR_CYAN;
		TTY_PrintChar('[', COLOR_WHITE);
		sprintf(buf, "%02d:%02d:%02d", info->time.hour, info->time.minute, info->time.second);
		TTY_Print(buf, COLOR_WHITE);
		TTY_PrintChar(']', COLOR_WHITE);
		TTY_PrintChar('[', COLOR_WHITE);
		TTY_Print(lvl, lvl_color);
		TTY_PrintChar(']', COLOR_WHITE);
		int show_len = (info->length < 256) ? info->length : 256;
		memcpy(buf, &PRINTK_text_buffer[info->start], (size_t)show_len);
		buf[show_len] = '\0';
		TTY_Print(buf, CurrentConsoleStyle.TextColor);
		TTY_PrintChar('\n', CurrentConsoleStyle.TextColor);
	}
	console_read += to_print;
	printk_restore_irq(iflags);
}

//读控制寄存器,崩溃现场里一起打出来
static inline uint64_t panic_read_cr0(void){ uint64_t v; __asm__ volatile("mov %%cr0,%0" : "=r"(v)); return v; }
static inline uint64_t panic_read_cr2(void){ uint64_t v; __asm__ volatile("mov %%cr2,%0" : "=r"(v)); return v; }
static inline uint64_t panic_read_cr3(void){ uint64_t v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static inline uint64_t panic_read_cr4(void){ uint64_t v; __asm__ volatile("mov %%cr4,%0" : "=r"(v)); return v; }

static void panic_line(const char* fmt, ...){
	char buf[256];
	__builtin_va_list args;
	__builtin_va_start(args, fmt);
	vsprintf(buf, fmt, args);
	__builtin_va_end(args);
	printk(PRINTK_EMERG"%s", buf);
	SerialWriteString(SERIAL_COM1, buf);
	SerialWriteString(SERIAL_COM1, "\n");
}

void panic_report(struct exc_frame *f, const char *msg){
	cli();
	panic_line("==:( KERNEL PANIC: %s ):===", msg);
	panic_line("RIP=%p RSP=%p CS =%p RFLAGS=%p SS=%p", (void*)f->rip, (void*)f->rsp,(void*)f->cs, (void*)f->rflags, (void*)f->ss);
	panic_line("RAX=%p RBX=%p RCX=%p RDX=%p", (void*)f->rax, (void*)f->rbx, (void*)f->rcx, (void*)f->rdx);
	panic_line("RSI=%p RDI=%p RBP=%p R8 =%p", (void*)f->rsi, (void*)f->rdi, (void*)f->rbp, (void*)f->r8);
	panic_line("R9 =%p R10=%p R11=%p R12=%p", (void*)f->r9, (void*)f->r10, (void*)f->r11, (void*)f->r12);
	panic_line("R13=%p R14=%p R15=%p", (void*)f->r13, (void*)f->r14, (void*)f->r15);
	panic_line("CR0=%p CR2=%p CR3=%p CR4=%p", (void*)panic_read_cr0(), (void*)panic_read_cr2(),(void*)panic_read_cr3(), (void*)panic_read_cr4());
	if(current_task)panic_line("task: pid=%d name=%s kstack=%p",(int)current_task->pid, current_task->name, current_task->kernel_stack);
	else panic_line("task: none");
	panic_line("The system will terminate, and then you can restart it.");
	print_to_console(PRINTK_COUNT_MAX);//把日志环形缓冲区刷到屏幕上
	cli();
	SYSTEM_STOP();
}

void panic(const char* fmt, ...){
	char buf[256];
	__builtin_va_list args;
	__builtin_va_start(args, fmt);
	vsprintf(buf, fmt, args);
	__builtin_va_end(args);
	panic_enter(buf, (uint64_t)__builtin_return_address(0));
}
