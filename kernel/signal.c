//由DeepSeek V4.1 Flash生成
#include <signals.h>
#include <task.h>
#include <syscalls.h>
#include <klib.h>
#include <print.h>
#include <spinlock.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define MAX_SIG 32

#define FRAME_SLOT(n) (*(uint64_t*)(user_kernel_stack_top - 128 + (uint64_t)(n) * 8))//syscall返回帧槽位

//用户栈信号帧
struct k_sigcontext {
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
	uint64_t eflags;
	uint16_t cs, gs, fs, __pad0;
	uint64_t err, trapno, oldmask, cr2;
	uint64_t fpstate;
	uint64_t reserved[8];
};
struct k_stack {
	uint64_t ss_sp;
	int32_t  ss_flags;
	int32_t  __pad;
	uint64_t ss_size;
};
struct k_ucontext {
	uint64_t uc_flags;
	uint64_t uc_link;
	struct k_stack uc_stack;
	struct k_sigcontext uc_mcontext;
	uint64_t uc_sigmask;
};
struct k_siginfo {
	int32_t si_signo;
	int32_t si_errno;
	int32_t si_code;
	int32_t __pad;
	uint64_t payload[14];
};
struct rt_sigframe {
	uint64_t pretcode;          //restorer地址
	struct k_ucontext uc;       //保存的用户上下文
	struct k_siginfo info;      //信号信息
};

//默认动作为忽略的信
static int signal_default_ignore(int sig){
	switch(sig){
	case SIGCHLD: case SIGURG: case SIGWINCH: case SIGCONT:
	case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
		return 1;
	default:
		return 0;
	}
}

//取动作表项
static k_sigaction_t *sig_action(task_struct *t, int sig){
	if(!t || !t->sigacts || sig <= 0 || sig >= MAX_SIG)return NULL;
	return &((k_sigaction_t*)t->sigacts)[sig];
}

//该信号当前是否可投递
static int sig_deliverable(task_struct *t, int sig){
	uint64_t h = SIG_DFL;
	k_sigaction_t *a = sig_action(t, sig);
	if(sig != SIGKILL && sig != SIGSTOP && a)h = a->handler;
	if(h == SIG_IGN)return 0;
	if(h == SIG_DFL && signal_default_ignore(sig))return 0;
	return 1;
}

//当前任务是否有可投递信号(阻塞循环据此返回-EINTR)
int SignalPending(void){
	task_struct *t = current_task;
	if(!t || !t->mm || !t->sigpending)return 0;
	uint64_t pend = t->sigpending & ~t->sigmask;
	for(int sig = 1; sig < MAX_SIG; sig++){
		if(!(pend & (1ULL << sig)))continue;
		if(sig_deliverable(t, sig))return 1;
	}
	return 0;
}

//发送信号: 置待投递位并唤醒阻塞中的目标任务
void SignalSend(task_struct *t, int sig, int code, pid_t sender, int status){
	if(!t || sig <= 0 || sig >= MAX_SIG)return;
	if(t->state == TASK_TERMINATED)return;
	t->sigpending |= 1ULL << sig;
	t->sig_last_code = code;
	t->sig_last_pid = sender;
	t->sig_last_status = status;
	if(t->state == TASK_BLOCKED)TaskWake(t);//让阻塞中的任务尽快回到投递点
}

//用户寄存器快照(系统调用出口与中断返回两条投递路径共用)
typedef struct uregs_snapshot {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t rip, rsp, rflags;
} uregs_snapshot_t;

//投递后新上下文的写入位置
typedef struct uregs_target {
	uint64_t *rip, *rsp, *rdi, *rsi, *rdx;
} uregs_target_t;

