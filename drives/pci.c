#include <drives/pci.h>
#include <io.h>

#define PCI_ADDR_PORT   0xCF8
#define PCI_DATA_PORT   0xCFC

//构造CAM地址
static inline uint32_t cam_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg){
    return 0x80000000UL |
           ((uint32_t)bus << 16) |
           ((uint32_t)(dev & 0x1F) << 11) |
           ((uint32_t)(func & 0x07) << 8) |
           ((uint32_t)reg & 0xFC);
}

uint32_t PciRead32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg){
    outl(PCI_ADDR_PORT, cam_addr(bus, dev, func, reg));
    return inl(PCI_DATA_PORT);
}

uint16_t PciRead16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg){
    uint32_t v = PciRead32(bus, dev, func, reg & 0xFC);
    return (uint16_t)((v >> ((reg & 2) * 8)) & 0xFFFF);
}

uint8_t PciRead8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg){
    uint32_t v = PciRead32(bus, dev, func, reg & 0xFC);
    return (uint8_t)((v >> ((reg & 3) * 8)) & 0xFF);
}

void PciWrite32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val){
    outl(PCI_ADDR_PORT, cam_addr(bus, dev, func, reg));
    outl(PCI_DATA_PORT, val);
}

void PciWrite16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint16_t val){
    uint32_t old = PciRead32(bus, dev, func, reg & 0xFC);
    uint32_t shift = (reg & 2) * 8;
    uint32_t v = (old & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    PciWrite32(bus, dev, func, reg & 0xFC, v);
}

void PciWrite8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint8_t val){
    uint32_t old = PciRead32(bus, dev, func, reg & 0xFC);
    uint32_t shift = (reg & 3) * 8;
    uint32_t v = (old & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    PciWrite32(bus, dev, func, reg & 0xFC, v);
}

int PciReadDevice(uint8_t bus, uint8_t dev, uint8_t func, pci_device_t *out){
    uint32_t v = PciRead32(bus, dev, func, PCI_REG_VENDOR);
    if(v == 0xFFFFFFFF) return -1;
    if(!out) return 0;
    out->bus = bus;
    out->dev = dev;
    out->func = func;
    out->vendor_id = (uint16_t)(v & 0xFFFF);
    out->device_id = (uint16_t)(v >> 16);
    out->class_code = PciRead8(bus, dev, func, PCI_REG_CLASS);
    out->subclass   = PciRead8(bus, dev, func, PCI_REG_SUBCLASS);
    out->progif     = PciRead8(bus, dev, func, PCI_REG_PROGIF);
    return 0;
}

void PciScanBus(uint8_t bus, pci_scan_callback cb, void *arg){
    if(!cb) return;
    for(uint8_t dev = 0; dev < 32; dev++){
        pci_device_t d;
        if(PciReadDevice(bus, dev, 0, &d) != 0) continue;
        cb(&d, arg);
        uint8_t hdr = PciRead8(bus, dev, 0, PCI_REG_HDRTYPE);
        if(hdr & 0x80){
            for(uint8_t func = 1; func < 8; func++){
                if(PciReadDevice(bus, dev, func, &d) == 0) cb(&d, arg);
            }
        }
    }
}

void PciScanAll(pci_scan_callback cb, void *arg){
    for(uint16_t bus = 0; bus < 256; bus++){
        PciScanBus(bus, cb, arg);
    }
}
