//物理内存管理-位图
#ifndef _MM_BITMAP_H
#define _MM_BITMAP_H 

#include <klib.h>

typedef struct bitmap_t {
    uint32_t bit_size;
    uint32_t hint;
    uint8_t *bits;
} bitmap_t;

//计算位图大小对应的字节数
uint32_t BitmapBitSize(uint32_t bit_size);

//获取一个比特位
_Bool BitmapGetBit(bitmap_t *bitmap,uint32_t index);

void BitmapInit(bitmap_t *bitmap,uint8_t *bits,uint32_t bit_size,_Bool value);//初始化位图
int BitmapAllocBits(bitmap_t *bitmap,_Bool value,uint32_t size);//分配连续size个值为value的位
void BitmapSetBits(bitmap_t *bitmap,uint32_t index,uint32_t size,_Bool value);//设置比特位
_Bool BitmapIsSet(bitmap_t *bitmap,uint32_t index);//设置单个比特位为1
int GetCountOf_InBitmap(bitmap_t *bitmap,_Bool value);//在位图中获取某个值的数量

#endif