/**
 * drives/vmd.c
 *
 * Intel VMD: 把藏在 VMD 后面的设备(NVMe)枚举出来
 *
 * 由DeepSeek V4.1 Flash生成
 */
#include <drives/vmd.h>
#include <drives/pci.h>
#include <mm/vmm.h>
#include <mm/pgtables.h>
#include <print.h>
#include <spinlock.h>

static vmd_ctrl_t g_vmd[VMD_MAX_CTRL];
static int        g_vmd_count = 0;
static spinlock_t g_vmd_lock = {0};

#define VMD_DEBUG 0
#if VMD_DEBUG
static int g_vmd_dbg_shown = 0;
static void vmd_dbg_cb(vmd_child_t *c,void *arg){
    (void)arg;
    if(g_vmd_dbg_shown >= 12)return;//最多打 12 个, 免得刷屏
    g_vmd_dbg_shown++;
    printk(PRINTK_INFO"VMD child %02x:%02x.%x %04x:%04x class=%02x/%02x/%02x bar0=0x%llx",
           c->bus,c->dev,c->func,c->vendor,c->device,
           c->class_code,c->subclass,c->progif,
           (unsigned long long)c->bar_phys[0]);
}
#endif

int VmdCount(void){
    return g_vmd_count;
}

vmd_ctrl_t *VmdGet(int index){
    if(index < 0 || index >= g_vmd_count)return NULL;
    return &g_vmd[index];
}

//探测一个内存BAR的大小(写全1读回再恢复原值)
static uint64_t vmd_bar_size(uint8_t bus,uint8_t dev,uint8_t func,int bar_index){
    uint8_t reg = (uint8_t)(PCI_REG_BAR0 + bar_index * 4);
    uint32_t lo = PciRead32(bus,dev,func,reg);
    if(!lo || (lo & 0x1))return 0;//空BAR或IO空间
    int is64 = ((lo & 0x6) == 0x4);
    uint32_t hi = is64 ? PciRead32(bus,dev,func,(uint8_t)(reg + 4)) : 0;
    //写全1, 读回掩码
    PciWrite32(bus,dev,func,reg,0xFFFFFFFFu);
    uint32_t mask_lo = PciRead32(bus,dev,func,reg);
    uint32_t mask_hi = 0;
    if(is64){
        PciWrite32(bus,dev,func,(uint8_t)(reg + 4),0xFFFFFFFFu);
        mask_hi = PciRead32(bus,dev,func,(uint8_t)(reg + 4));
    }
    //恢复原值
    PciWrite32(bus,dev,func,reg,lo);
    if(is64)PciWrite32(bus,dev,func,(uint8_t)(reg + 4),hi);
    if(mask_lo & 0x1)return 0;
    uint64_t mask = (uint64_t)(mask_lo & 0xFFFFFFF0u);
    if(is64 && mask_hi != 0xFFFFFFFFu)mask |= ((uint64_t)mask_hi << 32);
    if(!mask)return 0;
    uint64_t size = (~mask) + 1;
    if(size & (size - 1))return 0;//不是2的幂 -> 探测不可信
    return size;
}

//取某个BAR的物理地址(64位BAR才拼高位)
static uint64_t vmd_bar_addr(uint8_t bus,uint8_t dev,uint8_t func,int bar_index,int *size_out){
    uint8_t reg = (uint8_t)(PCI_REG_BAR0 + bar_index * 4);
    uint32_t lo = PciRead32(bus,dev,func,reg);
    if(size_out)*size_out = 0;
    if(!lo || (lo & 0x1))return 0;
    if((lo & 0x6) == 0x4){
        uint32_t hi = PciRead32(bus,dev,func,(uint8_t)(reg + 4));
        return ((uint64_t)hi << 32) | (uint64_t)(lo & 0xFFFFFFF0u);
    }
    return (uint64_t)(lo & 0xFFFFFFF0u);
}

//在能力列表里找 VSCAP 的 "SHDW" 影子寄存器(虚拟机才有)
static int vmd_find_shdw(uint8_t bus,uint8_t dev,uint8_t func,uint64_t *p1,uint64_t *p2){
    uint16_t status = PciRead16(bus,dev,func,PCI_REG_STATUS);
    if(!(status & (1u << 4)))return 0;//没有能力列表
    uint8_t pos = (uint8_t)(PciRead8(bus,dev,func,0x34) & 0xFC);
    for(int guard = 0; pos && guard < 48; guard++){
        uint8_t id   = PciRead8(bus,dev,func,pos);
        uint8_t next = (uint8_t)(PciRead8(bus,dev,func,(uint8_t)(pos + 1)) & 0xFC);
        if(id == 0x09){//vendor specific
            uint32_t magic = PciRead32(bus,dev,func,(uint8_t)(pos + 4));
            if(magic == VMD_VSCAP_MAGIC_SHDW){
                uint32_t l = PciRead32(bus,dev,func,(uint8_t)(pos + 8));
                uint32_t h = PciRead32(bus,dev,func,(uint8_t)(pos + 12));
                *p1 = ((uint64_t)h << 32) | l;
                l = PciRead32(bus,dev,func,(uint8_t)(pos + 16));
                h = PciRead32(bus,dev,func,(uint8_t)(pos + 20));
                *p2 = ((uint64_t)h << 32) | l;
                return 1;
            }
        }
        if(next <= pos)break;
        pos = next;
    }
    return 0;
}

