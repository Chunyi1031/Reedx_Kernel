#include <task.h>
#include <mm/pmm.h>
#include <idt.h>
#include <irq.h>
#include <spinlock.h>
#include <desc.h>

#define IA32_FS_BASE 0xC0000100

static inline uint64_t task_rdmsr(uint32_t msr){
	uint32_t low, high;
	__asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}
static inline void task_wrmsr(uint32_t msr, uint64_t val){
	uint32_t low = (uint32_t)val;
	uint32_t high = (uint32_t)(val >> 32);
	__asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr) : "memory");
}

static struct list_node task_list_head = {&task_list_head, &task_list_head};//任务链表头
task_struct* current_task = NULL;//当前运行的任务
task_struct* kernel_task = NULL;//内核任务
task_struct* idle_task = NULL;//idle任务
uint64_t user_kernel_stack_top = 0;//当前用户任务的内核栈顶
//PID分配器
static pid_t next_pid = 1;
pid_t AllocPid(void){
    return next_pid++;
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

void idle_thread(){
    SYSTEM_STOP();
}

void TaskInit(){
    //初始化链表
    task_list_head.prev = &task_list_head;
    task_list_head.next = &task_list_head;
    //填充内核进程结构体
    kernel_task = (task_struct*)(uintptr_t)PHYS_TO_VIRT(Pmm_Malloc(1));
    if (!kernel_task) SYSTEM_STOP();
    memset(kernel_task, 0, 4096);
    kernel_task->pid = 0;
    kernel_task->tgid = 0;
    kernel_task->state = TASK_READY;
    waitq_init(&kernel_task->child_wq);
    strcpy(kernel_task->name, "Reedx Kernel");
    kernel_task->kernel_stack = (void*)PHYS_TO_VIRT((uintptr_t)SYSTEM_BootParam->KernelStackAddress);
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
    if (current_task && current_task->pid == pid) return current_task;
    if (kernel_task && kernel_task->pid == pid) return kernel_task;
    return NULL;
}

//查找指定父进程的指定子进程
task_struct* TaskFindChild(pid_t parent_pid, pid_t child_pid){
    if(parent_pid <= 0 || child_pid <= 0)return NULL;
    struct list_node *pos;
    task_struct *task;
    list_for_each(pos, &task_list_head) {
        task = container_of(pos, task_struct, list);
        if(task->parent == parent_pid && task->pid == child_pid)return task;
    }
    if(current_task && current_task->parent == parent_pid && current_task->pid == child_pid)return current_task;
    return NULL;
}

//查找指定父进程的僵尸子进程
static task_struct* find_zombie_child(pid_t parent_pid, pid_t child_pid){
    if(parent_pid <= 0)return NULL;
    struct list_node *pos;
    task_struct *task;
    list_for_each(pos, &task_list_head) {
        task = container_of(pos, task_struct, list);
        if(task->parent != parent_pid)continue;
        if(task->state != TASK_TERMINATED)continue;
        if(child_pid > 0 && task->pid != child_pid)continue;
        return task;
    }
    return NULL;
}

task_struct* TaskFindZombie(pid_t parent_pid, pid_t child_pid){
    return find_zombie_child(parent_pid, child_pid);
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
        if(task->state != TASK_READY)continue;//僵尸/阻塞任务跳过
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
    current_task->state = TASK_TERMINATED;//标记为僵尸
    //僵尸重新挂回任务链表
    if(list_has_node(&current_task->list))list_del(&current_task->list);
    list_add_tail(&current_task->list, &task_list_head);
    //先回收自己的僵尸子进程
    task_struct *zombie;
    while((zombie = find_zombie_child(current_task->pid, 0)) != NULL)TaskKill(zombie);
    //唤醒父进程(若存在且活着,僵尸父进程不算)
    if(current_task->parent > 0){
        task_struct *parent = TaskFind(current_task->parent);
        if(parent && parent->state != TASK_TERMINATED)wake_up(&parent->child_wq);
    }
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
    if(t->kernel_stack)Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)t->kernel_stack),t->stack_size / 4096);
    if(t->mm){ mmput(t->mm); t->mm = NULL; }//释放用户地址空间
    memset(t,0,4096);
    Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)t),1);
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

