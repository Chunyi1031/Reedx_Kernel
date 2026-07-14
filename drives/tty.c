#include <drives/tty.h>

uint16_t TTY_PrintCol = 0;
uint16_t TTY_PrintRow = 0;
ConsoleStyle CurrentConsoleStyle = {COLOR_WHITE,COLOR_BLACK};

void TTY_PrintChar(const char c,uint32_t color){
    SerialWriteChar(SERIAL_COM1,c);
    uint16_t spl = TTY_PrintRow;
    uint16_t spr = TTY_PrintCol;
    //如果过界
    if(TTY_PrintRow > (SYSTEM_ScreenInfo.Height / 16)){
        TTY_PrintRow = 0;
    }
    //如果字符为换行符
    if(c == '\n'){
        TTY_PrintRow += 1;
        TTY_PrintCol = 0;
        return;
    }
    //如果字符为制表符
    if(c == '\t'){
        DrawChar(' ',TTY_PrintCol * 10,TTY_PrintRow * 18,color);
        TTY_PrintCol ++;
        return;
    }
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
    fillRect(0,0,SYSTEM_ScreenInfo.Width,SYSTEM_ScreenInfo.Height,CurrentConsoleStyle.BgColor);
}