//初始化一个 VMD 控制器
static int vmd_setup(pci_device_t *d){
    if(g_vmd_count >= VMD_MAX_CTRL)return -1;
    vmd_ctrl_t *v = &g_vmd[g_vmd_count];
    int tmp;
    memset(v,0,sizeof(*v));
    v->bus = d->bus;
    v->dev = d->dev;
    v->func = d->func;
    v->vendor = d->vendor_id;
    v->device = d->device_id;
    //打开内存空间访问和总线主控
    uint16_t cmd = PciRead16(d->bus,d->dev,d->func,PCI_REG_COMMAND);
    PciWrite16(d->bus,d->dev,d->func,PCI_REG_COMMAND,(uint16_t)(cmd | 0x6));
    //先读原值, 再探大小(探测会恢复原值)
    v->cfgbar_phys   = vmd_bar_addr(d->bus,d->dev,d->func,0,&tmp);
    v->membar1_phys  = vmd_bar_addr(d->bus,d->dev,d->func,2,&tmp);
    v->membar2_phys  = vmd_bar_addr(d->bus,d->dev,d->func,4,&tmp);
    v->cfgbar_size   = vmd_bar_size(d->bus,d->dev,d->func,0);
    v->membar1_size  = vmd_bar_size(d->bus,d->dev,d->func,2);
    v->membar2_size  = vmd_bar_size(d->bus,d->dev,d->func,4);
    v->membar2_child = v->membar2_phys + VMD_MEMBAR2_RESERVED;
    //域内总线起点(VMCAP 声明了范围限制才需要问 VMCONFIG)
    uint16_t vmcap = PciRead16(d->bus,d->dev,d->func,VMD_REG_VMCAP);
    if(vmcap & VMD_VMCAP_BUS_RESTRICT){
        v->bus_restrict = 1;
        uint16_t vmcfg = PciRead16(d->bus,d->dev,d->func,VMD_REG_VMCONFIG);
        switch((vmcfg >> VMD_VMCONFIG_BUS_SHIFT) & VMD_VMCONFIG_BUS_MASK){
            case 1:  v->busn_start = 128; break;
            case 2:  v->busn_start = 224; break;
            default: v->busn_start = 0;   break;
        }
    }
    //影子寄存器(有说明跑在虚拟机里, 需要做地址翻译; 裸机没有)
    if(vmd_find_shdw(d->bus,d->dev,d->func,&v->shdw_phys1,&v->shdw_phys2)){
        v->have_rh = 1;
        v->offset1 = v->membar1_phys - (v->shdw_phys1 & ~0xFULL);
        v->offset2 = v->membar2_phys - (v->shdw_phys2 & ~0xFULL);
    }
    //域内总线数 = CFGBAR 大小 / 1MB
    uint64_t sz = v->cfgbar_size;
    if(!sz || sz < (1u << 20) || sz > (256u << 20) || (sz & (sz - 1)))sz = 32u << 20;
    v->nbus = (uint8_t)(sz >> 20);
    if(!v->nbus)v->nbus = 1;
    v->cfgbar_size = sz;
    //配置窗口按总线惰性映射, 这里只记虚拟基址
    v->cfgbar = (volatile uint8_t*)PHYS_TO_VIRT((uintptr_t)v->cfgbar_phys);
    //MEMBAR1 先映射 4KB: 将来做 MSI 重映射/影子寄存器要用
    if(v->membar1_phys){
        vmm_map_page(KERNEL_PML4,PHYS_TO_VIRT(v->membar1_phys),v->membar1_phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE);
    }
    v->present = 1;
    g_vmd_count++;
    printk(PRINTK_INFO"VMD %02x:%02x.%x %04x:%04x cfg=0x%llx/%uMB mem1=0x%llx mem2=0x%llx bus=%u-%u%s",
           d->bus,d->dev,d->func,d->vendor_id,d->device_id,
           (unsigned long long)v->cfgbar_phys,(unsigned)(sz >> 20),
           (unsigned long long)v->membar1_phys,(unsigned long long)v->membar2_phys,
           (unsigned)v->busn_start,(unsigned)(v->busn_start + v->nbus - 1),
           v->have_rh ? " [shdw:VM]" : "");
    return 0;
}

