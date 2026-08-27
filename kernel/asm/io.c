#include <io.h>

void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %b0, %w1" : : "a"(value), "Nd"(port) : "memory");
}
uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %1, %0" : : "Nd"(port), "a"(val));
}
void insw(uint16_t port, void *dst, uint32_t count) {
    __asm__ volatile("rep insw" : "+D"(dst), "+c"(count) : "d"(port) : "memory");
}
void outsw(uint16_t port, const void *src, uint32_t count) {
    __asm__ volatile("rep outsw" : "+S"(src), "+c"(count) : "d"(port) : "memory");
}
void io_wait(void) {
    outb(0x80, 0);
}