//初始化互斥锁为未锁定状态
void mutex_init(mutex_t *m){
	m->locked = 0;
	m->owner = NULL;
	spin_lock_init(&m->lock);
	m->wq.prev = &m->wq;
	m->wq.next = &m->wq;
}

//加锁
void mutex_lock(mutex_t *m){
	uint64_t flags;
	if(!m)return;
	spin_lock_irqsave(&m->lock, flags);
    //如果未锁定，加锁
	if(!m->locked){
		m->locked = 1;//标记为已锁定
		m->owner = current_task;//设置持有者为当前任务
		spin_unlock_irqrestore(&m->lock, flags);
		return;
	}
	list_add_tail(&current_task->wait_node, &m->wq);//将当前任务加入等待队列
	current_task->state = TASK_BLOCKED;//标记为阻塞
	spin_unlock_irqrestore(&m->lock, flags);
	schedule();//让出CPU
}

//解锁
void mutex_unlock(mutex_t *m){
	uint64_t flags;
	task_struct *task;
	if(!m)return;
	spin_lock_irqsave(&m->lock, flags);
    //不是持有者,拒绝解锁
	if(m->owner != current_task){
		spin_unlock_irqrestore(&m->lock, flags);
		return;
	}
    //如果等待队列为空
	if(list_empty(&m->wq)){
        //清空数据
		m->locked = 0;
		m->owner = NULL;
	}else{
        //不为空，唤醒等待队列上的一个任务，并将其设为持有者
		task = container_of(m->wq.next, task_struct, wait_node);
		list_del(&task->wait_node);
		m->owner = task;
		if(task->state == TASK_BLOCKED)TaskListAdd(task);
	}
	spin_unlock_irqrestore(&m->lock, flags);
}

//非阻塞尝试加锁
_Bool mutex_trylock(mutex_t *m){
	uint64_t flags;
	_Bool ret;
	if(!m)return false;
	spin_lock_irqsave(&m->lock, flags);
    //如果未锁定，加锁并返回true
	if(!m->locked){
		m->locked = 1;
		m->owner = current_task;
		ret = true;
    //否则返回false
	}else{
		ret = false;
	}
	spin_unlock_irqrestore(&m->lock, flags);
	return ret;
}

//初始化消息队列
void msgq_init(msg_queue_t *q, void **buf, int capacity){
    //初始化结构体
	q->buf = buf;
	q->capacity = capacity;
	q->head = 0;
	q->tail = 0;
	sem_init(&q->slots, capacity);//初始所有槽位空闲
	sem_init(&q->items, 0);//初始无消息
	mutex_init(&q->lock);//初始化互斥锁
}

//发送消息
void msgq_send(msg_queue_t *q, void *msg){
	sem_down(&q->slots);//等待空闲槽位
	mutex_lock(&q->lock);//加锁
	q->buf[q->tail] = msg;//写入消息
	q->tail = (q->tail + 1) % q->capacity;//移动尾指针
	mutex_unlock(&q->lock);//解锁
	sem_up(&q->items);//通知有新消息
}

//接收消息
void *msgq_recv(msg_queue_t *q){
	void *msg;
	sem_down(&q->items);//等待消息
	mutex_lock(&q->lock);//加锁
	msg = q->buf[q->head];//读取消息
	q->head = (q->head + 1) % q->capacity;//移动头指针
	mutex_unlock(&q->lock);//解锁
	sem_up(&q->slots);//通知有空闲槽位
	return msg;
}

//非阻塞发送
_Bool msgq_trysend(msg_queue_t *q, void *msg){
	if(!sem_trydown(&q->slots))return false;
	mutex_lock(&q->lock);
	q->buf[q->tail] = msg;
	q->tail = (q->tail + 1) % q->capacity;
	mutex_unlock(&q->lock);
	sem_up(&q->items);
	return true;
}

