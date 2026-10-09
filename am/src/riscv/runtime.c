/* SPDX-License-Identifier: MPL-2.0 */
#include <am.h>
#include "../protosoc/platform.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

extern char _heap_start[], _heap_end[];

void* _sbrk(ptrdiff_t increment) {
	static uintptr_t current;
	if (!current) current = (uintptr_t) _heap_start;
	uintptr_t begin = current;
	if ((increment >= 0 && (uintptr_t) increment <= (uintptr_t) _heap_end - current) ||
	    (increment < 0 && (uintptr_t) (-(increment + 1)) + 1 <= current - (uintptr_t) _heap_start)) {
		current += increment;
		return (void*) begin;
	}
	errno = ENOMEM;
	return (void*) -1;
}

ssize_t _write(int fd, const void* data, size_t size) {
	if (fd != 1 && fd != 2) { errno = EBADF; return -1; }
	const char* bytes = data;
	for (size_t i = 0; i < size; ++i) am_platform_putch(bytes[i]);
	return size;
}

ssize_t _read(int fd, void* data, size_t size) {
	if (fd != 0) { errno = EBADF; return -1; }
	volatile uint8_t* uart = (volatile uint8_t*) AM_SOC_UART;
	size_t count = 0;
	while (count < size && (uart[5] & 1)) ((uint8_t*) data)[count++] = uart[0];
	if (!count && size) { errno = EAGAIN; return -1; }
	return count;
}

int _fstat(int fd, struct stat* status) {
	if (fd < 0 || fd > 2) { errno = EBADF; return -1; }
	*status = (struct stat) { .st_mode = S_IFCHR };
	return 0;
}
int _isatty(int fd) { return fd >= 0 && fd <= 2; }
int _close(int fd) { (void) fd; errno = EBADF; return -1; }
off_t _lseek(int fd, off_t offset, int whence) {
	(void) fd; (void) offset; (void) whence; errno = ESPIPE; return -1;
}
int _getpid(void) { return 1; }
int _kill(int pid, int signal) { (void) pid; (void) signal; errno = ESRCH; return -1; }
int _gettimeofday(struct timeval* value, void* timezone) {
	(void) timezone;
	uint64_t us = am_uptime_us();
	value->tv_sec = 946684800 + us / 1000000;
	value->tv_usec = us % 1000000;
	return 0;
}

#ifdef AM_SPIKE
/* Spike HTIF is used only for completion, never for files or proxy syscalls. */
__attribute__((section(".htif"), aligned(64))) volatile uint64_t tohost;
__attribute__((section(".htif"), aligned(64))) volatile uint64_t fromhost;
#endif

void am_platform_exit(int code) {
	/* exit() may already have closed stdout in Newlib's stdio cleanup. */
	char report[40];
	int length = snprintf(report, sizeof(report), "AM exit: %d\n", code);
	for (int i = 0; i < length; ++i) am_platform_putch(report[i]);
#ifdef AM_SPIKE
	tohost = ((uint64_t) (unsigned) code << 1) | 1;
#elif defined(AM_FPGA)
	while (!(*(volatile uint8_t*) (AM_SOC_UART + 5) & 0x40)) {}
	__asm__ volatile("fence iorw, iorw; fence.i" ::: "memory");
	((void (*)(void)) (uintptr_t) 0x20000004u)();
#else
	/* SIM_HALT reports a0 to the Verilator host. */
	register int status __asm__("a0") = code;
	__asm__ volatile("ebreak" : : "r"(status) : "memory");
#endif
	for (;;) __asm__ volatile("wfi");
}
__attribute__((noreturn)) void _exit(int code) { am_platform_exit(code); }

__attribute__((noreturn)) void am_trap_fatal(uint32_t cause, uint32_t pc, uint32_t value) {
	fprintf(stderr, "AM trap: cause=%lu pc=%08lx value=%08lx\n",
	        (unsigned long) cause, (unsigned long) pc, (unsigned long) value);
	fflush(NULL);
	am_platform_exit(1);
}
