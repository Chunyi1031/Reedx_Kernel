/**
 * include/acpi/power.h
 * 
 * Copyright (C) 2026 Liu Chunyi
 * 
 * Reedx电源管理
 */
#ifndef _POWER_H_
#define _POWER_H_

#include <klib.h>
#include <acpi/acpi.h>

#define ACPI_SLEEP_STATE_S5 0x07
#define ACPI_SLEEP_TYPE_S5  0x00

#define LINUX_REBOOT_MAGIC1     0xfee1dead
#define LINUX_REBOOT_MAGIC2     672274793
#define LINUX_REBOOT_MAGIC2A    85072278
#define LINUX_REBOOT_MAGIC2B    369367448
#define LINUX_REBOOT_MAGIC2C    537993216
#define LINUX_REBOOT_CMD_RESTART    0x01234567
#define LINUX_REBOOT_CMD_POWER_OFF  0x4321fedc
#define LINUX_REBOOT_CMD_HALT       0xcdef0123

void poweroff_acpi(struct acpi_table_fadt* fadt);
void reset_acpi(struct acpi_table_fadt* fadt);
void reset_legacy();

void SYSTEM_Shutdown();//系统标准关机
void SYSTEM_Restart();//系统标准重启
void SYSTEM_Halt();//系统标准停机

#endif