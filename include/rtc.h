/*
 * 由Hermes Agent + DeepSeek-V4-Pro参考Linux 7.1.3生成
 *
 * include/rtc.h — CMOS RTC 实时时钟接口与时区定义
 *
 * 参考：Linux 7.1.3 include/linux/mc146818rtc.h (寄存器定义)
 *              drivers/rtc/rtc-mc146818-lib.c (mc146818_avoid_UIP)
 *              drivers/rtc/lib.c (rtc_tm_to_time64 / rtc_time64_to_tm)
 *              UEFI Specification v2.10 §8.3 (GetTime returns UTC)
 */

#ifndef _INT_RTC_H_
#define _INT_RTC_H_

#include <types.h>

#define TIMEZONE_OFFSET_HOURS 8 //时区，UTC+8 = CST 中国标准时间
#define TIMEZONE_DST_RULE 0//夏时令规则：0=无, 1=美国/加拿大, 2=欧盟

#define RTC_IS_UTC 1//1=UTC，0=本地

//时间结构
typedef struct {
	uint16_t year;
	uint8_t  month;
	uint8_t  day;
	uint8_t  wday;
	uint8_t  hour;
	uint8_t  minute;
	uint8_t  second;
	uint8_t  dst;
} rtc_time_t;

extern _Bool rtc_efi_available;    //UEFI 运行时服务 GetTime 是否可用

static const char* weekdays[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};

void rtc_init(void);//启动时从CMOS RTC读取一次 UTC基准时间戳
uint64_t rtc_get_epoch(void);//返回当前UTC Unix时间戳
uint64_t rtc_tm_to_epoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);//年月日时分秒转UTC时间戳
void rtc_epoch_to_utc(uint64_t epoch, rtc_time_t *tm);//解析Unix时间戳
void rtc_get_utc(rtc_time_t *tm);//获取当前UTC分解时间
void rtc_get_local(rtc_time_t *tm);//获取当前本地分解时间

#endif
