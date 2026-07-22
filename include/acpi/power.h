#ifndef _POWER_H_
#define _POWER_H_

#include <klib.h>
#include <acpi/acpi.h>

#define ACPI_SLEEP_STATE_S5 0x07
#define ACPI_SLEEP_TYPE_S5  0x00

void poweroff_acpi(struct acpi_table_fadt* fadt);
void reset_acpi(struct acpi_table_fadt* fadt);
void reset_legacy();

void SYSTEM_Shutdown();//系统标准关机
void SYSTEM_Restart();//系统标准重启

#endif