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
#include <mm/pgtables.h>
#include <mm/vmm.h>
#include <delay.h>
#include <rtc.h>
#include <acpi/acpi.h>
#include <acpi/power.h>
#include <task.h>
#include <spinlock.h>
#include <syscalls.h>

BootParam *SYSTEM_BootParam = NULL;
uint64_t SYSTEM_CPU_Fquency = 0;
UEFI_MEMORY_MAP *SYSTEM_MemoryMap = NULL;
EFI_RUNTIME_SERVICES *UEFI_RuntimeServices = NULL;
_Bool UEFI_UseRT = false;

extern char __bss_start[], __bss_end[];

_Bool LoadBootParam(BootParam* boot_param);//加载引导参数
int InitSystem();//初始化化系统
void KernelMain();//内核主函数
void test_thread();
spinlock_t lock_test;

__attribute__((noreturn)) static void enter_user_mode(uintptr_t entry, uintptr_t stack_top, uintptr_t pgd);

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
__attribute__((naked, noinline, section(".text.user")))
static void user_main(void) {
    __asm__ volatile(
        "movq $1, %rax\n\t"                    //write(1, "Hello", 5)
        "movq $1, %rdi\n\t"
        "leaq 6f(%rip), %rsi\n\t"
        "movq $5, %rdx\n\t"
        "syscall\n\t"
        "jmp 7f\n\t"                            //跳过字符串数据
        "6:\n\t"
        ".ascii \"Hello\"\n\t"
        "7:\n\t"
        "jmp 7b\n\t"                            //死循环
    );
}

//结束标记函数：与 user_main 同节相邻，用于计算用户代码长度
__attribute__((naked, noinline, section(".text.user")))
static void user_main_end(void) {
    __asm__ volatile("ud2\n\t");
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
    sti();
    rtc_time_t time;
    rtc_get_local(&time);
    printk(PRINTK_INFO"UEFI PML4 at 0x%llx,Kernel PML4 at 0x%llx",UEFI_PML4,KERNEL_PML4);
    // ==== 用户态测试：跳到 ring3 ====
    mm_struct* umm = vmm_create_address_space();
    if(!umm)SYSTEM_STOP();
    if(!vmm_mmap(umm, 0x400000, PAGE_SIZE, VM_READ | VM_EXEC | VM_WRITE))SYSTEM_STOP();
    if(!vmm_mmap(umm, umm->start_stack, PAGE_SIZE, VM_READ | VM_WRITE))SYSTEM_STOP();
    uintptr_t *code_pte = (uintptr_t*)get_pte((uintptr_t)umm->pgd, 0x400000, 0, 0);
    if(!code_pte || !pte_is_present(*code_pte))SYSTEM_STOP();
    memcpy((void*)PHYS_TO_VIRT(pte_get_paddr(*code_pte)), (void*)user_main,
           (uintptr_t)user_main_end - (uintptr_t)user_main);
    early_printk("Entering user mode...\n");
    enter_user_mode(0x400000, umm->start_stack + PAGE_SIZE, (uintptr_t)umm->pgd);
    msleep(5000);
    printk(PRINTK_INFO"Type 'r' to reboot or type 's' to shutdown.");
    char key = GetKey();
    if(key == 's')SYSTEM_Shutdown();
    else SYSTEM_Restart();
}

__attribute__((noreturn))
static void enter_user_mode(uintptr_t entry, uintptr_t stack_top, uintptr_t pgd) {
    __asm__ volatile(
        "cli\n\t"
        "pushq %3\n\t"      //ss = __USER_DS
        "pushq %1\n\t"      //rsp = 用户栈顶
        "pushq $0x2\n\t"    //rflags = 0x2 (IF=0)
        "pushq %2\n\t"      //cs = __USER_CS
        "pushq %0\n\t"      //rip = 用户代码入口
        "mov %4, %%cr3\n\t" //切换到用户页表（高半内核映射仍存在）
        "iretq\n\t"
        :
        : "r"(entry), "r"(stack_top), "r"((uint64_t)__USER_CS), "r"((uint64_t)__USER_DS), "r"(pgd)
        : "memory"
    );
    __builtin_unreachable();
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
