#include <mm/vmm.h>
#include <mm/pgtables.h>
#include <mm/pmm.h>
#include <boot.h>
#include <print.h>
#include <task.h>

void InitKernelMapping() {
    //遍历所有物理内存描述符，逐页映射
    BootParam *bp = SYSTEM_BootParam;
    int desc_count = bp->MemoryInfo.MapSize / bp->MemoryInfo.DescriptorSize;
    for(int i = 0;i < desc_count;i ++){
        UEFI_MEMORY_DESCRIPTOR* desc =(UEFI_MEMORY_DESCRIPTOR*)(bp->MemoryInfo.Buffer + i * bp->MemoryInfo.DescriptorSize);
        if((!desc) || (!desc->PhysicalStart))continue;
        uint32_t type = desc->Type;
        if((type != CONVENTIONAL_MEMORY) && (type != BOOT_SERVICES_CODE) && (type != BOOT_SERVICES_DATA) && (type != RUNTIME_SERVICES_CODE) && (type != RUNTIME_SERVICES_DATA)\
        && (type != LOADER_CODE) && (type != LOADER_DATA) && (type != ACPI_RECLAIM_MEMORY))continue;
        uintptr_t paddr = desc->PhysicalStart;
        uintptr_t vaddr = PHYS_TO_VIRT(paddr);
        uint64_t pages = desc->NumberOfPages;
        for(uint64_t j = 0;j < pages;j ++){
            uintptr_t pa = paddr + j * PAGE_SIZE;
            uintptr_t va = vaddr + j * PAGE_SIZE;
            vmm_map_page(KERNEL_PML4, va, pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
        }
    }
    //再次逐页映射可用物理内存
    for (int i = 0; i < MemDescNum; i++) {
        uintptr_t paddr = MemDescAddr[i].Address;//获取地址
        uint64_t pages = MemDescAddr[i].PageSize;//获取页数量
        //逐页映射
        for(uint64_t j = 0; j < pages; j++) {
            uintptr_t pa = paddr + j * PAGE_SIZE;
            vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(pa), pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
        }
    }
    //映射内核
    if (bp && bp->KernelAddress && bp->KernelSize) {
        uint64_t k_pages = (bp->KernelSize + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t j = 0; j < k_pages; j++) {
            uintptr_t pa = bp->KernelAddress + j * PAGE_SIZE;
            vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(pa), pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
        }
    }
    //映射内核栈
    if (bp && bp->KernelStackAddress && bp->KernelStackSize) {
        uint64_t s_pages = (bp->KernelStackSize + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t j = 0; j < s_pages; j++) {
            uintptr_t pa = bp->KernelStackAddress + j * PAGE_SIZE;
            vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(pa), pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
        }
    }
    //映射帧缓冲
    if (bp && bp->ScreenInfo.FrameBuffer && bp->ScreenInfo.FrameBuffer_Size) {
        uintptr_t fb_phys = (uintptr_t)bp->ScreenInfo.FrameBuffer & PAGE_MASK;
        uint64_t fb_pages = (bp->ScreenInfo.FrameBuffer_Size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t j = 0; j < fb_pages; j++) {
            uintptr_t pa = fb_phys + j * PAGE_SIZE;
            vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(pa), pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
        }
        SYSTEM_FrameBuffer = (uint32_t*)PHYS_TO_VIRT((uintptr_t)bp->ScreenInfo.FrameBuffer);//更新帧缓冲区地址
    }
    //映射LAPIC/IOAPIC MMIO到高半
    vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(0xFEE00000), 0xFEE00000, PTE_PRESENT | PTE_WRITABLE);
    vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(0xFEC00000), 0xFEC00000, PTE_PRESENT | PTE_WRITABLE);
    PmmSwitchToHigh();//设置PMM到高半区
    kernel_high_ready = 1;//高半区映射建立完成
}

__attribute__((noinline, naked))
void switch_kernel_stack_to_high(void) {
    __asm__ volatile(
        "mov %%rsp, %%rax\n\t"
        "mov $0xFFFF800000000000, %%rbx\n\t"
        "add %%rbx, %%rax\n\t"
        "mov (%%rsp), %%rcx\n\t"
        "sub $8, %%rax\n\t"
        "mov %%rcx, (%%rax)\n\t"
        "mov %%rax, %%rsp\n\t"
        "and $0xFFFFFFFFFFFFFFF0, %%rsp\n\t"
        "ret\n\t"
        :
        :
        : "rax", "rbx", "rcx", "memory"
    );
}

