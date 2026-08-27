#ifndef _DISK_H_
#define _DISK_H_

#include <klib.h>
#include <drives/drive_path.h>
#include <drives/ata.h>

//磁盘控制器类型
#define DISK_CTRL_NONE   0
#define DISK_CTRL_ATA    1   //ATA兼容(PIO,通道0/1)
#define DISK_CTRL_AHCI   2   //SATA AHCI

//磁盘操作集合
struct disk_info;
typedef struct disk_ops {
    const char *name;//驱动名称
    int (*init)(struct disk_info *disk);//初始化/探测设备
    int (*read)(struct disk_info *disk, uint64_t lba, uint32_t count, void *buf);//读扇区
    int (*write)(struct disk_info *disk, uint64_t lba, uint32_t count, const void *buf);//写扇区
}disk_ops_t;

#define DISK_CTRL_MAX 3//支持的最大控制器类型数

//磁盘描述:由设备路径解析结果+PCI枚举共同确定
typedef struct disk_info {
    int      present;
    int      ctrl_type;      //DISK_CTRL_*
    //PCI位置
    uint8_t  pci_bus, pci_dev, pci_func;
    //ATA兼容控制器
    uint32_t cmd_base;       //命令块基址(IO或MMIO)
    uint32_t ctrl_base;      //控制块基址
    int      ata_channel;    //0=primary 1=secondary(来自设备路径ATAPI节点)
    int      ata_slave;      //0=master 1=slave
    int      has_atapi;
    //AHCI控制器
    uint64_t abar;           //AHCI MMIO基址(BAR5)
    int      has_sata;
    uint32_t sata_port;      //HBA端口号(来自设备路径SATA节点)
    //分区信息
    uint64_t partition_start_lba;
    uint64_t partition_size_lba;
    //挂载的驱动操作
    const disk_ops_t *ops;
    //设备参数
    char     model[41];      //型号名
    uint64_t total_sectors;  //总扇区数
    int      lba48;          //支持48位LBA
} disk_info_t;

int DiskDetect(device_path_info_t *dpi, disk_info_t *disk);//定位启动磁盘控制器
void DiskRegisterDriver(int ctrl_type, const disk_ops_t *ops);//注册磁盘驱动ops
const disk_ops_t *DiskGetDriver(int ctrl_type);//按控制器类型取得驱动ops
int DiskInit(disk_info_t *disk);//初始化磁盘
int DiskRead(disk_info_t *disk, uint64_t lba, uint32_t count, void *buf);//读取磁盘
int DiskWrite(disk_info_t *disk, uint64_t lba, uint32_t count, const void *buf);//写入磁盘

#endif