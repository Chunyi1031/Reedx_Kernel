#include <klib.h>
#include <desc.h>
#include <idt.h>
#include <irq.h>
#include <efi.h>
#include <print.h>
#include <mm/pmm.h>
#include <mm/pgtables.h>
#include <mm/vmm.h>
#include <delay.h>
#include <rtc.h>
#include <acpi/acpi.h>
#include <acpi/power.h>
#include <task.h>
#include <spinlock.h>
#include <syscalls.h>
#include <drives/display.h>
#include <drives/tty.h>
#include <drives/ps2kbd.h>
#include <drives/drive_path.h>
#include <drives/disk.h>
#include <fs.h>

BootParam *SYSTEM_BootParam = NULL;
uint64_t SYSTEM_CPU_Fquency = 0;
UEFI_MEMORY_MAP *SYSTEM_MemoryMap = NULL;
EFI_RUNTIME_SERVICES *UEFI_RuntimeServices = NULL;
_Bool UEFI_UseRT = false;
utsname_t system_utsname = {0};

extern char __bss_start[], __bss_end[];

_Bool LoadBootParam(BootParam* boot_param);//加载引导参数
int InitSystem();//初始化化系统
void KernelMain();//内核主函数
_Bool InitDiskAndFs();//初始化磁盘及文件系统
spinlock_t lock_test;

//内核入口
void KernelStart(BootParam* boot_param){
    memset(__bss_start, 0, __bss_end - __bss_start);
    setup_gdt();
    if(!LoadBootParam(boot_param))SYSTEM_STOP();
    int status = InitSystem();
    if(status != 0){
        print_error();
        early_printk("Kernel init failed:%d\n",status);
        SYSTEM_STOP();
    }
    //跳转到高地址
    void (*entry)(void) = (void*)PHYS_TO_VIRT((uintptr_t)KernelMain);
    entry();
    SYSTEM_STOP();
}

//用户态测试程序
extern char msg_filedata[];
extern char msg_exec_path0[],msg_exec_path1[],msg_exec_arg0[];
__attribute__((noinline, section(".text.user")))
static void user_main(void) {
    uintptr_t eargv[2];
    eargv[0] = (uintptr_t)msg_exec_arg0;
    eargv[1] = 0;
    syscall(SYS_EXECVE,(uintptr_t)msg_exec_path0,(uintptr_t)eargv,0);
    syscall(SYS_EXECVE,(uintptr_t)msg_exec_path1,(uintptr_t)eargv,0);
    syscall(SYS_EXIT,-1,0,0);
}
__asm__(
    ".pushsection .text.user, \"ax\", @progbits\n"
    "msg_exec_path0: .asciz \"/sbin/init\"\n"
    "msg_exec_path1: .asciz \"/bin/init\"\n"
    "msg_exec_arg0: .asciz \"init\"\n"
    ".popsection\n"
    "msg_filedata: .ascii \"user file data!\\n\"\n"
);
//结束标记函数
__attribute__((naked, noinline, section(".text.user")))
static void user_main_end(void) {
    __asm__ volatile("ud2\n\t");
}

void fill_utsname(){
    strcpy(system_utsname.sysname,"Reedx");
    strcpy(system_utsname.nodename,"(none)");
    strcpy(system_utsname.release,"0.0.1-beta");
    strcpy(system_utsname.version,"(unknown)");
    strcpy(system_utsname.machine,"x86_64");
    strcpy(system_utsname.domainname,"(none)");
}

void KernelMain(){
    setup_gdt();
    setup_tss();
    setup_idt();
    setup_exceptions();
    tsc_calibrate();
    rtc_init();
    InitAPIC();
    TaskInit();
    InitPrintk();
    KeyboardInit();
    InitSyscall();
    switch_kernel_info_to_high();
    switch_kernel_stack_to_high();
    fill_utsname();
    sti();
    rtc_time_t time;
    rtc_get_local(&time);
    if(!InitDiskAndFs())printk(PRINTK_WARNING"Disk init failed!");
    mm_struct* umm = vmm_create_address_space();
    if(!umm)SYSTEM_STOP();
    if(vmm_mmap(umm, 0x400000, PAGE_SIZE, VM_READ | VM_EXEC | VM_WRITE))SYSTEM_STOP();
    if(vmm_mmap(umm, umm->start_stack, PAGE_SIZE, VM_READ | VM_WRITE))SYSTEM_STOP();
    uintptr_t *code_pte = (uintptr_t*)get_pte((uintptr_t)umm->pgd, 0x400000, 0, 0);
    if(!code_pte || !pte_is_present(*code_pte))SYSTEM_STOP();
    memcpy((void*)PHYS_TO_VIRT(pte_get_paddr(*code_pte)), (void*)user_main,
           (uintptr_t)user_main_end - (uintptr_t)user_main);
    init_task = CreateProcess(0x400000, umm, "UserTask");//创建首个用户程序
    if(!init_task)panic("Cannot create init task");
    SYSTEM_STOP();
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
        UEFI_RuntimeServices = (EFI_RUNTIME_SERVICES*)PHYS_TO_VIRT(boot_param->RuntimeServices);
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
    UEFI_PML4 = get_cr3();
    KERNEL_PML4 = UEFI_PML4;
    if(Init_Physical_Memory_Manager() != 0)return 1;
    if (InitKernelPageTable() != 0) {
        print_error();
        early_printk("Kernel page table init failed\n");
        return 1;
    }
    print_ok();
    early_printk("Kernel page table ready\n");
    InitKernelMapping();
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
