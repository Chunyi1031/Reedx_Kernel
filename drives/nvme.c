#include <drives/nvme.h>
#include <drives/pci.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <print.h>
#include <delay.h>
#include <spinlock.h>

//扫描上下文
typedef struct {
    int      found;
    uint8_t  bus, dev, func;
    uint16_t vendor, device;
    uint32_t bar_raw0, bar_raw1;
    uint64_t bar;
} nvme_probe_t;
//NVMe控制器信息
typedef struct {
    int      present;
    uint8_t  bus, dev, func;
    uint64_t bar_phys;
    volatile uint8_t *mmio;
    uint64_t cap;
    uint32_t dstrd, db_stride, mqes, to, css, mpsmin, mpsmax;
    uint64_t asq, acq;
    uint32_t sq_tail;//SQ尾指针
    uint32_t cq_head;//CQ头指针
    uint8_t  cq_phase;//期望的phase
    uint32_t nsid;//命名空间ID
    uint64_t nsze;//命名空间块数
    uint32_t blksz;//逻辑块大小(字节)
    uint32_t lbads;//log2(块大小)
    uint16_t cid;//命令ID计数器
    uint8_t  mdts;//单次I/O最大传输指数
    uint32_t nn;//命名空间数量
    char     sn[21];//控制器序列号
    uint64_t iocq, iosq;//I/O队列物理地址
    uint32_t iosq_tail;//I/O SQ尾指针
    uint32_t iocq_head;//I/O CQ头指针
    uint8_t  iocq_phase;//I/O CQ期望phase
    uint8_t  ioq_ready;
    char     mn[41];//控制器型号
} nvme_ctrl_t;
static nvme_ctrl_t g_nvme;//全局NVMe控制器信息
static spinlock_t g_nvme_lock;//NVMe锁

//提交队列条目
typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;//命令ID
    uint32_t nsid;
    uint64_t rsvd1;
    uint64_t mptr;//元数据指针
    uint64_t prp1;//数据缓冲物理地址
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_sqe_t;
//完成队列条目
typedef struct {
    uint32_t cdw0;//命令相关的结果
    uint32_t rsvd1;
    uint16_t sq_head;//SQ当前头指针(设备消费到哪)
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} nvme_cqe_t;

#define NVME_RD32(o)    (*(volatile uint32_t*)(g_nvme.mmio + (o)))                   //读取32位寄存器
#define NVME_WR32(o,v)  (*(volatile uint32_t*)(g_nvme.mmio + (o)) = (uint32_t)(v))   //写入32位寄存器
#define NVME_RD64(o)    (*(volatile uint64_t*)(g_nvme.mmio + (o)))                   //读取64位寄存器
#define NVME_WR64(o,v)  (*(volatile uint64_t*)(g_nvme.mmio + (o)) = (uint64_t)(v))   //写入64位寄存器
#define NVME_ADMIN_QD 16  //管理队列深度

//门铃
#define NVME_DB_SQ(y)  (NVME_REG_DBS + 2u * (y) * g_nvme.db_stride)
#define NVME_DB_CQ(y)  (NVME_REG_DBS + (2u * (y) + 1u) * g_nvme.db_stride)

#define NVME_POLL_DELAY(t) do{ if((t) < 64)__asm__ volatile("pause"); else udelay(2); }while(0)

static void nvme_scan_cb(pci_device_t *d, void *arg){
    nvme_probe_t *ctx = (nvme_probe_t*)arg;
    //检查设备类型
    if(d->class_code != NVME_PCI_CLASS)return;
    if(d->subclass != NVME_PCI_SUBCLASS)return;
    //获取BAR寄存器
    uint32_t bar0 = PciRead32(d->bus, d->dev, d->func, PCI_REG_BAR0);
    uint32_t bar1 = PciRead32(d->bus, d->dev, d->func, PCI_REG_BAR1);
    uint64_t bar;
    if((bar0 & 0x6) == 0x4)bar = ((uint64_t)(bar0 & 0xFFFFFFF0ULL) | ((uint64_t)bar1 << 32));//64位内存映射
    else bar = (uint64_t)(bar0 & 0xFFFFFFF0ULL);//32位内存映射
    //保存信息
    ctx->found = 1;
    ctx->bus = d->bus;
    ctx->dev = d->dev;
    ctx->func = d->func;
    ctx->vendor = d->vendor_id;
    ctx->device = d->device_id;
    ctx->bar_raw0 = bar0;
    ctx->bar_raw1 = bar1;
    ctx->bar = bar;
}

