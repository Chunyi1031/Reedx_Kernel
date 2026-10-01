#include <kstring.h>

#pragma GCC optimize("Os")

char *strcpy(char *dest, const char *src)
{
	char *tmp = dest;
	while ((*dest++ = *src++) != '\0');
	return tmp;
}

char *strncpy(char *dest, const char *src, size_t count)
{
	char *tmp = dest;
	while(count){
		if ((*tmp = *src) != 0)src++;
		tmp++;
		count--;
	}
	return dest;
}

char *strcat(char *dest, const char *src)
{
	char *tmp = dest;
	while(*dest)dest++;
	while((*dest++ = *src++) != '\0');
	return tmp;
}

char *strncat(char *dest, const char *src, size_t count)
{
	char *tmp = dest;

	if (count) {
		while(*dest)dest++;
		while((*dest++ = *src++) != 0) {
			if(--count == 0) {
				*dest = '\0';
				break;
			}
		}
	}
	return tmp;
}

size_t strlcat(char *dest, const char *src, size_t count)
{
	size_t dsize = strlen(dest);
	size_t len = strlen(src);
	size_t res = dsize + len;

	dest += dsize;
	count -= dsize;
	if (len >= count)
		len = count-1;
	__builtin_memcpy(dest, src, len);
	dest[len] = 0;
	return res;
}

/**
 * strcmp - Compare two strings
 * @cs: One string
 * @ct: Another string
 */
int strcmp(const char *cs, const char *ct)
{
	unsigned char c1, c2;

	while (1) {
		c1 = *cs++;
		c2 = *ct++;
		if (c1 != c2)return c1 < c2 ? -1 : 1;
		if (!c1)break;
	}
	return 0;
}

/**
 * strncmp - Compare two length-limited strings
 * @cs: One string
 * @ct: Another string
 * @count: The maximum number of bytes to compare
 */
int strncmp(const char *cs, const char *ct, size_t count)
{
	unsigned char c1, c2;
	while (count) {
		c1 = *cs++;
		c2 = *ct++;
		if (c1 != c2)return c1 < c2 ? -1 : 1;
		if (!c1)break;
		count--;
	}
	return 0;
}

/**
 * strchr - Find the first occurrence of a character in a string
 * @s: The string to be searched
 * @c: The character to search for
 *
 * Note that the %NUL-terminator is considered part of the string, and can
 * be searched for.
 */
char *strchr(const char *s, int c)
{
	for (; *s != (char)c; ++s)
		if (*s == '\0')return NULL;
	return (char *)s;
}

/**
 * strrchr - Find the last occurrence of a character in a string
 * @s: The string to be searched
 * @c: The character to search for
 */
char *strrchr(const char *s, int c)
{
	const char *last = NULL;
	do {
		if (*s == (char)c)last = s;
	} while (*s++);
	return (char *)last;
}

/**
 * strnchr - Find a character in a length limited string
 * @s: The string to be searched
 * @count: The number of characters to be searched
 * @c: The character to search for
 *
 * Note that the %NUL-terminator is considered part of the string, and can
 * be searched for.
 */
char *strnchr(const char *s, size_t count, int c)
{
	while (count--) {
		if (*s == (char)c)return (char *)s;
		if (*s++ == '\0')break;
	}
	return NULL;
}

size_t strlen(const char *s)
{
	const char *sc;
	for (sc = s; *sc != '\0'; ++sc);
	return sc - s;
}

size_t strnlen(const char *s, size_t count)
{
	const char *sc;
	for (sc = s; count-- && *sc != '\0'; ++sc);
	return sc - s;
}

/**
 * memset - Fill a region of memory with the given value
 * @s: Pointer to the start of the area.
 * @c: The byte to fill the area with
 * @count: The size of the area.
 *
 * Do not use memset() to access IO space, use memset_io() instead.
 */
