// SPDX-License-Identifier: GPL-2.0-only
/**
 * lib/crc32.c
 *
 * Copyright (C) 2026 Liu Chunyi
 *
 * CRC32(IEEE)与CRC32C(Castagnoli)查表实现
 * 由DeepSeek V4.1 Flash生成
 */
#include <crc.h>

#define CRC32_POLY  0xEDB88320u//IEEE 802.3, 反射
#define CRC32C_POLY 0x82F63B78u//Castagnoli, 反射

static uint32_t g_crc32_tab[256];
static uint32_t g_crc32c_tab[256];
static int      g_crc_ready;

//生成反射式查表
static void crc_build_table(uint32_t *tab, uint32_t poly){
    for(uint32_t i = 0;i < 256;i ++){
        uint32_t c = i;
        for(int k = 0;k < 8;k ++)c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
        tab[i] = c;
    }
}

void CrcInit(void){
    if(g_crc_ready)return;
    crc_build_table(g_crc32_tab,  CRC32_POLY);
    crc_build_table(g_crc32c_tab, CRC32C_POLY);
    g_crc_ready = 1;
}

uint32_t Crc32(uint32_t crc, const void *buf, uint32_t len){
    if(!g_crc_ready)CrcInit();
    const uint8_t *p = (const uint8_t*)buf;
    crc = ~crc;//进入时取反
    while(len --)crc = g_crc32_tab[(crc ^ *p ++) & 0xFFu] ^ (crc >> 8);
    return ~crc;//返回前取反
}

uint32_t Crc32c(uint32_t crc, const void *buf, uint32_t len){
    if(!g_crc_ready)CrcInit();
    const uint8_t *p = (const uint8_t*)buf;
    crc = ~crc;
    while(len --)crc = g_crc32c_tab[(crc ^ *p ++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
