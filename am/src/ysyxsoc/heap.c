/* SPDX-License-Identifier: MPL-2.0 */
/* Small single-threaded allocator for the bare-metal ysyxSoC image.
 * Newlib's full allocator assumes host-style process growth and is needlessly
 * expensive on the RV32E target. The emulator has one lifetime, so a bump
 * arena plus copying realloc is sufficient and deterministic. */
#include <stddef.h>
#include <stdint.h>
#include <reent.h>
#include <string.h>

extern char _heap_start[], _heap_end[];

static uintptr_t cursor;

struct allocation {
	size_t size;
};

static uintptr_t align_up(uintptr_t value) {
	return (value + 7u) & ~(uintptr_t) 7u;
}

void* malloc(size_t size) {
	if (!size) size = 1;
	if (!cursor) cursor = align_up((uintptr_t) _heap_start);
	uintptr_t begin = cursor;
	uintptr_t end = align_up(begin + sizeof(struct allocation) + size);
	if (end < begin || end > (uintptr_t) _heap_end) return NULL;
	cursor = end;
	struct allocation* allocation = (struct allocation*) begin;
	allocation->size = size;
	return allocation + 1;
}

void* calloc(size_t count, size_t size) {
	if (count && size > (size_t) -1 / count) return NULL;
	size_t total = count * size;
	void* result = malloc(total);
	if (result) memset(result, 0, total);
	return result;
}

void free(void* pointer) { (void) pointer; }

void* realloc(void* pointer, size_t size) {
	if (!pointer) return malloc(size);
	if (!size) return NULL;
	void* result = malloc(size);
	if (result) {
		struct allocation* old = (struct allocation*) pointer - 1;
		size_t copy = old->size < size ? old->size : size;
		memcpy(result, pointer, copy);
	}
	return result;
}

/* Newlib stdio and its convenience allocation APIs call the reentrant forms. */
void* _malloc_r(struct _reent* reent, size_t size) {
	(void) reent;
	return malloc(size);
}

void _free_r(struct _reent* reent, void* pointer) {
	(void) reent;
	free(pointer);
}

void* _realloc_r(struct _reent* reent, void* pointer, size_t size) {
	(void) reent;
	return realloc(pointer, size);
}

void* _calloc_r(struct _reent* reent, size_t count, size_t size) {
	(void) reent;
	return calloc(count, size);
}
