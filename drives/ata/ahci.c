//由DeepSeek V4 Pro生成
#include <drives/disk.h>
#include <drives/ahci.h>
#include <drives/pci.h>
#include <drives/timer.h>
#include <io.h>
#include <print.h>
#include <delay.h>
#include <spinlock.h>
#include <mm/vmm.h>
#include <mm/pgtables.h>
#include <mm/pmm.h>

/* ================= AHCI HBA 寄存器 ================= */
// GHC 全局寄存器偏移
#define AHCI_GHC_CAP     0x00 // Host Capabilities
#define AHCI_GHC_GHC     0x04 // Global Host Control
#define AHCI_GHC_IS      0x08 // Interrupt Status
#define AHCI_GHC_PI      0x0C // Ports Implemented
#define AHCI_GHC_VS      0x10 // Version
#define AHCI_GHC_CAP2    0x24 // Host Capabilities Extended

// 端口寄存器偏移(每端口 0x80 字节, 从 0x100 开始)
#define AHCI_PxCLB   0x00 // Command List Base Address
#define AHCI_PxCLBU  0x04
#define AHCI_PxFB    0x08 // FIS Base Address
#define AHCI_PxFBU   0x0C
#define AHCI_PxIS    0x10 // Interrupt Status
#define AHCI_PxIE    0x14 // Interrupt Enable
#define AHCI_PxCMD   0x18 // Command and Status
#define AHCI_PxTFD   0x20 // Task File Data
#define AHCI_PxSIG   0x24 // Signature
#define AHCI_PxSSTS  0x28 // SATA Status
#define AHCI_PxSCTL  0x2C // SATA Control
#define AHCI_PxSERR  0x30 // SATA Error
#define AHCI_PxSACT  0x34 // SATA Active
#define AHCI_PxCI    0x38 // Command Issue
#define AHCI_PxSNTF  0x3C // SNotification

// GHC 位
#define AHCI_GHC_AE  0x80000000 // AHCI Enable
#define AHCI_GHC_HR  0x00000001 // HBA Reset

// PxCMD 位
#define AHCI_PxCMD_ST   0x0001 // Start
#define AHCI_PxCMD_FRE  0x0010 // FIS Receive Enable
#define AHCI_PxCMD_FR   0x4000 // FIS Receive Running (只读)
#define AHCI_PxCMD_CR   0x8000 // Command List Running (只读)

// PxIS 位
#define AHCI_PxIS_TFES  0x40000000 // Task File Error Status
#define AHCI_PxIS_HBFS  0x10000000 // Host Bus Fatal Error
#define AHCI_PxIS_HBDS  0x08000000 // Host Bus Data Error
#define AHCI_PxIS_IFS   0x04000000 // Interface Fatal Error
#define AHCI_PxIS_DHRS  0x00000001 // Device to Host Register FIS

// PxSSTS 的 DET 字段(低4位)
#define AHCI_SSTS_DET   0x0F
#define AHCI_SSTS_DET_READY 0x03 // 设备存在且通信就绪

// SATA 签名(设备类型)
#define SATA_SIG_ATA    0x00000101
#define SATA_SIG_ATAPI  0xEB140101

// FIS 类型
#define FIS_TYPE_REG_H2D 0x27 // Host to Device Register FIS

// ATA 命令
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC

#define AHCI_TIMEOUT_MS 5000 // 命令完成超时(毫秒)

