#include <task.h>
#include <syscalls.h>
#include <signals.h>
#include <elf.h>
#include <fs.h>
#include <klib.h>
#include <print.h>
#include <idt.h>
#include <delay.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/pgtables.h>

#define EXEC_STACK_PAGES  16       //用户栈大小(64KB)
#define EXEC_MAX_ARGV     32       //最多参数个数
#define EXEC_MAX_ENVP     32       //最多环境变量个数
#define EXEC_ARG_MAX      128      //单个参数/环境变量最大长度

#define IA32_FS_BASE 0xC0000100

static inline void exec_wrmsr(uint32_t msr, uint64_t val){
	uint32_t low = (uint32_t)val;
	uint32_t high = (uint32_t)(val >> 32);
	__asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr) : "memory");
}

//ELF加载结果
typedef struct elf_load_info {
    uint64_t entry;        //程序入口
    uint64_t phdr_addr;    //程序头表在内存中的地址(AT_PHDR)
    uint64_t phentsize;    //程序头表项大小(AT_PHENT)
    uint64_t phnum;        //程序头表项数(AT_PHNUM)
    uint64_t load_base;    //文件偏移0映射到的地址(AT_BASE)
    uint64_t dynamic_addr; //PT_DYNAMIC段地址
    char     interp[256];  //PT_INTERP解释器路径
} elf_load_info_t;

#define INTERP_BASE 0x7f8000000000ULL //动态链接器(ld.so)加载基址

//向新地址空间写数据
static int exec_write_mem(mm_struct *mm, uintptr_t vaddr, const void *src, uint64_t len){
    while(len){
        uintptr_t page = vaddr & PAGE_MASK;
        uintptr_t off  = vaddr & (PAGE_SIZE - 1);
        uint64_t chunk = len;
        if(chunk > PAGE_SIZE - off) chunk = PAGE_SIZE - off;
        uintptr_t *pte = (uintptr_t*)get_pte((uintptr_t)mm->pgd, page, 0, 0);
        if(!pte || !pte_is_present(*pte)) return -1;
        memcpy((void*)PHYS_TO_VIRT(pte_get_paddr(*pte) + off), src, chunk);
        vaddr += chunk;
        src = (const uint8_t*)src + chunk;
        len -= chunk;
    }
    return 0;
}

