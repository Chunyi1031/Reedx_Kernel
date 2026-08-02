#ifndef _TASK_H_
#define _TASK_H_

#include <klib.h>

#define TASK_READY      0
#define TASK_RUNNING    1
#define TASK_BLOCKED    2
#define TASK_TERMINATED 3

//线程上下文
typedef struct task_context {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) task_context_t;

struct list_node {
    struct list_node *prev;  // 指向前一个节点
    struct list_node *next;  // 指向下一个节点
};

typedef struct task_struct {
    task_context_t      context;    //线程上下文
    pid_t               pid;        //进程ID
    pid_t               tgid;       //线程组ID
    int                 state;      //进程状态
    char                name[16];   //进程名
    void*               kernel_stack;//内核栈指针
    uint64_t            stack_size; //内核栈大小
    struct list_node    list;       //链表节点
} task_struct;

extern task_struct* current_task;//当前运行的任务
extern task_struct* kernel_task;//内核任务

void TaskInit();//初始化多任务环境
void TaskListAdd(task_struct* t);//将任务加入任务链表
void TaskListRemove(task_struct* t);//将任务从任务链表中移除
task_struct* TaskFind(pid_t pid);//根据PID查找任务
task_struct* TaskPickNext(void);//从任务链表中取出下一个就绪任务
task_struct* TaskPeekNext(void);//查看下一个就绪任务
/**
 * @brief 获取所有存在的任务结构体指针
 * @param buf  输出缓冲区，存放 task_struct* 数组
 * @param max  缓冲区最大容量（指针个数）
 * @return 实际收集到的任务数量
 */
int TaskGetAll(task_struct **buf, int max);

void TaskExit();//任务退出
void schedule();//调度函数
void TaskKill(task_struct* t);//结束任务

task_struct* CreateKernelThread(void (*entry)(void),uint64_t stack_size,const char* name);//创建内核线程

#endif