//在用户栈上构造信号帧; 从快照保存上下文, 新上下文(handler入口)写入target
static void sig_build_frame(task_struct *t, int sig, k_sigaction_t *act, const uregs_snapshot_t *s, const uregs_target_t *tg){
	//帧地址: 帧底16字节对齐后-8, 使handler入口rsp%16==8(与call约定一致)
	uint64_t fa = ((s->rsp - sizeof(struct rt_sigframe)) & ~(uint64_t)15) - 8;
	struct rt_sigframe *f = (struct rt_sigframe*)fa;
	//ucontext: 保存当前用户上下文
	f->uc.uc_flags = 0;
	f->uc.uc_link = 0;
	f->uc.uc_stack.ss_sp = 0;
	f->uc.uc_stack.ss_flags = 2;//SS_DISABLE
	f->uc.uc_stack.ss_size = 0;
	f->uc.uc_mcontext.r8  = s->r8;
	f->uc.uc_mcontext.r9  = s->r9;
	f->uc.uc_mcontext.r10 = s->r10;
	f->uc.uc_mcontext.r11 = s->r11;
	f->uc.uc_mcontext.r12 = s->r12;
	f->uc.uc_mcontext.r13 = s->r13;
	f->uc.uc_mcontext.r14 = s->r14;
	f->uc.uc_mcontext.r15 = s->r15;
	f->uc.uc_mcontext.rdi = s->rdi;
	f->uc.uc_mcontext.rsi = s->rsi;
	f->uc.uc_mcontext.rbp = s->rbp;
	f->uc.uc_mcontext.rbx = s->rbx;
	f->uc.uc_mcontext.rdx = s->rdx;
	f->uc.uc_mcontext.rax = s->rax;
	f->uc.uc_mcontext.rcx = s->rcx;
	f->uc.uc_mcontext.rip = s->rip;      //被打断处
	f->uc.uc_mcontext.rsp = s->rsp;      //原用户栈指针
	f->uc.uc_mcontext.eflags = s->rflags;
	f->uc.uc_mcontext.cs = 0x23;
	f->uc.uc_sigmask = t->sigmask;       //sigreturn恢复用(旧掩码)
	//siginfo
	f->info.si_signo = sig;
	f->info.si_errno = 0;
	f->info.si_code = t->sig_last_code;
	f->info.payload[0] = (uint64_t)t->sig_last_pid;//si_pid
	f->info.payload[1] = 0;                        //si_uid
	if(sig == SIGCHLD)f->info.payload[2] = (uint64_t)t->sig_last_status;
	//restorer: handler返回后跳到这里执行rt_sigreturn
	f->pretcode = act->restorer;
	//新上下文: 用户态进入处理函数
	*tg->rip = act->handler;
	*tg->rsp = fa;
	*tg->rdi = (uint64_t)sig;            //rdi=signum
	*tg->rsi = (uint64_t)&f->info;       //rsi=&siginfo
	*tg->rdx = (uint64_t)&f->uc;         //rdx=&ucontext
	//处理期间屏蔽本信号及sa_mask(sigreturn恢复旧掩码)
	t->sigmask |= (1ULL << sig) | act->mask;
}

