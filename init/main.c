#include <klib.h>
#include <desc.h>
#include <idt.h>
#include <irq.h>
#include <drives/display.h>
#include <drives/tty.h>
#include <drives/ps2kbd.h>
#include <print.h>
#include <mm/pmm.h>
#include <delay.h>
#include <rtc.h>
#include <acpi/acpi.h>
#include <acpi/power.h>

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
    KeyboardInit();
    sti();
    mdelay(100);
    early_printk("Ticks: %u\n", (uint32_t)SYSTEM_TimerTicks);
    rtc_time_t time;
    rtc_get_local(&time);
    early_printk("Time: %d/%d/%d %d:%d:%d %s\n",time.year,time.month,time.day,time.hour,time.minute,time.second,weekdays[time.wday]);
    early_printk("ACPI: RSDP=%p XSDT=%p FADT=%p MADT=%p\n",SYSTEM_ACPI.rsdp,SYSTEM_ACPI.xsdt,SYSTEM_ACPI.fadt,SYSTEM_ACPI.madt);
    early_printk("reset:%x,%p\n",SYSTEM_ACPI.fadt->reset_register.space_id,SYSTEM_ACPI.fadt->reset_register.address);
    mdelay(10000);
    early_printk("Type 'r' to reboot or type 's' to shutdown.\n");
    char key = GetKey();
    if(key == 'r')SYSTEM_Restart();
    if(key == 's')SYSTEM_Shutdown();
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
    //ACPI
    SYSTEM_ACPI.rsdp = (struct acpi_table_rsdp*)boot_param->RSDP;
    return true;
}

int InitSystem(){
    InitSerial(SERIAL_COM1);
    TTY_Clear();
    if(Init_Physical_Memory_Manager() != 0)return 1;
    int Status = InitACPI(SYSTEM_ACPI.rsdp);
    if(Status != 0){
        print_error();
        early_printk("ACPI init failed:%d\n",Status);
        return Status;
    }
    print_ok();
    early_printk("ACPI init success\n");
    return 0;
}
