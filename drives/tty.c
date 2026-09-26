#include <drives/tty.h>
#include <syscalls.h>
#include <drives/ps2kbd.h>
#include <task.h>
#include <signals.h>
#include <delay.h>

uint16_t TTY_PrintCol = 0;
uint16_t TTY_PrintRow = 0;
_Bool TTY_ScreenEnabled = false;
ConsoleStyle CurrentConsoleStyle = {COLOR_WHITE,COLOR_BLACK};

//ANSI 转义序列解析
#define ANSI_IDLE    0      //空闲状态：正常显示字符，未进入任何转义序列
#define ANSI_ESC     1      //已收到ESC(0x1B)，等待下一个字符判断序列类型
#define ANSI_CSI     2      //已收到CSI(ESC [)，正在解析控制序列参数
#define ANSI_OSC     3      //已收到OSC(ESC ])，正在解析操作系统命令序列
#define ANSI_CHARSET 4      //已收到字符集选择序列，等待字符集标识符

#define ANSI_PARAM_MAX 8    //ANSI转义序列参数的最大数量

static int      g_ansi_state = ANSI_IDLE;
static int      g_ansi_np = 0;//已收集参数个数
static int      g_ansi_p[ANSI_PARAM_MAX];
static int      g_ansi_acc = -1;//正在拼的数字
static _Bool    g_ansi_priv = false;//CSI带'?'前缀
static uint32_t g_ansi_fg = 0;//0=未设置, 用调用者给的颜色
static uint32_t g_ansi_bg = 0;//0=未设置, 用当前控制台背景
static _Bool    g_ansi_bold = false;
static uint16_t g_ansi_save_col = 0, g_ansi_save_row = 0;
static _Bool    g_ansi_saved = false;

//ANSI0-15号色转帧缓冲颜色
static const uint32_t g_ansi_pal[16] = {
    COLOR_BLACK, COLOR_RED,     COLOR_GREEN,   COLOR_YELLOW,
    COLOR_BLUE,  COLOR_MAGENTA, COLOR_CYAN,    COLOR_LGREY,
    COLOR_DGREY, COLOR_RED,     COLOR_GREEN,   COLOR_YELLOW,
    COLOR_BLUE,  COLOR_MAGENTA, COLOR_CYAN,    COLOR_WHITE,
};

//256色转最接近的16色
static uint32_t ansi_256(int n){
    if(n < 0)n = 0;
    if(n < 16)return g_ansi_pal[n];
    if(n < 232){
        int v = n - 16;
        int rr = v / 36, gg = (v / 6) % 6, bb = v % 6;
        _Bool r = (rr * 51) >= 128, g = (gg * 51) >= 128, b = (bb * 51) >= 128;
        if(r && g && b)return COLOR_WHITE;
        if(r && g)return COLOR_YELLOW;
        if(r && b)return COLOR_MAGENTA;
        if(g && b)return COLOR_CYAN;
        if(r)return COLOR_RED;
        if(g)return COLOR_GREEN;
        if(b)return COLOR_BLUE;
        return COLOR_BLACK;
    }
    int v = (n - 232) * 255 / 23;
    if(v < 64)return COLOR_BLACK;
    if(v < 160)return COLOR_DGREY;
    if(v < 224)return COLOR_LGREY;
    return COLOR_WHITE;
}

//当前生效的前景色
static uint32_t TTY_EffectiveFg(uint32_t fallback){
    if(g_ansi_fg)return g_ansi_fg;
    if(g_ansi_bold && (fallback == COLOR_GREY || fallback == COLOR_BLACK))return COLOR_LGREY;
    return fallback;
}
//当前生效的背景色
static uint32_t TTY_EffectiveBg(void){
    return g_ansi_bg ? g_ansi_bg : CurrentConsoleStyle.BgColor;
}

//屏幕可用行列数
static uint16_t tty_cols(void){
    uint16_t c = (uint16_t)(SYSTEM_ScreenInfo.Width / 10);
    return c ? c : 1;
}
static uint16_t tty_rows(void){
    uint16_t r = (uint16_t)(SYSTEM_ScreenInfo.Height / 18);
    return r ? r : 1;
}

