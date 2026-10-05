/* SPDX-License-Identifier: MPL-2.0 */
/* Small libc surface required by the mGBA core on standard ysyx AM. */
#include <am.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

extern Area heap;

/* stdio streams do not exist on the stock ysyx AM platform.  These lightweight
 * functions keep source files that include <stdio.h> linkable. */
FILE* stdout;
FILE* stderr;

int vsnprintf(char* buffer, size_t size, const char* format, va_list ap);
void putch(char ch);

static int print_formatted(const char* format, va_list ap) {
	char buffer[512];
	int length = vsnprintf(buffer, sizeof(buffer), format, ap);
	if (length < 0) return length;
	size_t written = (size_t) length < sizeof(buffer) ? (size_t) length : sizeof(buffer) - 1;
	for (size_t i = 0; i < written; ++i) putch(buffer[i]);
	return length;
}

int vprintf(const char* format, va_list ap) {
	return print_formatted(format, ap);
}

int vfprintf(FILE* stream, const char* format, va_list ap) {
	(void) stream;
	return print_formatted(format, ap);
}

int fprintf(FILE* stream, const char* format, ...) {
	(void) stream;
	va_list ap;
	va_start(ap, format);
	int result = print_formatted(format, ap);
	va_end(ap);
	return result;
}

int fflush(FILE* stream) {
	(void) stream;
	return 0;
}

int strcasecmp(const char* left, const char* right) {
	while (*left && *right) {
		int a = *left++, b = *right++;
		if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
		if (a != b) return a - b;
	}
	return (unsigned char) *left - (unsigned char) *right;
}

int strncasecmp(const char* left, const char* right, size_t limit) {
	while (limit && *left && *right) {
		int a = *left++, b = *right++;
		if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
		if (a != b) return a - b;
		--limit;
	}
	if (!limit) return 0;
	return (unsigned char) *left - (unsigned char) *right;
}

/* The AM image has no errno object or host ctype table. */
int* __errno_location(void) {
	static int error;
	return &error;
}

const unsigned short int** __ctype_b_loc(void) {
	static unsigned short table[384];
	static const unsigned short* view;
	if (!view) {
		for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i) {
			int character = (int) i - 128;
			if (character == ' ' || (character >= '\t' && character <= '\r')) table[i] = 0x2000;
		}
		view = table + 128;
	}
	return &view;
}

div_t div(int numerator, int denominator) {
	div_t result;
	result.quot = numerator / denominator;
	result.rem = numerator % denominator;
	return result;
}

uint32_t __bswapsi2(uint32_t value) {
	return (value >> 24) | ((value >> 8) & 0x0000ff00u) |
	       ((value << 8) & 0x00ff0000u) | (value << 24);
}

struct allocation {
	size_t size;
};

static size_t align_size(size_t size) {
	return (size + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1);
}

void* malloc(size_t size) {
	size_t total = align_size(size) + sizeof(struct allocation);
	static uintptr_t current;
	if (!current) current = (uintptr_t) heap.start;
	if (!heap.end || total > (uintptr_t) heap.end - current) return NULL;
	struct allocation* allocation = (struct allocation*) current;
	allocation->size = size;
	current += total;
	return allocation + 1;
}

void* calloc(size_t count, size_t size) {
	if (size && count > SIZE_MAX / size) return NULL;
	size_t total = count * size;
	void* memory = malloc(total);
	if (memory) {
		unsigned char* bytes = memory;
		for (size_t i = 0; i < total; ++i) bytes[i] = 0;
	}
	return memory;
}

void free(void* memory) {
	(void) memory;
}

void* realloc(void* memory, size_t size) {
	if (!memory) return malloc(size);
	if (!size) return NULL;
	struct allocation* old = (struct allocation*) memory - 1;
	void* replacement = malloc(size);
	if (!replacement) return NULL;
	size_t copy = old->size < size ? old->size : size;
	unsigned char* dst = replacement;
	const unsigned char* src = memory;
	for (size_t i = 0; i < copy; ++i) dst[i] = src[i];
	return replacement;
}

