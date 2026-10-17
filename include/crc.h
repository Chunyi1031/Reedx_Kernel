// SPDX-License-Identifier: GPL-2.0-only
/**
 * include/crc.h
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * CRC校验
 * 
 * 由DeepSeek V4.1 Flash生成
 */
#ifndef _CRC_H_
#define _CRC_H_

#include <types.h>

void CrcInit(void);//建表

uint32_t Crc32(uint32_t crc, const void *buf, uint32_t len);
uint32_t Crc32c(uint32_t crc, const void *buf, uint32_t len);

#endif
