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

#define TTY_CELL_W  CHAR_CELL_W
#define TTY_CELL_H  CHAR_CELL_H

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
static _Bool    g_ansi_rev = false;//SGR 7反显
static uint16_t g_ansi_save_col = 0, g_ansi_save_row = 0;
static _Bool    g_ansi_saved = false;
static _Bool    g_cur_visible = true;//光标是否显示

static void tty_cursor_erase(void);
static void tty_cursor_draw(uint32_t color,uint32_t bg);

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

//SGR 7(反显)就是交换前景与背景
static void tty_make_colors(uint32_t fallback,uint32_t *out_fg,uint32_t *out_bg){
    uint32_t f = g_ansi_fg ? g_ansi_fg : fallback;
    uint32_t b = g_ansi_bg ? g_ansi_bg : CurrentConsoleStyle.BgColor;
    if(g_ansi_bold && (f == COLOR_GREY || f == COLOR_BLACK))f = COLOR_LGREY;
    if(g_ansi_rev){
        uint32_t t = f;
        f = b;
        b = t;
    }
    *out_fg = f;
    *out_bg = b;
}
//当前生效的前景色
static uint32_t TTY_EffectiveFg(uint32_t fallback){
    uint32_t f, b;
    tty_make_colors(fallback,&f,&b);
    return f;
}
//当前生效的背景色
static uint32_t TTY_EffectiveBg(void){
    uint32_t f, b;
    tty_make_colors(CurrentConsoleStyle.TextColor,&f,&b);
    return b;
}

//屏幕可用行列数
static uint16_t tty_cols(void){
    uint16_t c = (uint16_t)(SYSTEM_ScreenInfo.Width / TTY_CELL_W);
    return c ? c : 1;
}
static uint16_t tty_rows(void){
    uint16_t r = (uint16_t)(SYSTEM_ScreenInfo.Height / TTY_CELL_H);
    return r ? r : 1;
}

//按格子范围填充(擦除用)
static void tty_fill_cells(uint16_t c0,uint16_t r0,uint16_t c1,uint16_t r1,uint32_t bg){
    if(!TTY_ScreenEnabled)return;
    if(c0 >= tty_cols() || r0 >= tty_rows())return;
    if(c1 >= tty_cols())c1 = (uint16_t)(tty_cols() - 1);
    if(r1 >= tty_rows())r1 = (uint16_t)(tty_rows() - 1);
    if(c1 < c0 || r1 < r0)return;
    fillRect((uint16_t)(c0 * TTY_CELL_W),(uint16_t)(r0 * TTY_CELL_H),(uint16_t)((c1 - c0 + 1) * TTY_CELL_W),(uint16_t)((r1 - r0 + 1) * TTY_CELL_H),bg);
}

//读一个像素
static uint32_t tty_read_px(uint16_t x,uint16_t y){
    if(!ScreenFbMapped())return 0;
    return FbReadPixel((int)x,(int)y);
}

static void tty_move_cells(int row,int dst_col,int src_col,int count){
    if(count <= 0)return;
    if(row < 0 || row >= (int)tty_rows())return;
    if(dst_col < 0 || src_col < 0)return;
    int step = (dst_col < src_col) ? 1 : -1;
    for(int y = 0; y < TTY_CELL_H; y++){
        int py = row * TTY_CELL_H + y;
        if(py >= (int)SYSTEM_ScreenInfo.Height)break;
        int dx = dst_col * TTY_CELL_W, sx = src_col * TTY_CELL_W;
        for(int i = 0; i < count * TTY_CELL_W; i++){
            int k = (step > 0) ? i : (count * TTY_CELL_W - 1 - i);
            uint32_t px = tty_read_px((uint16_t)(sx + k),(uint16_t)py);
            DrawPiexl((uint16_t)(dx + k),(uint16_t)py,px);
        }
    }
}

static int g_scroll_top = 0;
static int g_scroll_bot = -1;

//滚动区域的下边界(行号)
static int tty_bot_row(void){
    int last = (int)tty_rows() - 1;
    if(g_scroll_bot >= 0 && g_scroll_bot < last)return g_scroll_bot;
    return last;
}

