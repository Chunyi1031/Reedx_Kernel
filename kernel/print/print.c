#include <print.h>

int early_printk(const char* fmt, ...){
    char buf[128];
    __builtin_va_list args;
	int ret;
	__builtin_va_start(args, fmt);
	ret = vsprintf(buf, fmt, args);
	__builtin_va_end(args);
    TTY_Print(buf,CurrentConsoleStyle.TextColor);
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