char* strdup(const char* source) {
	if (!source) return NULL;
	size_t size = 0;
	while (source[size]) ++size;
	char* result = malloc(size + 1);
	if (!result) return NULL;
	for (size_t i = 0; i <= size; ++i) result[i] = source[i];
	return result;
}

char* strndup(const char* source, size_t limit) {
	if (!source) return NULL;
	size_t size = 0;
	while (size < limit && source[size]) ++size;
	char* result = malloc(size + 1);
	if (!result) return NULL;
	for (size_t i = 0; i < size; ++i) result[i] = source[i];
	result[size] = 0;
	return result;
}

size_t strlcpy(char* dst, const char* src, size_t dstsize) {
	size_t length = 0;
	while (src[length]) ++length;
	if (dstsize) {
		size_t copy = length < dstsize - 1 ? length : dstsize - 1;
		for (size_t i = 0; i < copy; ++i) dst[i] = src[i];
		dst[copy] = 0;
	}
	return length;
}

char* strchr(const char* string, int character) {
	for (; *string; ++string) if ((unsigned char) *string == (unsigned char) character) return (char*) string;
	return character == 0 ? (char*) string : NULL;
}

char* strrchr(const char* string, int character) {
	const char* result = character == 0 ? string + strlen(string) : NULL;
	for (; *string; ++string) if ((unsigned char) *string == (unsigned char) character) result = string;
	return (char*) result;
}

char* strncat(char* dst, const char* src, size_t limit) {
	char* out = dst + strlen(dst);
	size_t i = 0;
	while (i < limit && src[i]) { out[i] = src[i]; ++i; }
	out[i] = 0;
	return dst;
}

static int digit_value(int c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'z') return c - 'a' + 10;
	if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
	return -1;
}

static unsigned long parse_unsigned(const char* string, char** end, int base) {
	while (*string == ' ' || *string == '\t' || *string == '\n' || *string == '\r') ++string;
	if (base == 0) {
		base = 10;
		if (string[0] == '0') {
			base = 8;
			if (string[1] == 'x' || string[1] == 'X') { base = 16; string += 2; }
		}
	} else if (base == 16 && string[0] == '0' && (string[1] == 'x' || string[1] == 'X')) {
		string += 2;
	}
	const char* start = string;
	unsigned long value = 0;
	for (;;) {
		int digit = digit_value(*string);
		if (digit < 0 || digit >= base) break;
		value = value * (unsigned) base + (unsigned) digit;
		++string;
	}
	if (end) *end = (char*) (string == start ? start : string);
	return value;
}

unsigned long strtoul(const char* string, char** end, int base) {
	while (*string == ' ' || *string == '\t' || *string == '\n' || *string == '\r') ++string;
	if (*string == '+') ++string;
	return parse_unsigned(string, end, base);
}

long strtol(const char* string, char** end, int base) {
	while (*string == ' ' || *string == '\t' || *string == '\n' || *string == '\r') ++string;
	bool negative = false;
	if (*string == '-' || *string == '+') { negative = *string == '-'; ++string; }
	unsigned long value = parse_unsigned(string, end, base);
	return negative ? -(long) value : (long) value;
}

int isspace(int character) {
	return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
	       character == '\f' || character == '\v';
}

int abs(int value) { return value < 0 ? -value : value; }

int atoi(const char* string) {
	return (int) strtol(string, NULL, 10);
}

static unsigned random_state = 1;

void srand(unsigned seed) { random_state = seed ? seed : 1; }

int rand(void) {
	random_state = random_state * 1103515245u + 12345u;
	return (int) ((random_state >> 1) & 0x7fffffff);
}

static void swap_bytes(unsigned char* a, unsigned char* b, size_t size) {
	while (size--) { unsigned char value = *a; *a++ = *b; *b++ = value; }
}

