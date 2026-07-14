#include <klib.h>
#include <desc.h>
#include <drives/display.h>
#include <drives/tty.h>

uint64_t SYSTEM_CPU_Fquency = 0;

_Bool LoadBootParam(BootParam* boot_param);//加载引导参数

__attribute__((optimize("-O0")))
void KernelStart(BootParam* boot_param){
    setup_gdt();
    if(!LoadBootParam(boot_param))while(1);
    InitSerial(SERIAL_COM1);
    TTY_Clear();
    TTY_Print("Hello World\n",COLOR_WHITE);
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