//系统调用返回用户前投递待处理信号(ret=本次系统调用返回值)
void SignalDeliverUser(long ret){
	task_struct *t = current_task;
	if(!t || !t->mm || !t->sigpending)return;
	for(int sig = 1; sig < MAX_SIG; sig++){
		uint64_t bit = 1ULL << sig;
		if(!(t->sigpending & bit))continue;
		if(t->sigmask & bit)continue;
		if(!sig_deliverable(t, sig)){ t->sigpending &= ~bit; continue; }//忽略类: 丢弃
		t->sigpending &= ~bit;
		k_sigaction_t *a = sig_action(t, sig);
		uint64_t h = SIG_DFL;
		if(sig != SIGKILL && sig != SIGSTOP && a)h = a->handler;
		if(h == SIG_DFL){
			//默认动作: 终止(记录信号, wait状态按Linux语义编码)
			t->sig_exit = sig;
			UserTaskExit(128 + sig);
		}
		//从syscall返回帧取用户上下文快照
		uregs_snapshot_t s;
		s.r15 = FRAME_SLOT(0);  s.r14 = FRAME_SLOT(1);  s.r13 = FRAME_SLOT(2);  s.r12 = FRAME_SLOT(3);
		s.r11 = FRAME_SLOT(4);  s.r10 = FRAME_SLOT(5);  s.r9  = FRAME_SLOT(6);  s.r8  = FRAME_SLOT(7);
		s.rbp = FRAME_SLOT(8);  s.rdi = FRAME_SLOT(9);  s.rsi = FRAME_SLOT(10); s.rdx = FRAME_SLOT(11);
		s.rcx = FRAME_SLOT(12); s.rbx = FRAME_SLOT(13);
		s.rax = (uint64_t)ret;      //系统调用返回值
		s.rip = FRAME_SLOT(12);     //用户被打断处
		s.rsp = FRAME_SLOT(15);     //原用户栈指针
		s.rflags = FRAME_SLOT(4);
		uregs_target_t tg;
		tg.rip = (uint64_t*)&FRAME_SLOT(12);
		tg.rsp = (uint64_t*)&FRAME_SLOT(15);
		tg.rdi = (uint64_t*)&FRAME_SLOT(9);
		tg.rsi = (uint64_t*)&FRAME_SLOT(10);
		tg.rdx = (uint64_t*)&FRAME_SLOT(11);
		sig_build_frame(t, sig, a, &s, &tg);
		return;//一次投递一个信号, 其余留待下次
	}
}

//中断返回用户前投递(IRQ栈帧由 kernel/asm/irq_handlers.S 传入:
//frame[1..15]=r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax,
//frame[18]=RIP, 19=CS, 20=RFLAGS, 21=RSP)
void SignalDeliverFromIRQ(uint64_t *frame){
	task_struct *t = current_task;
	if(!t || !t->mm || !t->sigpending)return;
	if((frame[19] & 3) != 3)return;//中断自内核态: 由syscall出口投递
	for(int sig = 1; sig < MAX_SIG; sig++){
		uint64_t bit = 1ULL << sig;
		if(!(t->sigpending & bit))continue;
		if(t->sigmask & bit)continue;
		if(!sig_deliverable(t, sig)){ t->sigpending &= ~bit; continue; }
		t->sigpending &= ~bit;
		k_sigaction_t *a = sig_action(t, sig);
		uint64_t h = SIG_DFL;
		if(sig != SIGKILL && sig != SIGSTOP && a)h = a->handler;
		if(h == SIG_DFL){
			t->sig_exit = sig;
			UserTaskExit(128 + sig);
		}
		//从IRQ栈帧取用户上下文快照
		uregs_snapshot_t s;
		s.r15 = frame[1];  s.r14 = frame[2];  s.r13 = frame[3];  s.r12 = frame[4];
		s.r11 = frame[5];  s.r10 = frame[6];  s.r9  = frame[7];  s.r8  = frame[8];
		s.rbp = frame[9];  s.rdi = frame[10]; s.rsi = frame[11]; s.rdx = frame[12];
		s.rcx = frame[13]; s.rbx = frame[14]; s.rax = frame[15];
		s.rip = frame[18]; s.rsp = frame[21]; s.rflags = frame[20];
		uregs_target_t tg;
		tg.rip = &frame[18];
		tg.rsp = &frame[21];
		tg.rdi = &frame[10];
		tg.rsi = &frame[11];
		tg.rdx = &frame[12];
		sig_build_frame(t, sig, a, &s, &tg);
		return;
	}
}

