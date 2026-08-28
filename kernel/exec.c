#include <task.h>
#include <syscalls.h>
#include <elf.h>
#include <fs.h>
#include <klib.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/pgtables.h>

#define EXEC_STACK_PAGES  16       //用户栈大小(64KB)
#define EXEC_MAX_ARGV     32       //最多参数个数
#define EXEC_MAX_ENVP     32       //最多环境变量个数
#define EXEC_ARG_MAX      128      //单个参数/环境变量最大长度

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

//解析ELF文件并把PT_LOAD段映射进新地址空间,返回入口地址
static int elf_load(fs_file_t *f, mm_struct *mm, uint64_t *entry_out){
    //读ELF头
    Elf64_Ehdr eh;
    if(FsSeek(f, 0, SEEK_SET) < 0)return -1;
    if(FsRead(f, &eh, sizeof(eh)) != sizeof(eh))return -1;
    //魔数与属性校验
    if(eh.e_ident[EI_MAG0] != ELFMAG0 || eh.e_ident[EI_MAG1] != ELFMAG1 || eh.e_ident[EI_MAG2] != ELFMAG2 || eh.e_ident[EI_MAG3] != ELFMAG3)return -1;
    if(eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB)return -1;
    if(eh.e_type != ET_EXEC || eh.e_machine != EM_X86_64)return -1;
    if(eh.e_entry >= USER_VADDR_MAX)return -1;
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
    //文件内容中转缓冲
    void *kbuf = (void*)PHYS_TO_VIRT(Pmm_Malloc(1));
    if(!kbuf){
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
        return -1;
    }
    //遍历程序头,加载PT_LOAD段
    for(uint64_t i = 0; i < eh.e_phnum; i++){
        Elf64_Phdr *ph = (Elf64_Phdr*)((uint8_t*)ph_buf + i * sizeof(Elf64_Phdr));
        if(ph->p_type != PT_LOAD) continue;
        if(ph->p_memsz < ph->p_filesz || ph->p_vaddr >= USER_VADDR_MAX){
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 1);
            return -1;
        }
        //权限转换
        uint64_t prot = 0;
        if(ph->p_flags & PF_R) prot |= VM_READ;
        if(ph->p_flags & PF_W) prot |= VM_WRITE;
        if(ph->p_flags & PF_X) prot |= VM_EXEC;
        //页对齐映射整段
        uintptr_t map_start = ph->p_vaddr & PAGE_MASK;
        uint64_t map_end   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & PAGE_MASK;
        if(!vmm_mmap(mm, map_start, map_end - map_start, prot)){
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 1);
            return -1;
        }
        //拷贝文件内容到新地址空间
        if(FsSeek(f, ph->p_offset, SEEK_SET) < 0){
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
            Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 1);
            return -1;
        }
        uint64_t copied = 0;
        while(copied < ph->p_filesz){
            uint64_t chunk = ph->p_filesz - copied;
            if(chunk > PAGE_SIZE)chunk = PAGE_SIZE;
            if((FsRead(f, kbuf, chunk) != chunk) || exec_write_mem(mm, ph->p_vaddr + copied, kbuf, chunk)){
                Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
                Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 1);
                return -1;
            }
            copied += chunk;
        }
    }
    Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)ph_buf), 1);
    Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)kbuf), 1);
    *entry_out = eh.e_entry;
    return 0;
}

//加载并执行新程序
int do_execve(const char *path, char *const argv[], char *const envp[]){
    if(!current_task || !current_task->mm) return -1;
    //打开ELF文件
    fs_file_t f;
    if(FsOpen(path, O_RDONLY, &f) != 0) return -1;
    //创建全新地址空间
    mm_struct *new_mm = vmm_create_address_space();
    if(!new_mm){
        FsClose(&f);
        return -1;
    }
    //加载程序段
    uint64_t entry = 0;
    if(elf_load(&f, new_mm, &entry)){
        FsClose(&f);
        vmm_destroy_address_space(new_mm);
        return -1;
    }
    FsClose(&f);//关闭文件
    //映射用户栈
    uintptr_t stack_base = new_mm->start_stack;
    uint64_t stack_size = (uint64_t)EXEC_STACK_PAGES * PAGE_SIZE;
    uintptr_t stack_top = stack_base + PAGE_SIZE;
    uintptr_t stack_bottom = stack_top - stack_size;
    if(!vmm_mmap(new_mm, stack_bottom, stack_size, VM_READ | VM_WRITE)){
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
    //envp指针数组
    sp -= 8 * ((uint64_t)envc + 1);
    uintptr_t envp_arr = sp;
    for(int i = 0; i < envc; i++)if(exec_write_mem(new_mm, envp_arr + 8 * i, &envp_va[i], 8))goto fail;
    uint64_t null_ptr = 0;
    if(exec_write_mem(new_mm, envp_arr + 8 * envc, &null_ptr, 8))goto fail;
    //argv指针数组
    sp -= 8 * ((uint64_t)argc + 1);
    uintptr_t argv_arr = sp;
    for(int i = 0; i < argc; i++)if(exec_write_mem(new_mm, argv_arr + 8 * i, &argv_va[i], 8))goto fail;
    if(exec_write_mem(new_mm, argv_arr + 8 * argc, &null_ptr, 8))goto fail;
    //argc压栈
    sp -= 8;
    uint64_t argc_val = (uint64_t)argc;
    if(exec_write_mem(new_mm, sp, &argc_val, 8))goto fail;
    sp &= ~0xFULL;
    //替换当前任务地址空间
    task_struct *t = current_task;
    mm_struct *old_mm = t->mm;
    t->mm = new_mm;
    set_cr3((uintptr_t)new_mm->pgd);
    mmput(old_mm);
    //修改syscall帧
    uint64_t *p = (uint64_t*)(user_kernel_stack_top - 128);
    p[4]  = 0x202;//用户RFLAGS(IF)
    p[9]  = argc_val;//rdi=argc
    p[10] = argv_arr;//rsi=argv
    p[12] = entry;//用户程序入口
    p[14] = 0;//rax=0
    p[15] = sp;//用户RSP=新栈顶
    return 0;
fail:
    vmm_destroy_address_space(new_mm);
    return -1;
}

//系统调用入口
long sys_execve(long path, long argv, long envp){
    if(!current_task || !current_task->mm)return -ENOSYS;
    //拷贝路径
    char kpath[256];
    if(copy_from_user(kpath, (void*)path, 255))return -EFAULT;
    kpath[255] = 0;
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
            if(copy_from_user(strbuf, (void*)up, EXEC_ARG_MAX)) goto err;
            strbuf[EXEC_ARG_MAX - 1] = 0;
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
            if(copy_from_user(strbuf, (void*)up, EXEC_ARG_MAX)) goto err;
            strbuf[EXEC_ARG_MAX - 1] = 0;
            kenvp[envc] = strbuf;
            strbuf += strlen(strbuf) + 1;
        }
    }
    kenvp[envc] = NULL;
    //加载并执行
    int r = do_execve(kpath, argc ? kargv : NULL, envc ? kenvp : NULL);
    Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)page), 2);
    return r < 0 ? -ENOEXEC : 0;
err:
    Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)page), 2);
    return -EFAULT;
}
