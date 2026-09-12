#include <mm/pmm.h>
#include <mm/vmm.h>
#include <print.h>
#include <boot.h>

OS_MEMORY_DESCRIPTOR* MemDescAddr = NULL;
int MemDescNum = 0;
uint64_t MemDescSize = 0;
static uintptr_t memdesc_phys_base = 0;//MemDesc 区域物理基址（用于与 UEFI 物理地址比较）

//检查内存映射
static int CheckMemoryMap(UEFI_MEMORY_MAP* MemoryMap){
    int status = 0;
    if(!MemoryMap->Buffer)status ++;
    if(MemoryMap->MapSize == 0)status ++;
    if(MemoryMap->DescriptorSize == 0)status ++;
    if(MemoryMap->DescriptorVersion == 0)status ++;
    if(!SYSTEM_BootParam->KernelStackAddress)status ++;
    return status;
}

static _Bool IsMemoryAvailable(UEFI_MEMORY_DESCRIPTOR *Desc) {
    UEFI_MEMORY_MAP* MemoryMap = SYSTEM_MemoryMap;
    if(Desc->PhysicalStart == memdesc_phys_base) return false;
    if((Desc->Type==CONVENTIONAL_MEMORY) || (Desc->Type==BOOT_SERVICES_CODE) || (Desc->Type==BOOT_SERVICES_DATA) || (Desc->Type==LOADER_CODE)) return true;
    return false;
}

void find_addr_in_bitmap(uintptr_t addr,OS_MEMORY_DESCRIPTOR **desc,uint64_t *offset){
    uintptr_t page_addr = ((addr >> 12) << 12);//转化为整页地址
    *desc = NULL;
    *offset = 0;
    //遍历描述符，寻找地址所在的描述符
    for(int i = 0; i < MemDescNum; i++) {
        OS_MEMORY_DESCRIPTOR *current = &MemDescAddr[i];
        uintptr_t start = current->Address;
        uintptr_t end = current->Address + current->PageSize * PAGE_SIZE;
        if(start <= page_addr && page_addr < end) {
            *desc = current;
            *offset = (page_addr - start) / PAGE_SIZE;
            return;
        }
    }
}

//获取某个内存描述符
UEFI_MEMORY_DESCRIPTOR* AnalysisMemoryMap(const uint32_t num){
    UEFI_MEMORY_MAP* MemoryMap = SYSTEM_MemoryMap;
    //注意：仅在 PMM 初始化期调用（高半映射未建立，UEFI identity 映射下直接用物理地址）
    UEFI_MEMORY_DESCRIPTOR* DescriptorAddr = (UEFI_MEMORY_DESCRIPTOR*)((uint8_t*)MemoryMap->Buffer + MemoryMap->DescriptorSize * num);//描述符的位置
    uint32_t maxDescriptors = MemoryMap->MapSize / MemoryMap->DescriptorSize;//计算描述符数量
    if (num >= maxDescriptors)return NULL;
    return DescriptorAddr;
}

