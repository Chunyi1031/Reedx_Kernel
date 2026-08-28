#include <drives/disk.h>
#include <drives/ata.h>
#include <drives/timer.h>
#include <io.h>
#include <print.h>
#include <delay.h>

//ATA命令块寄存器偏移
#define ATA_REG_DATA        0x00//数据寄存器(16位)
#define ATA_REG_ERROR       0x01//错误/特性
#define ATA_REG_SECCOUNT    0x02//扇区数
#define ATA_REG_LBA_LOW     0x03//LBA低字节
#define ATA_REG_LBA_MID     0x04//LBA中字节
#define ATA_REG_LBA_HIGH    0x05//LBA高字节
#define ATA_REG_DRIVE       0x06//驱动器/磁头选择
#define ATA_REG_STATUS      0x07//状态(读)
#define ATA_REG_COMMAND     0x07//命令(写,与STATUS同地址)

//状态位
#define ATA_SR_BSY   0x80//忙
#define ATA_SR_DRDY  0x40//就绪
#define ATA_SR_DRQ   0x08//数据请求
#define ATA_SR_ERR   0x01//错误

//命令
#define ATA_CMD_READ_PIO       0x20//LBA28读
#define ATA_CMD_READ_PIO_EXT   0x24//LBA48读
#define ATA_CMD_WRITE_PIO      0x30//LBA28写
#define ATA_CMD_WRITE_PIO_EXT  0x34//LBA48写
#define ATA_CMD_IDENTIFY       0xEC//识别设备
#define ATA_CMD_FLUSH_CACHE    0xE7//写回缓存

#define ATA_TIMEOUT   200000//BSY/DRQ轮询上限(无TSC时的回退)
#define ATA_TIMEOUT_MS 2000//BSY/DRQ轮询超时(毫秒,基于TSC)

//计算TSC截止时刻
static uint64_t ata_deadline(void){
    return rdtsc() + (tsc_freq_hz * ATA_TIMEOUT_MS) / 1000;
}

//等待BSY清除
static int ata_wait_bsy(disk_info_t *d){
    if(!tsc_freq_hz){//无TSC频率:回退到IO轮询
        for(int i = 0; i < ATA_TIMEOUT; i++){
            if(!(inb(d->cmd_base + ATA_REG_STATUS) & ATA_SR_BSY)) return 0;
            io_wait();
        }
        return -1;
    }
    uint64_t deadline = ata_deadline();
    while(rdtsc() < deadline){
        if(!(inb(d->cmd_base + ATA_REG_STATUS) & ATA_SR_BSY)) return 0;
    }
    return -1;
}

//等待DRQ(数据就绪)
static int ata_wait_drq(disk_info_t *d){
    if(!tsc_freq_hz){//无TSC频率:回退到IO轮询
        for(int i = 0; i < ATA_TIMEOUT; i++){
            uint8_t st = inb(d->cmd_base + ATA_REG_STATUS);
            if(st & ATA_SR_BSY){ io_wait(); continue; }//设备忙,继续等待
            if(st & ATA_SR_ERR) return -1;
            if(st & ATA_SR_DRQ) return 0;
            io_wait();
        }
        return -1;
    }
    uint64_t deadline = ata_deadline();
    while(rdtsc() < deadline){
        uint8_t st = inb(d->cmd_base + ATA_REG_STATUS);
        if(st & ATA_SR_BSY) continue;//设备忙,继续等待
        if(st & ATA_SR_ERR) return -1;
        if(st & ATA_SR_DRQ) return 0;
    }
    return -1;
}

//软复位
static int ata_soft_reset(disk_info_t *d){
    outb(d->ctrl_base, 0x04);
    for(int i = 0;i < 4;i++)io_wait();
    outb(d->ctrl_base, 0x00);
    io_wait();
    return ata_wait_bsy(d);
}

//选择驱动器并给出LBA高4位(LBA28)
static void ata_select(disk_info_t *d, uint64_t lba){
    outb(d->cmd_base + ATA_REG_DRIVE,0xE0 | ((d->ata_slave & 1) << 4) | ((lba >> 24) & 0x0F));
}

