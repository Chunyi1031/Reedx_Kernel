#include <drives/display.h>
#include <font.h>
#include <mm/pgtables.h>
#include <mm/vmm.h>

ScreenInfo SYSTEM_ScreenInfo;
uint32_t*  SYSTEM_FrameBuffer = NULL;

int ScreenFbMappedAt(uintptr_t fb){
    if(!fb)return 0;
    uintptr_t cr3 = get_cr3() & ~0xFFFULL;
    int use_high = kernel_high_ready;
#define TBL_PTR(p) (use_high ? (uint64_t*)PHYS_TO_VIRT(p) : (uint64_t*)(uintptr_t)(p))
    uint64_t e = TBL_PTR(cr3)[PML4_INDEX(fb)];
    if(!(e & PTE_PRESENT))return 0;
    uint64_t *next = TBL_PTR(e & 0x000FFFFFFFFFF000ULL);
    e = next[PDPT_INDEX(fb)];
    if(!(e & PTE_PRESENT))return 0;
    if(e & PTE_HUGE)return 1;//1GB大页
    next = TBL_PTR(e & 0x000FFFFFFFFFF000ULL);
    e = next[PD_INDEX(fb)];
    if(!(e & PTE_PRESENT))return 0;
    if(e & PTE_HUGE)return 1;//2MB大页
    next = TBL_PTR(e & 0x000FFFFFFFFFF000ULL);
    e = next[PT_INDEX(fb)];
    return (e & PTE_PRESENT) ? 1 : 0;
#undef TBL_PTR
}

int ScreenFbMapped(void){
    return ScreenFbMappedAt((uintptr_t)SYSTEM_FrameBuffer);
}

//字体列表
static const char fontlist[94] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z',
    '0','1','2','3','4','5','6','7','8','9',
    '`','~','.',',','/','\\',';','\'',':','"','<','>','(',')','[',']','{','}','#','$','%','&','*',' ',
    '!','|','@','-','+','=','_','?'
};

__attribute__((optimize("-O0")))
//rgb转16进制
uint32_t rgb(uint8_t red, uint8_t green, uint8_t blue){
    return ((uint32_t)255 << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
}

//画点
void DrawPiexl(uint16_t x,uint16_t y,uint32_t color){
    if(!ScreenFbMapped())return;
    int index = y * SYSTEM_ScreenInfo.Width + x;
    if(index < SYSTEM_ScreenInfo.FrameBufferSize && index >= 0){
        SYSTEM_FrameBuffer[index] = color;
    }
}

//填充矩形
void fillRect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint32_t color){
    for(int _y = y; _y < (h + y); _y ++){
        for(int _x = x; _x < (w + x); _x ++){
            DrawPiexl(_x,_y,color);
        }
    }
}

//字符（串）处理
//显示字符 —— 使用内置字体（include/font.h），不再依赖引导程序加载
void DrawChar(char c,int x,int y,uint32_t color){
    int char_index = 84;
    for(int i = 0;i < 94;i ++){
        if(fontlist[i] == c){
            char_index = i;
            break;
        }
    }
    int data_offset = char_index * 32;
    for (int row = 0; row < 16; row++) {
        uint16_t row_data = (font[data_offset + row*2] << 8) | font[data_offset + row*2 + 1];
        for (int col = 0; col < 10; col++) {
            if ((row_data & (0x8000 >> col)) != 0) {
                DrawPiexl(x + col, y + row, color);
            }
        }
    }
}
//显示字符串
void DrawString(char *s,int x, int y, uint32_t color) {
    int startX = x;//记录起始X坐标
    int currentX = x;
    int currentY = y;
    int charWidth = 10;//宽
    int charHeight = 16;//高
    int lineSpacing = 2;//行间距
    size_t size = strlen(s);
    for (int i = 0; i < size; i++) {
        char c = s[i];
        //处理换行符
        if (c == '\n') {
            currentX = startX;//X坐标回到起始位置
            currentY += charHeight + lineSpacing;//Y坐标下移一行
            continue;
        }
        if (currentX + charWidth > SYSTEM_ScreenInfo.Width) {
            currentX = startX;//X坐标回到起始位置
            currentY += charHeight + lineSpacing;//Y坐标下移一行
        }
        DrawChar(c, currentX, currentY, color);//绘制字符
        currentX += charWidth;//更新X坐标，准备绘制下一个字符
    }
}