//把第top~bot行整体上滚一行
static void tty_scroll_region(int top,int bot,uint32_t bg){
    if(!TTY_ScreenEnabled)return;
    if(!ScreenFbMapped())return;
    if(top < 0)top = 0;
    if(bot > (int)tty_rows() - 1)bot = (int)tty_rows() - 1;
    if(top >= bot)return;
    int y0 = top * TTY_CELL_H;
    int y1 = bot * TTY_CELL_H;
    FbScrollUp(y0,y1,TTY_CELL_H,bg);
}

//按当前滚动区域滚一行
static void tty_scroll_page(uint32_t bg){
    tty_scroll_region(g_scroll_top,tty_bot_row(),bg);
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
        g_ansi_fg = 0; g_ansi_bg = 0; g_ansi_bold = false; g_ansi_rev = false;
        return;
    }
    for(int i = 0; i < n; i++){
        int p = g_ansi_p[i];
        if(p == 0){ g_ansi_fg = 0; g_ansi_bg = 0; g_ansi_bold = false; g_ansi_rev = false; }
        else if(p == 1){ g_ansi_bold = true; }
        else if(p == 7){ g_ansi_rev = true; }//反显
        else if(p == 22){ g_ansi_bold = false; }
        else if(p == 27){ g_ansi_rev = false; }
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
static void ansi_param_push(void){
    int v = (g_ansi_acc < 0) ? 0 : g_ansi_acc;
    if(g_ansi_np < ANSI_PARAM_MAX)g_ansi_p[g_ansi_np] = v;
    g_ansi_np++;
    g_ansi_acc = -1;
}
//遇到终止字节: 没写数字就不补参数(ESC[m 要保持 0 个参数)
static void ansi_param_flush(void){
    if(g_ansi_acc >= 0){
        if(g_ansi_np < ANSI_PARAM_MAX)g_ansi_p[g_ansi_np] = g_ansi_acc;
        g_ansi_np++;
        g_ansi_acc = -1;
    }
}

//CSI 序列收尾: 按最终字节执行动作
static void ansi_csi_dispatch(char final){
    if(g_ansi_priv){
        if(final == 'h' || final == 'l'){
            for(int i = 0; i < g_ansi_np; i++){
                if(g_ansi_p[i] == 25){
                    if(final == 'h'){
                        g_cur_visible = true;
                        tty_cursor_draw(TTY_EffectiveFg(CurrentConsoleStyle.TextColor),TTY_EffectiveBg());
                    }else{
                        tty_cursor_erase();
                        g_cur_visible = false;
                    }
                }
            }
        }
        return;
    }
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
    case 'P':{
        int n2 = a0, c = TTY_PrintCol;
        if(c + n2 > (int)tty_cols())n2 = (int)tty_cols() - c;
        if(n2 > 0){
            tty_move_cells(TTY_PrintRow,c,c + n2,(int)tty_cols() - c - n2);
            tty_fill_cells((uint16_t)(tty_cols() - n2),TTY_PrintRow,(uint16_t)(tty_cols() - 1),TTY_PrintRow,TTY_EffectiveBg());
        }
        break;
    }
    case '@':{
        int n2 = a0, c = TTY_PrintCol;
        if(c + n2 > (int)tty_cols())n2 = (int)tty_cols() - c;
        if(n2 > 0){
            tty_move_cells(TTY_PrintRow,c + n2,c,(int)tty_cols() - c - n2);
            tty_fill_cells((uint16_t)c,TTY_PrintRow,(uint16_t)(c + n2 - 1),TTY_PrintRow,TTY_EffectiveBg());
        }
        break;
    }
    case 'X':{
        int n2 = a0, c = TTY_PrintCol;
        if(c + n2 > (int)tty_cols())n2 = (int)tty_cols() - c;
        if(n2 > 0)tty_fill_cells((uint16_t)c,TTY_PrintRow,(uint16_t)(c + n2 - 1),TTY_PrintRow,TTY_EffectiveBg());
        break;
    }
    case 'r':{
        int top = (g_ansi_np > 0 && g_ansi_p[0] > 0) ? g_ansi_p[0] : 1;
        int bot = (g_ansi_np > 1 && g_ansi_p[1] > 0) ? g_ansi_p[1] : (int)tty_rows();
        if(top < 1)top = 1;
        if(bot > (int)tty_rows())bot = (int)tty_rows();
        if(top < bot){
            g_scroll_top = top - 1;
            g_scroll_bot = bot - 1;
        }
        ansi_cursor_set(0,0);
        break;
    }
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
        if(c == ';'){ ansi_param_push(); return true; }
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

/*DeepSeek-V4.1-Flash*/
#define TTY_CURSOR_W 1
#define TTY_CURSOR_H TTY_CELL_H//与字形同高
static uint16_t g_cur_x = 0;
static uint16_t g_cur_y = 0;
static _Bool    g_cur_on = false;
static uint32_t g_cur_bg = 0;

//擦掉光标: 用画它时那一格的背景色回填, 不留下脏点
static void tty_cursor_erase(void){
    if(!g_cur_on)return;
    if(!TTY_ScreenEnabled)return;
    fillRect(g_cur_x,g_cur_y,TTY_CURSOR_W,TTY_CURSOR_H,g_cur_bg);
    g_cur_on = false;
}

//在当前位置(TTY_PrintCol/TTY_PrintRow)画光标
static void tty_cursor_draw(uint32_t color,uint32_t bg){
    if(!TTY_ScreenEnabled)return;
    if(!g_cur_visible)return;
    g_cur_x = (uint16_t)(TTY_PrintCol * TTY_CELL_W);
    g_cur_y = (uint16_t)(TTY_PrintRow * TTY_CELL_H);
    g_cur_bg = bg;
    g_cur_on = true;
    fillRect(g_cur_x,g_cur_y,TTY_CURSOR_W,TTY_CURSOR_H,color);
}
/*DeepSeek-V4.1-Flash-END*/

static void tty_put_char(const char c,uint32_t color){
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
            TTY_PrintCol = (SYSTEM_ScreenInfo.Width >= TTY_CELL_W) ? (SYSTEM_ScreenInfo.Width / TTY_CELL_W - 1) : 0;
        }
        return;
    }
    if(TTY_PrintRow > (uint16_t)tty_bot_row())TTY_PrintRow = (uint16_t)tty_bot_row();
    if(c == '\n'){
        TTY_PrintCol = 0;
        if(TTY_PrintRow >= (uint16_t)tty_bot_row()){
            TTY_PrintRow = (uint16_t)tty_bot_row();
            tty_scroll_page(bg);
        }else{
            TTY_PrintRow += 1;
        }
        return;
    }
    //如果列超出屏幕宽度，自动换行
    if((uint64_t)TTY_PrintCol * TTY_CELL_W + TTY_CELL_W > SYSTEM_ScreenInfo.Width){
        TTY_PrintCol = 0;
        if(TTY_PrintRow >= (uint16_t)tty_bot_row()){
            TTY_PrintRow = (uint16_t)tty_bot_row();
            tty_scroll_page(bg);
        }else{
            TTY_PrintRow += 1;
        }
    }
    //如果字符为制表符
    if(c == '\t'){
        fillRect(TTY_PrintCol * TTY_CELL_W,TTY_PrintRow * TTY_CELL_H,TTY_CELL_W,TTY_CELL_H,bg);
        TTY_PrintCol ++;
        return;
    }
    //先整格填充背景色, 再绘制前景字形
    fillRect(TTY_PrintCol * TTY_CELL_W,TTY_PrintRow * TTY_CELL_H,TTY_CELL_W,TTY_CELL_H,bg);
    DrawChar(c,TTY_PrintCol * TTY_CELL_W,TTY_PrintRow * TTY_CELL_H,fg);
    TTY_PrintCol ++;//记录打印位置
}

