#include <mm/pmm.h>
#include <print.h>

OS_MEMORY_DESCRIPTOR* MemDescAddr = NULL;
int MemDescNum = 0;
uint64_t MemDescSize = 0;

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
    if(Desc->PhysicalStart == (uintptr_t)MemDescAddr) return false;
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
    //设置位置 —— KernelAddress 替代旧 KernelStartAddress
    MemDescAddr = (OS_MEMORY_DESCRIPTOR*)(SYSTEM_BootParam->KernelAddress+SYSTEM_BootParam->KernelSize);
    MemDescAddr = (OS_MEMORY_DESCRIPTOR*)(((uintptr_t)MemDescAddr + PAGE_SIZE - 1)& ~(PAGE_SIZE - 1));
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
        if(n == 0){
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
    for(uint32_t i = 0;i < MemDescNum;i ++){
        OS_MEMORY_DESCRIPTOR* memdesc = &MemDescAddr[i];
        if(!memdesc)continue;
        BitmapInit(&memdesc->bitmap,(uint8_t *)((uintptr_t)MemDescAddr+desc_size+bm_size),memdesc->PageSize,PMM_FREE);
        bm_size += ((memdesc->PageSize + 7)& ~7) / 8;
    }
    MemDescSize = desc_size + bm_size;
    //将描述符空间标记为已占用
    uint64_t remain_allocsize = MemDescSize;//剩余需分配大小
    uintptr_t allocaddr = (uintptr_t)MemDescAddr;//分配区域的地址
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
    print_ok();
    early_printk("Physical Memory Manager initialization successful\n");
    return 0;
}

uint64_t GetMemoryTotalSize() {
    uint64_t total = 0;
    for(int i = 0;i < MemDescNum;i ++) total += MemDescAddr[i].PageSize * PAGE_SIZE;
    return total;
}

void* Pmm_Malloc(int pages) {
    if(pages <= 0)return NULL;
    for(int i = 0;i < MemDescNum;i ++){
        OS_MEMORY_DESCRIPTOR* desc = &MemDescAddr[i];
        int index = BitmapAllocBits(&desc->bitmap,PMM_FREE,pages);
        if(index >= 0)return (void*)(desc->Address + index * PAGE_SIZE);
    }
    return NULL;
}

void Pmm_Free(void* addr,int pages) {
    if(pages <= 0)return;
    OS_MEMORY_DESCRIPTOR* desc = NULL;
    uint64_t offset = 0;
    find_addr_in_bitmap((uintptr_t)addr,&desc,&offset);
    if(!desc)return;
    BitmapSetBits(&desc->bitmap,offset,pages,PMM_FREE);
}
