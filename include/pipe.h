#ifndef _PIPE_H_
#define _PIPE_H_

#include <types.h>

struct pipe;//管道对象

int pipeCreate(struct pipe **out, int flags);//创建管道
long pipeRead(struct pipe *p, void *buf, uint64_t len);//读取管道数据
long pipeWrite(struct pipe *p, const void *buf, uint64_t len);//写入数据到管道
int pipeReady(struct pipe *p);//查询管道就绪状态
void pipeCloseEnd(struct pipe *p, int is_write);//关闭一端
void pipeForkRef(struct pipe *p, int is_write);//fork时子进程继承该端, 引用计数+1

#endif
