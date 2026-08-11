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
static int text_pos = 0;//文本缓冲区当前写入位置
static int info_head = 0;//日志信息数组写入索引
static int info_count = 0;//已记录日志条数
static int console_read = 0;//print_to_console已读取条数（从最旧条目起算的偏移）

//日志级别字符串（Linux kern_levels.h 风格）
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
		print_to_console(1);
	}
}

int InitPrintk(){
	PRINTK_text_buffer = (char*)PHYS_TO_VIRT(Pmm_Malloc((PRINTK_TEXT_BUFFER_SIZE + 4095) & ~4095));
	PRINTK_log_info = (PRINTK_LOG_INFO_t*)PHYS_TO_VIRT(Pmm_Malloc(((sizeof(PRINTK_LOG_INFO_t) * PRINTK_COUNT_MAX) & ~4095) + 4095));
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

int printk(const char* fmt, ...){
	char buf[256];
	__builtin_va_list args;
	int level = PRINTK_LVL_NONE;
	const char *msg = fmt;
	int ret, len, idx;
	PRINTK_LOG_INFO_t *info;
	if (!PRINTK_text_buffer || !PRINTK_log_info)goto fallback;
	//解析,级别前缀
	if (fmt[0] == '<' && fmt[1] >= '0' && fmt[1] <= '7' && fmt[2] == '>') {
		level = fmt[1] - '0';
		msg = fmt + 3;
	}
    cli();
    //格式化字符
	__builtin_va_start(args, fmt);
	ret = vsprintf(buf, msg, args);
	__builtin_va_end(args);
	len = ret + 1;
	if (text_pos + len > PRINTK_TEXT_BUFFER_SIZE)text_pos = 0;//文本缓冲区不够时回绕
	//日志信息数组满时覆盖最旧条目
	if (info_count >= PRINTK_COUNT_MAX) {
		info_count = PRINTK_COUNT_MAX;
		info_head = (info_head + 1) % PRINTK_COUNT_MAX;
	}
    //将日志写入环形缓冲区
	idx = (info_head + info_count) % PRINTK_COUNT_MAX;
	memcpy(&PRINTK_text_buffer[text_pos], buf, (size_t)len);
	info = &PRINTK_log_info[idx];
	info->level = (uint8_t)level;
	rtc_get_local(&info->time);
	info->start = text_pos;
	info->length = ret;
	text_pos += len;
	info_count++;
    sti();
	return ret;
fallback:
    //未初始化时回退到无级别解析的直出
	__builtin_va_start(args, fmt);
	ret = vsprintf(buf, fmt, args);
	__builtin_va_end(args);
	TTY_Print(buf, CurrentConsoleStyle.TextColor);
	return ret;
}

void print_to_console(int count){
	cli();
	int i, idx, to_print;
	char buf[320];
	PRINTK_LOG_INFO_t *info;
	const char *lvl;
	uint32_t lvl_color;
	if (!PRINTK_text_buffer || !PRINTK_log_info || info_count == 0){sti();return;}
	//环形缓冲区满时旧条目被覆盖，游标可能失效，兜底复位
	if (console_read > info_count)console_read = 0;
	to_print = info_count - console_read;
	if (to_print <= 0){sti();return;}
	if (count < to_print)to_print = count;
	//最旧条目索引 + 已读偏移 = 下一条待打印索引
	//原理：本环形缓冲区约定 info_head 始终指向最旧条目（未满时即 0），
	//      因此 info_head + console_read 即为目标位置
	idx = (info_head + console_read) % PRINTK_COUNT_MAX;
	for (i = 0; i < to_print; i++) {
		info = &PRINTK_log_info[(idx + i) % PRINTK_COUNT_MAX];
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
	sti();
}
