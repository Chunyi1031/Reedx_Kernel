/*
 * lib/vsprintf.c — 格式化字符串输出
 *
 * 参考：Linux 7.1.3 lib/vsprintf.c
 *
 * 支持的格式化说明符：
 *   %s    字符串
 *   %c    字符
 *   %d/%i 有符号十进制整数
 *   %u    无符号十进制整数
 *   %x/%X 无符号十六进制（小写/大写）
 *   %p    指针（0x 前缀十六进制）
 *   %l    长整数前缀（%ld, %lu, %lx, %lX）
 *   %%    百分号本身
 */

#include <kstring.h>

/* 判断字符是否为十进制数字 */
static int is_digit(int c)
{
	return (c >= '0' && c <= '9');
}

/* 从字符串中解析十进制整数并推进指针 */
static int skip_atoi(const char **s)
{
	int i = 0;
	while (is_digit(**s))
		i = i * 10 + *((*s)++) - '0';
	return i;
}

/*
 * number — 将无符号整数转为字符串写入缓冲区
 *
 * @str:       输出缓冲区指针（函数会推进它）
 * @num:       要转换的数值
 * @base:      进制（10 或 16）
 * @width:     最小字段宽度（0=不填充）
 * @padc:      填充字符
 * @hex_upper: 十六进制字母是否大写
 */
static void number(char **str, uint64_t num, int base, int width,
		   char padc, int hex_upper)
{
	char tmp[32];
	const char *digits = hex_upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int i = 0, len, padlen;

	if (num == 0)
		tmp[i++] = '0';
	else {
		while (num) {
			tmp[i++] = digits[num % base];
			num /= base;
		}
	}
	len = i;
	padlen = (width > len) ? width - len : 0;
	while (padlen--)
		*(*str)++ = padc;
	while (i--)
		*(*str)++ = tmp[i];
}

/*
 * vsprintf — 格式化字符串到缓冲区
 *
 * @buf:  输出缓冲区
 * @fmt:  格式字符串
 * @args: 可变参数列表
 *
 * 返回写入的字符数（不含末尾 '\0'）。
 */
int vsprintf(char *buf, const char *fmt, __builtin_va_list args)
{
	char *str = buf;
	char *s;
	int width, base, hex_upper, long_flag;

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			*str++ = *fmt;
			continue;
		}
		/* %% */
		if (*(fmt + 1) == '%') {
			*str++ = '%';
			fmt++;
			continue;
		}
		/* 解析标志和宽度 */
		char padc = ' ';
		fmt++;
		if (*fmt == '0') {
			padc = '0';
			fmt++;
		}
		width = skip_atoi(&fmt);
		/* 长度修饰符 */
		long_flag = 0;
		if (*fmt == 'l') {
			long_flag = 1;
			fmt++;
		}

		base = 10;
		hex_upper = 0;

		switch (*fmt) {
		case 'c':
			*str++ = (char)__builtin_va_arg(args, int);
			break;
		case 's':
			s = __builtin_va_arg(args, char *);
			if (!s) s = "(null)";
			while (*s) *str++ = *s++;
			break;
		case 'd':
		case 'i': {
			int64_t num = long_flag
				? __builtin_va_arg(args, int64_t)
				: (int64_t)__builtin_va_arg(args, int32_t);
			if (num < 0) {
				*str++ = '-';
				num = -num;
			}
			number(&str, (uint64_t)num, 10, width, padc, 0);
			break;
		}
		case 'u': {
			uint64_t num = long_flag
				? __builtin_va_arg(args, uint64_t)
				: (uint64_t)__builtin_va_arg(args, uint32_t);
			number(&str, num, 10, width, padc, 0);
			break;
		}
		case 'x':
			base = 16;
			hex_upper = 0;
			goto hex_common;
		case 'X':
			base = 16;
			hex_upper = 1;
			goto hex_common;
		case 'p':
			*str++ = '0';
			*str++ = 'x';
			base = 16;
			width = 16;
			padc = '0';
			hex_upper = 0;
			long_flag = 1;
			goto hex_common;
		hex_common: {
			uint64_t num = long_flag
				? __builtin_va_arg(args, uint64_t)
				: (uint64_t)__builtin_va_arg(args, uint32_t);
			number(&str, num, base, width, padc, hex_upper);
			break;
		}
		default:
			*str++ = '%';
			if (*fmt) *str++ = *fmt;
			break;
		}
	}
	*str = '\0';
	return str - buf;
}

int sprintf(char *buf, const char *fmt, ...)
{
	__builtin_va_list args;
	int ret;
	__builtin_va_start(args, fmt);
	ret = vsprintf(buf, fmt, args);
	__builtin_va_end(args);
	return ret;
}
