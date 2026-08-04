#include <klib.h>
#include <desc.h>
#include <idt.h>
#include <irq.h>
#include <drives/display.h>
#include <drives/tty.h>
#include <drives/ps2kbd.h>
#include <efi.h>
#include <print.h>
#include <mm/pmm.h>
#include <delay.h>
#include <rtc.h>
#include <acpi/acpi.h>
#include <acpi/power.h>
#include <task.h>
#include <spinlock.h>

BootParam *SYSTEM_BootParam = NULL;
uint64_t SYSTEM_CPU_Fquency = 0;
UEFI_MEMORY_MAP *SYSTEM_MemoryMap = NULL;
EFI_RUNTIME_SERVICES *UEFI_RuntimeServices = NULL;
_Bool UEFI_UseRT = false;

extern char __bss_start[], __bss_end[];

_Bool LoadBootParam(BootParam* boot_param);//加载引导参数
int InitSystem();//初始化化系统
void test_thread();
void condvar_test_thread();
void condvar_waiter_thread();
spinlock_t lock_test;

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
    TaskInit();
    InitPrintk();
    KeyboardInit();
    sti();
    rtc_time_t time;
    rtc_get_local(&time);
    printk("Time: %d/%d/%d %d:%d:%d %s\n",time.year,time.month,time.day,time.hour,time.minute,time.second,weekdays[time.wday]);
    printk("ACPI: RSDP=%p XSDT=%p FADT=%p MADT=%p\n",SYSTEM_ACPI.rsdp,SYSTEM_ACPI.xsdt,SYSTEM_ACPI.fadt,SYSTEM_ACPI.madt);
    CreateKernelThread(test_thread,4096,"test");
    task_struct* list[10];
    int count;
    count = TaskGetAll(list,10);
    for(int i = 0;i < count;i ++){
        printk("PID:%d  Stack:%p  Name:%s\n",list[i]->pid,list[i]->kernel_stack,list[i]->name);
    }
    msleep(5000);
    printk(PRINTK_INFO"Type 'r' to reboot or type 's' to shutdown.");
    char key = GetKey();
    if(key == 's')SYSTEM_Shutdown();
    else SYSTEM_Restart();
    SYSTEM_STOP();
}

void test_thread(){
    while(1){
        fillRect(400,400,10,10,COLOR_RED);
        msleep(1000);
        fillRect(400,400,10,10,COLOR_GREEN);
        msleep(1000);
        fillRect(400,400,10,10,COLOR_BLUE);
        msleep(1000);
    }
}

_Bool LoadBootParam(BootParam* boot_param){
    if(!boot_param)return false;
    SYSTEM_BootParam = boot_param;
    //屏幕信息
    SYSTEM_ScreenInfo.Width = boot_param->ScreenInfo.Width;
    SYSTEM_ScreenInfo.Height = boot_param->ScreenInfo.Hieght;
    SYSTEM_ScreenInfo.FrameBufferBase = (void*)boot_param->ScreenInfo.FrameBuffer;
    SYSTEM_ScreenInfo.FrameBufferSize = boot_param->ScreenInfo.FrameBuffer_Size;
    SYSTEM_FrameBuffer = (uint32_t*)SYSTEM_ScreenInfo.FrameBufferBase;
    //屏幕日志开关
    TTY_ScreenEnabled = boot_param->PrintLog;
    //内存映射
    SYSTEM_MemoryMap = &boot_param->MemoryInfo;
    //ACPI
    SYSTEM_ACPI.rsdp = (struct acpi_table_rsdp*)boot_param->RSDP;
    //运行时服务
    if(boot_param){
        UEFI_RuntimeServices = boot_param->RuntimeServices;
        UEFI_UseRT = true;
    }else{
        UEFI_RuntimeServices = NULL;
        UEFI_UseRT = false;
    }
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
