//由DeepSeek V4.1 Flash生成
#ifndef _SIGNAL_H_
#define _SIGNAL_H_

#include <types.h>

//标准信号编号(x86_64)
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGWINCH 28

//handler特殊值
#define SIG_DFL  0
#define SIG_IGN  1

//sa_flags
#define SA_RESTORER 0x04000000
#define SA_SIGINFO  4

//si_code
#define SI_USER    0
#define SI_KERNEL  0x80
#define SI_TKILL   (-6)

//内核信号动作表项(rt_sigaction的内核布局, 32字节; musl/glibc均按此转换)
typedef struct k_sigaction {
	uint64_t handler;   //SIG_DFL/SIG_IGN/用户处理函数地址
	uint64_t flags;     //SA_*
	uint64_t restorer;  //handler返回后的sigreturn跳板
	uint64_t mask;      //处理期间额外屏蔽的信号
} k_sigaction_t;

struct task_struct;

void SignalForkCopy(struct task_struct *child, struct task_struct *parent);//fork复制动作表
void SignalExecReset(struct task_struct *t);//exec后处理函数复位为默认
void SignalFree(struct task_struct *t);//任务销毁释放动作表
void SignalSend(struct task_struct *t, int sig, int code, pid_t sender, int status);//发送信号
int  SignalPending(void);//当前任务是否有可投递信号(供阻塞循环返回-EINTR)
void SignalDeliverUser(long ret);//syscall返回用户前投递(可能改写返回帧/终止任务)
void SignalDeliverFromIRQ(uint64_t *frame);//中断返回用户前投递(IRQ栈帧指针)
long SignalReturn(void);//rt_sigreturn: 从用户栈信号帧恢复上下文
long SignalDoSigaction(long signum, long act, long oldact, long sigsetsize);//rt_sigaction

#endif