//解析ELF文件并把PT_LOAD段映射进新地址空间,返回入口/程序头/解释器信息
static int elf_load(fs_file_t *f, mm_struct *mm, uint64_t preferred_base, elf_load_info_t *info){
    //读ELF头
    Elf64_Ehdr eh;
    if(FsSeek(f, 0, SEEK_SET) < 0)return -1;
    if(FsRead(f, &eh, sizeof(eh)) != sizeof(eh))return -1;
    //魔数与属性校验
    if(eh.e_ident[EI_MAG0] != ELFMAG0 || eh.e_ident[EI_MAG1] != ELFMAG1 || eh.e_ident[EI_MAG2] != ELFMAG2 || eh.e_ident[EI_MAG3] != ELFMAG3)return -1;
    if(eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB)return -1;
    if((eh.e_type != ET_EXEC && eh.e_type != ET_DYN) || eh.e_machine != EM_X86_64)return -1;
    if(eh.e_phentsize != sizeof(Elf64_Phdr))return -1;
    if(!eh.e_phnum || eh.e_phnum > 64)return -1;
    //读程序头表
    uint64_t ph_size = (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr);
    void *ph_buf = (void*)PHYS_TO_VIRT(Pmm_Malloc(1));
    if(!ph_buf) return -1;
    if((FsSeek(f, eh.e_phoff, SEEK_SET) < 0) || (FsRead(f, ph_buf, ph_size) != ph_size)){
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
        return -1;
    }
    //提取PT_INTERP路径/PT_DYNAMIC地址
    uint64_t base_off = 0;//首个PT_LOAD
    uint64_t dyn_va = 0;
    int found_base = 0;
    //遍历程序头表
    for(uint64_t i = 0; i < eh.e_phnum; i++){
        Elf64_Phdr *ph = (Elf64_Phdr*)((uint8_t*)ph_buf + i * sizeof(Elf64_Phdr));
        if(ph->p_type == PT_INTERP){
            uint64_t ilen = ph->p_filesz;
            if(ilen > sizeof(info->interp) - 1) ilen = sizeof(info->interp) - 1;
            if(FsSeek(f, ph->p_offset, SEEK_SET) >= 0){
                memset(info->interp, 0, sizeof(info->interp));
                FsRead(f, info->interp, ilen);
            }
        }
        if(ph->p_type == PT_DYNAMIC) dyn_va = ph->p_vaddr;
        if(ph->p_type == PT_LOAD && !found_base){
            base_off = ph->p_vaddr - ph->p_offset;
            found_base = 1;
        }
    }
    if(!found_base){
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
        return -1;
    }
    uint64_t load_base = (eh.e_type == ET_DYN) ? (preferred_base & PAGE_MASK) : base_off;
    //文件内容中转缓冲
    void *kbuf = (void*)PHYS_TO_VIRT(Pmm_Malloc(32));
    if(!kbuf){
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
        return -1;
    }
    //加载PT_LOAD段
    for(uint64_t i = 0; i < eh.e_phnum; i++){
        Elf64_Phdr *ph = (Elf64_Phdr*)((uint8_t*)ph_buf + i * sizeof(Elf64_Phdr));
        if(ph->p_type != PT_LOAD) continue;
        //段运行时虚拟地址
        uint64_t seg_va = load_base + ph->p_vaddr - base_off;
        if(ph->p_memsz < ph->p_filesz || seg_va >= USER_VADDR_MAX || seg_va + ph->p_memsz > USER_VADDR_MAX){
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 32);
            return -1;
        }
        //权限转换
        uint64_t prot = 0;
        if(ph->p_flags & PF_R) prot |= VM_READ;
        if(ph->p_flags & PF_W) prot |= VM_WRITE;
        if(ph->p_flags & PF_X) prot |= VM_EXEC;
        //页对齐映射整段
        uintptr_t map_start = seg_va & PAGE_MASK;
        uint64_t map_end   = (seg_va + ph->p_memsz + PAGE_SIZE - 1) & PAGE_MASK;
        if(vmm_mmap(mm, map_start, map_end - map_start, prot)){
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 32);
            return -1;
        }
        //拷贝文件内容到新地址空间
        if(FsSeek(f, ph->p_offset, SEEK_SET) < 0){
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 32);
            return -1;
        }
        uint64_t copied = 0;
        while(copied < ph->p_filesz){
            uint64_t chunk = ph->p_filesz - copied;
            if(chunk > 32 * PAGE_SIZE)chunk = 32 * PAGE_SIZE;
            if((FsRead(f, kbuf, chunk) != chunk) || exec_write_mem(mm, seg_va + copied, kbuf, chunk)){
                Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
                Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 32);
                return -1;
            }
            copied += chunk;
        }
    }
    Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
    Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 32);
    //入口 = 加载基址 + (e_entry - base_off)
    info->entry = load_base + eh.e_entry - base_off;
    if(info->entry >= USER_VADDR_MAX)return -1;
    info->load_base = load_base;
    info->dynamic_addr = dyn_va ? (load_base + dyn_va - base_off) : 0;
    info->phentsize = eh.e_phentsize;
    info->phnum = eh.e_phnum;
    info->phdr_addr = load_base + eh.e_phoff;
    return 0;
}