//按格子范围填充(擦除用)
static void tty_fill_cells(uint16_t c0,uint16_t r0,uint16_t c1,uint16_t r1,uint32_t bg){
    if(!TTY_ScreenEnabled)return;
    if(c0 >= tty_cols() || r0 >= tty_rows())return;
    if(c1 >= tty_cols())c1 = (uint16_t)(tty_cols() - 1);
    if(r1 >= tty_rows())r1 = (uint16_t)(tty_rows() - 1);
    if(c1 < c0 || r1 < r0)return;
    fillRect((uint16_t)(c0 * 10),(uint16_t)(r0 * 18),(uint16_t)((c1 - c0 + 1) * 10),(uint16_t)((r1 - r0 + 1) * 18),bg);
}

//光标定位
static void ansi_cursor_set(int col,int row){
    if(col < 0)col = 0;
    if(row < 0)row = 0;
    if(col > (int)tty_cols() - 1)col = (int)tty_cols() - 1;
    if(row > (int)tty_rows() - 1)row = (int)tty_rows() - 1;
    TTY_PrintCol = (uint16_t)col;
    TTY_PrintRow = (uint16_t)row;
}

/*DeepSeek-V4.1-Flash*/
//SGR(m)参数处理
static void ansi_sgr(void){
    int n = g_ansi_np;
    if(n == 0){//ESC[m 等价于 ESC[0m
        g_ansi_fg = 0; g_ansi_bg = 0; g_ansi_bold = false;
        return;
    }
    for(int i = 0; i < n; i++){
        int p = g_ansi_p[i];
        if(p == 0){ g_ansi_fg = 0; g_ansi_bg = 0; g_ansi_bold = false; }
        else if(p == 1){ g_ansi_bold = true; }
        else if(p == 22){ g_ansi_bold = false; }
        else if(p == 39){ g_ansi_fg = 0; }
        else if(p == 49){ g_ansi_bg = 0; }
        else if(p >= 30 && p <= 37){ g_ansi_fg = g_ansi_pal[p - 30]; }
        else if(p >= 90 && p <= 97){ g_ansi_fg = g_ansi_pal[8 + (p - 90)]; }
        else if(p >= 40 && p <= 47){ g_ansi_bg = g_ansi_pal[p - 40]; }
        else if(p >= 100 && p <= 107){ g_ansi_bg = g_ansi_pal[8 + (p - 100)]; }
        else if(p == 38 || p == 48){//38;5;N / 48;5;N / 38;2;r;g;b
            int mode = (i + 1 < n) ? g_ansi_p[i + 1] : -1;
            if(mode == 5 && i + 2 < n){
                uint32_t c = ansi_256(g_ansi_p[i + 2]);
                if(p == 38)g_ansi_fg = c; else g_ansi_bg = c;
                i += 2;
            }else if(mode == 2 && i + 4 < n){
                int r = g_ansi_p[i + 2], g = g_ansi_p[i + 3], b = g_ansi_p[i + 4];
                if(r < 0)r = 0;
                if(r > 255)r = 255;
                if(g < 0)g = 0;
                if(g > 255)g = 255;
                if(b < 0)b = 0;
                if(b > 255)b = 255;
                uint32_t c = rgb((uint8_t)r,(uint8_t)g,(uint8_t)b);
                if(p == 38)g_ansi_fg = c; else g_ansi_bg = c;
                i += 4;
            }else{
                break;
            }
        }
    }
}

static void ansi_param_begin(void){
    g_ansi_np = 0;
    g_ansi_acc = -1;
    g_ansi_priv = false;
}
static void ansi_param_flush(void){
    if(g_ansi_acc >= 0){
        if(g_ansi_np < ANSI_PARAM_MAX)g_ansi_p[g_ansi_np] = g_ansi_acc;
        g_ansi_np++;
        g_ansi_acc = -1;
    }
}