int Init_Physical_Memory_Manager() {
    //检查内存映射
    if (CheckMemoryMap(SYSTEM_MemoryMap) != 0) {
        print_error();
        early_printk("Physical Memory Manager initialization failed: UEFI Memory Map not found\n");
        return 1;
    }
    memdesc_phys_base = SYSTEM_BootParam->KernelAddress + SYSTEM_BootParam->KernelSize;
    memdesc_phys_base = (memdesc_phys_base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    MemDescAddr = (OS_MEMORY_DESCRIPTOR*)memdesc_phys_base;
    if (!MemDescAddr) {
        print_error();
        early_printk("Physical Memory Manager initialization failed: UEFI Memory Desc not found\n");
        return 2;
    }
    //遍历EFI描述符,创建物理内存管理描述符
    uint32_t desc_count = SYSTEM_MemoryMap->MapSize / SYSTEM_MemoryMap->DescriptorSize;
    for(uint32_t n = 0;n < desc_count;n ++){
        UEFI_MEMORY_DESCRIPTOR* curr_efidesc = AnalysisMemoryMap(n);
        if(!curr_efidesc)continue;
        if(!IsMemoryAvailable(curr_efidesc))continue;
        //第一块，直接创建
        if(MemDescNum == 0){
            MemDescAddr[MemDescNum].Address = curr_efidesc->PhysicalStart;
            MemDescAddr[MemDescNum].PageSize = curr_efidesc->NumberOfPages;
            MemDescNum ++;
        }else{
            //如果内存连续,合并
            if((MemDescAddr[MemDescNum-1].Address + MemDescAddr[MemDescNum-1].PageSize * PAGE_SIZE) == curr_efidesc->PhysicalStart){
                MemDescAddr[MemDescNum-1].PageSize += curr_efidesc->NumberOfPages;
            //否则创建下一个
            }else{
                MemDescAddr[MemDescNum].Address = curr_efidesc->PhysicalStart;
                MemDescAddr[MemDescNum].PageSize = curr_efidesc->NumberOfPages;
                MemDescNum ++;
            }
        }
    }
    //分配位图并计算描述符大小+位图大小
    uint64_t desc_size = sizeof(OS_MEMORY_DESCRIPTOR) * MemDescNum;
    uint64_t bm_size = 0;
    uint64_t total_pages = 0;
    for(uint32_t i = 0;i < MemDescNum;i ++){
        OS_MEMORY_DESCRIPTOR* memdesc = &MemDescAddr[i];
        if(!memdesc)continue;
        //预留区内存未被bss清零,refs字段可能是垃圾值,必须显式初始化
        memdesc->refs = NULL;
        BitmapInit(&memdesc->bitmap,(uint8_t *)((uintptr_t)MemDescAddr+desc_size+bm_size),memdesc->PageSize,PMM_FREE);
        bm_size += ((memdesc->PageSize + 7)& ~7) / 8;
        total_pages += memdesc->PageSize;
    }
    MemDescSize = desc_size + bm_size;
    //将描述符空间标记为已占用
    uint64_t remain_allocsize = MemDescSize;//剩余需分配大小
    uintptr_t allocaddr = memdesc_phys_base;//分配区域的地址（物理，用于位图查找）
    uint32_t desc_pagesize = (((remain_allocsize + 4095) / 4096) * 4096) / PAGE_SIZE;//分配区域页数
    while(desc_pagesize > 0){
        OS_MEMORY_DESCRIPTOR* desc_bmaddr_desc = NULL;//分配区域的描述符
        uint64_t desc_bmaddr_bit = 0;//分配区域的位图偏移
        find_addr_in_bitmap(allocaddr,&desc_bmaddr_desc,&desc_bmaddr_bit);//获取描述符和位图偏移
        if(!desc_bmaddr_desc)break;
        //如果剩余空间大于等于需要分配的空间，分配需要分配的空间
        if(desc_bmaddr_desc->bitmap.bit_size-desc_bmaddr_bit >= desc_pagesize){
            uint32_t allocsize = desc_pagesize;
            BitmapSetBits(&desc_bmaddr_desc->bitmap,desc_bmaddr_bit,allocsize,PMM_USED);
            desc_pagesize -= allocsize;
            allocaddr += allocsize * PAGE_SIZE;
        //否则先分配剩余空间
        }else{
            uint32_t allocsize = desc_bmaddr_desc->bitmap.bit_size-desc_bmaddr_bit;
            BitmapSetBits(&desc_bmaddr_desc->bitmap,desc_bmaddr_bit,allocsize,PMM_USED);
            desc_pagesize -= allocsize;
            allocaddr += allocsize * PAGE_SIZE;
        }
    }
    for(uint32_t i = 0;i < MemDescNum;i ++){
        OS_MEMORY_DESCRIPTOR* desc = &MemDescAddr[i];
        uintptr_t desc_end = desc->Address + desc->PageSize * PAGE_SIZE;
        if(desc->Address >= 0x100000)break;
        uintptr_t ov_start = desc->Address > 0 ? desc->Address : 0;
        uintptr_t ov_end = desc_end < 0x100000 ? desc_end : 0x100000;
        if(ov_start >= ov_end)continue;
        uint32_t start_bit = (ov_start - desc->Address) / PAGE_SIZE;
        uint32_t page_count = (ov_end - ov_start) / PAGE_SIZE;
        BitmapSetBits(&desc->bitmap,start_bit,page_count,PMM_USED);
    }
    //标记内核与内核栈区域为已占用
    {
        uintptr_t starts[2];
        uint64_t sizes[2];
        starts[0] = SYSTEM_BootParam->KernelAddress;
        sizes[0]  = SYSTEM_BootParam->KernelSize;
        starts[1] = SYSTEM_BootParam->KernelStackAddress;
        sizes[1]  = SYSTEM_BootParam->KernelStackSize;
        for(int r = 0; r < 2; r ++){
            uintptr_t addr = starts[r];
            uint64_t left = sizes[r];
            if(!addr || !left)continue;
            while(left > 0){
                OS_MEMORY_DESCRIPTOR *d = NULL;
                uint64_t off = 0;
                find_addr_in_bitmap(addr, &d, &off);
                if(!d)break;//不在可管理内存内
                uint64_t remain = (uint64_t)(d->bitmap.bit_size - off) * PAGE_SIZE;
                uint64_t chunk = left < remain ? left : remain;
                if(chunk == 0)break;
                uint32_t pages = (uint32_t)(chunk / PAGE_SIZE);
                if(pages == 0)break;
                BitmapSetBits(&d->bitmap, (uint32_t)off, pages, PMM_USED);
                left -= pages * (uint64_t)PAGE_SIZE;
                addr += pages * (uint64_t)PAGE_SIZE;
            }
        }
    }
    //分配每页引用计数数组
    uint64_t refs_pages = (total_pages * sizeof(uint32_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t* refs_base = (uint32_t*)Pmm_Malloc((int)refs_pages);
    if(refs_base){
        memset(refs_base, 0, total_pages * sizeof(uint32_t));
        uint32_t* refs_cur = refs_base;
        for(uint32_t i = 0;i < MemDescNum;i ++){
            MemDescAddr[i].refs = refs_cur;
            refs_cur += MemDescAddr[i].PageSize;
        }
    }
    print_ok();
    early_printk("Physical Memory Manager initialization successful\n");
    return 0;
}

uint64_t GetMemoryTotalSize() {
    uint64_t total = 0;
    for(int i = 0;i < MemDescNum;i ++) total += MemDescAddr[i].PageSize * PAGE_SIZE;
    return total;
}

/*
 * PmmSwitchToHigh — 内核高半映射建立后，将 PMM 描述符/位图指针切换为高半地址。
 * 此后代码可能运行在用户页表下（低半区无映射），必须经高半访问。
 * 由 InitKernelMapping 末尾调用。
 */
void PmmSwitchToHigh(void){
    if (!MemDescAddr) return;
    //先转换各描述符内的位图指针（当前 MemDescAddr 仍为物理地址，identity 映射下可读）
    for (int i = 0; i < MemDescNum; i++) {
        if (MemDescAddr[i].bitmap.bits)
            MemDescAddr[i].bitmap.bits = (uint8_t*)PHYS_TO_VIRT((uintptr_t)MemDescAddr[i].bitmap.bits);
        if (MemDescAddr[i].refs)
            MemDescAddr[i].refs = (uint32_t*)PHYS_TO_VIRT((uintptr_t)MemDescAddr[i].refs);
    }
    //最后转换描述符数组基址
    MemDescAddr = (OS_MEMORY_DESCRIPTOR*)PHYS_TO_VIRT((uintptr_t)MemDescAddr);
}

static spinlock_t g_pmm_lock = {0};//全局物理内存锁

void* Pmm_Malloc(int pages) {
    uint64_t flags;
    spin_lock_irqsave(&g_pmm_lock, flags);
    void *r = NULL;
    if(pages > 0){
        for(int i = 0;i < MemDescNum;i ++){
            OS_MEMORY_DESCRIPTOR* desc = &MemDescAddr[i];
            int index = BitmapAllocBits(&desc->bitmap,PMM_FREE,pages);
            if(index >= 0){
                if(desc->refs)for(int j = 0;j < pages;j ++)desc->refs[index + j] = 1;
                r = (void*)(desc->Address + index * PAGE_SIZE);
                break;
            }
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return r;
}

//增加物理页引用计数
void Pmm_RefInc(void* addr){
    uint64_t flags;
    spin_lock_irqsave(&g_pmm_lock, flags);
    OS_MEMORY_DESCRIPTOR* desc = NULL;
    uint64_t offset = 0;
    find_addr_in_bitmap((uintptr_t)addr,&desc,&offset);
    if(desc && desc->refs){
        desc->refs[offset] ++;
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void Pmm_Free(void* addr,int pages) {
    uint64_t flags;
    spin_lock_irqsave(&g_pmm_lock, flags);
    if(pages > 0){
        OS_MEMORY_DESCRIPTOR* desc = NULL;
        uint64_t offset = 0;
        find_addr_in_bitmap((uintptr_t)addr,&desc,&offset);
        if(desc){
            for(int i = 0;i < pages;i ++){
                uint64_t bit = offset + i;
                if(!desc->refs || desc->refs[bit] == 0){
                    BitmapSetBits(&desc->bitmap,bit,1,PMM_FREE);//无计数信息则直接释放
                    continue;
                }
                desc->refs[bit] --;//引用-1
                if(desc->refs[bit] == 0)BitmapSetBits(&desc->bitmap,bit,1,PMM_FREE);//归零才真正释放
            }
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}
