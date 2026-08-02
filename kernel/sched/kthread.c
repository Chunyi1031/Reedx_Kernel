#include <task.h>
#include <mm/pmm.h>
#include <idt.h>

static pid_t next_pid = 1;

task_struct* CreateKernelThread(void (*entry)(void),uint64_t stack_size,const char* name) {
    //参数检查
    if(!entry || stack_size == 0)return NULL;
    //分配任务结构体
    task_struct* task = (task_struct*)Pmm_Malloc(1);
    if(!task)return NULL;
    memset(task,0,4096);
    //分配栈
    uint32_t stack_pages = ((stack_size + 4095) & ~4095) / 4096;
    stack_size = stack_pages * 4096;
    void* stack = Pmm_Malloc(stack_pages);
    if(!stack){
        Pmm_Free(task,1);
        return NULL;
    }
    memset(stack,0,stack_size);
    cli();
    //初始化任务结构体
    task->pid = next_pid++;
    task->tgid = 0;
    task->state = TASK_READY;
    task->kernel_stack = stack;
    task->stack_size = stack_size;
    strcpy(task->name, name);
    uint64_t stack_bottom = (uint64_t)stack + stack_size;//计算栈底（高地址）
    //预留18个槽位，并16字节对齐
    uint64_t stack_frame = ((stack_bottom - 18 * 8) & ~15ULL);
    uint64_t *frame = (uint64_t *)stack_frame;
    //初始化栈帧
    memset(frame, 0, 18 * 8);
    frame[15] = 0x202;//rflags
    frame[16] = (uintptr_t)entry;//rip
    frame[17] = (uint64_t)TaskExit;//返回地址
    task->context.rsp = stack_frame;//设置线程上下文的rsp
    TaskListAdd(task);//将任务加入任务链表
    sti();
    return task;
}
