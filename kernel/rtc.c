/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux 7.1.3生成
 *
 * 参考：Linux 7.1.3 arch/x86/kernel/rtc.c (rtc_cmos_read)
 *              drivers/rtc/rtc-mc146818-lib.c (mc146818_avoid_UIP)
 *              drivers/rtc/lib.c (rtc_tm_to_time64 / rtc_time64_to_tm)
 *              Tomohiko Sakamoto (day-of-week algorithm)
 *              UEFI Specification v2.10 §8.3 (GetTime), §4.6 (Table Signatures)
 */

#include <rtc.h>
#include <io.h>
#include <irq.h>
#include <efi.h>
#include <print.h>
#include <drives/timer.h>
#include <mm/pgtables.h>


/* UEFI 运行时服务表签名 ("RUNTSERV" ASCII，小端) */
#define EFI_RT_SERVICES_SIGNATURE  0x56524553544e5552ULL
#define RTC_EFI_YEAR_MIN  2020  // 合理年份下限
#define RTC_EFI_YEAR_MAX  2100  // 合理年份上限

//CMOS RTC寄存器（MC146818 / Intel ICH 兼容）
#define CMOS_INDEX      0x70
#define CMOS_DATA       0x71
#define RTC_SECONDS     0x00
#define RTC_MINUTES     0x02
#define RTC_HOURS       0x04
#define RTC_WDAY        0x06  //1=周日..7=周六
#define RTC_DAY_MONTH   0x07
#define RTC_MONTH       0x08
#define RTC_YEAR        0x09
#define RTC_REG_A       0x0A
#define RTC_REG_B       0x0B
#define RTC_REG_C       0x0C  //读取后自动清零各中断标志
#define RTC_REG_D       0x0D
#define RTC_UIP         (1 << 7)
#define RTC_DM_BINARY   (1 << 2)
#define RTC_24H         (1 << 1)

static uint64_t boot_epoch = 0;//启动时的UTC Unix时间戳
_Bool rtc_efi_available = false;//UEFI 运行时服务 GetTime 是否可用

//读CMOS 寄存器
static uint8_t cmos_read(uint8_t reg){
	outb(CMOS_INDEX, reg);
	return inb(CMOS_DATA);
}

//BCD转二进制
static uint8_t bcd_to_bin(uint8_t bcd){
	return (bcd >> 4) * 10 + (bcd & 0x0F);
}

//闰年判断
static int is_leap_year(uint32_t year){
	return (!(year % 4) && year % 100) || !(year % 400);
}

//Tomohiko Sakamoto星期算法，返回0=周日..6=周六
static uint8_t compute_wday(uint16_t year, uint8_t month, uint8_t day){
	static const int8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
	int32_t y = year;
	y -= month < 3;
	return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
}

//根据TIMEZONE_DST_RULE判断给定UTC日期是否处于夏时令
static uint8_t is_dst(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t wday){
#if TIMEZONE_DST_RULE == 0
	(void)year; (void)month; (void)day; (void)hour; (void)wday;
	return 0;
#elif TIMEZONE_DST_RULE == 1
	//US:3月第2个周日 02:00,11月第1个周日 02:00
	uint8_t mar_sun2 = 14 - compute_wday(year, 3, 1);
	uint8_t nov_sun1 = 7  - compute_wday(year, 11, 1) + 1;
	if (month > 3 && month < 11) return 1;
	if (month == 3 && (day > mar_sun2 || (day == mar_sun2 && hour >= 2)))
		return 1;
	if (month == 11 && (day < nov_sun1 || (day == nov_sun1 && hour < 2)))
		return 1;
	return 0;
#elif TIMEZONE_DST_RULE == 2
	//EU:3月最后一个周日01:00,10月最后一个周日 01:00
	uint8_t mar_sun_last = 31 - compute_wday(year, 3, 31);
	uint8_t oct_sun_last = 31 - compute_wday(year, 10, 31);
	if (month > 3 && month < 10) return 1;
	if (month == 3 && (day > mar_sun_last || (day == mar_sun_last && hour >= 1)))
		return 1;
	if (month == 10 && (day < oct_sun_last || (day == oct_sun_last && hour < 1)))
		return 1;
	return 0;
#else
	#error "TIMEZONE_DST_RULE must be 0, 1, or 2"
#endif
}