//等待RDY
static int nvme_wait_rdy(int want, uint32_t ms_max){
    //循环等待
    for(uint32_t i = 0; i < ms_max; i++){
        uint32_t csts = NVME_RD32(NVME_REG_CSTS);//读取控制器状态寄存器
        if(csts == 0xFFFFFFFFULL)return -1;//设备消失
        if(csts & NVME_CSTS_CFS)return -2;//致命错误
        //检查RDY位
        uint32_t rdy = csts & NVME_CSTS_RDY;
        if((want && rdy) || (!want && !rdy))return 0;
        mdelay(1);
    }
    return -3;//超时
}
//复位
static int nvme_ctrl_reset(void){
    //禁用控制器
    uint32_t cc = NVME_RD32(NVME_REG_CC);
    cc &= ~NVME_CC_EN;
    NVME_WR32(NVME_REG_CC, cc);
    int r = nvme_wait_rdy(0, g_nvme.to * 500u);
    if(r)return r;
    NVME_WR32(NVME_REG_INTMS, 0xFFFFFFFFu); //屏蔽全部中断类型
    NVME_WR32(NVME_REG_INTMC, 0xFFFFFFFFu); //清掉固件留下的挂起中断
    return 0;
}
//建admin队列
static int nvme_admin_queue_setup(void){
    //分配admin队列
    uint64_t asq = (uint64_t)Pmm_Malloc(1);
    uint64_t acq = (uint64_t)Pmm_Malloc(1);
    if(!asq || !acq)return -1;
    memset((void*)PHYS_TO_VIRT(asq), 0, PAGE_SIZE);
    memset((void*)PHYS_TO_VIRT(acq), 0, PAGE_SIZE);
    g_nvme.asq = asq;
    g_nvme.acq = acq;
    g_nvme.sq_tail  = 0;
    g_nvme.cq_head  = 0;
    g_nvme.cq_phase = 1;
    //写AQA/ASQ/ACQ
    NVME_WR32(NVME_REG_AQA, ((NVME_ADMIN_QD - 1) << 16) | (NVME_ADMIN_QD - 1));
    NVME_WR64(NVME_REG_ASQ, asq);
    NVME_WR64(NVME_REG_ACQ, acq);
    return 0;
}
//使能
static int nvme_ctrl_enable(void){
    uint32_t cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS(0) | NVME_CC_IOSQES | NVME_CC_IOCQES;
    NVME_WR32(NVME_REG_CC, cc);              //应为 0x460001
    return nvme_wait_rdy(1, g_nvme.to * 500u);
}

static int nvme_probe(void){
    nvme_probe_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    //扫描所有PCI总线
    PciScanAll(nvme_scan_cb,&ctx);
    if(!ctx.found){
        printk(PRINTK_ERR"NVMe: controller not found");
        return -1;
    }
    //打开内存空间访问和总线主控
    uint16_t cmd = PciRead16(ctx.bus, ctx.dev, ctx.func, PCI_REG_COMMAND);
    cmd |= 0x6;
    PciWrite16(ctx.bus, ctx.dev, ctx.func, PCI_REG_COMMAND, cmd);
    //映射BAR寄存器到内核虚拟地址空间
    for(int i = 0; i < 4; i++){
        vmm_map_page(KERNEL_PML4,(uintptr_t)PHYS_TO_VIRT(ctx.bar) + (uintptr_t)i * 4096,ctx.bar + (uintptr_t)i * 4096,PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE);
    }
    //填充全局控制器信息
    memset(&g_nvme, 0, sizeof(g_nvme));
    g_nvme.present  = 1;
    g_nvme.bus      = ctx.bus;
    g_nvme.dev      = ctx.dev;
    g_nvme.func     = ctx.func;
    g_nvme.bar_phys = ctx.bar;
    g_nvme.mmio     = (volatile uint8_t*)PHYS_TO_VIRT(ctx.bar);
    //读能力寄存器
    g_nvme.cap = NVME_RD64(NVME_REG_CAP);
    g_nvme.mqes      = NVME_CAP_MQES(g_nvme.cap);
    g_nvme.dstrd     = NVME_CAP_DSTRD(g_nvme.cap);
    g_nvme.db_stride = 4u << g_nvme.dstrd;//门铃步长 = 4<<DSTRD (本设备为4字节)
    g_nvme.to        = NVME_CAP_TO(g_nvme.cap);
    g_nvme.css       = NVME_CAP_CSS(g_nvme.cap);
    g_nvme.mpsmin    = NVME_CAP_MPSMIN(g_nvme.cap);
    g_nvme.mpsmax    = NVME_CAP_MPSMAX(g_nvme.cap);
    return 0;
}

