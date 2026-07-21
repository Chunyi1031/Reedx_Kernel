#include <acpi/acpi.h>
#include <klib.h>

sys_acpi_info SYSTEM_ACPI;

static struct acpi_table_fadt* find_fadt(struct acpi_table_xsdt* xsdt){
    if(memcmp(xsdt->header.signature,ACPI_SIG_XSDT,4) != 0)return NULL;
    int entry_count = (xsdt->header.length - sizeof(struct acpi_table_xsdt)) / sizeof(struct acpi_table_fadt*);
    for(int i = 0;i < entry_count;i++){
        struct acpi_table_fadt* entry = (struct acpi_table_fadt*)xsdt->table_offset_entry[i];
        if(memcmp(entry->header.signature,ACPI_SIG_FADT,4) == 0)return entry;
    }
    return NULL;
}

static struct acpi_table_madt* find_madt(struct acpi_table_xsdt* xsdt){
    if(memcmp(xsdt->header.signature,ACPI_SIG_XSDT,4) != 0)return NULL;
    int entry_count = (xsdt->header.length - sizeof(struct acpi_table_xsdt)) / sizeof(struct acpi_table_madt*);
    for(int i = 0;i < entry_count;i++){
        struct acpi_table_madt* entry = (struct acpi_table_madt*)xsdt->table_offset_entry[i];
        if(memcmp(entry->header.signature,ACPI_SIG_MADT,4) == 0)return entry;
    }
    return NULL;
}

int InitACPI(struct acpi_table_rsdp* rsdp){
    memset(&SYSTEM_ACPI,0,sizeof(SYSTEM_ACPI));
    if(!rsdp)return 1;//参数检查
    SYSTEM_ACPI.rsdp = rsdp;
    //检查表
    if(memcmp(rsdp->signature,ACPI_SIG_RSDP,8) != 0)return 2;
    if(rsdp->revision < 2)return 2;//ACPI2.0+使用XSDT
    //找RSDT表
    if(!rsdp->rsdt_physical_address)return 3;
    SYSTEM_ACPI.rsdt = (struct acpi_table_rsdt*)(u64)rsdp->rsdt_physical_address;
    //找XSDT表
    if(!rsdp->xsdt_physical_address)return 3;
    SYSTEM_ACPI.xsdt = (struct acpi_table_xsdt*)rsdp->xsdt_physical_address;
    //找FADT表
    SYSTEM_ACPI.fadt = find_fadt(SYSTEM_ACPI.xsdt);
    if(!SYSTEM_ACPI.fadt)return 4;
    //找MADT表
    SYSTEM_ACPI.madt = find_madt(SYSTEM_ACPI.xsdt);
    if(!SYSTEM_ACPI.madt)return 5;
    //成功
    SYSTEM_ACPI.is_init = true;
    return 0;
}