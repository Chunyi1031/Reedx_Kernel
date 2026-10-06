#include <mm/bitmap.h>

//计算位图大小对应的字节数
uint32_t BitmapBitSize(uint32_t bit_size){
    return (bit_size + 8 - 1) / 8;
}

//获取一个比特位
_Bool BitmapGetBit(bitmap_t *bitmap,uint32_t index){
    _Bool result = bitmap->bits[index / 8] >> (index % 8) & 0x1;
    return result;
}

//初始化位图
void BitmapInit(bitmap_t *bitmap,uint8_t *bits,uint32_t bit_size,_Bool value){
    bitmap->bit_size = bit_size;
    bitmap->hint = 0;
    bitmap->bits = bits;
    size_t bytes = BitmapBitSize(bit_size);
    if(value){
        memset((void*)bitmap->bits,0xFF,bytes);
    }else{
        memset((void*)bitmap->bits,0,bytes);
    }
}

//在范围内查找连续size个值为value的位，找到返回起始位号
static int BitmapFindRun(bitmap_t *bitmap,uint32_t from,uint32_t to,_Bool value,uint32_t size){
    uint32_t run = 0, start = 0;
    for(uint32_t i = from;i < to;i ++){
        if(BitmapGetBit(bitmap,i) == value){
            if(run == 0)start = i;
            if(++run >= size)return (int)start;
        }else{
            run = 0;
        }
    }
    return -1;
}

//分配连续size个值为value的位
int BitmapAllocBits(bitmap_t *bitmap,_Bool value,uint32_t size){
    if(size == 0 || size > bitmap->bit_size)return -1;
    uint32_t begin = bitmap->hint;
    if(begin >= bitmap->bit_size || begin + size > bitmap->bit_size)begin = 0;
    int r = BitmapFindRun(bitmap,begin,bitmap->bit_size,value,size);
    if(r < 0){
        uint32_t end = begin + size;
        if(end > bitmap->bit_size)end = bitmap->bit_size;
        r = BitmapFindRun(bitmap,0,end,value,size);
    }
    if(r < 0)return -1;
    BitmapSetBits(bitmap,(uint32_t)r,size,1);
    bitmap->hint = (uint32_t)r + size;
    if(bitmap->hint >= bitmap->bit_size)bitmap->hint = 0;
    return r;
}

//设置比特位
void BitmapSetBits(bitmap_t *bitmap,uint32_t index,uint32_t size,_Bool value){
    for(int i = 0;i < size && index < bitmap->bit_size;i ++){
        if(value){
            bitmap->bits[index / 8] |= (1 << (index % 8));
        }else{
            bitmap->bits[index / 8] &= ~(1 << (index % 8));
        }
        index ++;
    }
}

//设置单个比特位为1
_Bool BitmapIsSet(bitmap_t *bitmap,uint32_t index){
    return BitmapGetBit(bitmap,index);
}

int GetCountOf_InBitmap(bitmap_t *bitmap,_Bool value){
    int count = 0;
    for(int i = 0;i < bitmap->bit_size;i ++){
        if(BitmapGetBit(bitmap,i) == value)count ++;
    }
    return count;
}