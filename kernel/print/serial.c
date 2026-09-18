#include <serial.h>
#include <io.h>

_Bool SerialIsInited = false;

//https://wiki.osdev.org/Serial_Ports
int InitSerial(uint16_t PORT){
    outb(PORT + 1, 0x00);    // Disable all interrupts
    outb(PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(PORT + 1, 0x00);    //                  (hi byte)
    outb(PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
    outb(PORT + 4, 0x1E);    // Set in loopback mode, test the serial chip
    outb(PORT + 0, 0xAE);    // Test serial chip (send byte 0xAE and check if serial returns same byte)
    // Check if serial is faulty (i.e: not same byte as sent)
    if(inb(PORT + 0) != 0xAE) {
        return 1;
    }
    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    outb(PORT + 4, 0x0F);
    SerialIsInited = true;
    return 0;
}

_Bool IsTransmitEmpty(uint16_t port) {
    if(SerialIsInited){
        return inb(port + 5) & 0x20;
    }else{
        return false;
    }
}

void SerialWriteChar(uint16_t port, char c) {
    uint32_t guard = 0;
    if(!SerialIsInited)return;//检查是否初始化
    while(!IsTransmitEmpty(port) && ++guard < 100000);
    outb(port, (uint8_t)c);//发送字符
}

void SerialWriteString(uint16_t port, const char* str) {
    while(*str){
        SerialWriteChar(port, *str);
        str++;
    }
}