//提交一条admin命令
static int nvme_admin_submit(nvme_sqe_t *sqe){
    //命令ID
    g_nvme.cid++;
    if(!g_nvme.cid)g_nvme.cid++;
    sqe->cid = g_nvme.cid;
    //把SQE写进SQ的第sq_tail条
    uint32_t idx = g_nvme.sq_tail & (NVME_ADMIN_QD - 1);
    memcpy((void*)(PHYS_TO_VIRT(g_nvme.asq) + (uint64_t)idx * sizeof(nvme_sqe_t)),sqe, sizeof(nvme_sqe_t));
    __sync_synchronize();//内存屏障
    //敲SQ0尾门铃
    g_nvme.sq_tail++;
    NVME_WR32(NVME_DB_SQ(0), g_nvme.sq_tail & (NVME_ADMIN_QD - 1));
    //轮询CQ0的第cq_head条
    volatile nvme_cqe_t *cq = (volatile nvme_cqe_t*)PHYS_TO_VIRT(g_nvme.acq);
    uint32_t i = g_nvme.cq_head & (NVME_ADMIN_QD - 1);
    int r = -3;
    for(uint32_t t = 0; t < 200000; t++){
        uint16_t st = cq[i].status;
        if(st == 0xFFFF)break;//设备消失
        if((st & 1) == g_nvme.cq_phase){
            if(cq[i].cid != sqe->cid || cq[i].sq_id != 0){
                printk(PRINTK_ERR"NVMe: cqe mismatch cid=%u want=%u sqid=%u sqhead=%u status=0x%x",
                       cq[i].cid, sqe->cid, cq[i].sq_id, cq[i].sq_head, cq[i].status);
                r = -4;
            }else{
                uint32_t sc = (st >> 1) & 0xFF;//状态码
                uint32_t sct= (st >> 9) & 0x7;//状态类型
                r = (int)((sct << 8) | sc);
                if(r)printk(PRINTK_ERR"NVMe: cmd 0x%x failed sct=%u sc=%u sqhead=%u",sqe->opcode, sct, sc, cq[i].sq_head);
            }
            break;
        }
        NVME_POLL_DELAY(t);
    }
    if(r == -3)printk(PRINTK_ERR"NVMe: cmd 0x%x timeout sqtail=%u cqhead=%u csts=0x%x",
                      sqe->opcode, g_nvme.sq_tail, g_nvme.cq_head, NVME_RD32(NVME_REG_CSTS));
    //不管成败都要还CQ头门铃
    g_nvme.cq_head++;
    if(g_nvme.cq_head >= NVME_ADMIN_QD){
        g_nvme.cq_head = 0;
        g_nvme.cq_phase ^= 1;
    }
    NVME_WR32(NVME_DB_CQ(0), g_nvme.cq_head);
    return r;
}