//加载并执行新程序
int do_execve(const char *path, char *const argv[], char *const envp[]){
    if(!current_task || !current_task->mm) return -1;
    //打开ELF文件
    fs_file_t f;
    if(FsOpen(path, O_RDONLY, &f) != 0) return -ENOENT;
    //创建全新地址空间
    mm_struct *new_mm = vmm_create_address_space();
    if(!new_mm){
        FsClose(&f);
        return -ENOMEM;
    }
    //加载程序段
    elf_load_info_t info;
    memset(&info, 0, sizeof(info));
    if(elf_load(&f, new_mm, 0, &info)){
        FsClose(&f);
        vmm_destroy_address_space(new_mm);
        return -ENOEXEC;//非合法ELF
    }
    FsClose(&f);//关闭文件
    //若有PT_INTERP, 加载动态链接器到同一地址空间, 执行入口改为解释器入口
    uint64_t exec_entry = info.entry;
    uint64_t at_base = 0;
    if(info.interp[0]){
        fs_file_t lf;
        if(FsOpen(info.interp, O_RDONLY, &lf) != 0){
            vmm_destroy_address_space(new_mm);
            return -ENOENT;//动态链接器缺失
        }
        elf_load_info_t ldi;
        memset(&ldi, 0, sizeof(ldi));
        int lr = elf_load(&lf, new_mm, INTERP_BASE, &ldi);
        FsClose(&lf);
        if(lr){
            vmm_destroy_address_space(new_mm);
            return -ENOEXEC;
        }
        exec_entry = ldi.entry;
        at_base = ldi.load_base;
    }
    //映射用户栈
    uintptr_t stack_base = new_mm->start_stack;
    uint64_t stack_size = (uint64_t)EXEC_STACK_PAGES * PAGE_SIZE;
    uintptr_t stack_top = stack_base + PAGE_SIZE;
    uintptr_t stack_bottom = stack_top - stack_size;
    if(vmm_mmap(new_mm, stack_bottom, stack_size, VM_READ | VM_WRITE)){
        vmm_destroy_address_space(new_mm);
        return -1;
    }
    //统计参数个数
    int argc = 0;
    while(argc < EXEC_MAX_ARGV && argv && argv[argc]) argc++;
    int envc = 0;
    while(envc < EXEC_MAX_ENVP && envp && envp[envc]) envc++;
    //构造新用户栈
    uintptr_t sp = stack_top;
    uintptr_t argv_va[EXEC_MAX_ARGV];
    uintptr_t envp_va[EXEC_MAX_ENVP];
    for(int i = 0; i < argc; i++){
        int len = strlen(argv[i]);
        if(len >= EXEC_ARG_MAX) goto fail;
        sp -= (uint64_t)len + 1;
        if(exec_write_mem(new_mm, sp, argv[i], (uint64_t)len + 1))goto fail;
        argv_va[i] = sp;
    }
    for(int i = 0; i < envc; i++){
        int len = strlen(envp[i]);
        if(len >= EXEC_ARG_MAX) goto fail;
        sp -= (uint64_t)len + 1;
        if(exec_write_mem(new_mm, sp, envp[i], (uint64_t)len + 1))goto fail;
        envp_va[i] = sp;
    }
    sp &= ~0xFULL;//16字节对齐
    //AT_RANDOM:16字节随机数据(栈canary等)
    uint64_t rnd[2];
    rnd[0] = rdtsc();
    rnd[1] = 0x6c6f6379c0ffee00ULL;
    sp -= 16;
    if(exec_write_mem(new_mm, sp, rnd, 16)) goto fail;
    uintptr_t random_addr = sp;
    //AT_HWCAP
    uint32_t cpuid_edx;
    {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
        cpuid_edx = d;
    }
    //auxv辅助向量
    uint64_t auxv[][2] = {
        {AT_PHDR,   info.phdr_addr},
        {AT_PHENT,  info.phentsize},
        {AT_PHNUM,  info.phnum},
        {AT_BASE,   at_base},
        {AT_PAGESZ, PAGE_SIZE},
        {AT_HWCAP,  (uint64_t)cpuid_edx},
        {AT_ENTRY,  info.entry},
        {AT_SECURE, 0},
        {AT_RANDOM, random_addr},
        {AT_NULL,   0},
    };
    int auxv_cnt = (int)(sizeof(auxv) / sizeof(auxv[0]));
    sp -= 8;                          //argc
    sp -= 8 * ((uint64_t)argc + 1);   //argv指针数组
    sp -= 8 * ((uint64_t)envc + 1);   //envp指针数组
    sp -= 8 * 2 * (uint64_t)auxv_cnt; //auxv数组
    sp -= 16;                         //对齐余量
    sp &= ~0xFULL;
    uintptr_t argc_addr = sp;
    uintptr_t argv_arr  = sp + 8;
    uintptr_t envp_arr  = sp + 8 * ((uint64_t)argc + 2);
    uintptr_t auxv_arr  = envp_arr + 8 * ((uint64_t)envc + 1);
    uint64_t argc_val = (uint64_t)argc;
    uint64_t null_ptr = 0;
    if(exec_write_mem(new_mm, argc_addr, &argc_val, 8))goto fail;
    for(int i = 0; i < argc; i++)if(exec_write_mem(new_mm, argv_arr + 8 * i, &argv_va[i], 8))goto fail;
    if(exec_write_mem(new_mm, argv_arr + 8 * argc, &null_ptr, 8))goto fail;
    for(int i = 0; i < envc; i++)if(exec_write_mem(new_mm, envp_arr + 8 * i, &envp_va[i], 8))goto fail;
    if(exec_write_mem(new_mm, envp_arr + 8 * envc, &null_ptr, 8))goto fail;
    for(int i = 0; i < auxv_cnt; i++){
        if(exec_write_mem(new_mm, auxv_arr + i * 16, auxv[i], 16)) goto fail;
    }
    sp = argc_addr;
    //替换当前任务地址空间
    task_struct *t = current_task;
    mm_struct *old_mm = t->mm;
    t->mm = new_mm;
    set_cr3((uintptr_t)new_mm->pgd);
    mmput(old_mm);
    //清除FS基址(竞态修复: 字段与MSR必须原子更新)
    cli();
    t->fs_base = 0;
    exec_wrmsr(IA32_FS_BASE, 0);
    sti();
    SignalExecReset(t);
    //修改syscall帧
    uint64_t *p = (uint64_t*)(user_kernel_stack_top - 128);
    for(int i = 0; i < 16; i++) p[i] = 0;
    p[4]  = 0x202;//用户RFLAGS(IF)
    p[9]  = argc_val;//rdi=argc
    p[10] = argv_arr;//rsi=argv
    p[12] = exec_entry;//用户程序入口(有解释器时为ld.so入口)
    p[14] = 0;//rax=0
    p[15] = sp;//用户RSP=新栈顶
    if(t->vfork_parent > 0){
        task_struct *vp = TaskFind(t->vfork_parent);
        if(vp){ vp->vfork_waiting = 0; TaskWake(vp); }
        t->vfork_parent = 0;
    }
    return 0;
fail:
    vmm_destroy_address_space(new_mm);
    return -ENOEXEC;//栈/参数构建失败
}