//CSI 序列收尾: 按最终字节执行动作
static void ansi_csi_dispatch(char final){
    if(g_ansi_priv)return;//私有模式，没有光标硬件可控制, 静默忽略
    int a0 = (g_ansi_np > 0) ? g_ansi_p[0] : 0;
    if(a0 <= 0)a0 = 1;//光标类序列默认参数为1
    int col = TTY_PrintCol, row = TTY_PrintRow;
    switch(final){
    case 'm': ansi_sgr(); break;
    case 'A': ansi_cursor_set(col,row - a0); break;
    case 'B': ansi_cursor_set(col,row + a0); break;
    case 'C': ansi_cursor_set(col + a0,row); break;
    case 'D': ansi_cursor_set(col - a0,row); break;
    case 'E': ansi_cursor_set(0,row + a0); break;
    case 'F': ansi_cursor_set(0,row - a0); break;
    case 'G': ansi_cursor_set(a0 - 1,row); break;
    case 'd': ansi_cursor_set(col,a0 - 1); break;
    case 'H': case 'f':{
        int r = (g_ansi_np > 0 && g_ansi_p[0] > 0) ? g_ansi_p[0] : 1;
        int c2 = (g_ansi_np > 1 && g_ansi_p[1] > 0) ? g_ansi_p[1] : 1;
        ansi_cursor_set(c2 - 1,r - 1);
        break;
    }
    case 'J':{
        uint32_t bg = TTY_EffectiveBg();
        if(g_ansi_np == 0 || g_ansi_p[0] == 0){
            tty_fill_cells(TTY_PrintCol,TTY_PrintRow,(uint16_t)(tty_cols() - 1),(uint16_t)(tty_rows() - 1),bg);
        }else if(g_ansi_p[0] == 1){
            tty_fill_cells(0,0,TTY_PrintCol,TTY_PrintRow,bg);
        }else{
            TTY_Clear();
        }
        break;
    }
    case 'K':{
        uint32_t bg = TTY_EffectiveBg();
        if(g_ansi_np == 0 || g_ansi_p[0] == 0){
            tty_fill_cells(TTY_PrintCol,TTY_PrintRow,(uint16_t)(tty_cols() - 1),TTY_PrintRow,bg);
        }else if(g_ansi_p[0] == 1){
            tty_fill_cells(0,TTY_PrintRow,TTY_PrintCol,TTY_PrintRow,bg);
        }else{
            tty_fill_cells(0,TTY_PrintRow,(uint16_t)(tty_cols() - 1),TTY_PrintRow,bg);
        }
        break;
    }
    case 's': g_ansi_save_col = TTY_PrintCol; g_ansi_save_row = TTY_PrintRow; g_ansi_saved = true; break;
    case 'u': if(g_ansi_saved)ansi_cursor_set(g_ansi_save_col,g_ansi_save_row); break;
    default: break;//其余序列静默忽略
    }
}

//喂入一个字节
static _Bool ansi_feed(char c){
    switch(g_ansi_state){
    case ANSI_IDLE:
        if((uint8_t)c == 0x1B){ g_ansi_state = ANSI_ESC; return true; }
        return false;
    case ANSI_ESC:
        if(c == '['){ g_ansi_state = ANSI_CSI; ansi_param_begin(); return true; }
        if(c == ']'){ g_ansi_state = ANSI_OSC; return true; }//OSC: 吞到 BEL 或 ST
        if(c == '(' || c == ')' || c == '*' || c == '+'){ g_ansi_state = ANSI_CHARSET; return true; }
        if(c == '7'){ g_ansi_save_col = TTY_PrintCol; g_ansi_save_row = TTY_PrintRow; g_ansi_saved = true; g_ansi_state = ANSI_IDLE; return true; }
        if(c == '8'){ if(g_ansi_saved)ansi_cursor_set(g_ansi_save_col,g_ansi_save_row); g_ansi_state = ANSI_IDLE; return true; }
        if(c == 'c'){ TTY_Clear(); ansi_cursor_set(0,0); g_ansi_state = ANSI_IDLE; return true; }
        g_ansi_state = ANSI_IDLE;//未知的两字节序列: 丢弃
        return true;
    case ANSI_CSI:
        if(c >= '0' && c <= '9'){
            g_ansi_acc = (g_ansi_acc < 0 ? 0 : g_ansi_acc) * 10 + (c - '0');
            if(g_ansi_acc > 9999)g_ansi_acc = 9999;
            return true;
        }
        if(c == ';'){ ansi_param_flush(); return true; }
        if(c == '?'){ g_ansi_priv = true; return true; }
        if(c == ' ' || c == '>' || c == '<' || c == '=')return true;//中间字节, 忽略
        if(c >= 0x40 && c <= 0x7E){ ansi_param_flush(); ansi_csi_dispatch(c); g_ansi_state = ANSI_IDLE; return true; }
        g_ansi_state = ANSI_IDLE;//非法序列: 放弃解析
        return true;
    case ANSI_OSC:
        if((uint8_t)c == 0x07){ g_ansi_state = ANSI_IDLE; return true; }//BEL 结束
        if((uint8_t)c == 0x1B){ g_ansi_state = ANSI_ESC; return true; }//可能是 ST
        return true;
    case ANSI_CHARSET:
        g_ansi_state = ANSI_IDLE;//吞掉字符集设计符
        return true;
    }
    g_ansi_state = ANSI_IDLE;
    return false;
}
/*DeepSeek-V4.1-Flash-END*/