static void vmd_scan_cb(pci_device_t *d,void *arg){
    int *count = (int*)arg;
    if(d->class_code != PCI_CLASS_MASS_STORAGE)return;
    if(d->subclass != PCI_SUBCLASS_VMD)return;
    if(vmd_setup(d) == 0 && count)(*count)++;
}

int VmdInitAll(void){
    if(g_vmd_count)return g_vmd_count;//已经初始化过
    int count = 0;
    PciScanAll(vmd_scan_cb,&count);
#if VMD_DEBUG
    if(count)printk(PRINTK_INFO"VMD: %d children in total",VmdScanChildren(vmd_dbg_cb,NULL));
#endif
    return count;
}

//把域内某条总线的 1MB 配置窗口映射进来(首次访问时做)
static int vmd_map_bus(vmd_ctrl_t *v,int busnr){
    if(busnr < 32 && (v->bus_map_mask & (1u << busnr)))return 0;//已经映射过
    uint64_t off = (uint64_t)busnr << 20;
    if(off + (1u << 20) > v->cfgbar_size)return -1;
    for(uint64_t p = 0; p < (1u << 20); p += PAGE_SIZE){
        uintptr_t pa = (uintptr_t)(v->cfgbar_phys + off + p);
        vmm_map_page(KERNEL_PML4,PHYS_TO_VIRT(pa),pa,
                     PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE);
    }
    if(busnr < 32)v->bus_map_mask |= (1u << busnr);
    return 0;
}

//域内设备配置空间指针
static volatile uint8_t *vmd_cfg_ptr(int idx,uint8_t bus,uint8_t dev,uint8_t func,uint8_t reg){
    vmd_ctrl_t *v = VmdGet(idx);
    if(!v || !v->cfgbar)return NULL;
    if(bus < v->busn_start)return NULL;
    uint8_t bn = (uint8_t)(bus - v->busn_start);
    if(bn >= v->nbus)return NULL;
    if(vmd_map_bus(v,bn))return NULL;
    uint32_t off = ((uint32_t)bn << 20) | ((uint32_t)(dev & 0x1F) << 15) | ((uint32_t)(func & 0x7) << 12) | reg;
    return v->cfgbar + off;
}

uint32_t VmdRead32(int idx,uint8_t bus,uint8_t dev,uint8_t func,uint8_t reg){
    uint64_t flags;
    spin_lock_irqsave(&g_vmd_lock,flags);
    volatile uint8_t *p = vmd_cfg_ptr(idx,bus,dev,func,reg);
    uint32_t r = p ? *(volatile uint32_t*)p : 0xFFFFFFFFu;
    spin_unlock_irqrestore(&g_vmd_lock,flags);
    return r;
}

uint16_t VmdRead16(int idx,uint8_t bus,uint8_t dev,uint8_t func,uint8_t reg){
    uint64_t flags;
    spin_lock_irqsave(&g_vmd_lock,flags);
    volatile uint8_t *p = vmd_cfg_ptr(idx,bus,dev,func,reg);
    uint16_t r = p ? *(volatile uint16_t*)p : 0xFFFFu;
    spin_unlock_irqrestore(&g_vmd_lock,flags);
    return r;
}

uint8_t VmdRead8(int idx,uint8_t bus,uint8_t dev,uint8_t func,uint8_t reg){
    uint64_t flags;
    spin_lock_irqsave(&g_vmd_lock,flags);
    volatile uint8_t *p = vmd_cfg_ptr(idx,bus,dev,func,reg);
    uint8_t r = p ? *p : 0xFFu;
    spin_unlock_irqrestore(&g_vmd_lock,flags);
    return r;
}

//★写必须回读: VMD 会把配置写变成 posted write, 回读才保证真正写完
void VmdWrite32(int idx,uint8_t bus,uint8_t dev,uint8_t func,uint8_t reg,uint32_t val){
    uint64_t flags;
    spin_lock_irqsave(&g_vmd_lock,flags);
    volatile uint8_t *p = vmd_cfg_ptr(idx,bus,dev,func,reg);
    if(p){ *(volatile uint32_t*)p = val; (void)*(volatile uint32_t*)p; }
    spin_unlock_irqrestore(&g_vmd_lock,flags);
}