//非阻塞接收
void *msgq_tryrecv(msg_queue_t *q){
	void *msg;
	if(!sem_trydown(&q->items))return NULL;
	mutex_lock(&q->lock);
	msg = q->buf[q->head];
	q->head = (q->head + 1) % q->capacity;
	mutex_unlock(&q->lock);
	sem_up(&q->slots);
	return msg;
}

//初始化条件变量
void cond_init(condition_t *cv){
	cv->wq.prev = &cv->wq;
	cv->wq.next = &cv->wq;
	spin_lock_init(&cv->lock);
}

//原子地释放mutex并睡眠,被唤醒后重新获取mutex
void cond_wait(condition_t *cv, mutex_t *m){
	uint64_t flags;
	if(!cv || !m)return;
	spin_lock_irqsave(&cv->lock, flags);
	mutex_unlock(m);//释放锁
    //睡眠
	list_add_tail(&current_task->wait_node, &cv->wq);
	current_task->state = TASK_BLOCKED;
	spin_unlock_irqrestore(&cv->lock, flags);
	schedule();//切走
	mutex_lock(m);//醒来后重新获取mutex
}

//唤醒等待队列上的一个任务
void cond_signal(condition_t *cv){
	unsigned long flags;
	task_struct *task;
	if(!cv)return;
	spin_lock_irqsave(&cv->lock, flags);
	if(!list_empty(&cv->wq)){
		task = container_of(cv->wq.next, task_struct, wait_node);
		list_del(&task->wait_node);
		if(task->state == TASK_BLOCKED)TaskListAdd(task);
	}
	spin_unlock_irqrestore(&cv->lock, flags);
}

//唤醒等待队列上的全部任务
void cond_broadcast(condition_t *cv){
	uint64_t flags;
	task_struct *task;
	if(!cv)return;
	spin_lock_irqsave(&cv->lock, flags);
	while(!list_empty(&cv->wq)){
		task = container_of(cv->wq.next, task_struct, wait_node);
		list_del(&task->wait_node);
		if(task->state == TASK_BLOCKED)TaskListAdd(task);
	}
	spin_unlock_irqrestore(&cv->lock, flags);
}

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
    }else if(current_task && (current_task->state == TASK_BLOCKED)){
        if(!list_has_node(&current_task->list))list_add_tail(&current_task->list, &task_list_head);
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
        //切换地址空间
        uintptr_t prev_pgd = sched_prev->mm ? (uintptr_t)sched_prev->mm->pgd : (uintptr_t)KERNEL_PML4;
        uintptr_t next_pgd = next->mm ? (uintptr_t)next->mm->pgd : (uintptr_t)KERNEL_PML4;
        if(prev_pgd != next_pgd)set_cr3(next_pgd);
        //更新用户任务TSS.rsp0与syscall内核栈顶
        if(next->mm){
            user_kernel_stack_top = (uint64_t)next->kernel_stack + next->stack_size;
            cpu_tss.rsp0 = user_kernel_stack_top;
        }
        //切换FS段基址
        if(sched_prev->mm) sched_prev->fs_base = task_rdmsr(IA32_FS_BASE);
        if(next->mm)task_wrmsr(IA32_FS_BASE, next->fs_base);
        current_task = next;
        switch_to(&sched_prev->context.rsp, &next->context.rsp);//切换任务上下文
    }
    //清理已终止任务
    if(sched_prev->state == TASK_TERMINATED){
        task_struct *parent = (sched_prev->parent > 0) ? TaskFind(sched_prev->parent) : NULL;
        if(!parent || parent->state == TASK_TERMINATED)TaskKill(sched_prev);//有存活的父进程则保留为僵尸进程，否则清理
    }
}

//构造iretq帧返回ring3
__attribute__((naked))
void user_trampoline(void){
    __asm__ volatile(
        "pushq %rbx\n\t"    // ss
        "pushq %r15\n\t"    // 用户 rsp
        "pushq %r14\n\t"    // 用户 rflags
        "pushq %r13\n\t"    // cs
        "pushq %r12\n\t"    // rip
        "iretq\n\t"
    );
}
