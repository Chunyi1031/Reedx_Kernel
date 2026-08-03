#include <task.h>
#include <mm/pmm.h>
#include <idt.h>
#include <irq.h>
#include <spinlock.h>

static struct list_node task_list_head = {&task_list_head, &task_list_head};//任务链表头
task_struct* current_task = NULL;//当前运行的任务
task_struct* kernel_task = NULL;//内核任务
task_struct* idle_task = NULL;//idle任务

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

void idle_thread(){
    SYSTEM_STOP();
}

void TaskInit(){
    //初始化链表
    task_list_head.prev = &task_list_head;
    task_list_head.next = &task_list_head;
    //填充内核进程结构体
    kernel_task = (task_struct*)Pmm_Malloc(1);
    if (!kernel_task) SYSTEM_STOP();
    memset(kernel_task, 0, 4096);
    kernel_task->pid = 0;
    kernel_task->tgid = 0;
    kernel_task->state = TASK_READY;
    strcpy(kernel_task->name, "Reedx Kernel");
    kernel_task->kernel_stack = (void*)SYSTEM_BootParam->KernelStackAddress;
    kernel_task->stack_size = SYSTEM_BootParam->KernelStackSize;
    //将内核任务设为当前任务
    current_task = kernel_task;
    current_task->state = TASK_RUNNING;
    idle_task = CreateKernelThread(idle_thread,4096,"Idle Task");//创建IDLE任务
}

void TaskListAdd(task_struct* t){
    if (!t) return;
    if (list_has_node(&t->list))list_del(&t->list);//如果已经在链表中，先移除
    list_add_tail(&t->list, &task_list_head);//尾插，保证公平
    t->state = TASK_READY;
}

void TaskListRemove(task_struct* t){
    if (!t) return;
    if (list_has_node(&t->list))list_del(&t->list);
}

task_struct* TaskFind(pid_t pid){
    if (pid <= 0) return NULL;
    struct list_node *pos;
    task_struct *task;
    list_for_each(pos, &task_list_head) {
        task = container_of(pos, task_struct, list);
        if (task->pid == pid) return task;
    }
    return NULL;
}


task_struct* TaskPickNext(void) {
    //如果链表为空，返回 NULL
    if (list_empty(&task_list_head)) {
        return NULL;
    }
    //从链表头开始找第一个就绪任务
    struct list_node *pos;
    struct list_node *safe_next;
    task_struct *task;
    list_for_each_safe(pos, safe_next, &task_list_head) {
        task = container_of(pos, task_struct, list);
        list_del(pos);//从链表中移除该任务
        return task;
    }
    //没有就绪任务
    return NULL;
}

task_struct* TaskPeekNext(void) {
    if (list_empty(&task_list_head))return NULL;
    struct list_node *pos;
    task_struct *task; 
    list_for_each(pos, &task_list_head) {
        task = container_of(pos, task_struct, list);
        return task;
    }
    return NULL;
}

int TaskGetAll(task_struct **buf, int max){
    struct list_node *pos;
    task_struct *task;
    int count = 0;
    if (!buf || max <= 0) return 0;
    list_for_each(pos, &task_list_head) {
        if (count >= max) return count;
        task = container_of(pos, task_struct, list);
        buf[count++] = task;
    }
    if (current_task) {
        _Bool found = false;
        for (int i = 0; i < count; i++) {
            if (buf[i] == current_task) { found = true; break; }
        }
        if (!found && count < max) buf[count++] = current_task;
    }
    if (kernel_task && kernel_task != current_task) {
        _Bool found = false;
        for (int i = 0; i < count; i++) {
            if (buf[i] == kernel_task) { found = true; break; }
        }
        if (!found && count < max) buf[count++] = kernel_task;
    }
    return count;
}

void TaskExit(){
    current_task->state = TASK_TERMINATED;
    schedule();
    SYSTEM_STOP();
}

void TaskKill(task_struct* t){
    if(!t)return;
    if((t == kernel_task) || (t == idle_task))return;//不能杀死内核任务和idle任务
    t->state = TASK_TERMINATED;//标记为终止
    if(t == current_task)schedule();//如果杀死的是当前任务，立即调度
    cli();
    TaskListRemove(t);//从就绪队列移除
    if(t->kernel_stack)Pmm_Free(t->kernel_stack,t->stack_size / 4096);//回收栈
    Pmm_Free(t,1);
    memset(t,0,4096);
    sti();
}

/*DeepSeek V4 Pro*/
static DEFINE_WAIT_QUEUE(timeout_wq);//超时等待队列

//将当前任务挂到等待队列并主动让出CPU
void sleep_on(wait_queue_head_t *wq){
    if(!wq || !current_task)return;
    list_add_tail(&current_task->wait_node, wq);
    current_task->state = TASK_BLOCKED;
    schedule();
}

//从等待队列中唤醒一个任务
static void wake_up_one(wait_queue_head_t *wq){
    task_struct *task;
    if(!wq || list_empty(wq))return;
    task = container_of(wq->next, task_struct, wait_node);
    list_del(&task->wait_node);
    task->wake_up_ticks = 0;
    if(task->state == TASK_BLOCKED)TaskListAdd(task);
}

//唤醒等待队列上的一个任务
void wake_up(wait_queue_head_t *wq){
    wake_up_one(wq);
}