//读取512字节识别数据到id(256字)
static int ata_identify(disk_info_t *d, uint16_t *id){
    //写入IDENTIFY DEVICE指令
    outb(d->cmd_base + ATA_REG_DRIVE, 0xA0 | ((d->ata_slave & 1) << 4));
    outb(d->cmd_base + ATA_REG_SECCOUNT, 0);
    outb(d->cmd_base + ATA_REG_LBA_LOW, 0);
    outb(d->cmd_base + ATA_REG_LBA_MID, 0);
    outb(d->cmd_base + ATA_REG_LBA_HIGH, 0);
    outb(d->cmd_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    //读取状态
    uint8_t st = inb(d->cmd_base + ATA_REG_STATUS);
    if(st == 0 || st == 0xFF)return -1;
    //等待硬盘就绪
    if(ata_wait_bsy(d))return -1;
    if(ata_wait_drq(d))return -1;
    for(int i = 0; i < 256; i++)id[i] = inw(d->cmd_base + ATA_REG_DATA);//读取数据
    return 0;
}

//PIO传输
static int ata_pio_transfer(disk_info_t *d, uint64_t lba, uint32_t count,uint8_t *buf, int write){
    if(ata_wait_bsy(d)) return -1;//发命令前等设备空闲
    //传输count个扇区
    while(count){
        //检查是否超过LBA28单次最大扇区数
        int lba48 = ((lba + count - 1) > 0x0FFFFFFFULL);
        uint32_t chunk = lba48 ? 65536 : 256;
        if(count < chunk) chunk = count;
        //LBA48时先写高8位寄存器
        if(lba48){
            outb(d->cmd_base + ATA_REG_DRIVE, 0x40 | ((d->ata_slave & 1) << 4));
            outb(d->cmd_base + ATA_REG_SECCOUNT, (chunk >> 8) & 0xFF);
            outb(d->cmd_base + ATA_REG_LBA_LOW,  (lba >> 24) & 0xFF);
            outb(d->cmd_base + ATA_REG_LBA_MID,  (lba >> 32) & 0xFF);
            outb(d->cmd_base + ATA_REG_LBA_HIGH, (lba >> 40) & 0xFF);
        }
        //写低8位寄存器
        outb(d->cmd_base + ATA_REG_DRIVE,0xE0 | ((d->ata_slave & 1) << 4) | (lba48 ? 0 : ((lba >> 24) & 0x0F)));
        outb(d->cmd_base + ATA_REG_SECCOUNT, chunk & 0xFF);
        outb(d->cmd_base + ATA_REG_LBA_LOW,  lba & 0xFF);
        outb(d->cmd_base + ATA_REG_LBA_MID,  (lba >> 8) & 0xFF);
        outb(d->cmd_base + ATA_REG_LBA_HIGH, (lba >> 16) & 0xFF);
        outb(d->cmd_base + ATA_REG_COMMAND,write ? (lba48 ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_WRITE_PIO) : (lba48 ? ATA_CMD_READ_PIO_EXT : ATA_CMD_READ_PIO));
        //按扇区传输(每扇区512字节=256字)
        for(uint32_t s = 0; s < chunk; s++){
            if(ata_wait_drq(d)) return -1;
            if(write) outsw(d->cmd_base + ATA_REG_DATA, buf + s * 512, 256);
            else insw (d->cmd_base + ATA_REG_DATA, buf + s * 512, 256);
        }
        //等待命令完成并检查错误
        if(ata_wait_bsy(d))return -1;
        if(inb(d->cmd_base + ATA_REG_STATUS) & ATA_SR_ERR)return -1;
        lba += chunk;
        buf += chunk * 512;
        count -= chunk;
    }
    return 0;
}

//缓存刷新
static int ata_flush(disk_info_t *d){
    if(ata_wait_bsy(d)) return -1;//等设备空闲再发命令
    outb(d->cmd_base + ATA_REG_COMMAND, ATA_CMD_FLUSH_CACHE);
    if(ata_wait_bsy(d)) return -1;
    return (inb(d->cmd_base + ATA_REG_STATUS) & ATA_SR_ERR) ? -1 : 0;
}

//驱动初始化
static int ata_disk_init(disk_info_t *d){
    if(!d || d->ctrl_type != DISK_CTRL_ATA) return -1;
    ata_soft_reset(d);//软复位
    ata_select(d, 0);//选择驱动器
    ata_wait_bsy(d);//等待就绪
    //识别设备
    uint16_t id[256];
    if(ata_identify(d, id))return -1;
    for(int i = 0; i < 20; i++){
        d->model[i * 2]     = (id[27 + i] >> 8) & 0xFF;
        d->model[i * 2 + 1] = id[27 + i] & 0xFF;
    }
    d->model[40] = '\0';
    for(int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = '\0';
    //48位LBA支持
    d->lba48 = (id[83] & (1 << 10)) != 0;
    if(d->lba48){
        d->total_sectors = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) | ((uint64_t)id[101] << 16) | id[100];
    }else{
        d->total_sectors = ((uint32_t)id[61] << 16) | id[60];
    }
    return 0;
}

static int ata_disk_read(disk_info_t *d, uint64_t lba, uint32_t count, void *buf){
    return ata_pio_transfer(d, lba, count, (uint8_t*)buf, 0);
}

static int ata_disk_write(disk_info_t *d, uint64_t lba, uint32_t count, const void *buf){
    int r = ata_pio_transfer(d, lba, count, (uint8_t*)buf, 1);
    if(!r) r = ata_flush(d);
    return r;
}

//ATA PIO驱动操作集
const disk_ops_t ata_pio_ops = {
    .name  = "ATA-PIO",
    .init  = ata_disk_init,
    .read  = ata_disk_read,
    .write = ata_disk_write,
};

//注册ATA PIO驱动
void AtaRegisterDriver(void){
    DiskRegisterDriver(DISK_CTRL_ATA, &ata_pio_ops);
}