void switch_kernel_info_to_high(void) {
    uintptr_t old_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(old_rsp));//获取栈
    uintptr_t new_rsp = PHYS_TO_VIRT(old_rsp);
    //确保当前 RSP 所在页及前一页在高地址已映射（栈向下增长）
    uintptr_t cur_page = old_rsp & PAGE_MASK;
    for (int i = 0; i < 2; i++) {
        uintptr_t pa = cur_page - i * PAGE_SIZE;
        uintptr_t *pte = (uintptr_t*)get_pte(KERNEL_PML4, PHYS_TO_VIRT(pa), 0, 0);
        if (!pte || !pte_is_present(*pte)) {
            vmm_map_page(KERNEL_PML4, PHYS_TO_VIRT(pa), pa, PTE_PRESENT | PTE_WRITABLE | PTE_CAN_COVERED);
        }
    }
    current_task->context.rsp = new_rsp;
}

//在head后面插入节点（头插法）
static inline void list_add(struct list_node *node, struct list_node *head) {
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
}
//在head前面插入节点（尾插法）
static inline void list_add_tail(struct list_node *node, struct list_node *head) {
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}
//删除节点
static inline void list_del(struct list_node *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = NULL;
    node->next = NULL;
}
//判断链表是否为空
static inline _Bool list_empty(struct list_node *head) {
    return head->next == head;
}
//判断节点是否在链表中（是否被删除）
static inline _Bool list_has_node(struct list_node *node) {
    return node->prev != NULL && node->next != NULL;
}
#define offsetof(type, member) ((size_t)&(((type*)0)->member))
#define container_of(ptr, type, member) ((type*)((char*)(ptr) - offsetof(type, member)))
#define list_for_each(pos, head) for(pos = (head)->next; pos != (head); pos = pos->next) //正向遍历（不安全删除）
#define list_for_each_safe(pos, tmp, head) for(pos = (head)->next, tmp = pos->next;pos != (head);pos = tmp, tmp = pos->next) //正向遍历（安全删除）
#define list_for_each_prev(pos, head) for(pos = (head)->prev; pos != (head); pos = pos->prev) //反向遍历（不安全删除）
#define list_for_each_entry(pos, head, member) \
    for(pos = container_of((head)->next, typeof(*pos), member); &pos->member != (head); pos = container_of(pos->member.next, typeof(*pos), member))
#define list_for_each_entry_safe(pos, tmp, head, member) \
    for(pos = container_of((head)->next, typeof(*pos), member), tmp = container_of(pos->member.next, typeof(*tmp), member); &pos->member != (head); pos = tmp, tmp = container_of(tmp->member.next, typeof(*tmp), member))

mm_struct* vmm_create_address_space(void){
    //分配结构体
    uintptr_t mms_addr = PHYS_TO_VIRT(Pmm_Malloc(1));
    if(!mms_addr)return NULL;
    mm_struct* mm = (mm_struct*)mms_addr;
    memset(mm, 0, sizeof(mm_struct));
    //分配PML4
    uintptr_t pml4_phys = (uintptr_t)Pmm_Malloc(1);
    if(!pml4_phys){
        Pmm_Free((void*)VIRT_TO_PHYS(mms_addr),1);
        return NULL;
    }
    uintptr_t* pml4 = (uintptr_t*)PHYS_TO_VIRT(pml4_phys);
    memset(pml4,0,PAGE_SIZE);
    //复制内核高半区映射
    uintptr_t* kernel_pml4 = (uintptr_t*)PHYS_TO_VIRT((uintptr_t)KERNEL_PML4);
    for(int i = 256;i < 512;i++)pml4[i] = kernel_pml4[i];
    //初始化结构体
    mm->pgd = (pml4_t*)pml4_phys;//保存物理地址，用于加载 CR3
    mm->mm_users = 1;//当前任务
    mm->mm_count = 1;//初始引用计数
    spin_lock_init(&mm->mm_lock);
    mm->mmap.next = &mm->mmap;
    mm->mmap.prev = &mm->mmap;
    mm->mmap_cache = NULL;
    //设置用户空间布局
    mm->start_code = 0x400000;
    mm->end_code   = 0x400000;
    mm->start_data = 0x600000;
    mm->end_data   = 0x600000;
    mm->start_brk  = 0x700000;
    mm->brk        = 0x700000;
    mm->start_stack = 0x7FFFFFFFF000ULL;
    mm->total_vm = 0;
    mm->rss = 0;
    return mm;
}