//创建I/O完成队列(0x05)
static int nvme_create_io_cq(void){
    uint64_t cq = (uint64_t)Pmm_Malloc(1);
    if(!cq)return -1;
    memset((void*)PHYS_TO_VIRT(cq), 0, PAGE_SIZE);
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_CREATE_CQ;
    sqe.prp1 = cq;
    sqe.cdw10 = ((NVME_IO_QD - 1) << 16) | NVME_IO_QID;
    sqe.cdw11 = NVME_CQ_PC;
    int r = nvme_admin_submit(&sqe);
    if(r)return r;
    g_nvme.iocq = cq;
    g_nvme.iocq_head = 0;
    g_nvme.iocq_phase = 1;
    return 0;
}
//创建I/O提交队列(0x01)
static int nvme_create_io_sq(void){
    uint64_t sq = (uint64_t)Pmm_Malloc(1);
    if(!sq)return -1;
    memset((void*)PHYS_TO_VIRT(sq), 0, PAGE_SIZE);
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_CREATE_SQ;
    sqe.prp1 = sq;
    sqe.cdw10 = ((NVME_IO_QD - 1) << 16) | NVME_IO_QID;
    sqe.cdw11 = NVME_SQ_PC | (NVME_IO_QID << 16);
    int r = nvme_admin_submit(&sqe);
    if(r)return r;
    g_nvme.iosq      = sq;
    g_nvme.iosq_tail = 0;
    return 0;
}

static int nvme_io_queues_setup(void){
    int r;
    if((r = nvme_create_io_cq())){ printk(PRINTK_ERR"NVMe: create io cq failed r=%d", r); return r; }
    if((r = nvme_create_io_sq())){ printk(PRINTK_ERR"NVMe: create io sq failed r=%d", r); return r; }
    g_nvme.ioq_ready = 1;
    return 0;
}

//提交一条I/O命令到SQ1/CQ1
static int nvme_io_submit(nvme_sqe_t *sqe){
    //命令ID
    g_nvme.cid++;
    if(!g_nvme.cid)g_nvme.cid++;
    sqe->cid = g_nvme.cid;
    //把SQE写进SQ1的第iosq_tail条
    uint32_t idx = g_nvme.iosq_tail & (NVME_IO_QD - 1);
    memcpy((void*)(PHYS_TO_VIRT(g_nvme.iosq) + (uint64_t)idx * sizeof(nvme_sqe_t)),sqe, sizeof(nvme_sqe_t));
    __sync_synchronize();//内存屏障
    //敲SQ1尾门铃
    g_nvme.iosq_tail++;
    NVME_WR32(NVME_DB_SQ(NVME_IO_QID), g_nvme.iosq_tail & (NVME_IO_QD - 1));
    //轮询CQ1的第iocq_head条
    volatile nvme_cqe_t *cq = (volatile nvme_cqe_t*)PHYS_TO_VIRT(g_nvme.iocq);
    uint32_t i = g_nvme.iocq_head & (NVME_IO_QD - 1);
    int r = -3;
    for(uint32_t t = 0; t < 200000; t++){
        uint16_t st = cq[i].status;
        if(st == 0xFFFF)break;//设备消失
        if((st & 1) == g_nvme.iocq_phase){
            if(cq[i].cid != sqe->cid || cq[i].sq_id != NVME_IO_QID){
                printk(PRINTK_ERR"NVMe: io cqe mismatch cid=%u want=%u sqid=%u status=0x%x",
                       cq[i].cid, sqe->cid, cq[i].sq_id, cq[i].status);
                r = -4;
            }else{
                uint32_t sc = (st >> 1) & 0xFF;//状态码
                uint32_t sct= (st >> 9) & 0x7;//状态类型
                r = (int)((sct << 8) | sc);
                if(r)printk(PRINTK_ERR"NVMe: io cmd 0x%x failed sct=%u sc=%u",sqe->opcode, sct, sc);
            }
            break;
        }
        NVME_POLL_DELAY(t);
    }
    if(r == -3)printk(PRINTK_ERR"NVMe: io cmd 0x%x timeout sqtail=%u cqhead=%u csts=0x%x",
                      sqe->opcode, g_nvme.iosq_tail, g_nvme.iocq_head, NVME_RD32(NVME_REG_CSTS));
    g_nvme.iocq_head++;
    if(g_nvme.iocq_head >= NVME_IO_QD){
        g_nvme.iocq_head = 0;
        g_nvme.iocq_phase ^= 1;
    }
    NVME_WR32(NVME_DB_CQ(NVME_IO_QID), g_nvme.iocq_head);
    return r;
}

