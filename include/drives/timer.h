#ifndef _TIMER_H_
#define _TIMER_H_

#include <klib.h>


#define PIT_OSC_FREQ 1193182

#define PIT_CMD_MODE_PORT   0x43
#define PIT_CHL0_DATA_PORT  0x40

#define PIT_CHANNLE0    (0 << 6)
#define PIT_LOAD_LOHI   (3 << 4)
#define PIT_MODE0       (0 << 1)
#define PIT_MODE2       (2 << 1) 

#define OS_TICK_HZ 100//系统定时器中断频率

#define CALIBRATION_MS  10 //校准周期 (ms)
#define OS_TICK_HZ_CAL  (1000 / CALIBRATION_MS)

extern uint64_t tsc_freq_hz;//校准后的TSC频率 (Hz)，由tsc_calibrate()填入

void tsc_calibrate(void);

#endif