static void sort_range(unsigned char* base, size_t count, size_t size,
	                     int (*compare)(const void*, const void*)) {
	if (count < 2) return;
	size_t middle = count / 2;
	sort_range(base, middle, size, compare);
	sort_range(base + middle * size, count - middle, size, compare);
	for (size_t i = middle; i < count; ++i) {
		size_t j = i;
		while (j > 0 && compare(base + j * size, base + (j - 1) * size) < 0) {
			swap_bytes(base + j * size, base + (j - 1) * size, size);
			--j;
		}
	}
}

void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*)) {
	if (base && size && compare) sort_range(base, count, size, compare);
}

void abort(void) { halt(1); }

time_t time(time_t* result) {
	AM_TIMER_UPTIME_T uptime;
	ioe_read(AM_TIMER_UPTIME, &uptime);
	time_t value = (time_t) (946684800ull + uptime.us / 1000000ull);
	if (result) *result = value;
	return value;
}

int gettimeofday(struct timeval* value, void* timezone) {
	(void) timezone;
	if (!value) return -1;
	AM_TIMER_UPTIME_T uptime;
	ioe_read(AM_TIMER_UPTIME, &uptime);
	value->tv_sec = (long) (946684800ull + uptime.us / 1000000ull);
	value->tv_usec = (long) (uptime.us % 1000000ull);
	return 0;
}

int timespec_get(struct timespec* value, int base) {
	if (!value || base != TIME_UTC) return 0;
	AM_TIMER_UPTIME_T uptime;
	ioe_read(AM_TIMER_UPTIME, &uptime);
	value->tv_sec = (time_t) (946684800ull + uptime.us / 1000000ull);
	value->tv_nsec = (long) ((uptime.us % 1000000ull) * 1000ull);
	return base;
}

static bool leap_year(int year) {
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

struct tm* localtime_r(const time_t* value, struct tm* result) {
	if (!value || !result) return NULL;
	int64_t seconds = *value;
	int64_t days = seconds / 86400;
	int64_t remainder = seconds % 86400;
	if (remainder < 0) { remainder += 86400; --days; }
	int year = 1970;
	while (days >= (leap_year(year) ? 366 : 365)) {
		days -= leap_year(year) ? 366 : 365;
		++year;
	}
	while (days < 0) {
		--year;
		days += leap_year(year) ? 366 : 365;
	}
	static const unsigned char month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int month = 0;
	while (month < 11) {
		int length = month_days[month] + (month == 1 && leap_year(year));
		if (days < length) break;
		days -= length;
		++month;
	}
	result->tm_sec = remainder % 60;
	result->tm_min = remainder / 60 % 60;
	result->tm_hour = remainder / 3600;
	result->tm_mday = (int) days + 1;
	result->tm_mon = month;
	result->tm_year = year - 1900;
	result->tm_yday = 0;
	for (int i = 0; i < month; ++i) result->tm_yday += month_days[i] + (i == 1 && leap_year(year));
	result->tm_yday += result->tm_mday - 1;
	result->tm_wday = (int) ((days + 4) % 7);
	if (result->tm_wday < 0) result->tm_wday += 7;
	result->tm_isdst = 0;
	return result;
}

time_t mktime(struct tm* value) {
	if (!value) return (time_t) -1;
	int year = value->tm_year + 1900;
	int64_t days = 0;
	for (int y = 1970; y < year; ++y) days += leap_year(y) ? 366 : 365;
	for (int y = year; y < 1970; ++y) days -= leap_year(y - 1) ? 366 : 365;
	static const unsigned char month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	for (int month = 0; month < value->tm_mon; ++month) days += month_days[month] + (month == 1 && leap_year(year));
	days += value->tm_mday - 1;
	return (time_t) (days * 86400 + value->tm_hour * 3600 + value->tm_min * 60 + value->tm_sec);
}
