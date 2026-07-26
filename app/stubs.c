// Minimal newlib syscall stubs (pattern from apps/doom/stubs.c).
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define HEAP_SIZE (16 * 1024)
static uint8_t g_heap[HEAP_SIZE] __attribute__((aligned(8)));
static uint8_t *g_heap_ptr = g_heap;

void *_sbrk(ptrdiff_t incr) {
    uint8_t *prev = g_heap_ptr;
    if (g_heap_ptr + incr > g_heap + HEAP_SIZE) { errno = ENOMEM; return (void *)-1; }
    g_heap_ptr += incr;
    return prev;
}

int _write(int file, char *ptr, int len) { (void)file; (void)ptr; return len; }
int _read(int file, char *ptr, int len)   { (void)file; (void)ptr; return 0; }
int _close(int file)                      { (void)file; return 0; }
int _lseek(int file, int ptr, int dir)    { (void)file; (void)dir; return ptr; }
int _fstat(int file, struct stat *st)     { (void)file; st->st_mode = S_IFCHR; return 0; }
int _isatty(int file)                     { (void)file; return 1; }
int _unlink(const char *name)             { (void)name; return -1; }
int _getpid(void)                         { return 1; }
int _kill(int pid, int sig)               { (void)pid; (void)sig; return -1; }
int _link(const char *o, const char *n)   { (void)o; (void)n; return -1; }
void _exit(int status)                    { (void)status; for (;;) {} }
