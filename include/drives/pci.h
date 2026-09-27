#ifndef _DRIVES_PCI_H_
#define _DRIVES_PCI_H_

#include <klib.h>

uint32_t PciRead32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);//读取32位配置空间
uint16_t PciRead16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);//读取16位配置空间
uint8_t  PciRead8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);//读取8位配置空间
void PciWrite32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val);//写入32位配置空间
void PciWrite16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint16_t val);//写入16位配置空间
void PciWrite8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint8_t val);//写入8位配置空间

//常见寄存器偏移
#define PCI_REG_VENDOR      0x00
#define PCI_REG_DEVICE      0x02
#define PCI_REG_COMMAND     0x04
#define PCI_REG_STATUS      0x06
#define PCI_REG_REVISION    0x08
#define PCI_REG_PROGIF      0x09
#define PCI_REG_SUBCLASS    0x0A
#define PCI_REG_CLASS       0x0B
#define PCI_REG_HDRTYPE     0x0E
#define PCI_REG_BAR0        0x10
#define PCI_REG_BAR1        0x14
#define PCI_REG_BAR2        0x18
#define PCI_REG_BAR3        0x1C
#define PCI_REG_BAR4        0x20
#define PCI_REG_BAR5        0x24

//class code
#define PCI_CLASS_MASS_STORAGE   0x01
#define PCI_SUBCLASS_ATA         0x01
#define PCI_SUBCLASS_SATA_AHCI   0x06
#define PCI_SUBCLASS_NVME        0x08
#define PCI_PROGIF_NVME          0x02

//设备描述
typedef struct pci_device {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, progif;
} pci_device_t;

typedef void (*pci_scan_callback)(pci_device_t *d, void *arg);

int  PciReadDevice(uint8_t bus, uint8_t dev, uint8_t func, pci_device_t *out);//读取设备信息
void PciScanBus(uint8_t bus, pci_scan_callback cb, void *arg);//扫描指定PCI总线
void PciScanAll(pci_scan_callback cb, void *arg);//扫描所有PCI总线

#endif