//唤醒等待队列上的全部任务
void wake_up_all(wait_queue_head_t *wq){
    while(!list_empty(wq))wake_up_one(wq);
}

//睡眠指定毫秒(精度受限于OS_TICK_HZ=100,即10ms粒度)
void msleep(uint64_t ms){
    if(ms == 0 || !current_task)return;
    current_task->wake_up_ticks = SYSTEM_TimerTicks + (ms + 9) / 10;
    sleep_on(&timeout_wq);
}

//遍历超时队列,将已到期的任务移到就绪队列
void timeout_wake_check(void){
    task_struct *task;
    struct list_node *pos;
    if(list_empty(&timeout_wq))return;
    list_for_each(pos, &timeout_wq) {
    	task = container_of(pos, task_struct, wait_node);
    	if(task->wake_up_ticks > 0 && task->wake_up_ticks <= SYSTEM_TimerTicks){
    		list_del(&task->wait_node);
    		task->wake_up_ticks = 0;
    		if(task->state == TASK_BLOCKED)TaskListAdd(task);
    		if(list_empty(&timeout_wq))return;
    		pos = &timeout_wq;
    	}
    }
}

//初始化信号量
void sem_init(semaphore_t *sem, int initial_count){
	sem->count = initial_count;
	spin_lock_init(&sem->lock);
	sem->wq.prev = &sem->wq;
	sem->wq.next = &sem->wq;
}

//P操作:获取信号量,若count<=0则阻塞等待
void sem_down(semaphore_t *sem){
	unsigned long flags;
	if(!sem)return;
	spin_lock_irqsave(&sem->lock, flags);
	sem->count--;
	if(sem->count < 0){
		list_add_tail(&current_task->wait_node, &sem->wq);
		current_task->state = TASK_BLOCKED;
		spin_unlock_irqrestore(&sem->lock, flags);
		schedule();
		return;
	}
	spin_unlock_irqrestore(&sem->lock, flags);
}

//V操作:释放信号量,若有等待者则唤醒一个
void sem_up(semaphore_t *sem){
	unsigned long flags;
	task_struct *task;
	if(!sem)return;
	spin_lock_irqsave(&sem->lock, flags);
	sem->count++;
	if(sem->count <= 0){
		task = container_of(sem->wq.next, task_struct, wait_node);
		list_del(&task->wait_node);
		task->wake_up_ticks = 0;
		if(task->state == TASK_BLOCKED)TaskListAdd(task);
	}
	spin_unlock_irqrestore(&sem->lock, flags);
}

//非阻塞尝试获取信号量:成功返回1,失败返回0
_Bool sem_trydown(semaphore_t *sem){
	unsigned long flags;
	_Bool ret;
	if(!sem)return false;
	spin_lock_irqsave(&sem->lock, flags);
	if(sem->count > 0){
		sem->count--;
		ret = true;
	}else{
		ret = false;
	}
	spin_unlock_irqrestore(&sem->lock, flags);
	return ret;
}
/*DeepSeek V4 Pro-END*/

//保存上文，切换下文
__attribute__((naked))
void switch_to(void *prev, void *next) {
    __asm__ volatile (
        //保存当前寄存器到prev的栈
        "pushfq\n"
        "pushq %%rax\n"
        "pushq %%rcx\n"
        "pushq %%rdx\n"
        "pushq %%rbx\n"
        "pushq %%rbp\n"
        "pushq %%rsi\n"
        "pushq %%rdi\n"
        "pushq %%r8\n"
        "pushq %%r9\n"
        "pushq %%r10\n"
        "pushq %%r11\n"
        "pushq %%r12\n"
        "pushq %%r13\n"
        "pushq %%r14\n"
        "pushq %%r15\n"

        "movq %%rsp, (%%rdi)\n" //保存当前栈指针 prev->context.rsp
        "movq (%%rsi), %%rsp\n" //先读取next的rsp

        //从新栈恢复所有寄存器
        "popq %%r15\n"
        "popq %%r14\n"
        "popq %%r13\n"
        "popq %%r12\n"
        "popq %%r11\n"
        "popq %%r10\n"
        "popq %%r9\n"
        "popq %%r8\n"
        "popq %%rdi\n"
        "popq %%rsi\n"
        "popq %%rbp\n"
        "popq %%rbx\n"
        "popq %%rdx\n"
        "popq %%rcx\n"
        "popq %%rax\n"
        "popfq\n"

        "ret\n"
        :
        : "D"(prev), "S"(next)
        : "memory"
    );
}

static task_struct* sched_prev = NULL;
void schedule(){
    //如果当前任务正在运行，放入就绪队列
    if(current_task && (current_task->state == TASK_RUNNING)){
        current_task->state = TASK_READY;
        TaskListAdd(current_task);
    }
    task_struct* next = TaskPickNext();
    //如果没有就绪任务，返回内核任务
    if(!next){
        next = kernel_task;
        if(!next)return;//如果失败，返回
    }
    //切换任务
    sched_prev = current_task;
    next->state = TASK_RUNNING;
    if(next != sched_prev){
        current_task = next;
        switch_to(&sched_prev->context.rsp, &next->context.rsp);
    }
    if(sched_prev->state == TASK_TERMINATED)TaskKill(sched_prev);
}