void TTY_PrintChar(const char c,uint32_t color){
    SerialWriteChar(SERIAL_COM1,c);
    if(!TTY_ScreenEnabled)return;
    tty_cursor_erase();//光标马上要挪了, 先擦掉
    tty_put_char(c,color);
    tty_cursor_draw(TTY_EffectiveFg(color),TTY_EffectiveBg());
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
    if(!TTY_ScreenEnabled)return;
    tty_cursor_erase();
    tty_cursor_draw(TTY_EffectiveFg(CurrentConsoleStyle.TextColor),TTY_EffectiveBg());
}

void TTY_Clear(){
    if(!TTY_ScreenEnabled)return;
    fillRect(0,0,SYSTEM_ScreenInfo.Width,SYSTEM_ScreenInfo.Height,TTY_EffectiveBg());
    g_cur_on = false;
    tty_cursor_draw(TTY_EffectiveFg(CurrentConsoleStyle.TextColor),TTY_EffectiveBg());
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

//终端默认模式
#define TTY_TERM_DEFAULT_INIT { \
    .iflag = 0x6D02,/*BRKINT|ICRNL|IXON|IXANY|IMAXBEL|IUTF8*/ \
    .oflag = 0x0005,/*OPOST|ONLCR*/ \
    .cflag = 0x04BF,/*B38400|CS8|CREAD|HUPCL*/ \
    .lflag = TTY_ISIG|TTY_ICANON|TTY_ECHO|TTY_ECHOE|TTY_ECHOK|TTY_ECHOCTL|TTY_ECHOKE|TTY_IEXTEN, \
    .line = 0, \
    .cc = {3,28,127,21,4,0,1,0,17,19,26,0,18,15,23,22}, \
    .ispeed = 38400, \
    .ospeed = 38400, \
}