void TTY_PrintChar(const char c,uint32_t color){
    SerialWriteChar(SERIAL_COM1,c);
    if(!TTY_ScreenEnabled)return;
    if(ansi_feed(c))return;//属于转义序列, 交给解析器, 不上屏
    //设置颜色
    uint32_t fg = TTY_EffectiveFg(color);
    uint32_t bg = TTY_EffectiveBg();
    //回车
    if(c == '\r'){
        TTY_PrintCol = 0;
        return;
    }
    if((((uint8_t)c < 0x20) && c != '\n' && c != '\t' && c != '\b') || (uint8_t)c == 0x7F)return;
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
        fillRect(TTY_PrintCol * 10,TTY_PrintRow * 18,10,18,bg);
        TTY_PrintCol ++;
        return;
    }
    //先整格填充背景色, 再绘制前景字形
    fillRect(TTY_PrintCol * 10,TTY_PrintRow * 18,10,18,bg);
    DrawChar(c,TTY_PrintCol * 10,TTY_PrintRow * 18,fg);
    TTY_PrintCol ++;//记录打印位置
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
    fillRect(0,0,SYSTEM_ScreenInfo.Width,SYSTEM_ScreenInfo.Height,TTY_EffectiveBg());
}

/*DeepSeek-V4.1-Flash*/
//设置后续输出的颜色
static void TTY_SetStyle(uint32_t fg,uint32_t bg){
    g_ansi_fg = fg;
    g_ansi_bg = bg;
    g_ansi_bold = false;
}

//内核侧带颜色打印
void TTY_PrintColor(const char *str,uint32_t fg,uint32_t bg){
    uint32_t sfg = g_ansi_fg, sbg = g_ansi_bg;
    _Bool    sbl = g_ansi_bold;
    TTY_SetStyle(fg,bg);
    TTY_Print(str,CurrentConsoleStyle.TextColor);
    g_ansi_fg = sfg;
    g_ansi_bg = sbg;
    g_ansi_bold = sbl;
}

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
        if(c == 0x1B){
            char e1 = 0;
            for(int i = 0; i < 20 && e1 == 0; i++){
                e1 = GetKey_NoBlock();
                if(!e1)udelay(1000);
            }
            if(e1 == '[' || e1 == 'O'){
                for(;;){
                    char e2 = 0;
                    for(int i = 0; i < 20 && e2 == 0; i++){
                        e2 = GetKey_NoBlock();
                        if(!e2)udelay(1000);
                    }
                    if(e2 == 0)break;
                    if(e2 >= 0x40 && e2 <= 0x7E)break;
                    if(e2 < 0x20 || e2 > 0x3F)break;
                }
            }else if(e1 != 0 && g_line_len < TTY_LINE_MAX - 1){
                g_line[g_line_len++] = e1;
                if(g_tty_term.lflag & TTY_ECHO)tty_echo(&e1, 1);
            }
            continue;
        }
        //Ctrl+C(VINTR)
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

//终端输入是否可读
_Bool TTY_ReadReady(void){
    if(g_line_len > 0)return true;
    if(g_tty_intr_pending)return true;
    if(!(g_tty_term.lflag & TTY_ICANON))return Kbd_Available() > 0;
    return Kbd_HasLine();
}
/*DeepSeek-V4.1-Flash-END*/