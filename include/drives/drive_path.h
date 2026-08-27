#ifndef _EFI_DRIVE_PATH_H_
#define _EFI_DRIVE_PATH_H_

#include <klib.h>

//设备路径类型
#define DEVICE_PATH_TYPE_HARDWARE      0x01
#define DEVICE_PATH_TYPE_ACPI          0x02
#define DEVICE_PATH_TYPE_MESSAGING     0x03
#define DEVICE_PATH_TYPE_MEDIA         0x04
#define DEVICE_PATH_TYPE_BIOS          0x05
#define DEVICE_PATH_TYPE_END           0x7F

//硬件设备路径子类型
#define HARDWARE_SUBTYPE_PCI           0x01
#define HARDWARE_SUBTYPE_PCCARD        0x02
#define HARDWARE_SUBTYPE_MMAP          0x03
#define HARDWARE_SUBTYPE_VENDOR        0x04

//消息设备路径子类型(Messaging)
#define MESSAGING_SUBTYPE_ATAPI        0x01//传统ATA/ATAPI(PATA硬盘/光驱)
#define MESSAGING_SUBTYPE_SCSI         0x02
#define MESSAGING_SUBTYPE_USB          0x05
#define MESSAGING_SUBTYPE_SATA         0x12
#define MESSAGING_SUBTYPE_NVME         0x17

//媒体设备路径子类型
#define MEDIA_SUBTYPE_HARDDRIVE        0x01
#define MEDIA_SUBTYPE_CDROM            0x02
#define MEDIA_SUBTYPE_FILEPATH         0x04

//结束节点
#define END_SUBTYPE_ENTIRE             0xFF
#define END_SUBTYPE_INSTANCE           0x01

#define DEVICE_PATH_LENGTH(dp) (((dp)->Length[1] << 8) | (dp)->Length[0])//获取路径长度
#define IS_DEVICE_PATH_END(dp) ((dp)->Type == DEVICE_PATH_TYPE_END)//判断是否是结束节点
#define NEXT_DEVICE_PATH(dp)   ((EFI_DEVICE_PATH_PROTOCOL*)((uint8_t*)(dp) + DEVICE_PATH_LENGTH(dp)))//跳到下一个节点
#define IS_END_ENTIRE(dp)      ((dp)->Type == DEVICE_PATH_TYPE_END && (dp)->SubType == END_SUBTYPE_ENTIRE)//检查是否是完整结束

//PCI设备路径
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;//Type=0x01,SubType=0x01
    uint8_t Function;
    uint8_t Device;
} PCI_DEVICE_PATH;

//ATAPI设备路径(ATA)
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;//Type=0x03,SubType=0x05
    uint8_t PrimarySecondary;//0=Primary,1=Secondary
    uint8_t SlaveMaster;//0=Master, 1=Slave
    uint16_t LUN;//逻辑单元号
} ATAPI_DEVICE_PATH;

//SATA设备路径
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;//Type=0x03,SubType=0x12
    uint16_t HBAPortNumber;//HBA端口号
    uint16_t PortMultiplierPortNumber;
    uint16_t Lun;
} SATA_DEVICE_PATH;

//NVMe设备路径
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;//Type=0x03,SubType=0x17
    uint32_t NamespaceId;
    uint64_t NamespaceUuid;
} NVME_DEVICE_PATH;

//硬盘分区设备路径
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;//Type=0x04, SubType=0x01
    uint32_t PartitionNumber;//分区号 (1=第一个)
    uint64_t PartitionStart;//起始 LBA
    uint64_t PartitionSize;//扇区数
    uint8_t Signature[16];//磁盘签名
    uint8_t MBRType;//0x01=MBR, 0x02=GPT
    uint8_t SignatureType;//0x00=无, 0x01=MBR签名, 0x02=GPT GUID
} HARDDRIVE_DEVICE_PATH;

//文件路径节点
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;//Type=0x04,SubType=0x04
    uint16_t PathName[1];
} FILEPATH_DEVICE_PATH;

//设备路径解析结果
typedef struct {
    //PCI位置
    uint8_t pci_bus;
    uint8_t pci_device;
    uint8_t pci_function;
    int found_pci;
    //ATA位置
    int ata_channel;
    int ata_slave;
    int found_ata;
    //SATA位置
    uint16_t sata_port;
    int found_sata;
    //NVMe
    uint32_t nvme_namespace;
    int found_nvme;
    //分区信息
    uint32_t partition_number;
    uint64_t partition_start_lba;
    uint64_t partition_size_lba;
    uint8_t partition_signature[16];
    uint8_t partition_mbr_type;
    uint8_t partition_signature_type;
    int found_partition;
    //文件路径
    char filepath[256];
    int found_filepath;
} device_path_info_t;

/**
 * 解析 UEFI 设备路径
 * @path: 设备路径起始指针
 * @info: 输出解析结果
 * @author DeepSeek
 * 
 * 返回值：0 成功，-1 失败
 */
int ParseDevicePath(EFI_DEVICE_PATH_PROTOCOL *path, device_path_info_t *info);

#endif