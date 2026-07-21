#include <klib.h>
#include <desc.h>
#include <idt.h>
#include <irq.h>
#include <drives/display.h>
#include <drives/tty.h>
#include <print.h>
#include <mm/pmm.h>
#include <delay.h>
#include <rtc.h>

BootParam *SYSTEM_BootParam = NULL;
uint64_t SYSTEM_CPU_Fquency = 0;
UEFI_MEMORY_MAP *SYSTEM_MemoryMap = NULL;

extern char __bss_start[], __bss_end[];

_Bool LoadBootParam(BootParam* boot_param);//加载引导参数
int InitSystem();//初始化化系统

void KernelStart(BootParam* boot_param){
    memset(__bss_start, 0, __bss_end - __bss_start);
    setup_gdt();
    setup_idt();
    if(!LoadBootParam(boot_param))SYSTEM_STOP();
    int status = InitSystem();
    if(status != 0){
        print_error();
        early_printk("Kernel init failed:%d\n",status);
        SYSTEM_STOP();
    }
    setup_exceptions();
    tsc_calibrate();
    rtc_init();
    InitAPIC();
    sti();
    mdelay(1000);
    early_printk("Ticks: %u\n", (uint32_t)SYSTEM_TimerTicks);
    rtc_time_t time;
    rtc_get_local(&time);
    early_printk("%d/%d/%d %d:%d:%d %s\n",time.year,time.month,time.day,time.hour,time.minute,time.second,weekdays[time.wday]);
    SYSTEM_STOP();
}

_Bool LoadBootParam(BootParam* boot_param){
    if(!boot_param)return false;
    SYSTEM_BootParam = boot_param;
    //屏幕数据
    SYSTEM_ScreenInfo = boot_param->screen_info;
    SYSTEM_FrameBuffer = (uint32_t*)SYSTEM_ScreenInfo.FrameBufferBase;
    kfont_data = (uint8_t*)boot_param->Font_Buffer;
    //内存映射
    SYSTEM_MemoryMap = &boot_param->memory_info;
    SYSTEM_CPU_Fquency = 2000000000;//CPU参考频率
    return true;
}

int InitSystem(){
    InitSerial(SERIAL_COM1);
    TTY_Clear();
    if(Init_Physical_Memory_Manager() != 0)return false;
    return 0;
}