ktermios_t g_tty_term = TTY_TERM_DEFAULT_INIT;

static char g_line[TTY_LINE_MAX];//行编辑缓冲
static int  g_line_len = 0;
static volatile pid_t g_tty_fg_pid = 0;      //最后读本终端的任务(近似前台任务)
static volatile int   g_tty_intr_pending = 0;//键盘IRQ捕获的无读者Ctrl+C, 待定时器安全点投递
static volatile pid_t g_tty_term_owner = 0;  //最后一个改过终端模式的任务
static volatile int   g_tty_reader_waiting = 0;//是否有任务正阻塞在终端读

static inline void tty_echo(const char *s,int n);

//键盘IRQ调用
_Bool TTY_KeyInput(char c){
    if((g_tty_term.lflag & TTY_ISIG) && c == (char)g_tty_term.cc[0] && g_tty_fg_pid > 0 && !g_tty_reader_waiting){
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
    if(g_tty_term.lflag & TTY_ECHO)tty_echo("^C\n", 3);
    SignalSend(t, SIGINT, SI_KERNEL, 0, 0);
    TaskSignalDescendants(t, SIGINT);
}

static inline void tty_echo(const char *s, int n){
    for(int i = 0; i < n; i++)TTY_PrintChar(s[i], CurrentConsoleStyle.TextColor);
}

//按ECHOCTL规则回显
static void tty_echo_ctl(const char *s,int n){
    for(int i = 0; i < n; i++){
        uint8_t ch = (uint8_t)s[i];
        if(ch == 0x7F){
            tty_echo("^?", 2);
        }else if(ch < 0x20){
            char e[2];
            e[0] = '^';
            e[1] = (char)(ch + 0x40);
            tty_echo(e, 2);
        }else{
            tty_echo(&s[i], 1);
        }
    }
}

//回显一个输入字符
static inline void tty_echo_char(char c){
    if(!(g_tty_term.lflag & TTY_ECHO))return;
    if(g_tty_term.lflag & TTY_ECHOCTL)tty_echo_ctl(&c, 1);
    else tty_echo(&c, 1);
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

//这个字符回显后占几列
static int tty_echo_width(uint8_t c){
    if((g_tty_term.lflag & TTY_ECHOCTL) && (c < 0x20 || c == 0x7F))return 2;
    return 1;
}

//退掉行缓冲里最后一个字符
static void tty_erase_last(void){
    if(g_line_len <= 0)return;
    g_line_len--;
    int w = tty_echo_width((uint8_t)g_line[g_line_len]);
    if(g_tty_term.lflag & TTY_ECHO){
        for(int i = 0; i < w; i++)tty_echo("\b \b", 3);
    }
}

static long tty_term_read(char *kbuf, long count);//先声明: 外层包装要用

//行规程读取
long TTY_TermRead(char *kbuf, long count){
    g_tty_reader_waiting = 1;
    long r = tty_term_read(kbuf, count);
    g_tty_reader_waiting = 0;
    return r;
}

//行规程读取主体
static long tty_term_read(char *kbuf, long count){
    int ccount = (int)(count < TTY_LINE_MAX ? count : TTY_LINE_MAX);
    if(ccount <= 0)return 0;
    g_tty_fg_pid = current_task ? current_task->pid : 0;//记住前台任务
    //非规范模式: 单键直返
    if(!(g_tty_term.lflag & TTY_ICANON)){
        char c;
        do{
            c = GetKey();
        //Ctrl+Z(VSUSP)/Ctrl+\(VQUIT)不能当输入字符交给程序
        }while((g_tty_term.lflag & TTY_ISIG) && (c == (char)g_tty_term.cc[10] || c == (char)g_tty_term.cc[1]));
        tty_echo_char(c);
        kbuf[0] = c;
        return 1;
    }
    int ready = tty_line_take(kbuf, ccount);
    if(ready > 0)return ready;
    for(;;){
        char c = GetKey();
        if(c == '\r' && (g_tty_term.iflag & 0x0100))c = '\n';
        if((uint8_t)c == 0x1B){
            char seq[24];
            int n = 0;
            seq[n++] = (char)0x1B;
            char e1 = 0;
            for(int i = 0; i < 20 && e1 == 0; i++){
                e1 = GetKey_NoBlock();
                if(!e1)udelay(1000);
            }
            if(e1 == '[' || e1 == 'O'){
                seq[n++] = e1;
                for(;;){
                    char e2 = 0;
                    for(int i = 0; i < 20 && e2 == 0; i++){
                        e2 = GetKey_NoBlock();
                        if(!e2)udelay(1000);
                    }
                    if(e2 == 0)break;
                    if(n < (int)sizeof(seq) - 1)seq[n++] = e2;
                    if(e2 >= 0x40 && e2 <= 0x7E)break;
                    if(e2 < 0x20 || e2 > 0x3F)break;
                }
            }else if(e1 != 0){
                seq[n++] = e1;
            }
            //字节照样进输入缓冲
            for(int i = 0; i < n; i++){
                if(g_line_len < TTY_LINE_MAX - 1)g_line[g_line_len++] = seq[i];
            }
            if(g_tty_term.lflag & TTY_ECHO)tty_echo_ctl(seq,n);
            continue;
        }
        //Ctrl+C(VINTR)
        if((g_tty_term.lflag & TTY_ISIG) && c == (char)g_tty_term.cc[0]){
            g_tty_intr_pending = 0;//本路径自行处理, 避免定时器重复投递
            if(g_tty_term.lflag & TTY_ECHO)tty_echo("^C", 2);
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
            tty_erase_last();
            continue;
        }
        //Ctrl+U(VKILL): 清行
        if(c == (char)g_tty_term.cc[3]){
            while(g_line_len > 0)tty_erase_last();
            continue;
        }
        //Ctrl+Z(VSUSP)/Ctrl+\(VQUIT)
        if((g_tty_term.lflag & TTY_ISIG) && (c == (char)g_tty_term.cc[10] || c == (char)g_tty_term.cc[1])){
            if(g_tty_term.lflag & TTY_ECHO)tty_echo(c == (char)g_tty_term.cc[10] ? "^Z" : "^\\", 2);
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
        tty_echo_char(c);
        continue;
flush:
        {
            return tty_line_take(kbuf, ccount);
        }
    }
}

//导出为内核 struct termios(TCGETS): 36字节, 只有 iflag..lflag + line + cc[19]
void TTY_TermExportLegacy(void *dst){
    tty_termios_legacy_t *u = (tty_termios_legacy_t*)dst;
    memset(u, 0, sizeof(*u));
    u->iflag = g_tty_term.iflag;
    u->oflag = g_tty_term.oflag;
    u->cflag = g_tty_term.cflag;
    u->lflag = g_tty_term.lflag;
    u->line  = g_tty_term.line;
    memcpy(u->cc, g_tty_term.cc, 19);//NCCS=19
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
    g_tty_term_owner = current_task ? current_task->pid : 0;//记下是谁改的
}

void TTY_TermRestoreOnExit(pid_t pid){
    if(pid <= 0 || pid != g_tty_term_owner)return;
    g_tty_term = (ktermios_t)TTY_TERM_DEFAULT_INIT;
    g_tty_term_owner = 0;
    g_line_len = 0;//丢掉它可能留在行缓冲里的半行
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