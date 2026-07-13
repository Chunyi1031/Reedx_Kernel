#ifndef _STRING_H_
#define _STRING_H_

#include <klib.h>

int strcasecmp(const char *s1, const char *s2);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t count);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t count);
size_t strlcat(char *dest, const char *src, size_t count);
int strcmp(const char *cs, const char *ct);
int strncmp(const char *cs, const char *ct, size_t count);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strnchr(const char *s, size_t count, int c);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t count);
void *memset(void *s, int c, size_t count);
void *memset16(uint16_t *s, uint16_t v, size_t count);
void *memset32(uint32_t *s, uint32_t v, size_t count);
void *memset64(uint64_t *s, uint64_t v, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int memcmp(const void *cs, const void *ct, size_t count);
char *strstr(const char *s1, const char *s2);
char *strnstr(const char *s1, const char *s2, size_t len);

int vsprintf(char *buf, const char *fmt, __builtin_va_list args);
int sprintf(char *buf, const char *fmt, ...);

#endif