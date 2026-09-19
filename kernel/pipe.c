#include <pipe.h>
#include <task.h>
#include <signals.h>
#include <klib.h>
#include <spinlock.h>
#include <syscalls.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define PIPE_BUF_SIZE 4096

struct pipe {
    char             buf[PIPE_BUF_SIZE];//环形缓冲
    uint32_t         head;              //读位置
    uint32_t         count;             //缓冲内字节数
    uint32_t         readers;           //打开的读端数量
    uint32_t         writers;           //打开的写端数量
    int              nonblock;          //O_NONBLOCK
    spinlock_t       lock;
    struct list_node read_wq;           //读端阻塞队列
    struct list_node write_wq;          //写端阻塞队列
};

//本地链表
static inline void plist_add_tail(struct list_node *n, struct list_node *h){
    n->prev = h->prev;
    n->next = h;
    h->prev->next = n;
    h->prev = n;
}
static inline void plist_del(struct list_node *n){
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n->next = NULL;
}
static inline int plist_empty(struct list_node *h){ return h->next == h; }
static inline task_struct *plist_task(struct list_node *n){
    return (task_struct*)((char*)n - (size_t)&((task_struct*)0)->wait_node);
}

//唤醒等待队列上全部任务
static void pipe_wake_all(struct list_node *wq){
    while(!plist_empty(wq)){
        task_struct *t = plist_task(wq->next);
        plist_del(&t->wait_node);
        if(t->state == TASK_BLOCKED)TaskListAdd(t);
    }
}

int pipeCreate(struct pipe **out, int flags){
    //分配管道对象
    struct pipe *p = (struct pipe*)PHYS_TO_VIRT(Pmm_Malloc(2));
    if(!p)return -ENOMEM;
    //初始化管道对象
    memset(p, 0, 8192);
    p->head = 0;
    p->count = 0;
    p->readers = 1;
    p->writers = 1;
    p->nonblock = (flags & O_NONBLOCK) ? 1 : 0;
    spin_lock_init(&p->lock);
    p->read_wq.prev = &p->read_wq;
    p->read_wq.next = &p->read_wq;
    p->write_wq.prev = &p->write_wq;
    p->write_wq.next = &p->write_wq;
    *out = p;
    return 0;
}

long pipeRead(struct pipe *p, void *buf, uint64_t len){
    if(!p || !buf || !len)return 0;
    uint64_t flags;
    spin_lock_irqsave(&p->lock, flags);
    while(1){
        if(p->count > 0)break;//已有数据可读
        //所有写端已关闭，EOF
        if(p->writers == 0){
            spin_unlock_irqrestore(&p->lock, flags);
            return 0;
        }
        //非阻塞模式，无数据直接返回
        if(p->nonblock){
            spin_unlock_irqrestore(&p->lock, flags);
            return -EAGAIN;
        }
        //有待投递信号，中断读取
        if(SignalPending()){
            spin_unlock_irqrestore(&p->lock, flags);
            return -EINTR;
        }
        //阻塞等待数据
        plist_add_tail(&current_task->wait_node, &p->read_wq);
        current_task->state = TASK_BLOCKED;
        spin_unlock_irqrestore(&p->lock, flags);
        schedule();
        spin_lock_irqsave(&p->lock, flags);
    }
    //读取数据
    uint64_t n = 0;
    while(n < len && p->count > 0){
        ((char*)buf)[n++] = p->buf[p->head];
        p->head = (p->head + 1) & (PIPE_BUF_SIZE - 1);
        p->count--;
    }
    if(!plist_empty(&p->write_wq))pipe_wake_all(&p->write_wq);//腾出空间唤醒写端
    spin_unlock_irqrestore(&p->lock, flags);
    return (long)n;
}

long pipeWrite(struct pipe *p, const void *buf, uint64_t len){
    if(!p || !buf || !len)return 0;
    uint64_t flags;
    uint64_t written = 0;
    spin_lock_irqsave(&p->lock, flags);
    //循环写入数据
    while(written < len){
        //所有读端已关闭，写入失败
        if(p->readers == 0){
            spin_unlock_irqrestore(&p->lock, flags);
            return written ? (long)written : -EPIPE;
        }
        //管道已满
        if(p->count == PIPE_BUF_SIZE){
            //非阻塞时，直接返回
            if(p->nonblock){
                spin_unlock_irqrestore(&p->lock, flags);
                return written ? (long)written : -EAGAIN;
            }
            //有待投递信号，中断写入
            if(SignalPending()){
                spin_unlock_irqrestore(&p->lock, flags);
                return written ? (long)written : -EINTR;
            }
            //阻塞等待缓冲区有足够空间
            plist_add_tail(&current_task->wait_node, &p->write_wq);
            current_task->state = TASK_BLOCKED;
            spin_unlock_irqrestore(&p->lock, flags);
            schedule();
            spin_lock_irqsave(&p->lock, flags);
            continue;
        }
        uint64_t space = PIPE_BUF_SIZE - p->count;//剩余空间
        uint64_t n = len - written;//剩余待写字节
        if(n > space)n = space;//如果空间不够，能写多少写多少
        //执行写入
        for(uint64_t i = 0; i < n; i++){
            uint32_t tail = (p->head + p->count) & (PIPE_BUF_SIZE - 1);
            p->buf[tail] = ((const char*)buf)[written + i];
            p->count++;
        }
        written += n;
    }
    if(!plist_empty(&p->read_wq))pipe_wake_all(&p->read_wq);//有数据唤醒读端
    spin_unlock_irqrestore(&p->lock, flags);
    return (long)written;
}

void pipeCloseEnd(struct pipe *p, int is_write){
    if(!p)return;
    uint64_t flags;
    spin_lock_irqsave(&p->lock, flags);
    //写端
    if(is_write){
        if(p->writers > 0)p->writers--;
        if(p->writers == 0 && !plist_empty(&p->read_wq))pipe_wake_all(&p->read_wq);//唤醒读端返回EOF
    //读端
    }else{
        if(p->readers > 0)p->readers--;
        if(p->readers == 0 && !plist_empty(&p->write_wq))pipe_wake_all(&p->write_wq);//唤醒写端返回EPIPE
    }
    int dead = (p->readers == 0 && p->writers == 0);
    spin_unlock_irqrestore(&p->lock, flags);
    if(dead)Pmm_Free((void*)VIRT_TO_PHYS((uintptr_t)p), 2);//释放管道对象
}

void pipeForkRef(struct pipe *p, int is_write){
    if(!p)return;
    unsigned long flags;
    spin_lock_irqsave(&p->lock, flags);
    if(is_write)p->writers++;
    else p->readers++;
    spin_unlock_irqrestore(&p->lock, flags);
}