void VmdWrite16(int idx,uint8_t bus,uint8_t dev,uint8_t func,uint8_t reg,uint16_t val){
    uint64_t flags;
    spin_lock_irqsave(&g_vmd_lock,flags);
    volatile uint8_t *p = vmd_cfg_ptr(idx,bus,dev,func,reg);
    if(p){ *(volatile uint16_t*)p = val; (void)*(volatile uint16_t*)p; }
    spin_unlock_irqrestore(&g_vmd_lock,flags);
}

void VmdWrite8(int idx,uint8_t bus,uint8_t dev,uint8_t func,uint8_t reg,uint8_t val){
    uint64_t flags;
    spin_lock_irqsave(&g_vmd_lock,flags);
    volatile uint8_t *p = vmd_cfg_ptr(idx,bus,dev,func,reg);
    if(p){ *p = val; (void)*p; }
    spin_unlock_irqrestore(&g_vmd_lock,flags);
}

int VmdScanChildren(vmd_child_cb cb,void *arg){
    int found = 0;
    for(int i = 0;i < g_vmd_count;i++){
        vmd_ctrl_t *v = &g_vmd[i];
        for(int b = 0;b < v->nbus;b++){
            uint8_t bus = (uint8_t)(v->busn_start + b);
            for(int d = 0;d < 32;d++){
                int nfunc = 1;
                for(int f = 0;f < nfunc;f++){
                    uint32_t id = VmdRead32(i,bus,(uint8_t)d,(uint8_t)f,PCI_REG_VENDOR);
                    uint16_t vendor = (uint16_t)(id & 0xFFFF);
                    //空槽: VMD 会读回 0xFFFF / 0x0000 / 0xFFFFFFFF
                    if(vendor == 0xFFFF || vendor == 0 || id == 0xFFFFFFFFu)continue;
                    if(f == 0){
                        uint8_t hdr = VmdRead8(i,bus,(uint8_t)d,0,PCI_REG_HDRTYPE);
                        if(hdr & 0x80)nfunc = 8;//多功能设备要扫全部 function
                    }
                    vmd_child_t c;
                    memset(&c,0,sizeof(c));
                    c.ctrl_index = i;
                    c.bus = bus;
                    c.dev = (uint8_t)d;
                    c.func = (uint8_t)f;
                    c.vendor = vendor;
                    c.device = (uint16_t)(id >> 16);
                    uint32_t cc = VmdRead32(i,bus,(uint8_t)d,(uint8_t)f,PCI_REG_REVISION);
                    c.progif     = (uint8_t)(cc >> 8);
                    c.subclass   = (uint8_t)(cc >> 16);
                    c.class_code = (uint8_t)(cc >> 24);
                    for(int k = 0;k < VMD_MAX_BARS;k++){
                        c.bar_raw[k] = VmdRead32(i,bus,(uint8_t)d,(uint8_t)f,(uint8_t)(PCI_REG_BAR0 + k * 4));
                    }
                    //换算物理地址: 64位BAR吃掉下一个BAR槽
                    for(int k = 0;k < VMD_MAX_BARS;k++){
                        uint32_t lo = c.bar_raw[k];
                        if(!lo || (lo & 0x1))continue;//空BAR或IO空间
                        if((lo & 0x6) == 0x4){
                            uint64_t hi = (k + 1 < VMD_MAX_BARS) ? c.bar_raw[k + 1] : 0;
                            c.bar_phys[k] = (hi << 32) | (uint64_t)(lo & 0xFFFFFFF0u);
                            if(k + 1 < VMD_MAX_BARS)c.bar_phys[k + 1] = 0;
                            k++;
                        }else{
                            c.bar_phys[k] = (uint64_t)(lo & 0xFFFFFFF0u);
                        }
                    }
                    if(cb)cb(&c,arg);
                    found++;
                }
            }
        }
    }
    return found;
}

static void vmd_find_nvme_cb(vmd_child_t *c,void *arg){
    vmd_child_t *out = (vmd_child_t*)arg;
    if(out->vendor)return;//已经找到了
    if(c->class_code != PCI_CLASS_MASS_STORAGE)return;
    if(c->subclass != PCI_SUBCLASS_NVME)return;
    *out = *c;
    //打开子设备的内存空间访问和总线主控(要走 VMD 的配置窗口)
    uint16_t cmd = VmdRead16(c->ctrl_index,c->bus,c->dev,c->func,PCI_REG_COMMAND);
    VmdWrite16(c->ctrl_index,c->bus,c->dev,c->func,PCI_REG_COMMAND,(uint16_t)(cmd | 0x6));
}

int VmdFindNvme(vmd_child_t *out){
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!g_vmd_count)return -1;
    VmdScanChildren(vmd_find_nvme_cb,out);
    if(!out->vendor)return -1;
    if(!out->bar_phys[0])return -2;//BAR 没配好(BIOS 正常会配)
    return 0;
}
