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

extern char __bss_start[], __bss_end[];

_Bool LoadBootParam(BootParam* boot_param);//加载引导参数
int InitSystem();//初始化化系统
void KernelMain();//内核主函数
void test_thread();
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
extern char msg1[], msg2[], msg3[], msg4[], msg5[],msg_path1[], msg_path2[], msg_path3[], msg_filedata[];
__attribute__((noinline, section(".text.user")))
static void user_main(void) {
    //write(1,msg,len)
    syscall(SYS_WRITE,1,(uintptr_t)msg1,22);
    //文件测试
    int fd = syscall(SYS_OPEN,(uintptr_t)msg_path3,O_RDWR,0);//覆盖写入+读取
    if(fd >= 3){
        char rbuf[128];
        uint64_t n = syscall(SYS_READ,fd,(uintptr_t)rbuf,127);
        if(n > 0) syscall(SYS_WRITE,1,(uintptr_t)rbuf,n);
        syscall(SYS_LSEEK,fd,0,0);//SEEK_SET:回文件开头
        syscall(SYS_WRITE,fd,(uintptr_t)msg5,3);//覆盖前3字节
        syscall(SYS_LSEEK,fd,0,0);//回开头再读,应看到覆盖后的内容
        n = syscall(SYS_READ,fd,(uintptr_t)rbuf,127);
        if(n > 0) syscall(SYS_WRITE,1,(uintptr_t)rbuf,n);
        syscall(SYS_CLOSE,fd,0,0);
    }
    fd = syscall(SYS_OPEN,(uintptr_t)msg_path3,O_WRONLY|O_APPEND,0);//追加写测试
    if(fd >= 3){
        syscall(SYS_WRITE,fd,(uintptr_t)msg5,3);
        syscall(SYS_CLOSE,fd,0,0);
        fd = syscall(SYS_OPEN,(uintptr_t)msg_path3,O_RDONLY,0);
        if(fd >= 3){
            char rbuf[128];
            uint64_t n = syscall(SYS_READ,fd,(uintptr_t)rbuf,127);
            if(n > 0) syscall(SYS_WRITE,1,(uintptr_t)rbuf,n);
            syscall(SYS_CLOSE,fd,0,0);
        }
    }
    char ln = '\n';
    syscall(SYS_WRITE,1,&ln,1);
    //fork()
    pid_t pid = syscall(SYS_FORK,0,0,0);
    if(!pid){
        syscall(SYS_WRITE,1,(uintptr_t)msg2,14);
        syscall(SYS_EXIT,42,0,0);//exit(42):退出码42
    }else{
        syscall(SYS_WRITE,1,(uintptr_t)msg3,15);
        //waitpid(pid,&status,0):回收子进程
        int status = -1;
        long w = syscall(SYS_WAIT4,pid,(uintptr_t)&status,0);
        if(w == pid && status == 42){
            syscall(SYS_WRITE,1,(uintptr_t)msg4,20);//"waitpid got code 42\n"
        }
        syscall(SYS_EXIT,0,0,0);//exit(0)
    }
}
__asm__(
    ".pushsection .text.user, \"ax\", @progbits\n"
    "msg1: .ascii \"Hello World in ring3!\\n\"\n"
    "msg2: .ascii \"Child process\\n\"\n"
    "msg3: .ascii \"Parent process\\n\"\n"
    "msg4: .ascii \"waitpid got code 42\\n\"\n"
    "msg5: .ascii \"ABC\"\n"
    "msg_path1: .asciz \"/hello.txt\"\n"
    "msg_path2: .asciz \"/note.txt\"\n"
    "msg_path3: .asciz \"/SYS/TEST.TXT\"\n"
    ".popsection\n"
    "msg_filedata: .ascii \"user file data!\\n\"\n"
);
//结束标记函数
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
    //磁盘检测+文件系统挂载(必须在创建用户任务之前,用户程序依赖根文件系统)
    AtaRegisterDriver();
    disk_info_t disk = {0};
    device_path_info_t Device;
    ParseDevicePath(SYSTEM_BootParam->DiskInfo.DevicePath,&Device);
    early_printk("[DISK] path: pci=%x.%x ata=%d/%d sata=%u nvme=%u part=%u lba=%llu\n",
        Device.pci_device, Device.pci_function,
        Device.found_ata ? Device.ata_channel : -1,
        Device.found_ata ? Device.ata_slave : -1,
        Device.found_sata ? Device.sata_port : 0xFFFF,
        Device.found_nvme ? Device.nvme_namespace : 0,
        Device.found_partition ? Device.partition_number : 0,
        Device.found_partition ? Device.partition_start_lba : 0);
    if(DiskDetect(&Device,&disk) == 0){
        if(disk.ctrl_type == DISK_CTRL_AHCI){
            printk("[DISK] AHCI controller %02x:%02x.%x abar=%llx port=%u part_start=%llu part_size=%llu\n",
                disk.pci_bus, disk.pci_dev, disk.pci_func,
                disk.abar, disk.sata_port,
                disk.partition_start_lba, disk.partition_size_lba);
        }else if(disk.ctrl_type == DISK_CTRL_ATA){
            printk("[DISK] ATA controller %02x:%02x.%x cmd=%x ctrl=%x channel=%d slave=%d part_start=%llu part_size=%llu\n",
                disk.pci_bus, disk.pci_dev, disk.pci_func,
                disk.cmd_base, disk.ctrl_base,
                disk.ata_channel, disk.ata_slave,
                disk.partition_start_lba, disk.partition_size_lba);
        }
        //初始化磁盘设备
        if(DiskInit(&disk) == 0){
            printk("[DISK] %s model=%s sectors=%llu lba48=%d\n",
                disk.ops->name, disk.model, disk.total_sectors, disk.lba48);
            uint8_t sector[512];
            if(DiskRead(&disk, disk.partition_start_lba, 1, sector) == 0){
                printk("[DISK] read lba=%llu OK, first bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    disk.partition_start_lba,
                    sector[0], sector[1], sector[2], sector[3],
                    sector[4], sector[5], sector[6], sector[7]);
            }else{
                printk("[DISK] read lba=%llu failed\n", disk.partition_start_lba);
            }
        }else{
            printk("[DISK] driver not ready\n");
        }
    }else{
        printk("[DISK] controller not found\n");
    }
    FsInit(&disk);//FAT32优先挂载到根目录,失败回退ramfs
    mm_struct* umm = vmm_create_address_space();
    if(!umm)SYSTEM_STOP();
    if(!vmm_mmap(umm, 0x400000, PAGE_SIZE, VM_READ | VM_EXEC | VM_WRITE))SYSTEM_STOP();
    if(!vmm_mmap(umm, umm->start_stack, PAGE_SIZE, VM_READ | VM_WRITE))SYSTEM_STOP();
    uintptr_t *code_pte = (uintptr_t*)get_pte((uintptr_t)umm->pgd, 0x400000, 0, 0);
    if(!code_pte || !pte_is_present(*code_pte))SYSTEM_STOP();
    memcpy((void*)PHYS_TO_VIRT(pte_get_paddr(*code_pte)), (void*)user_main,
           (uintptr_t)user_main_end - (uintptr_t)user_main);
    printk("Creating user task...\n");
    CreateProcess(0x400000, umm, "User Test");//创建用户任务
    CreateKernelThread(test_thread,4096,"test");
    msleep(5000);
    printk(PRINTK_INFO"Type 'r' to reboot or type 's' to shutdown.");
    char key = GetKey();
    if(key == 's')SYSTEM_Shutdown();
    else SYSTEM_Restart();
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
