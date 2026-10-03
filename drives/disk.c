#include <drives/disk.h>
#include <drives/pci.h>
#include <drives/vmd.h>

//驱动注册表
static const disk_ops_t *g_disk_drivers[DISK_CTRL_MAX];

void DiskRegisterDriver(int ctrl_type, const disk_ops_t *ops){
    if(ctrl_type > DISK_CTRL_NONE && ctrl_type < DISK_CTRL_MAX)g_disk_drivers[ctrl_type] = ops;
}

const disk_ops_t *DiskGetDriver(int ctrl_type){
    if(ctrl_type <= DISK_CTRL_NONE || ctrl_type >= DISK_CTRL_MAX) return NULL;
    return g_disk_drivers[ctrl_type];
}

int DiskInit(disk_info_t *disk){
    if(!disk || !disk->ops || !disk->ops->init) return -1;
    return disk->ops->init(disk);
}

int DiskRead(disk_info_t *disk, uint64_t lba, uint32_t count, void *buf){
    if(!disk || !disk->ops || !disk->ops->read) return -1;
    return disk->ops->read(disk, lba, count, buf);
}

int DiskWrite(disk_info_t *disk, uint64_t lba, uint32_t count, const void *buf){
    if(!disk || !disk->ops || !disk->ops->write) return -1;
    return disk->ops->write(disk, lba, count, buf);
}

//扫描上下文
typedef struct {
    uint8_t want_dev;
    uint8_t want_func;
    int     want_subclass;//0=任意 1=ATA 6=AHCI
    int     want_channel;//设备路径ATAPI节点通道
    disk_info_t *disk;
} scan_ctx_t;

/*DeepSeek V4 Pro*/
static void scan_cb(pci_device_t *d, void *arg){
    scan_ctx_t *ctx = (scan_ctx_t*)arg;
    if(ctx->disk->present) return;
    //匹配设备路径给出的PCI位置(dev/func)
    if(d->dev != ctx->want_dev || d->func != ctx->want_func) return;
    if(d->class_code != PCI_CLASS_MASS_STORAGE) return;
    if(ctx->want_subclass && d->subclass != ctx->want_subclass){
        if(!(ctx->want_subclass == PCI_SUBCLASS_NVME && d->subclass == PCI_SUBCLASS_VMD))return;//允许VMD子类匹配NVMe
    }

    if(d->subclass == PCI_SUBCLASS_SATA_AHCI){
        //AHCI:HBA MMIO基址在BAR5
        uint32_t bar5 = PciRead32(d->bus, d->dev, d->func, PCI_REG_BAR5);
        ctx->disk->ctrl_type = DISK_CTRL_AHCI;
        ctx->disk->abar = bar5 & 0xFFFFFFF0ULL;
        ctx->disk->pci_bus = d->bus;
        ctx->disk->pci_dev = d->dev;
        ctx->disk->pci_func = d->func;
        ctx->disk->present = 1;
    }else if(d->subclass == PCI_SUBCLASS_ATA){
        //ATA兼容控制器:BAR0/BAR1=命令块,BAR2/BAR3=控制块(通常IO空间)
        uint32_t bar0 = PciRead32(d->bus, d->dev, d->func, PCI_REG_BAR0);
        uint32_t bar1 = PciRead32(d->bus, d->dev, d->func, PCI_REG_BAR1);
        uint32_t bar2 = PciRead32(d->bus, d->dev, d->func, PCI_REG_BAR2);
        uint32_t bar3 = PciRead32(d->bus, d->dev, d->func, PCI_REG_BAR3);
        uint32_t cmd = (bar0 & 0xFFFFFFFCu);
        uint32_t ctrl = (bar2 & 0xFFFFFFFCu);
        //BAR未配置(0)时回退标准端口(主通道1F0/3F6,从通道170/376)
        if(!cmd){
            cmd  = ctx->want_channel ? 0x170 : 0x1F0;
            ctrl = ctx->want_channel ? 0x376 : 0x3F6;
        }
        ctx->disk->ctrl_type = DISK_CTRL_ATA;
        ctx->disk->cmd_base = cmd;
        ctx->disk->ctrl_base = ctrl;
        ctx->disk->pci_bus = d->bus;
        ctx->disk->pci_dev = d->dev;
        ctx->disk->pci_func = d->func;
        ctx->disk->present = 1;
/*DeepSeek V4 Pro-END*/
    }else if(d->subclass == PCI_SUBCLASS_VMD){
        //BAR由nvme驱动向VMD层要
        ctx->disk->ctrl_type = DISK_CTRL_NVME;
        ctx->disk->abar = 0;
        ctx->disk->pci_bus = d->bus;
        ctx->disk->pci_dev = d->dev;
        ctx->disk->pci_func = d->func;
        ctx->disk->present = 1;
    }else if(d->subclass == PCI_SUBCLASS_NVME){
        //获取BAR寄存器
        uint32_t bar0 = PciRead32(d->bus, d->dev, d->func, PCI_REG_BAR0);
        uint32_t bar1 = PciRead32(d->bus, d->dev, d->func, PCI_REG_BAR1);
        uint64_t bar;
        if((bar0 & 0x6) == 0x4)bar = ((uint64_t)(bar0 & 0xFFFFFFF0ULL)) | ((uint64_t)bar1 << 32);
        else bar = (uint64_t)(bar0 & 0xFFFFFFF0ULL);
        //保存信息
        ctx->disk->ctrl_type = DISK_CTRL_NVME;
        ctx->disk->abar = bar;
        ctx->disk->pci_bus = d->bus;
        ctx->disk->pci_dev = d->dev;
        ctx->disk->pci_func = d->func;
        ctx->disk->present = 1;
    }
}

int DiskDetect(device_path_info_t *dpi, disk_info_t *disk){
    if(!dpi || !disk) return -1;
    memset(disk, 0, sizeof(disk_info_t));
    if(!dpi->found_pci) return -1;
    //初始化扫描上下文结构体
    scan_ctx_t ctx;
    ctx.want_dev = dpi->pci_device;
    ctx.want_func = dpi->pci_function;
    ctx.want_channel = dpi->ata_channel;
    ctx.disk = disk;
    //按设备路径类型限定控制器子类
    if(dpi->found_sata)ctx.want_subclass = PCI_SUBCLASS_SATA_AHCI;
    else if(dpi->found_ata)ctx.want_subclass = PCI_SUBCLASS_ATA;
    else if(dpi->found_nvme)ctx.want_subclass = PCI_SUBCLASS_NVME;
    else ctx.want_subclass = 0;
    //扫描PCI总线，寻找设备
    PciScanAll(scan_cb, &ctx);
    if(!disk->present)return -1;
    //补充设备路径信息
    disk->ata_channel = dpi->ata_channel;
    disk->ata_slave = dpi->ata_slave;
    disk->has_atapi = dpi->found_ata;
    disk->has_sata = dpi->found_sata;
    disk->sata_port = dpi->sata_port;
    disk->partition_start_lba = dpi->partition_start_lba;
    disk->partition_size_lba = dpi->partition_size_lba;
    disk->ops = DiskGetDriver(disk->ctrl_type);//挂载对应驱动操作
    return 0;
}
