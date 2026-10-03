/**
 * include/drives/vmd.h
 *
 * Intel VMD (Volume Management Device) 支持
 *
 * 由DeepSeek V4.1 Flash生成
 */
#ifndef _DRIVES_VMD_H_
#define _DRIVES_VMD_H_

#include <klib.h>

#define VMD_MAX_CTRL   4      //最多支持几个 VMD 控制器
#define VMD_MAX_BARS   6      //每条 PCI 总线的 BAR 数

//VMD 的 class/subclass
#define PCI_SUBCLASS_VMD     0x04

//VMD 自己配置空间里的寄存器
#define VMD_REG_VMCAP        0x40
#define VMD_REG_VMCONFIG     0x44
#define VMD_REG_VMLOCK       0x70
#define VMD_VMCAP_BUS_RESTRICT   (1u << 0)  //VMCAP[0]: 支持总线范围限制
#define VMD_VMCONFIG_BUS_SHIFT   8          //VMCONFIG[9:8]: 0/1/2 -> 起点 0/128/224
#define VMD_VMCONFIG_BUS_MASK    0x3u
#define VMD_VMCONFIG_MSI_REMAP   (1u << 1)  //保留: 将来做 MSI 重映射用

//VSCAP 里的地址影子寄存器(只有 hypervisor 会提供, 裸机上没有)
#define VMD_VSCAP_MAGIC_SHDW     0x53484457u//"SHDW"

//MEMBAR2 里 VMD 自用的大小(子设备窗口从 base + 0x2000 开始)
#define VMD_MEMBAR2_RESERVED     0x2000

//域内子设备
typedef struct vmd_child {
    int      ctrl_index;                //属于哪个 VMD 控制器
    uint8_t  bus, dev, func;            //域内总线号 + 位置
    uint16_t vendor, device;
    uint8_t  class_code, subclass, progif;
    uint32_t bar_raw[VMD_MAX_BARS];     //配置空间里读到的原始 BAR
    uint64_t bar_phys[VMD_MAX_BARS];    //换算后的物理地址(0 = 该 BAR 不存在/是高位)
} vmd_child_t;

typedef void (*vmd_child_cb)(vmd_child_t *c, void *arg);

//一个 VMD 控制器
typedef struct vmd_ctrl {
    int      present;
    uint8_t  bus, dev, func;            //VMD 在传统 PCI 上的位置
    uint16_t vendor, device;
    uint64_t cfgbar_phys,  cfgbar_size; //BAR0/1
    uint64_t membar1_phys, membar1_size;//BAR2/3
    uint64_t membar2_phys, membar2_size;//BAR4/5
    uint64_t membar2_child;             //子设备窗口起点 = membar2_phys + 0x2000
    volatile uint8_t *cfgbar;           //配置窗口的虚拟地址(按总线惰性映射)
    uint32_t bus_map_mask;              //已映射的总线位图(最多 32 条)
    uint8_t  busn_start;                //域内总线起点(0/128/224)
    uint8_t  nbus;                      //域内总线数(CFGBAR 大小 / 1MB)
    uint8_t  bus_restrict;              //VMCAP 是否声明了总线范围限制
    int      have_rh;                   //VSCAP 里有 "SHDW"(说明跑在虚拟机里)
    uint64_t shdw_phys1, shdw_phys2;    //影子寄存器里的物理地址
    uint64_t offset1, offset2;          //CPU地址 - 物理地址(裸机恒为 0)
} vmd_ctrl_t;

int  VmdInitAll(void);                  //扫描并初始化所有 VMD 控制器, 返回个数
int  VmdCount(void);
vmd_ctrl_t *VmdGet(int index);
int  VmdScanChildren(vmd_child_cb cb, void *arg);//遍历所有域内设备, 返回个数
int  VmdFindNvme(vmd_child_t *out);     //找一个 NVMe, 成功返回 0

//域内设备配置空间读写(内含 VMD 要求的写回读与串行化)
uint32_t VmdRead32(int idx, uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
uint16_t VmdRead16(int idx, uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
uint8_t  VmdRead8 (int idx, uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
void VmdWrite32(int idx, uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val);
void VmdWrite16(int idx, uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint16_t val);
void VmdWrite8 (int idx, uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint8_t val);

#endif
