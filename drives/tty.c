#include <drives/tty.h>
#include <syscalls.h>
#include <drives/ps2kbd.h>
#include <task.h>
#include <signals.h>

uint16_t TTY_PrintCol = 0;
uint16_t TTY_PrintRow = 0;
_Bool TTY_ScreenEnabled = false;
ConsoleStyle CurrentConsoleStyle = {COLOR_WHITE,COLOR_BLACK};

void TTY_PrintChar(const char c,uint32_t color){
    SerialWriteChar(SERIAL_COM1,c);
    if(!TTY_ScreenEnabled)return;
    //退格
    if(c == '\b'){
        if(TTY_PrintCol > 0){
            TTY_PrintCol--;
        }else if(TTY_PrintRow > 0){
            TTY_PrintRow--;
            TTY_PrintCol = (SYSTEM_ScreenInfo.Width >= 10) ? (SYSTEM_ScreenInfo.Width / 10 - 1) : 0;
        }
        return;
    }
    uint16_t spl = TTY_PrintRow;
    uint16_t spr = TTY_PrintCol;
    //屏幕已满,立即清屏回到顶部
    if((uint64_t)TTY_PrintRow * 18 + 16 > SYSTEM_ScreenInfo.Height){
        TTY_Clear();
        TTY_PrintRow = 0;
        TTY_PrintCol = 0;
    }
    //如果字符为换行符
    if(c == '\n'){
        TTY_PrintRow += 1;
        TTY_PrintCol = 0;
        //如果换行后超出屏幕,清屏回到顶部
        if((uint64_t)TTY_PrintRow * 18 + 16 > SYSTEM_ScreenInfo.Height){
            TTY_Clear();
            TTY_PrintRow = 0;
            TTY_PrintCol = 0;
        }
        return;
    }
    //如果列超出屏幕宽度，自动换行
    if((uint64_t)TTY_PrintCol * 10 + 10 > SYSTEM_ScreenInfo.Width){
        TTY_PrintRow += 1;
        TTY_PrintCol = 0;
        //如果换行后超出屏幕，清屏回到顶部
        if((uint64_t)TTY_PrintRow * 18 + 16 > SYSTEM_ScreenInfo.Height){
            TTY_Clear();
            TTY_PrintRow = 0;
            TTY_PrintCol = 0;
        }
    }
    //如果字符为制表符
    if(c == '\t'){
        fillRect(TTY_PrintCol * 10,TTY_PrintRow * 18,10,18,CurrentConsoleStyle.BgColor);
        TTY_PrintCol ++;
        return;
    }
    //先整格填充背景色, 再绘制前景字形
    fillRect(TTY_PrintCol * 10,TTY_PrintRow * 18,10,18,CurrentConsoleStyle.BgColor);
    DrawChar(c,TTY_PrintCol * 10,TTY_PrintRow * 18,color);//绘制文字
    TTY_PrintCol ++;//记录打印位置
    for(int i = 0;i < 16;i ++){
        DrawPiexl(spr * 10,spl * 18+i,CurrentConsoleStyle.BgColor);
    }
}

void TTY_Print(const char *str,uint32_t color){
    uint16_t i = 0;
    while(str[i]){
        TTY_PrintChar(str[i],color);
        i ++;
    }
}

void TTY_SetCursor(uint16_t col,uint16_t row){
    TTY_PrintCol = col;
    TTY_PrintRow = row;
}

void TTY_Clear(){
    if(!TTY_ScreenEnabled)return;
    fillRect(0,0,SYSTEM_ScreenInfo.Width,SYSTEM_ScreenInfo.Height,CurrentConsoleStyle.BgColor);
}

/*DeepSeek-V4.1-Flash*/
ktermios_t g_tty_term = {
    .iflag = 0x6D02,//BRKINT|ICRNL|IXON|IXANY|IMAXBEL|IUTF8
    .oflag = 0x0005,//OPOST|ONLCR(输出侧换行已由TTY_PrintChar处理, 此处仅上报)
    .cflag = 0x04BF,//B38400|CS8|CREAD|HUPCL
    .lflag = TTY_ISIG|TTY_ICANON|TTY_ECHO|TTY_ECHOE|TTY_ECHOK|TTY_ECHOCTL|TTY_ECHOKE|TTY_IEXTEN,
    .line = 0,
    .cc = {3,28,127,21,4,0,1,0,17,19,26,0,18,15,23,22},
    .ispeed = 38400,
    .ospeed = 38400,
};

static char g_line[TTY_LINE_MAX];//行编辑缓冲
static int  g_line_len = 0;
static volatile pid_t g_tty_fg_pid = 0;      //最后读本终端的任务(近似前台任务)
static volatile int   g_tty_intr_pending = 0;//键盘IRQ捕获的无读者Ctrl+C, 待定时器安全点投递

//键盘IRQ调用: 无终端读者时的Ctrl+C由定时器安全点投递(用于中断正在运行的前台命令)
_Bool TTY_KeyInput(char c){
    if((g_tty_term.lflag & TTY_ISIG) && c == (char)g_tty_term.cc[0] && g_tty_fg_pid > 0){
        g_tty_intr_pending = 1;
        return true;//消费该键, 不再进入按键缓冲
    }
    return false;
}

//定时器IRQ调用(安全点): 投递挂起的Ctrl+C(前台任务+其子孙, 近似前台进程组)
void TTY_IntrCheck(void){
    if(!g_tty_intr_pending)return;
    g_tty_intr_pending = 0;
    task_struct *t = TaskFind((pid_t)g_tty_fg_pid);
    if(!t)return;
    SignalSend(t, SIGINT, SI_KERNEL, 0, 0);
    TaskSignalDescendants(t, SIGINT);
}

