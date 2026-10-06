#include <task.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <idt.h>
#include <desc.h>
#include <print.h>

//创建进程
task_struct* CreateProcess(uintptr_t entry, mm_struct* mm, const char* name) {
    if(!entry || !mm) return NULL;
    //分配任务结构体
    task_struct* task = (task_struct*)(uintptr_t)PHYS_TO_VIRT(Pmm_Malloc(TASK_STRUCT_PAGES));
    if(!task)return NULL;
    memset(task,0,TASK_STRUCT_PAGES * 4096);
    //分配内核栈
    void* stack = (void*)(uintptr_t)PHYS_TO_VIRT(Pmm_Malloc(TASK_KERNEL_STACK_PAGES));
    if(!stack){
        Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)task),TASK_STRUCT_PAGES);
        return NULL;
    }
    memset(stack,0,TASK_KERNEL_STACK_PAGES * 4096);
    cli();
    //初始化任务结构体
    task->pid = AllocPid();
    task->tgid = task->pid;
    task->state = TASK_READY;
    waitq_init(&task->child_wq);
    task->mm = mm;
    task->kernel_stack = stack;
    task->stack_size = TASK_KERNEL_STACK_PAGES * 4096;
    strcpy(task->name, name);
    strcpy(task->cwd, "/");//初始工作目录为根
    //伪造首次切换的栈帧
    uint64_t stack_bottom = (uint64_t)stack + TASK_KERNEL_STACK_PAGES * 4096;
    uint64_t* f = (uint64_t*)(stack_bottom - 17 * 8);
    memset(f, 0, 17 * 8);
    f[0]  = mm->start_stack + PAGE_SIZE - 16;//用户栈顶
    f[1]  = 0x202;//用户rflags
    f[2]  = (uint64_t)__USER_CS;//用户代码段
    f[3]  = entry;//用户入口 RIP
    f[11] = (uint64_t)__USER_DS;//用户数据段（ss）
    f[15] = 0x202;//popfq恢复的内核态rflags
    f[16] = (uint64_t)user_trampoline;//ret目标
    task->context.rsp = (uint64_t)f;
    TaskListAdd(task);//加入调度队列
    sti();
    return task;
}