//IDENTIFY Controller
static int nvme_identify_ctrl(uint64_t buf){
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_IDENTIFY;
    sqe.nsid   = 0;
    sqe.prp1   = buf;
    sqe.cdw10  = NVME_CNS_CTRL;
    return nvme_admin_submit(&sqe);
}
//IDENTIFY Namespace
static int nvme_identify_ns(uint32_t nsid, uint64_t buf){
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_IDENTIFY;
    sqe.nsid   = nsid;
    sqe.prp1   = buf;
    sqe.cdw10  = NVME_CNS_NS;
    return nvme_admin_submit(&sqe);
}
//读控制器信息
static int nvme_ctrl_probe(void){
    uint64_t buf = (uint64_t)Pmm_Malloc(1);
    if(!buf)return -1;
    memset((void*)PHYS_TO_VIRT(buf), 0, PAGE_SIZE);
    int r = nvme_identify_ctrl(buf);
    if(r){ printk(PRINTK_ERR"NVMe: identify ctrl failed r=%d", r); return r; }
    uint8_t *id = (uint8_t*)PHYS_TO_VIRT(buf);
    g_nvme.mdts = *(uint8_t*)(id + 77);   //单次I/O上限 = 2^mdts × 4KB
    g_nvme.nn   = *(uint32_t*)(id + 516); //命名空间数量
    //型号(offset 24, 40字节)与序列号(offset 4, 20字节), 空格填充需裁剪
    memcpy(g_nvme.mn, id + 24, 40); g_nvme.mn[40] = 0;
    memcpy(g_nvme.sn, id + 4,  20); g_nvme.sn[20] = 0;
    for(int i = 39; i >= 0 && g_nvme.mn[i] == ' '; i--)g_nvme.mn[i] = 0;
    for(int i = 19; i >= 0 && g_nvme.sn[i] == ' '; i--)g_nvme.sn[i] = 0;
    return 0;
}
//读命名空间1
static int nvme_ns_probe(void){
    uint64_t buf = (uint64_t)Pmm_Malloc(1);
    if(!buf)return -1;
    memset((void*)PHYS_TO_VIRT(buf), 0, PAGE_SIZE);
    int r = nvme_identify_ns(1, buf);
    if(r){
        printk(PRINTK_ERR"NVMe: identify ns failed r=%d", r);
        return r;
    }
    uint8_t *ns = (uint8_t*)PHYS_TO_VIRT(buf);
    uint64_t nsze  = *(uint64_t*)(ns + 0);//块数
    uint8_t  flbas = *(uint8_t*)(ns + 26) & 0x0F;//当前格式索引
    uint32_t lbaf  = *(uint32_t*)(ns + 128 + 4 * flbas);//LBA格式表
    uint32_t lbads = (lbaf >> 16) & 0xFF;//log2(块大小)
    g_nvme.nsid  = 1;
    g_nvme.nsze  = nsze;
    g_nvme.lbads = lbads;
    g_nvme.blksz = 1u << lbads;
    if(!nsze || !g_nvme.blksz)return -2;
    return 0;
}

//NVMe读写
static int nvme_read_write(int write, uint64_t slba, uint32_t nlb, uint64_t prp1, uint64_t prp2){
    if(!g_nvme.ioq_ready)return -1;
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = write ? NVME_NVM_WRITE : NVME_NVM_READ;//设置操作码
    sqe.nsid   = g_nvme.nsid;//命名空间ID
    sqe.prp1   = prp1;//数据缓冲区物理地址
    sqe.prp2   = prp2;//第二页
    sqe.cdw10  = (uint32_t)(slba & 0xFFFFFFFF);//起始LBA低32位
    sqe.cdw11  = (uint32_t)(slba >> 32);//起始LBA高32位
    sqe.cdw12  = nlb - 1;//传输的块数减1（0表示1块）
    return nvme_io_submit(&sqe);//提交命令
}

static int nvme_bringup(void){
    if(nvme_probe())return -1;
    int r;
    if((r = nvme_ctrl_reset()))       { printk(PRINTK_ERR"NVMe: reset failed r=%d", r);       return r; }
    if((r = nvme_admin_queue_setup())){ printk(PRINTK_ERR"NVMe: admin queue failed r=%d", r); return r; }
    if((r = nvme_ctrl_enable()))      { printk(PRINTK_ERR"NVMe: enable failed r=%d", r);      return r; }
    if((r = nvme_ctrl_probe()))       { printk(PRINTK_ERR"NVMe: identify ctrl failed r=%d", r); return r; }
    if((r = nvme_ns_probe()))         { printk(PRINTK_ERR"NVMe: identify ns failed r=%d", r);   return r; }
    if((r = nvme_io_queues_setup()))  { printk(PRINTK_ERR"NVMe: io queue setup failed r=%d", r); return r; }
    return 0;
}

