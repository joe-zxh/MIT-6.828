// Simple implementation of cprintf console output for the kernel,
// based on printfmt() and the kernel console's cputchar().

#include <inc/types.h>
#include <inc/stdio.h>
#include <inc/stdarg.h>


static void
putch(int ch, int *cnt) //就是输出一个字符串而已
{
	cputchar(ch);
	*cnt++;
}

int
vcprintf(const char *fmt, va_list ap)
{
	int cnt = 0;

	vprintfmt((void*)putch, &cnt, fmt, ap);
	return cnt;
}

int
cprintf(const char *fmt, ...)
{ // 这个是比较靠近高级 的接口
	va_list ap;
	int cnt;

	va_start(ap, fmt); //相当于让指针 ap指向fmt的下一个参数，也就是参数列表
	cnt = vcprintf(fmt, ap); // 再把参数列表ap作为参数传递给vcprintf
	va_end(ap);

	return cnt;
}

