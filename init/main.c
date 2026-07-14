#include <klib.h>
#include <desc.h>
#include <drives/display.h>
#include <drives/tty.h>
#include <print.h>
#include <mm/bitmap.h>

uint64_t SYSTEM_CPU_Fquency = 0;

_Bool LoadBootParam(BootParam* boot_param);//加载引导参数
int InitSystem();//初始化化系统

__attribute__((optimize("-O0")))
void KernelStart(BootParam* boot_param){
    setup_gdt();
    if(!LoadBootParam(boot_param))SYSTEM_STOP();
    int status = InitSystem();
    if(status != 0){
        print_ok();
        early_printk("Kernel init failed:%d\n",status);
        SYSTEM_STOP();
    }
    bitmap_t bitmap;
    uint8_t bits[2];
    BitmapInit(&bitmap,bits,16,false);
    early_printk("%d\n",BitmapGetBit(&bitmap,8));
    BitmapSetBits(&bitmap,8,1,true);
    early_printk("%d %d %d\n",BitmapGetBit(&bitmap,7),BitmapGetBit(&bitmap,8),BitmapGetBit(&bitmap,9));
    SYSTEM_STOP();
}

_Bool LoadBootParam(BootParam* boot_param){
    if(!boot_param)return false;
    //屏幕数据
    SYSTEM_ScreenInfo = boot_param->screen_info;
    SYSTEM_FrameBuffer = (uint32_t*)SYSTEM_ScreenInfo.FrameBufferBase;
    kfont_data = (uint8_t*)boot_param->Font_Buffer;
    SYSTEM_CPU_Fquency = 2000000000;//CPU参考频率
    return true;
}

int InitSystem(){
    InitSerial(SERIAL_COM1);
    TTY_Clear();
    return 0;
}