/* ================= 结构体定义 ================= */
// 命令头(每端口 32 个, 每个 32 字节)
typedef struct {
    uint8_t  cfl:5;        // Command FIS Length(以 DW 计)
    uint8_t  atapi:1;      // ATAPI
    uint8_t  write:1;      // 写
    uint8_t  prefetch:1;   // 可预取
    uint8_t  reset:1;
    uint8_t  bist:1;
    uint8_t  clear_busy:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;        // Port Multiplier Port
    uint16_t prdtl;        // PRDT 条目数
    volatile uint32_t prdbc; // PRD Byte Count(传输字节数)
    uint32_t ctba;         // Command Table Base Address(低)
    uint32_t ctbau;        // (高)
    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

// Host to Device Register FIS(20 字节, 作 CFIS)
typedef struct {
    uint8_t  fis_type;     // 0x27
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;          // 1=command
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

// PRDT 条目(16 字节)
typedef struct {
    uint32_t dba;          // Data Base Address(低)
    uint32_t dbau;         // (高)
    uint32_t rsv0;
    uint32_t dbc:22;       // Data Byte Count(字节数-1)
    uint32_t rsv1:9;
    uint32_t i:1;          // Interrupt on Complete
} __attribute__((packed)) hba_prdt_entry_t;

// 命令表(128 字节 + PRDT)
typedef struct {
    uint8_t  cfis[64];     // Command FIS
    uint8_t  acmd[16];     // ATAPI 命令
    uint8_t  rsv[48];
    hba_prdt_entry_t prdt[1]; // PRDT 条目(可变长)
} __attribute__((packed)) hba_cmd_table_t;

// 接收 FIS 区域(256 字节对齐, 256 字节)
typedef struct {
    uint8_t dsfis[28];     // DMA Setup FIS
    uint8_t rsv0[4];
    uint8_t psfis[20];     // PIO Setup FIS
    uint8_t rsv1[12];
    uint8_t rfis[20];      // D2H Register FIS
    uint8_t rsv2[4];
    uint8_t sdbfis[8];     // Set Device Bits FIS
    uint8_t ufis[64];      // Unknown FIS
    uint8_t rsv3[96];
} __attribute__((packed)) hba_fis_t;

/* ================= 全局状态 ================= */
static volatile uint8_t *g_hba;         // HBA MMIO 虚拟地址
static uint32_t           g_port;       // 端口号
static volatile hba_cmd_header_t *g_clb;// 命令头(虚拟地址)
static uint32_t           g_clb_phys;   // 命令头物理地址
static volatile hba_fis_t *g_fis;       // FIS(虚拟地址)
static uint32_t           g_fis_phys;   // FIS 物理地址
static hba_cmd_table_t   *g_cmd_table;  // 命令表(虚拟地址)
static uint32_t           g_ctba_phys;  // 命令表物理地址
static spinlock_t         g_ahci_lock;  // AHCI 操作锁

// 取端口寄存器基址(虚拟地址)
static volatile uint32_t *port_regs(void){
    return (volatile uint32_t*)(g_hba + 0x100 + g_port * 0x80);
}

// 计算超时截止时刻
static uint64_t ahci_deadline(void){
    return rdtsc() + (tsc_freq_hz * AHCI_TIMEOUT_MS) / 1000;
}

// 等待端口命令引擎停止(CR 清零)
static int ahci_wait_cr_clear(void){
    uint64_t deadline = ahci_deadline();
    while(port_regs()[AHCI_PxCMD / 4] & AHCI_PxCMD_CR){
        if(rdtsc() > deadline) return -1;
    }
    return 0;
}

// 等待端口 FIS 接收停止(FR 清零)
static int ahci_wait_fr_clear(void){
    uint64_t deadline = ahci_deadline();
    while(port_regs()[AHCI_PxCMD / 4] & AHCI_PxCMD_FR){
        if(rdtsc() > deadline) return -1;
    }
    return 0;
}

// 停止端口(命令引擎 + FIS 接收)
static int ahci_port_stop(void){
    volatile uint32_t *px = port_regs();
    // 清 ST, 等 CR 清
    px[AHCI_PxCMD / 4] &= ~AHCI_PxCMD_ST;
    if(ahci_wait_cr_clear()) return -1;
    // 清 FRE, 等 FR 清
    px[AHCI_PxCMD / 4] &= ~AHCI_PxCMD_FRE;
    if(ahci_wait_fr_clear()) return -1;
    return 0;
}

// 启动端口
static void ahci_port_start(void){
    volatile uint32_t *px = port_regs();
    // 等 CR 清(若仍在运行)
    while(px[AHCI_PxCMD / 4] & AHCI_PxCMD_CR);
    // 设 FRE + ST
    px[AHCI_PxCMD / 4] |= AHCI_PxCMD_FRE;
    px[AHCI_PxCMD / 4] |= AHCI_PxCMD_ST;
}

// 构建并发送一个 DMA 命令(count 个扇区, 每扇区 512 字节, count<=256)
static int ahci_dma_xfer(uint64_t lba, uint32_t count, void *buf, int write){
    volatile uint32_t *px = port_regs();
    uint32_t slot = 0;
    // 等槽位空闲(PxCI 位清)
    uint64_t deadline = ahci_deadline();
    while(px[AHCI_PxCI / 4] & (1u << slot)){
        if(rdtsc() > deadline) return -1;
    }
    // 填充命令头
    g_clb[slot].cfl = sizeof(fis_reg_h2d_t) / 4;
    g_clb[slot].atapi = 0;
    g_clb[slot].write = write;
    g_clb[slot].prefetch = 1;
    g_clb[slot].reset = 0;
    g_clb[slot].bist = 0;
    g_clb[slot].clear_busy = 0;
    g_clb[slot].pmp = 0;
    g_clb[slot].prdbc = 0;
    g_clb[slot].ctba = g_ctba_phys;
    g_clb[slot].ctbau = 0;
    // 填充命令表 CFIS
    memset((void*)g_cmd_table, 0, sizeof(hba_cmd_table_t));
    fis_reg_h2d_t *cfis = (fis_reg_h2d_t*)g_cmd_table->cfis;
    cfis->fis_type = FIS_TYPE_REG_H2D;
    cfis->c = 1;
    cfis->command = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    cfis->device = 0x40; // LBA 模式
    cfis->lba0 = (lba >>  0) & 0xFF;
    cfis->lba1 = (lba >>  8) & 0xFF;
    cfis->lba2 = (lba >> 16) & 0xFF;
    cfis->lba3 = (lba >> 24) & 0xFF;
    cfis->lba4 = (lba >> 32) & 0xFF;
    cfis->lba5 = (lba >> 40) & 0xFF;
    cfis->countl = count & 0xFF;
    cfis->counth = (count >> 8) & 0xFF;
    // 填充 PRDT(逐页拆分)
    uint64_t bytes = (uint64_t)count * 512;
    uint64_t off = 0;
    int prdtl = 0;
    while(off < bytes){
        uintptr_t pa = VIRT_TO_PHYS((uintptr_t)buf + off);
        uint32_t chunk = bytes - off;
        uint32_t page_left = 4096 - (pa & 0xFFF);
        if(chunk > page_left) chunk = page_left;
        g_cmd_table->prdt[prdtl].dba = (uint32_t)(pa & 0xFFFFFFFF);
        g_cmd_table->prdt[prdtl].dbau = (uint32_t)(pa >> 32);
        g_cmd_table->prdt[prdtl].rsv0 = 0;
        g_cmd_table->prdt[prdtl].dbc = chunk - 1;
        g_cmd_table->prdt[prdtl].i = 1;
        prdtl++;
        off += chunk;
    }
    g_clb[slot].prdtl = prdtl;
    // 清错误状态
    px[AHCI_PxSERR / 4] = px[AHCI_PxSERR / 4]; // 写回以清除
    px[AHCI_PxIS / 4] = px[AHCI_PxIS / 4];     // 清除中断状态
    // 发命令
    px[AHCI_PxCI / 4] = (1u << slot);
    // 等完成
    deadline = ahci_deadline();
    while(px[AHCI_PxCI / 4] & (1u << slot)){
        if(rdtsc() > deadline) return -1;
    }
    // 检查错误
    uint32_t is = px[AHCI_PxIS / 4];
    if(is & (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS)){
        return -1;
    }
    uint32_t tfd = px[AHCI_PxTFD / 4];
    if(tfd & 0x1) return -1; // ERR
    return 0;
}

#define AHCI_MAX_SECTORS 1024 //单条命令最多扇区数

// 读扇区
static int ahci_disk_read(disk_info_t *d, uint64_t lba, uint32_t count, void *buf){
    (void)d;
    if(!buf || !count) return 0;
    uint64_t flags;
    spin_lock_irqsave(&g_ahci_lock, flags);
    int r = 0;
    uint8_t *p = (uint8_t*)buf;
    while(count){
        uint32_t chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;//单条命令最多256扇区
        if(ahci_dma_xfer(lba, chunk, p, 0)){ r = -1; break; }
        lba += chunk;
        p += (uint64_t)chunk * 512;
        count -= chunk;
    }
    spin_unlock_irqrestore(&g_ahci_lock, flags);
    return r;
}

// 写扇区
static int ahci_disk_write(disk_info_t *d, uint64_t lba, uint32_t count, const void *buf){
    (void)d;
    if(!buf || !count) return 0;
    uint64_t flags;
    spin_lock_irqsave(&g_ahci_lock, flags);
    int r = 0;
    const uint8_t *p = (const uint8_t*)buf;
    while(count){
        uint32_t chunk = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;//单条命令最多256扇区
        if(ahci_dma_xfer(lba, chunk, (void*)p, 1)){ r = -1; break; }
        lba += chunk;
        p += (uint64_t)chunk * 512;
        count -= chunk;
    }
    spin_unlock_irqrestore(&g_ahci_lock, flags);
    return r;
}

// IDENTIFY 获取型号与扇区数
static int ahci_identify(disk_info_t *d, uint16_t *id){
    // 用槽位 0 发 IDENTIFY, 数据写到 id(512 字节)
    volatile uint32_t *px = port_regs();
    uint32_t slot = 0;
    uint64_t deadline = ahci_deadline();
    while(px[AHCI_PxCI / 4] & (1u << slot)){
        if(rdtsc() > deadline) return -1;
    }
    uintptr_t id_phys = VIRT_TO_PHYS((uintptr_t)id);
    g_clb[slot].cfl = sizeof(fis_reg_h2d_t) / 4;
    g_clb[slot].atapi = 0;
    g_clb[slot].write = 0;
    g_clb[slot].prefetch = 1;
    g_clb[slot].reset = 0;
    g_clb[slot].bist = 0;
    g_clb[slot].clear_busy = 0;
    g_clb[slot].pmp = 0;
    g_clb[slot].prdbc = 0;
    g_clb[slot].ctba = g_ctba_phys;
    g_clb[slot].ctbau = 0;
    memset((void*)g_cmd_table, 0, sizeof(hba_cmd_table_t));
    fis_reg_h2d_t *cfis = (fis_reg_h2d_t*)g_cmd_table->cfis;
    cfis->fis_type = FIS_TYPE_REG_H2D;
    cfis->c = 1;
    cfis->command = ATA_CMD_IDENTIFY;
    cfis->device = 0;
    cfis->countl = 1;
    g_cmd_table->prdt[0].dba = (uint32_t)(id_phys & 0xFFFFFFFF);
    g_cmd_table->prdt[0].dbau = (uint32_t)(id_phys >> 32);
    g_cmd_table->prdt[0].rsv0 = 0;
    g_cmd_table->prdt[0].dbc = 511; // 512 字节
    g_cmd_table->prdt[0].i = 1;
    g_clb[slot].prdtl = 1;
    px[AHCI_PxSERR / 4] = px[AHCI_PxSERR / 4];
    px[AHCI_PxIS / 4] = px[AHCI_PxIS / 4];
    px[AHCI_PxCI / 4] = (1u << slot);
    deadline = ahci_deadline();
    while(px[AHCI_PxCI / 4] & (1u << slot)){
        if(rdtsc() > deadline) return -1;
    }
    if(px[AHCI_PxIS / 4] & (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS)) return -1;
    if(px[AHCI_PxTFD / 4] & 0x1) return -1;
    return 0;
}

// 驱动初始化
static int ahci_disk_init(disk_info_t *d){
    if(!d || d->ctrl_type != DISK_CTRL_AHCI) return -1;
    if(!d->abar) return -1;
    // 使能 PCI 总线主控 + 内存空间
    uint16_t cmd = PciRead16(d->pci_bus, d->pci_dev, d->pci_func, PCI_REG_COMMAND);
    cmd |= 0x6; // bit1=Memory Space, bit2=Bus Master
    PciWrite16(d->pci_bus, d->pci_dev, d->pci_func, PCI_REG_COMMAND, cmd);
    // 映射 HBA MMIO(2 页)
    for(int i = 0; i < 2; i++){
        vmm_map_page(KERNEL_PML4,
            (uintptr_t)PHYS_TO_VIRT(d->abar) + (uintptr_t)i * 4096,
            d->abar + (uintptr_t)i * 4096,
            PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE);
    }
    g_hba = (volatile uint8_t*)PHYS_TO_VIRT((uintptr_t)d->abar);
    g_port = d->sata_port;
    // 检查端口是否实现
    uint32_t pi = *(volatile uint32_t*)(g_hba + AHCI_GHC_PI);
    if(!(pi & (1u << g_port))) return -1;
    // AHCI Enable
    *(volatile uint32_t*)(g_hba + AHCI_GHC_GHC) |= AHCI_GHC_AE;
    // 分配命令头(1 页, 1KB 对齐)与 FIS(1 页, 256 对齐)
    uintptr_t clb_p = (uintptr_t)Pmm_Malloc(1);
    uintptr_t fis_p = (uintptr_t)Pmm_Malloc(1);
    uintptr_t ctba_p = (uintptr_t)Pmm_Malloc(1);
    if(!clb_p || !fis_p || !ctba_p) return -1;
    g_clb = (volatile hba_cmd_header_t*)PHYS_TO_VIRT(clb_p);
    g_fis = (volatile hba_fis_t*)PHYS_TO_VIRT(fis_p);
    g_cmd_table = (hba_cmd_table_t*)PHYS_TO_VIRT(ctba_p);
    g_clb_phys = (uint32_t)clb_p;
    g_fis_phys = (uint32_t)fis_p;
    g_ctba_phys = (uint32_t)ctba_p;
    memset((void*)g_clb, 0, 4096);
    memset((void*)g_fis, 0, 4096);
    memset((void*)g_cmd_table, 0, 4096);
    // 停止端口, 配置命令头/FIS, 重启
    if(ahci_port_stop()) return -1;
    volatile uint32_t *px = port_regs();
    px[AHCI_PxCLB / 4] = g_clb_phys;
    px[AHCI_PxCLBU / 4] = 0;
    px[AHCI_PxFB / 4] = g_fis_phys;
    px[AHCI_PxFBU / 4] = 0;
    px[AHCI_PxSERR / 4] = px[AHCI_PxSERR / 4];
    px[AHCI_PxIS / 4] = px[AHCI_PxIS / 4];
    px[AHCI_PxIE / 4] = 0;
    ahci_port_start();
    // 等设备就绪
    uint64_t deadline = ahci_deadline();
    while((px[AHCI_PxSSTS / 4] & AHCI_SSTS_DET) != AHCI_SSTS_DET_READY){
        if(rdtsc() > deadline) return -1;
    }
    // IDENTIFY 获取型号与扇区数
    uint16_t id[256];
    if(ahci_identify(d, id)) return -1;
    for(int i = 0; i < 20; i++){
        d->model[i * 2]     = (id[27 + i] >> 8) & 0xFF;
        d->model[i * 2 + 1] = id[27 + i] & 0xFF;
    }
    d->model[40] = '\0';
    for(int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = '\0';
    d->lba48 = (id[83] & (1 << 10)) != 0;
    if(d->lba48){
        d->total_sectors = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) | ((uint64_t)id[101] << 16) | id[100];
    }else{
        d->total_sectors = ((uint32_t)id[61] << 16) | id[60];
    }
    printk(PRINTK_INFO"AHCI disk: %s, %llu sectors", d->model, d->total_sectors);
    return 0;
}

// AHCI 驱动操作集
const disk_ops_t ahci_ops = {
    .name  = "AHCI",
    .init  = ahci_disk_init,
    .read  = ahci_disk_read,
    .write = ahci_disk_write,
};

// 注册 AHCI 驱动
void AhciRegisterDriver(void){
    spin_lock_init(&g_ahci_lock);
    DiskRegisterDriver(DISK_CTRL_AHCI, &ahci_ops);
}
