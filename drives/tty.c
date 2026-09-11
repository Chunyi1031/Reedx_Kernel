#include <drives/tty.h>

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