//经disk层的传输
static int nvme_dma_xfer(int write, uint64_t lba, uint32_t count, void *buf){
    uint32_t per = (PAGE_SIZE * 2) / g_nvme.blksz;//每块最多几扇区
    uint32_t per_page = PAGE_SIZE / g_nvme.blksz;
    uintptr_t va = (uintptr_t)buf;
    //页对齐的内核高半区缓冲区: 直接当PRP用, 省掉一次拷贝
    if(va >= KERNEL_VIRTUAL_ADDR_START && !(va & (PAGE_SIZE - 1)) && count <= per){
        uint64_t phys = (uint64_t)VIRT_TO_PHYS(va);
        return nvme_read_write(write, lba, count, phys,count > per_page ? phys + PAGE_SIZE : 0);
    }
    uint64_t bounce = (uint64_t)Pmm_Malloc(2);//2页物理连续
    if(!bounce)return -1;
    uint8_t *b = (uint8_t*)PHYS_TO_VIRT(bounce);
    uint8_t *p = (uint8_t*)buf;
    int r = 0;
    while(count){
        uint32_t chunk = count > per ? per : count;
        uint32_t bytes = chunk * g_nvme.blksz;
        if(write)memcpy(b, p, bytes);
        r = nvme_read_write(write, lba, chunk, bounce, bytes > PAGE_SIZE ? bounce + PAGE_SIZE : 0);
        if(r)break;
        if(!write)memcpy(p, b, bytes);
        lba   += chunk;
        p     += bytes;
        count -= chunk;
    }
    Pmm_Free((void*)bounce, 2);
    return r;
}

//初始化
static int nvme_disk_init(disk_info_t *d){
    //完整拉起控制器
    if(!g_nvme.present){
        int r = nvme_bringup();
        if(r)return r;
    }
    //检查块大小
    if(g_nvme.blksz != 512){
        printk(PRINTK_WARNING"NVMe: blksz=%u != 512, unsupported", g_nvme.blksz);
        return -1;
    }
    //保存信息
    memcpy(d->model, g_nvme.mn, sizeof(d->model));
    d->model[40] = 0;
    d->total_sectors = g_nvme.nsze;
    d->lba48 = 1;
    printk(PRINTK_INFO"NVMe disk: %s SN=%s, %llu sectors x %u bytes",
           d->model, g_nvme.sn, (unsigned long long)d->total_sectors, g_nvme.blksz);
    return 0;
}

//读扇区
static int nvme_disk_read(disk_info_t *d, uint64_t lba, uint32_t count, void *buf){
    (void)d;
    if(!buf || !count)return 0;
    uint64_t flags;
    spin_lock_irqsave(&g_nvme_lock, flags);
    int r = nvme_dma_xfer(0, lba, count, buf);
    spin_unlock_irqrestore(&g_nvme_lock, flags);
    return r;
}

//写扇区
static int nvme_disk_write(disk_info_t *d, uint64_t lba, uint32_t count, const void *buf){
    (void)d;
    if(!buf || !count)return 0;
    void *_buf = (void*)buf;
    uint64_t flags;
    spin_lock_irqsave(&g_nvme_lock, flags);
    int r = nvme_dma_xfer(1, lba, count, _buf);
    spin_unlock_irqrestore(&g_nvme_lock, flags);
    return r;
}

//NVMe 驱动操作集
const disk_ops_t nvme_ops = {
    .name  = "NVMe",
    .init  = nvme_disk_init,
    .read  = nvme_disk_read,
    .write = nvme_disk_write,
};

//注册NVMe驱动
void NvmeRegisterDriver(void){
    spin_lock_init(&g_nvme_lock);
    DiskRegisterDriver(DISK_CTRL_NVME, &nvme_ops);
}