//系统调用入口
long sys_execve(long path, long argv, long envp, long a4, long a5, long a6){
	(void)a4; (void)a5; (void)a6;
    if(!current_task || !current_task->mm)return -ENOSYS;
    //拷贝路径
    char kpath[256];
    if(strncpy_from_user(kpath, (void*)path, 256) < 0)return -EFAULT;
    //分配临时页存放参数字符串
    void *page = (void*)PHYS_TO_VIRT(Pmm_Malloc(2));
    if(!page)return -ENOMEM;
    memset(page, 0, 2 * PAGE_SIZE);
    char *kargv[EXEC_MAX_ARGV + 1];
    char *kenvp[EXEC_MAX_ENVP + 1];
    char *strbuf = (char*)page;
    //解析argv
    int argc = 0;
    if(argv){
        for(; argc < EXEC_MAX_ARGV; argc++){
            uintptr_t up;
            if(copy_from_user(&up, (void*)((char**)argv + argc), sizeof(up)))goto err;
            if(!up) break;
            if((uintptr_t)strbuf - (uintptr_t)page + EXEC_ARG_MAX > 2 * PAGE_SIZE)goto err;
            if(strncpy_from_user(strbuf, (void*)up, EXEC_ARG_MAX) < 0) goto err;
            kargv[argc] = strbuf;
            strbuf += strlen(strbuf) + 1;
        }
    }
    kargv[argc] = NULL;
    //解析envp
    int envc = 0;
    if(envp){
        for(; envc < EXEC_MAX_ENVP; envc++){
            uintptr_t up;
            if(copy_from_user(&up, (void*)((char**)envp + envc), sizeof(up))) goto err;
            if(!up) break;
            if((uintptr_t)strbuf - (uintptr_t)page + EXEC_ARG_MAX > 2 * PAGE_SIZE) goto err;
            if(strncpy_from_user(strbuf, (void*)up, EXEC_ARG_MAX) < 0) goto err;
            kenvp[envc] = strbuf;
            strbuf += strlen(strbuf) + 1;
        }
    }
    kenvp[envc] = NULL;
    //加载并执行
    int r = do_execve(kpath, argc ? kargv : NULL, envc ? kenvp : NULL);
    Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)page), 2);
    return r;
err:
    Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)page), 2);
    return -EFAULT;
}