static inline void tty_echo(const char *s, int n){
    for(int i = 0; i < n; i++)TTY_PrintChar(s[i], CurrentConsoleStyle.TextColor);
}

static int tty_line_take(char *kbuf, int ccount){
    if(g_line_len <= 0)return 0;
    int n = g_line_len < ccount ? g_line_len : ccount;
    memcpy(kbuf, g_line, n);
    if(n < g_line_len){
        memmove(g_line, g_line + n, g_line_len - n);
        g_line_len -= n;
    }else{
        g_line_len = 0;
    }
    return n;
}

//行规程读取
long TTY_TermRead(char *kbuf, long count){
    int ccount = (int)(count < TTY_LINE_MAX ? count : TTY_LINE_MAX);
    if(ccount <= 0)return 0;
    g_tty_fg_pid = current_task ? current_task->pid : 0;//记住前台任务
    //非规范模式: 单键直返
    if(!(g_tty_term.lflag & TTY_ICANON)){
        char c = GetKey();
        if(g_tty_term.lflag & TTY_ECHO)tty_echo(&c, 1);
        kbuf[0] = c;
        return 1;
    }
    int ready = tty_line_take(kbuf, ccount);
    if(ready > 0)return ready;
    for(;;){
        char c = GetKey();
        //Ctrl+C(VINTR): 清行, 回显^C, 投递SIGINT, read返回EINTR
        if((g_tty_term.lflag & TTY_ISIG) && c == (char)g_tty_term.cc[0]){
            g_tty_intr_pending = 0;//本路径自行处理, 避免定时器重复投递
            if(g_tty_term.lflag & TTY_ECHO)tty_echo("^C\n", 3);
            g_line_len = 0;
            if(current_task)SignalSend(current_task, SIGINT, SI_KERNEL, 0, 0);
            return -EINTR;
        }
        //Ctrl+D(VEOF): 空行返回EOF(0), 否则先返回已编辑内容
        if(c == (char)g_tty_term.cc[4]){
            if(g_line_len == 0)return 0;
            goto flush;
        }
        //退格(VERASE/DEL/0x08)
        if(c == (char)g_tty_term.cc[2] || c == 127 || c == '\b'){
            if(g_line_len > 0){
                g_line_len--;
                if(g_tty_term.lflag & TTY_ECHO)tty_echo("\b \b", 3);
            }
            continue;
        }
        //Ctrl+U(VKILL): 清行
        if(c == (char)g_tty_term.cc[3]){
            while(g_line_len > 0){
                g_line_len--;
                if(g_tty_term.lflag & TTY_ECHO)tty_echo("\b \b", 3);
            }
            continue;
        }
        //普通字符/回车
        if(c == '\n'){
            if(g_line_len < TTY_LINE_MAX)g_line[g_line_len++] = '\n';
            if(g_tty_term.lflag & TTY_ECHO)tty_echo("\n", 1);
            goto flush;
        }
        if(g_line_len >= TTY_LINE_MAX - 1){//行满: 先返回已缓冲内容(不丢字符)
            goto flush;
        }
        g_line[g_line_len++] = c;
        if(g_tty_term.lflag & TTY_ECHO)tty_echo(&c, 1);
        continue;
flush:
        {
            return tty_line_take(kbuf, ccount);
        }
    }
}

//导出为老式 struct termios(glibc x86_64 布局60字节)
void TTY_TermExportLegacy(void *dst){
    tty_termios_legacy_t *u = (tty_termios_legacy_t*)dst;
    memset(u, 0, sizeof(*u));
    u->iflag = g_tty_term.iflag;
    u->oflag = g_tty_term.oflag;
    u->cflag = g_tty_term.cflag;
    u->lflag = g_tty_term.lflag;
    u->line  = g_tty_term.line;
    memcpy(u->cc, g_tty_term.cc, 19);//NCCS=19, 其余保持0
    u->ispeed = g_tty_term.ispeed;
    u->ospeed = g_tty_term.ospeed;
}

//导出为 struct termios2(glibc TCGETS2, 44字节)
void TTY_TermExport2(void *dst){
    tty_termios2_t *u = (tty_termios2_t*)dst;
    memset(u, 0, sizeof(*u));
    u->iflag = g_tty_term.iflag;
    u->oflag = g_tty_term.oflag;
    u->cflag = g_tty_term.cflag;
    u->lflag = g_tty_term.lflag;
    u->line  = g_tty_term.line;
    memcpy(u->cc, g_tty_term.cc, 19);
    u->ispeed = g_tty_term.ispeed;
    u->ospeed = g_tty_term.ospeed;
}

static void tty_term_import(const uint32_t flags[4], uint8_t line, const uint8_t *cc){
    g_tty_term.iflag = flags[0];
    g_tty_term.oflag = flags[1];
    g_tty_term.cflag = flags[2];
    g_tty_term.lflag = flags[3];
    g_tty_term.line  = line;
    memcpy(g_tty_term.cc, cc, 19);
}

void TTY_TermImportLegacy(const void *src){
    const tty_termios_legacy_t *u = (const tty_termios_legacy_t*)src;
    uint32_t f[4] = {u->iflag, u->oflag, u->cflag, u->lflag};
    tty_term_import(f, u->line, u->cc);
}

void TTY_TermImport2(const void *src){
    const tty_termios2_t *u = (const tty_termios2_t*)src;
    uint32_t f[4] = {u->iflag, u->oflag, u->cflag, u->lflag};
    tty_term_import(f, u->line, u->cc);
}
/*DeepSeek-V4.1-Flash-END*/