void mmget(mm_struct *mm){
    if (!mm) return;
    spin_lock(&mm->mm_lock);
    mm->mm_count++;
    spin_unlock(&mm->mm_lock);
}

//减少mm_count，归零时释放地址空间
static void mmdrop(mm_struct *mm) {
    if (!mm) return;
    spin_lock(&mm->mm_lock);
    if (--mm->mm_count == 0) {
        spin_unlock(&mm->mm_lock);
        vmm_destroy_address_space(mm);
        return;
    }
    spin_unlock(&mm->mm_lock);
}

void mmput(mm_struct *mm){
    if (!mm) return;
    spin_lock(&mm->mm_lock);
    if (mm->mm_users > 0)mm->mm_users--;//减少用户计数
    //如果还有用户，直接返回
    if (mm->mm_users > 0) {
        spin_unlock(&mm->mm_lock);
        return;
    }
    spin_unlock(&mm->mm_lock);
    mmdrop(mm);
}

//递归释放用户空间页表
static void vmm_free_user_pagetable(uint64_t table_phys, int level) {
    if (!table_phys) return;
    uint64_t *table = (uint64_t*)PHYS_TO_VIRT(table_phys);
    for(int i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & PTE_PRESENT)) continue;
        uint64_t child_phys = pte_get_paddr(entry);
        if (level < 3 && !(entry & PTE_HUGE)) {
            vmm_free_user_pagetable(child_phys, level + 1);
        } else {
            Pmm_Free((void*)child_phys, 1);
        }
    }
    Pmm_Free((void*)table_phys, 1);
}

void vmm_destroy_address_space(mm_struct *mm){
    if (!mm) return;
    if(mm->pgd) {
        uintptr_t *pml4 = (uintptr_t*)PHYS_TO_VIRT(mm->pgd);
        //遍历用户空间
        for(int i = 0; i < 256; i++) {
            uintptr_t entry = pml4[i];
            if(!(entry & PTE_PRESENT))continue;//检查页是否存在
            uint64_t pdpt_phys = pte_get_paddr(entry);//获取PDPT
            vmm_free_user_pagetable(pdpt_phys, 1);//递归释放PDPT及其子页表
            pml4[i] = 0;//清除PML4表项
        }
        //释放 PML4 页本身
        Pmm_Free((void*)mm->pgd, 1);
        mm->pgd = NULL;
    }
    Pmm_Free((void*)VIRT_TO_PHYS(mm),1);//释放mm_struct本身
}