void *memset(void *s, int c, size_t count)
{
	char *xs = s;
	while (count--)*xs++ = c;
	return s;
}

/**
 * memset16() - Fill a memory area with a uint16_t
 * @s: Pointer to the start of the area.
 * @v: The value to fill the area with
 * @count: The number of values to store
 *
 * Differs from memset() in that it fills with a uint16_t instead
 * of a byte.  Remember that @count is the number of uint16_ts to
 * store, not the number of bytes.
 */
void *memset16(uint16_t *s, uint16_t v, size_t count)
{
	uint16_t *xs = s;
	while (count--)*xs++ = v;
	return s;
}

/**
 * memset32() - Fill a memory area with a uint32_t
 * @s: Pointer to the start of the area.
 * @v: The value to fill the area with
 * @count: The number of values to store
 *
 * Differs from memset() in that it fills with a uint32_t instead
 * of a byte.  Remember that @count is the number of uint32_ts to
 * store, not the number of bytes.
 */
void *memset32(uint32_t *s, uint32_t v, size_t count)
{
	uint32_t *xs = s;
	while (count--)*xs++ = v;
	return s;
}

/**
 * memset64() - Fill a memory area with a uint64_t
 * @s: Pointer to the start of the area.
 * @v: The value to fill the area with
 * @count: The number of values to store
 *
 * Differs from memset() in that it fills with a uint64_t instead
 * of a byte.  Remember that @count is the number of uint64_ts to
 * store, not the number of bytes.
 */
void *memset64(uint64_t *s, uint64_t v, size_t count)
{
	uint64_t *xs = s;
	while (count--)*xs++ = v;
	return s;
}

/**
 * memcpy - Copy one area of memory to another
 * @dest: Where to copy to
 * @src: Where to copy from
 * @count: The size of the area.
 *
 * You should not use this function to access IO space, use memcpy_toio()
 * or memcpy_fromio() instead.
 */
void *memcpy(void *dest, const void *src, size_t count)
{
	char *tmp = dest;
	const char *s = src;
	while (count--)*tmp++ = *s++;
	return dest;
}

/**
 * memmove - Copy one area of memory to another
 * @dest: Where to copy to
 * @src: Where to copy from
 * @count: The size of the area.
 *
 * Unlike memcpy(), memmove() copes with overlapping areas.
 */
void *memmove(void *dest, const void *src, size_t count)
{
	char *tmp;
	const char *s;
	if (dest <= src) {
		tmp = dest;
		s = src;
		while (count--)*tmp++ = *s++;
	} else {
		tmp = dest;
		tmp += count;
		s = src;
		s += count;
		while (count--)*--tmp = *--s;
	}
	return dest;
}

int memcmp(const void *cs, const void *ct, size_t count)
{
	const unsigned char *su1, *su2;
	int res = 0;
	for (su1 = cs, su2 = ct; 0 < count; ++su1, ++su2, count--)
		if ((res = *su1 - *su2) != 0)break;
	return res;
}

/**
 * strstr - Find the first substring in a %NUL terminated string
 * @s1: The string to be searched
 * @s2: The string to search for
 */
char *strstr(const char *s1, const char *s2)
{
	size_t l1, l2;

	l2 = strlen(s2);
	if (!l2)return (char *)s1;
	l1 = strlen(s1);
	while (l1 >= l2) {
		l1--;
		if (!memcmp(s1, s2, l2))return (char *)s1;
		s1++;
	}
	return NULL;
}

/**
 * strnstr - Find the first substring in a length-limited string
 * @s1: The string to be searched
 * @s2: The string to search for
 * @len: the maximum number of characters to search
 */
char *strnstr(const char *s1, const char *s2, size_t len)
{
	size_t l2;

	l2 = strlen(s2);
	if (!l2)return (char *)s1;
	while (len >= l2) {
		len--;
		if (!memcmp(s1, s2, l2))return (char *)s1;
		s1++;
	}
	return NULL;
}
