#include <acpi/power.h>
#include <idt.h>
#include <delay.h>
#include <io.h>
#include <print.h>
#include <efi.h>

void poweroff_acpi(struct acpi_table_fadt* fadt){
    //检查FADT表
	if(!fadt)return;
    if(memcmp(fadt->header.signature,ACPI_SIG_FADT,4) != 0)return;
    //获取PML1a IO端口并检查
	uint32_t pm1a_cnt_addr = fadt->pm1a_control_block;
	if(pm1a_cnt_addr == 0)return;
    //获取 SLP_TYPa/SLP_TYPb
	uint8_t slp_typa = 0;
	uint8_t slp_typb = 0;
	uint32_t fadt_len = fadt->header.length;
	uint8_t* fadt_bytes = (uint8_t*)fadt;
    //ACPI 1.0 FADT
	if(fadt_len <= 244){
		slp_typa = fadt_bytes[0x8C];
		slp_typb = fadt_bytes[0x8D];
    //ACPI 2.0+ FADT
	}else{
		slp_typa = fadt_bytes[0x8C];
		slp_typb = fadt_bytes[0x8D];
        //如果固件未保持向后兼容，退回到常见S5编码
		if((slp_typa & 0x7) == 0 && (slp_typb & 0x7) == 0){
			slp_typa = 0x07;
			slp_typb = 0x07;
		}
	}
    //读PM1_CNT当前值
	uint16_t pm1a_val = inw(pm1a_cnt_addr);
	uint16_t pm1b_val = 0;
	if(fadt->pm1b_control_block)pm1b_val = inw(fadt->pm1b_control_block);
    //清除SLP_TYP和，保留SCI_EN等
	uint16_t mask = (7 << 10) | (1 << 13);
	pm1a_val &= ~mask;
	pm1a_val |= ((slp_typa & 0x7) << 10) | (1 << 13);
	outw(pm1a_cnt_addr, pm1a_val);//写入PML1a
    io_wait();
    //写入PML1b
	if(fadt->pm1b_control_block){
		pm1b_val &= ~mask;
		pm1b_val |= ((slp_typb & 0x7) << 10) | (1 << 13);
		outw(fadt->pm1b_control_block, pm1b_val);
        io_wait();
	}
	mdelay(10);//延迟已等待生效
    //重试
	pm1a_val = ((slp_typa & 0x7) << 10) | (1 << 13);
	outw(pm1a_cnt_addr, pm1a_val);
	if(fadt->pm1b_control_block){
		pm1b_val = ((slp_typb & 0x7) << 10) | (1 << 13);
		outw(fadt->pm1b_control_block, pm1b_val);
	}
	mdelay(10);//延迟已等待生效
}

void reset_acpi(struct acpi_table_fadt* fadt){
	//检查FADT表
	if(!fadt)return;
    if(memcmp(fadt->header.signature,ACPI_SIG_FADT,4) != 0)return;
	//获取寄存器值
	uintptr_t addr = fadt->reset_register.address;
	uint8_t value = fadt->reset_value;
	uint8_t space_id = fadt->reset_register.space_id;
	//检查寄存器值
	if((!addr) || (addr == 0xAFAFAFAFAFAFAFAF))return;
	if((value == 0) || (value == 0xAF))return;
	//执行
	switch (space_id){
		case ACPI_GAS_SYSTEM_MEMORY:
			uint8_t *reg = (uint8_t*)addr;
			*reg = value;
			break;
		case ACPI_GAS_SYSTEM_IO:
			outb(addr,value);
			io_wait();
			break;
		case ACPI_GAS_PCI:
			//PCI ... 后续实现
			break;
		default:
			return;
	}
	mdelay(10);//延迟已等待生效
	//如果失败，再次执行
	switch (space_id){
		case ACPI_GAS_SYSTEM_MEMORY:
			uint8_t *reg = (uint8_t*)addr;
			*reg = value;
			break;
		case ACPI_GAS_SYSTEM_IO:
			outb(addr,value);
			io_wait();
			break;
		case ACPI_GAS_PCI:
			//PCI ... 后续实现
			break;
		default:
			return;
	}
	mdelay(10);//延迟已等待生效
}

//https://wiki.osdev.org/Reboot
void reset_legacy(){
	uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
}

void SYSTEM_Shutdown(){
	struct acpi_table_fadt* fadt = SYSTEM_ACPI.fadt;
	if(!fadt)return;
	if(memcmp(fadt->header.signature,ACPI_SIG_FADT,4) != 0)return;
	early_printk("The machine will shutdown!\n");
	mdelay(500);
	cli();
	poweroff_acpi(fadt);
	early_printk("ACPI shutdown failed!Try to VM shutdown\n");
	outw(0xB004, 0x2000);
	outw(0x604, 0x2000);
	outw(0x4004, 0x3400);
	outw(0x600, 0x34);
	sti();
	early_printk("Shutdown failed!\n");
}
void SYSTEM_Restart(){
	struct acpi_table_fadt* fadt = SYSTEM_ACPI.fadt;
	if(!fadt)return;
	if(memcmp(fadt->header.signature,ACPI_SIG_FADT,4) != 0)return;
	early_printk("The machine will reboot!\n");
	mdelay(500);
	cli();
	if(UEFI_UseRT && UEFI_RuntimeServices->ResetSystem){
		UEFI_RuntimeServices->ResetSystem(EfiResetCold, 0, 0, NULL);
		mdelay(100);
		early_printk("UEFI Runtime Services restart failed!Try to ACPI shutdown\n");
	}
	reset_acpi(fadt);
	early_printk("ACPI reboot failed!Try to VM shutdown\n");
	outb(0x64, 0xFE);
	sti();
	early_printk("Restart failed!\n");
}