static void pte_set_cow(uint64_t *pte) {
    if (!pte || !(*pte & PTE_PRESENT)) return;
    *pte &= ~PTE_WRITABLE;
    *pte |= PTE_COW;
}
static void pte_clear_cow(uint64_t *pte) {
    if (!pte) return;
    *pte &= ~PTE_COW;
    *pte |= PTE_WRITABLE;
}
static int pte_is_cow(uint64_t pte) {
    return (pte & PTE_COW) != 0;
}
//递归复制页表（COW）
static void vmm_clone_pagetable(uintptr_t src_table, uintptr_t dst_table, int level) {
    uintptr_t *src_virt = (uintptr_t*)PHYS_TO_VIRT(src_table);
    uintptr_t *dst_virt = (uintptr_t*)PHYS_TO_VIRT(dst_table);
    for (int i = 0; i < 512; i++) {
        uintptr_t entry = src_virt[i];
        if (!(entry & PTE_PRESENT)) continue;
        uintptr_t child_phys = pte_get_paddr(entry);
        uint64_t flags = entry & 0xFFFULL;
        if (level < 3 && !(entry & PTE_HUGE)) {
            uintptr_t new_child_phys = (uintptr_t)Pmm_Malloc(1);
            if (!new_child_phys)return;
            uintptr_t *new_child_virt = (uintptr_t*)PHYS_TO_VIRT(new_child_phys);
            memset(new_child_virt, 0, PAGE_SIZE);
            vmm_clone_pagetable(child_phys, new_child_phys, level + 1);
            dst_virt[i] = new_child_phys | (flags & ~PTE_WRITABLE) | PTE_COW;
        } else {
            Pmm_RefInc((void*)child_phys);//父子共享叶子页,引用计数+1
            dst_virt[i] = child_phys | (flags & ~PTE_WRITABLE) | PTE_COW;
        }
    }
}
//创建一个VMA结构体
static vm_area_t* vma_create(mm_struct *mm, uintptr_t start, uintptr_t end, uint64_t flags) {
    if(!mm || start >= end) return NULL;
    uintptr_t vma_paddr = (uintptr_t)Pmm_Malloc(1);
    if(!vma_paddr) {
        printk("[VMM] Failed to allocate VMA\n");
        return NULL;
    }
    vm_area_t *vma = (vm_area_t*)PHYS_TO_VIRT(vma_paddr);
    memset(vma, 0, sizeof(vm_area_t));
    vma->vm_start = start;
    vma->vm_end = end;
    vma->vm_flags = flags;
    vma->vm_page_count = (end - start) / PAGE_SIZE;
    vma->vm_mm = mm;
    return vma;
}
//将VMA插入mm_struct的VMA链表
static void vma_insert(mm_struct *mm, vm_area_t *vma) {
    if (!mm || !vma) return;
    spin_lock(&mm->mm_lock);
    struct list_node *pos;
    vm_area_t *cur;
    _Bool inserted = false;
    list_for_each_entry(cur, &mm->mmap, vm_list) {
        if(cur->vm_start > vma->vm_start) {
            list_add_tail(&vma->vm_list, &cur->vm_list);
            inserted = true;
            break;
        }
    }
    if(!inserted) {
        list_add_tail(&vma->vm_list, &mm->mmap);
    }
    mm->mmap_cache = vma;
    mm->total_vm += vma->vm_page_count;
    spin_unlock(&mm->mm_lock);
}
//克隆一个VMA（COW）
static vm_area_t* vma_clone(vm_area_t *src_vma, mm_struct *dst_mm) {
    if(!src_vma || !dst_mm)return NULL;
    //分配新的VMA
    uintptr_t vma_paddr = (uintptr_t)Pmm_Malloc(1);
    if(!vma_paddr)return NULL;
    vm_area_t *dst_vma = (vm_area_t*)PHYS_TO_VIRT(vma_paddr);
    memcpy(dst_vma, src_vma, sizeof(vm_area_t));
    dst_vma->vm_mm = dst_mm;//更新VMA的mm指向
    //重置链表节点
    dst_vma->vm_list.prev = NULL;
    dst_vma->vm_list.next = NULL;
    return dst_vma;
}
//return NULL;
static int vma_clone_range(mm_struct *src_mm, mm_struct *dst_mm) {
    if (!src_mm || !dst_mm) return -1;
    vm_area_t *src_vma;
    int count = 0;
    //遍历源VMA链表
    list_for_each_entry(src_vma, &src_mm->mmap, vm_list) {
        //克隆每个VMA
        vm_area_t *dst_vma = vma_clone(src_vma, dst_mm);
        if (!dst_vma)return -1;
        vma_insert(dst_mm, dst_vma);//插入目标VMA链表
        count++;
    }
    return 0;
}

