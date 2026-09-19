#ifndef _PIPE_H_
#define _PIPE_H_

#include <types.h>

struct pipe;//管道对象

int pipeCreate(struct pipe **out, int flags);//创建管道
long pipeRead(struct pipe *p, void *buf, uint64_t len);//读取管道数据
long pipeWrite(struct pipe *p, const void *buf, uint64_t len);//写入数据到管道
//关闭一端: is_write=1 关闭写端, 0 关闭读端; 两端都关闭时释放对象
void pipeCloseEnd(struct pipe *p, int is_write);
//fork 时子进程继承该端, 引用计数+1
void pipeForkRef(struct pipe *p, int is_write);

#endif
