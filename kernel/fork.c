#include <fork.h>
#include <task.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <idt.h>
#include <klib.h>
#include <print.h>
#include <pipe.h>
#include <signals.h>

__attribute__((naked))
void fork_trampoline(void){
	__asm__ volatile(
		"xorq %rax, %rax\n\t"//子进程返回 0
		"movq (%rsp), %rcx\n\t"//用户RIP
		"movq 8(%rsp), %r11\n\t"//用户RFLAGS
		"movq 16(%rsp), %rsp\n\t"//用户RSP
		"sysretq\n\t"
	);
}

//执行fork
pid_t do_fork(void){
	if (!current_task || !current_task->mm) return -1;
	task_struct *parent = current_task;
	//COW复制地址空间
	mm_struct *child_mm = vmm_clone_address_space(parent->mm);
	if (!child_mm) return -1;
	//分配子task_struct与内核栈
	task_struct *child = (task_struct*)(uintptr_t)PHYS_TO_VIRT(Pmm_Malloc(1));
	void *stack = (void*)(uintptr_t)PHYS_TO_VIRT(Pmm_Malloc(1));
	if (!child || !stack) {
		if (child) Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)child), 1);
		if (stack) Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)stack), 1);
		vmm_destroy_address_space(child_mm);
		return -1;
	}
	memset(child, 0, 4096);
	memset(stack, 0, 4096);
	cli();
	//拷贝父进程当前内核栈帧到子栈对应位置
	uintptr_t cur_rsp;
	__asm__ volatile("movq %%rsp, %0" : "=r"(cur_rsp));
	uintptr_t parent_top = user_kernel_stack_top;
	uintptr_t child_top = (uintptr_t)stack + 4096;
	memcpy((void*)(child_top - (parent_top - cur_rsp)), (void*)cur_rsp, parent_top - cur_rsp);
	//构造子进程首次调度的伪帧
	uint64_t *f = (uint64_t*)(child_top - 160);
	uint64_t *p = (uint64_t*)(parent_top - 128);
	f[0]  = p[0];//r15
	f[1]  = p[1];//r14
	f[2]  = p[2];//r13
	f[3]  = p[3];//r12
	f[4]  = p[4];//r11
	f[5]  = p[5];//r10
	f[6]  = p[6];//r9
	f[7]  = p[7];//r8
	f[8]  = p[9];//rdi
	f[9]  = p[10];//rsi
	f[10] = p[8];//rbp
	f[11] = p[13];//rbx
	f[12] = p[11];//rdx
	f[13] = p[12];//rcx
	f[14] = 0;//rax子进程返回值
	f[15] = 0x202;//popfq用rflags
	f[16] = (uint64_t)fork_trampoline;//ret目标
	f[17] = p[12];//用户RIP
	f[18] = p[4];//用户RFLAGS
	f[19] = p[15];//用户RSP
	child->context.rsp = (uint64_t)f;
	//初始化子任务并加入就绪队列
	child->pid = AllocPid();
	child->tgid = child->pid;
	child->state = TASK_READY;
	child->parent = parent->pid;//记录父子关系
	child->exit_code = 0;
	waitq_init(&child->child_wq);
	child->mm = child_mm;
	child->kernel_stack = stack;
	child->stack_size = 4096;
	strcpy(child->name, parent->name);
	child->fs_base = parent->fs_base;//继承TLS的FS段基址
	strcpy(child->cwd, parent->cwd);//继承当前工作目录
	//继承父进程文件描述符表
	for (int i = 0; i < MAX_FD; i++) {
		child->files[i] = parent->files[i];
		if (child->files[i].used && child->files[i].node) child->files[i].node->refs++;
		if (child->files[i].used && child->files[i].pipe) pipeForkRef((struct pipe*)child->files[i].pipe, (child->files[i].flags & O_ACCMODE) == O_WRONLY);
	}
	SignalForkCopy(child, parent);//复制信号动作表
	TaskListAdd(child);
	sti();
	return child->pid;
}

long sys_fork(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if (!current_task || !current_task->mm) return -ENOSYS;//检查当前任务是否存在或为内核任务
	pid_t pid = do_fork();//执行fork
	return pid < 0 ? -EAGAIN : (long)pid;
}

long sys_vfork(long a, long b, long c, long a4, long a5, long a6){
	(void)a; (void)b; (void)c; (void)a4; (void)a5; (void)a6;
	if(!current_task || !current_task->mm)return -ENOSYS;
	task_struct *parent = current_task;
	parent->vfork_waiting = 1;
	pid_t pid = do_fork();
	if(pid < 0){
		parent->vfork_waiting = 0;
		return -EAGAIN;
	}
	task_struct *child = TaskFind(pid);
	if(child)child->vfork_parent = parent->pid;
	while(parent->vfork_waiting)sleep_on(&parent->child_wq);
	return (long)pid;
}