//rt_sigreturn: restorer执行后从用户栈帧恢复上下文, 返回被保存的rax
long SignalReturn(void){
	if(!current_task || !current_task->mm)return -EFAULT;
	uint64_t rsp = FRAME_SLOT(15);
	if(rsp < 16 || rsp > USER_VADDR_MAX)return -EFAULT;
	struct rt_sigframe *f = (struct rt_sigframe*)(rsp - 8);//ret已弹出pretcode
	FRAME_SLOT(0)  = f->uc.uc_mcontext.r15;
	FRAME_SLOT(1)  = f->uc.uc_mcontext.r14;
	FRAME_SLOT(2)  = f->uc.uc_mcontext.r13;
	FRAME_SLOT(3)  = f->uc.uc_mcontext.r12;
	FRAME_SLOT(4)  = f->uc.uc_mcontext.eflags;
	FRAME_SLOT(5)  = f->uc.uc_mcontext.r10;
	FRAME_SLOT(6)  = f->uc.uc_mcontext.r9;
	FRAME_SLOT(7)  = f->uc.uc_mcontext.r8;
	FRAME_SLOT(8)  = f->uc.uc_mcontext.rbp;
	FRAME_SLOT(9)  = f->uc.uc_mcontext.rdi;
	FRAME_SLOT(10) = f->uc.uc_mcontext.rsi;
	FRAME_SLOT(11) = f->uc.uc_mcontext.rdx;
	FRAME_SLOT(12) = f->uc.uc_mcontext.rip;
	FRAME_SLOT(13) = f->uc.uc_mcontext.rbx;
	FRAME_SLOT(15) = f->uc.uc_mcontext.rsp;
	current_task->sigmask = f->uc.uc_sigmask;
	return (long)f->uc.uc_mcontext.rax;//rax槽由汇编用返回值填充
}

//rt_sigaction: 存/取信号动作表项
long SignalDoSigaction(long signum, long act, long oldact, long sigsetsize){
	if(!current_task)return -ENOSYS;
	if(sigsetsize != 8)return -EINVAL;
	if(signum <= 0 || signum >= MAX_SIG)return -EINVAL;
	k_sigaction_t *tbl = (k_sigaction_t*)current_task->sigacts;
	if(act && !tbl){
		//惰性分配动作表(1页, 可容纳MAX_SIG项)
		tbl = (k_sigaction_t*)PHYS_TO_VIRT(Pmm_Malloc(1));
		if(!tbl)return -ENOMEM;
		memset(tbl, 0, 4096);
		current_task->sigacts = tbl;
	}
	if(oldact){
		k_sigaction_t zero;
		memset(&zero, 0, sizeof(zero));
		if(copy_to_user((void*)oldact, tbl ? &tbl[signum] : &zero, sizeof(k_sigaction_t)))return -EFAULT;
	}
	if(act){
		k_sigaction_t na;
		if(copy_from_user(&na, (void*)act, sizeof(k_sigaction_t)))return -EFAULT;
		if(signum == SIGKILL || signum == SIGSTOP)return 0;//不可捕获, 静默忽略
		tbl[signum] = na;
	}
	return 0;
}

//fork: 子进程复制父进程的信号动作表
void SignalForkCopy(task_struct *child, task_struct *parent){
	if(!child)return;
	child->sigacts = NULL;
	child->sigpending = 0;
	if(parent && parent->sigacts){
		void *p = (void*)PHYS_TO_VIRT(Pmm_Malloc(1));
		if(p){
			memcpy(p, parent->sigacts, 4096);
			child->sigacts = p;
		}
	}
}

//exec: 处理函数复位为默认(SIG_IGN保留), 待投递信号保留
void SignalExecReset(task_struct *t){
	if(!t || !t->sigacts)return;
	k_sigaction_t *tbl = (k_sigaction_t*)t->sigacts;
	for(int i = 0; i < MAX_SIG; i++){
		if(tbl[i].handler != SIG_IGN){
			tbl[i].handler = SIG_DFL;
			tbl[i].flags = 0;
			tbl[i].restorer = 0;
			tbl[i].mask = 0;
		}
	}
}

//任务销毁: 释放动作表
void SignalFree(task_struct *t){
	if(t && t->sigacts){
		Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)t->sigacts), 1);
		t->sigacts = NULL;
	}
}
