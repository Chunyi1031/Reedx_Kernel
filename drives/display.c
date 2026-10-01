#include <drives/display.h>
#include <font.h>
#include <mm/pgtables.h>
#include <mm/pmm.h>
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

//绘制参数缓存
static int g_fb_ok = 0;
static uint32_t *g_fb = NULL;
static int g_fb_w = 0, g_fb_h = 0;
static uint32_t *g_back = NULL;

//8字节快速搬移
static void fb_fast_copy(void *dst,const void *src,size_t bytes){
    uint8_t *d = dst;
    const uint8_t *s = src;
    size_t n = bytes;
    __asm__ volatile(
        "cmpq $8, %[n]\n\t"
        "jb .Lfc_tail%=\n\t"
        ".Lfc_loop%=:\n\t"
        "movq (%[s]), %%rax\n\t"
        "movq %%rax, (%[d])\n\t"
        "addq $8, %[s]\n\t"
        "addq $8, %[d]\n\t"
        "subq $8, %[n]\n\t"
        "cmpq $8, %[n]\n\t"
        "jae .Lfc_loop%=\n\t"
        ".Lfc_tail%=:\n"
        : [d] "+r"(d), [s] "+r"(s), [n] "+r"(n)
        :
        : "rax", "cc", "memory");
    while (n--)*d++ = *s++;
}

//申请影子缓冲
static void fb_back_alloc(void){
    if(g_back || !g_fb_ok || !kernel_high_ready)return;//高半区就绪后才能用PMM
    uint64_t bytes = (uint64_t)g_fb_w * g_fb_h * 4;
    if(!bytes || bytes > (64ULL << 20))return;//实在太大就不做影子了
    void *pa = Pmm_Malloc((int)((bytes + PAGE_SIZE - 1) / PAGE_SIZE));
    if(!pa)return;
    g_back = (uint32_t*)PHYS_TO_VIRT((uintptr_t)pa);
    fb_fast_copy(g_back,g_fb,bytes);//把当前画面抄一份, 保持内容一致
}

//刷新绘制参数
static void fb_refresh(void){
    g_fb   = SYSTEM_FrameBuffer;
    g_fb_w = (int)SYSTEM_ScreenInfo.Width;
    g_fb_h = (int)SYSTEM_ScreenInfo.Height;
    uint64_t pixels = SYSTEM_ScreenInfo.FrameBufferSize / 4;
    g_fb_ok = (g_fb && g_fb_w > 0 && g_fb_h > 0 && pixels >= (uint64_t)g_fb_w * g_fb_h && ScreenFbMapped()) ? 1 : 0;
    if(g_fb_ok)fb_back_alloc();
}

//把影子缓冲里的一块矩形写回显存(没有影子缓冲时是空操作)
static void fb_flush_rect(int x0,int y0,int x1,int y1){
    if(!g_back)return;
    if(x0 < 0)x0 = 0;
    if(y0 < 0)y0 = 0;
    if(x1 > g_fb_w)x1 = g_fb_w;
    if(y1 > g_fb_h)y1 = g_fb_h;
    if(x0 >= x1 || y0 >= y1)return;
    uint32_t *d = g_fb   + (size_t)y0 * g_fb_w + x0;
    uint32_t *s = g_back + (size_t)y0 * g_fb_w + x0;
    if(x0 == 0 && x1 == g_fb_w){
        fb_fast_copy(d,s,(size_t)(y1 - y0) * g_fb_w * 4);
        return;
    }
    size_t n = (size_t)(x1 - x0) * 4;
    for(int y = y0; y < y1; y++){
        fb_fast_copy(d,s,n);
        d += g_fb_w;
        s += g_fb_w;
    }
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
    if(!g_fb_ok || g_fb != SYSTEM_FrameBuffer)fb_refresh();
    if(!g_fb_ok)return;
    //范围检查
    if(x >= g_fb_w || y >= g_fb_h)return;
    uint64_t idx = (uint64_t)y * g_fb_w + x;
    if(g_back)g_back[idx] = color;//影子缓冲
    g_fb[idx] = color;//显存
}

//填充矩形
void fillRect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint32_t color){
    if(!g_fb_ok || g_fb != SYSTEM_FrameBuffer)fb_refresh();
    if(!g_fb_ok || !w || !h)return;
    int x0 = x, x1 = (int)x + w;
    int y0 = y, y1 = (int)y + h;
    if(x0 > g_fb_w)x0 = g_fb_w;
    if(x1 > g_fb_w)x1 = g_fb_w;
    if(y0 > g_fb_h)y0 = g_fb_h;
    if(y1 > g_fb_h)y1 = g_fb_h;
    uint32_t *t = g_back ? g_back : g_fb;//有影子就画在内存里
    for(int yy = y0; yy < y1; yy++){
        uint32_t *p = t + (uint64_t)yy * g_fb_w + x0;
        for(int n = x1 - x0; n > 0; n--)*p++ = color;
    }
    fb_flush_rect(x0,y0,x1,y1);
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
    if(!g_fb_ok || g_fb != SYSTEM_FrameBuffer)fb_refresh();
    if(!g_fb_ok)return;
    uint32_t *t = g_back ? g_back : g_fb;//有影子就画在内存里
    for(int row = 0; row < 16; row++){
        int yy = y + row;
        if(yy < 0 || yy >= g_fb_h)continue;
        uint16_t row_data = (uint16_t)((font[data_offset + row*2] << 8) | font[data_offset + row*2 + 1]);
        if(!row_data)continue;//空行直接跳过
        uint32_t *line = t + (uint64_t)yy * g_fb_w;
        for(int col = 0; col < 10; col++){
            int xx = x + col;
            if(xx < 0 || xx >= g_fb_w)continue;
            if(row_data & (0x8000 >> col))line[xx] = color;
        }
    }
    fb_flush_rect(x,y,x + 10,y + 16);//整格写回显存
}

//把[y0,y1+dy)整体上移dy行, 其中最后的[y1,y1+dy)(也就是最下面dy行)填bg色
void FbScrollUp(int y0,int y1,int dy,uint32_t bg){
    if(!g_fb_ok || g_fb != SYSTEM_FrameBuffer)fb_refresh();
    if(!g_fb_ok || dy <= 0)return;
    if(y0 < 0)y0 = 0;
    if(y1 > g_fb_h)y1 = g_fb_h;
    int ye = y1 + dy;
    if(ye > g_fb_h)ye = g_fb_h;
    if(y0 + dy >= ye)return;
    uint32_t *t = g_back ? g_back : g_fb;//有影子就在内存里搬(不读显存!)
    fb_fast_copy(t + (size_t)y0 * g_fb_w,t + (size_t)(y0 + dy) * g_fb_w, (size_t)(ye - y0 - dy) * g_fb_w * 4);
    for(int y = ye - dy; y < ye; y++){
        uint32_t *p = t + (size_t)y * g_fb_w;
        for(int x = 0; x < g_fb_w; x++)p[x] = bg;
    }
    fb_flush_rect(0,y0,g_fb_w,ye);//一次性顺序写回显存
}

//读一个像素(优先从影子缓冲读)
uint32_t FbReadPixel(int x,int y){
    if(!g_fb_ok || g_fb != SYSTEM_FrameBuffer)fb_refresh();
    if(!g_fb_ok)return 0;
    if(x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h)return 0;
    return g_back ? g_back[(uint64_t)y * g_fb_w + x] : g_fb[(uint64_t)y * g_fb_w + x];
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