static uint64_t date_to_epoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second){
	static const uint16_t moff[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
	uint32_t yrs = year - 1970;
	uint32_t total_days;
	total_days = yrs * 365 + (yrs + 2) / 4;
	total_days += moff[month - 1];
	if (month > 2 && is_leap_year(year))total_days++;
	total_days += day - 1;
	return (uint64_t)total_days * 86400 + hour * 3600 + minute * 60 + second;
}

//分解日期时间（wday/dst自动填充）
static void epoch_to_date(uint64_t epoch, rtc_time_t *tm){
	static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
	uint32_t days, year, mon;
	days = epoch / 86400;
	tm->hour   = (epoch % 86400) / 3600;
	tm->minute = (epoch % 3600) / 60;
	tm->second = epoch % 60;
	year = 1970;
	while (1) {
		uint32_t diy = is_leap_year(year) ? 366 : 365;
		if (days < diy) break;
		days -= diy;
		year++;
	}
	tm->year = year;
	for (mon = 0; mon < 12; mon++) {
		uint32_t md = mdays[mon];
		if (mon == 1 && is_leap_year(year)) md = 29;
		if (days < md) break;
		days -= md;
	}
	tm->month = mon + 1;
	tm->day   = days + 1;
	tm->wday  = compute_wday(tm->year, tm->month, tm->day);
	tm->dst   = is_dst(tm->year, tm->month, tm->day, tm->hour, tm->wday);
}

//从CMOS RTC安全读取日期时间
#define CMOS_BCD_DECODE(field) \
	if (!(reg_b & RTC_DM_BINARY)) field = bcd_to_bin(field)

static void cmos_read_time(rtc_time_t *tm){
	uint8_t sec, min, hour, wday, day, mon, yr, reg_b, century;
	uint8_t tries;
	//清空 Register C
	(void)cmos_read(RTC_REG_C);
	for (tries = 0; tries < 5; tries++) {
		while (cmos_read(RTC_REG_A) & RTC_UIP);
		sec  = cmos_read(RTC_SECONDS);
		min  = cmos_read(RTC_MINUTES);
		hour = cmos_read(RTC_HOURS);
		wday = cmos_read(RTC_WDAY);
		day  = cmos_read(RTC_DAY_MONTH);
		mon  = cmos_read(RTC_MONTH);
		yr   = cmos_read(RTC_YEAR);
		if(sec == cmos_read(RTC_SECONDS))break;
	}
	reg_b = cmos_read(RTC_REG_B);
	CMOS_BCD_DECODE(sec);
	CMOS_BCD_DECODE(min);
	CMOS_BCD_DECODE(hour);
	CMOS_BCD_DECODE(day);
	CMOS_BCD_DECODE(mon);
	CMOS_BCD_DECODE(yr);
	century = cmos_read(0x32);
	if (century >= 19 && century <= 21)tm->year = century * 100 + yr;
	else if (yr >= 70)tm->year = 1900 + yr;
	else tm->year = 2000 + yr;
	tm->month  = mon;
	tm->day    = day;
	tm->hour   = hour;
	tm->minute = min;
	tm->second = sec;
	//星期
	if (!(reg_b & RTC_DM_BINARY))tm->wday = (wday == 0 || wday > 7) ? compute_wday(tm->year, mon, day) : wday - 1;
	else tm->wday = compute_wday(tm->year, mon, day);
	tm->dst = is_dst(tm->year, tm->month, tm->day, tm->hour, tm->wday);
}

/*
 * 检查 UEFI 运行时服务表签名是否有效。
 * 原理：ExitBootServices 后引导服务内存被回收（填充 0xAF），
 * 若 RuntimeServices 表本身或其函数指针指向已回收内存，
 * 调用会返回垃圾数据。通过签名校验 + 年份范围校验双重保险。
 */
__attribute__((ms_abi, target("no-sse")))
static _Bool rtc_validate_efi_rt(void){
	if (!UEFI_UseRT) return false;
	if (!UEFI_RuntimeServices) return false;
	if (UEFI_RuntimeServices->Hdr.Signature != EFI_RT_SERVICES_SIGNATURE) {
		return false;
	}
	if (!UEFI_RuntimeServices->GetTime) return false;
	return true;
}

/*
 * 通过 UEFI Runtime Services GetTime() 获取当前时间。
 * 返回 true 表示成功且数值合理，false 表示不可用/失败/数值异常。
 */
__attribute__((ms_abi, target("no-sse")))
static _Bool rtc_read_efi(rtc_time_t *tm){
	EFI_TIME et;
	EFI_STATUS s;
	if (!rtc_efi_available) return false;
	memset(&et, 0, sizeof(et));
	uintptr_t saved_cr3 = get_cr3();
	s = UEFI_RuntimeServices->GetTime(&et, NULL);
	if (get_cr3() != saved_cr3) set_cr3(KERNEL_PML4);
	if (s != 0) {
		rtc_efi_available = false;
		return false;
	}
	if (et.Year < RTC_EFI_YEAR_MIN || et.Year > RTC_EFI_YEAR_MAX) {
		rtc_efi_available = false;
		return false;
	}
	if (et.Month < 1 || et.Month > 12 || et.Day < 1 || et.Day > 31) {
		rtc_efi_available = false;
		return false;
	}
	tm->year   = et.Year;
	tm->month  = et.Month;
	tm->day    = et.Day;
	tm->hour   = et.Hour;
	tm->minute = et.Minute;
	tm->second = et.Second;
	tm->wday   = compute_wday(et.Year, et.Month, et.Day);
	tm->dst    = et.Daylight;
	return true;
}

void rtc_init(void){
	rtc_time_t tm;
	uint64_t raw_epoch;
	/* 签名校验：排除 ExitBootServices 后被回收的无效 RuntimeServices 指针 */
	rtc_efi_available = rtc_validate_efi_rt();
	/* 始终从 CMOS 读取一次作为回退基准 */
	cmos_read_time(&tm);
	raw_epoch = date_to_epoch(tm.year, tm.month, tm.day, tm.hour, tm.minute, tm.second);
#if RTC_IS_UTC
	boot_epoch = raw_epoch;
#else
	boot_epoch = raw_epoch - TIMEZONE_OFFSET_HOURS * 3600;
#endif
}

uint64_t rtc_get_epoch(void){
	if (rtc_efi_available) {
		rtc_time_t tm;
		if (rtc_read_efi(&tm))
			return date_to_epoch(tm.year, tm.month, tm.day, tm.hour, tm.minute, tm.second);
	}
	return boot_epoch + SYSTEM_TimerTicks / OS_TICK_HZ;
}

void rtc_epoch_to_utc(uint64_t epoch, rtc_time_t *tm){
	epoch_to_date(epoch, tm);
}

void rtc_get_utc(rtc_time_t *tm){
	if (rtc_read_efi(tm)) return;
	epoch_to_date(rtc_get_epoch(), tm);
}

void rtc_get_local(rtc_time_t *tm){
	if (rtc_read_efi(tm)) return;
	uint64_t local_epoch;
	local_epoch = rtc_get_epoch() + TIMEZONE_OFFSET_HOURS * 3600;
	epoch_to_date(local_epoch, tm);
	tm->dst = is_dst(tm->year, tm->month, tm->day, tm->hour, tm->wday);
}
