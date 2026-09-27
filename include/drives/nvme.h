#ifndef _DRIVER_DISK_NVME_H_
#define _DRIVER_DISK_NVME_H_

#include <drives/disk.h>

//PCI标识
#define NVME_PCI_CLASS      0x01
#define NVME_PCI_SUBCLASS   0x08
#define NVME_PCI_PROGIF     0x02

//寄存器偏移(相对BAR0)
#define NVME_REG_CAP    0x0000   //64位控制器能力
#define NVME_REG_VS     0x0008   //32位版本
#define NVME_REG_INTMS  0x000C   //中断屏蔽
#define NVME_REG_INTMC  0x0010   //中断清除
#define NVME_REG_CC     0x0014   //配置寄存器
#define NVME_REG_CSTS   0x001C   //控制器状态
#define NVME_REG_AQA    0x0024   //admin 队列属性(深度-1)
#define NVME_REG_ASQ    0x0028   //64位 admin提交队列基址(物理地址)
#define NVME_REG_ACQ    0x0030   //64位 admin完成队列基址(物理地址)
#define NVME_REG_DBS    0x1000   //门铃区起点

//CAP位域
#define NVME_CAP_MQES(cap)   ((uint32_t)((cap) & 0xFFFFULL))     //bits15:0  最大条目数-1
#define NVME_CAP_CQR(cap)    (uint32_t)(((cap) >> 16) & 0x1)     //bit16     1=队列必须物理连续
#define NVME_CAP_AMS(cap)    (uint32_t)(((cap) >> 17) & 0x7)     //bits19:17 仲裁机制
#define NVME_CAP_TO(cap)     (uint32_t)(((cap) >> 24) & 0xFF)    //bits31:24 超时,单位500ms
#define NVME_CAP_DSTRD(cap)  (uint32_t)(((cap) >> 32) & 0xF)     //bits35:32 门铃步长=4<<DSTRD
#define NVME_CAP_NSSRS(cap)  (uint32_t)(((cap) >> 36) & 0x1)     //bit36     NVM子系统复位支持
#define NVME_CAP_CSS(cap)    (uint32_t)(((cap) >> 37) & 0xFF)    //bits44:37 命令集支持
#define NVME_CAP_MPSMIN(cap) (uint32_t)(((cap) >> 48) & 0xF)     //bits51:48 最小页
#define NVME_CAP_MPSMAX(cap) (uint32_t)(((cap) >> 52) & 0xF)     //bits55:52 最大页
#define NVME_CAP_CSS_NVM     (1u << 0)                           //NVM命令集
#define NVME_CAP_CMBS(cap)   (uint32_t)(((cap) >> 57) & 0x1)     //控制器内存缓冲

//CC位域
#define NVME_CC_EN      0x00000001  //bit0 使能
#define NVME_CC_CSS_NVM 0x00000000  //bits6:4 命令集: 000=NVM
#define NVME_CC_MPS(ps) (((uint32_t)(ps) & 0xF) << 7)   //bits10:7 页大小
#define NVME_CC_IOSQES  0x00060000  //bits19:16 提交队列条目大小 2^6=64
#define NVME_CC_IOCQES  0x00400000  //bits23:20 完成队列条目大小 2^4=16

//CSTS位域
#define NVME_CSTS_RDY   0x00000001  //bit0 就绪
#define NVME_CSTS_CFS   0x00000002  //bit1 致命状态

#define NVME_ADMIN_IDENTIFY   0x06
#define NVME_CNS_CTRL         0x01   //查询控制器
#define NVME_CNS_NS           0x00   //查询命名空间
#define NVME_ADMIN_CREATE_SQ  0x01   //创建I/O提交队列
#define NVME_ADMIN_CREATE_CQ  0x05   //创建I/O完成队列
#define NVME_CQ_PC            0x1    //CQ: 物理连续
#define NVME_CQ_IEN           0x2    //CQ: 中断使能(我们轮询, 不置)
#define NVME_SQ_PC            0x1    //SQ: 物理连续
#define NVME_IO_QID           1      //I/O队列号
#define NVME_IO_QD            16     //I/O队列深度

#define NVME_NVM_WRITE        0x01   //I/O队列: 写
#define NVME_NVM_READ         0x02   //I/O队列: 读

void NvmeRegisterDriver(void);//注册NVMe驱动到磁盘子系统

#endif