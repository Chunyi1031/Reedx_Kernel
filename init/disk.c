#include <klib.h>
#include <print.h>
#include <drives/disk.h>
#include <drives/ahci.h>
#include <drives/nvme.h>
#include <fs.h>

_Bool InitDiskAndFs(){
    //初始化磁盘驱动
    AtaRegisterDriver();//注册ATA驱动
    AhciRegisterDriver();//注册AHCI驱动
    NvmeRegisterDriver();//注册NVMe驱动
    //解析磁盘路径
    disk_info_t disk = {0};
    device_path_info_t Device;
    ParseDevicePath(SYSTEM_BootParam->DiskInfo.DevicePath,&Device);
    printk(PRINTK_INFO"Disk path: pci=%x.%x ata=%d/%d sata=%u nvme=%u part=%u lba=%llu",
        Device.pci_device, Device.pci_function,
        Device.found_ata ? Device.ata_channel : -1,
        Device.found_ata ? Device.ata_slave : -1,
        Device.found_sata ? Device.sata_port : 0xFFFF,
        Device.found_nvme ? Device.nvme_namespace : 0,
        Device.found_partition ? Device.partition_number : 0,
        Device.found_partition ? Device.partition_start_lba : 0);
    if(DiskDetect(&Device,&disk) == 0){
        if(disk.ctrl_type == DISK_CTRL_AHCI){
            printk(PRINTK_INFO"AHCI controller %02x:%02x.%x abar=%llx port=%u part_start=%llu part_size=%llu",
                disk.pci_bus, disk.pci_dev, disk.pci_func,
                disk.abar, disk.sata_port,
                disk.partition_start_lba, disk.partition_size_lba);
        }else if(disk.ctrl_type == DISK_CTRL_ATA){
            printk(PRINTK_INFO"ATA controller %02x:%02x.%x cmd=%x ctrl=%x channel=%d slave=%d part_start=%llu part_size=%llu",
                disk.pci_bus, disk.pci_dev, disk.pci_func,
                disk.cmd_base, disk.ctrl_base,
                disk.ata_channel, disk.ata_slave,
                disk.partition_start_lba, disk.partition_size_lba);
        }else if(disk.ctrl_type == DISK_CTRL_NVME){
            printk(PRINTK_INFO"NVMe controller %02x:%02x.%x bar=%llx part_start=%llu part_size=%llu",
                disk.pci_bus, disk.pci_dev, disk.pci_func,
                disk.abar,
                disk.partition_start_lba, disk.partition_size_lba);
        }
        //初始化磁盘设备
        if(DiskInit(&disk) != 0){
            printk(PRINTK_WARNING"Disk driver not ready");
            FsInit(NULL);//磁盘不可用, 回退到ramfs
            return false;
        }
    }else{
        printk(PRINTK_WARNING"Disk controller not found");
        FsInit(NULL);//未找到磁盘控制器, 回退到ramfs
        return false;
    }
    FsInit(&disk);//初始化文件系统
    return true;
}