/*
 * 最小 newlib 系统调用桩。
 * 本固件只用 vsnprintf + memcpy，不使用文件/进程/信号；这里提供链接所需的
 * 弱符号，避免把半主机（semihosting）拖进来。
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

extern char end;      /* 链接脚本 .heap_stack 起点 */
extern char _estack;  /* RAM 顶部 */

static char *heap_end;
static char *heap_limit;

void *_sbrk(ptrdiff_t incr)
{
    char *prev;

    if (heap_end == NULL) {
        heap_end = &end;
        /* 链接脚本在 RAM 顶部预留了 4KB heap + 8KB stack；这里必须给
         * 栈留足 8KB，否则分配会侵入栈区。 */
        heap_limit = (char *)((uintptr_t)&_estack - 0x2000u);
    }
    prev = heap_end;
    if (heap_end + incr > heap_limit) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_end += incr;
    return prev;
}

int _write(int fd, char *buf, int len)
{
    (void)fd;
    (void)buf;
    return len;
}

int _read(int fd, char *buf, int len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return 0;
}

int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; (void)st; return -1; }
int _isatty(int fd) { (void)fd; return 1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int code) { (void)code; for (;;) { } }