mm_struct* vmm_clone_address_space(mm_struct *src_mm){
    if (!src_mm || !src_mm->pgd) return NULL;
    //创建新的mm_struct
    mm_struct *dst_mm = vmm_create_address_space();
    if (!dst_mm)return NULL;
    //复制父进程的用户空间
    uintptr_t *src_pml4 = (uintptr_t*)PHYS_TO_VIRT((uintptr_t)src_mm->pgd);
    uintptr_t *dst_pml4 = (uintptr_t*)PHYS_TO_VIRT((uintptr_t)dst_mm->pgd);
    for(int i = 0; i < 256; i++) {
        uintptr_t entry = src_pml4[i];
        if(!(entry & PTE_PRESENT)) continue;
        uintptr_t child_phys = pte_get_paddr(entry);
        uint64_t flags = entry & 0xFFFULL;
        //分配新的PDPT页
        uintptr_t new_pdpt_phys = (uintptr_t)Pmm_Malloc(1);
        if(!new_pdpt_phys) {
            vmm_destroy_address_space(dst_mm);
            return NULL;
        }
        uintptr_t *new_pdpt_virt = (uintptr_t*)PHYS_TO_VIRT(new_pdpt_phys);
        memset(new_pdpt_virt, 0, PAGE_SIZE);
        vmm_clone_pagetable(child_phys, new_pdpt_phys, 1);//递归克隆
        dst_pml4[i] = new_pdpt_phys | (flags & ~PTE_WRITABLE) | PTE_COW;//设置COW标志
    }
    //复制父进程的元数据
    dst_mm->start_code = src_mm->start_code;
    dst_mm->end_code   = src_mm->end_code;
    dst_mm->start_data = src_mm->start_data;
    dst_mm->end_data   = src_mm->end_data;
    dst_mm->start_brk  = src_mm->start_brk;
    dst_mm->brk        = src_mm->brk;
    dst_mm->start_stack = src_mm->start_stack;
    dst_mm->total_vm   = src_mm->total_vm;
    dst_mm->rss        = src_mm->rss;
    //复制VMA链表
    if (vma_clone_range(src_mm, dst_mm) != 0) {
        vmm_destroy_address_space(dst_mm);
        return NULL;
    }
    return dst_mm;
}

vm_area_t* find_vma(mm_struct *mm, uintptr_t addr) {
    if (!mm || list_empty(&mm->mmap)) return NULL;
    if (mm->mmap_cache) {
        vm_area_t *cached = mm->mmap_cache;
        if (addr >= cached->vm_start && addr < cached->vm_end) {
            return cached;
        }
    }
    vm_area_t *vma;
    list_for_each_entry(vma, &mm->mmap, vm_list) {
        if (addr < vma->vm_end) {
            if (addr >= vma->vm_start) {
                mm->mmap_cache = vma;
                return vma;
            }
            return NULL;
        }
    }
    return NULL;
}

void* vmm_mmap(mm_struct *mm, uintptr_t vaddr, uint64_t length, uint64_t flags) {
    if (!mm || !length) return NULL;
    vaddr &= PAGE_MASK;
    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    vm_area_t *vma = vma_create(mm, vaddr, vaddr + pages * PAGE_SIZE, flags);
    if (!vma) return NULL;
    for (uint64_t i = 0; i < pages; i++) {
        void *pa = Pmm_Malloc(1);
        if (!pa) return NULL;
        memset((void*)PHYS_TO_VIRT((uintptr_t)pa), 0, PAGE_SIZE);
        uint64_t pte_flags = PTE_PRESENT | PTE_USER;
        if (flags & VM_WRITE) pte_flags |= PTE_WRITABLE;
        if (!(flags & VM_EXEC) && cpu_nx_enabled) pte_flags |= PTE_NO_EXECUTE;
        if (vmm_map_page((uintptr_t)mm->pgd, vaddr + i * PAGE_SIZE, (uintptr_t)pa, pte_flags) != 0) {
            Pmm_Free(pa, 1);
            return NULL;
        }
        mm->rss++;
    }
    vma_insert(mm, vma);
    return (void*)vaddr;
}

int vmm_map_user_page(mm_struct *mm, uintptr_t vaddr, uintptr_t paddr, uint64_t flags) {
    if (!mm || !mm->pgd) return -1;
    vaddr &= PAGE_MASK;
    uint64_t *table = (uint64_t*)PHYS_TO_VIRT((uintptr_t)mm->pgd);
    //遍历并分配中间页表（PML4→PDPT→PD）
    for (int level = 0; level < 3; level++) {
        int index = (int)((vaddr >> (39 - level * 9)) & 0x1FF);
        uint64_t entry = table[index];
        if (!(entry & PTE_PRESENT)) {
            uintptr_t new_phys = (uintptr_t)Pmm_Malloc(1);
            if (!new_phys) return -1;
            uint64_t *new_table = (uint64_t*)PHYS_TO_VIRT(new_phys);
            memset(new_table, 0, PAGE_SIZE);
            table[index] = new_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            entry = table[index];
        }
        if (entry & PTE_HUGE) return -1;//不支持大页
        table = (uint64_t*)PHYS_TO_VIRT(pte_get_paddr(entry));
    }
    //设置叶子 PTE
    int index = (int)PT_INDEX(vaddr);
    table[index] = (paddr & PAGE_MASK) | flags;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 0;
}