/* SPDX-License-Identifier: MPL-2.0
 * SRAM recovery program. Receive a CRC-checked application into SDRAM;
 * persistent NOR contents are never accessed. No writable global storage.
 */
#include "boot.h"

void boot_recovery(void) {
	boot_uart_init();
	boot_puts("RAM load failed\n");
	boot_jump(BOOTROM_BASE + 4);
}

static int receive(void* buffer, unsigned bytes) {
	uint8_t* p = buffer;
	for (unsigned i = 0; i < bytes; ++i) {
		int c = boot_getc();
		if (c < 0) return 0;
		p[i] = c;
	}
	return 1;
}

static void decimal(uint32_t value) {
	char digits[10];
	unsigned count = 0;
	do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
	while (count) boot_putc(digits[--count]);
}

void boot_main(void) {
	boot_uart_init();
	uint32_t hz = soc_read32(SOC_CLK_HZ);
	boot_puts("RAM loader: clock_hz="); decimal(hz); boot_puts("\nRAM READY\n");
	boot_header header;
	uint32_t baud;
	if (!receive(&header, sizeof(header)) || !receive(&baud, sizeof(baud)) ||
	    !boot_header_ok(&header, APP_MAGIC) || header.load != SDRAM_BASE ||
	    header.entry & 3 || header.bytes > soc_read32(SOC_SDRAM_BYTES) ||
	    baud < 115200 || baud > 2000000) {
		boot_recovery(); return;
	}
	unsigned divisor = (hz + 8 * baud) / (16 * baud);
	unsigned actual = divisor ? hz / (16 * divisor) : 0;
	unsigned error = actual > baud ? actual - baud : baud - actual;
	if (!divisor || divisor > 65535 || error > baud / 50) { boot_recovery(); return; }
	boot_putc('K'); boot_uart_drain();
	volatile uint8_t* uart = (volatile uint8_t*) UART_BASE;
	uart[3] = 0x80; uart[0] = divisor; uart[1] = divisor >> 8; uart[3] = 3;
	uint8_t* dst = (uint8_t*) (uintptr_t) header.load;
	for (uint32_t offset = 0; offset < header.bytes; ) {
		unsigned count = header.bytes - offset;
		if (count > 256) count = 256;
		if (!receive(dst + offset, count)) { boot_recovery(); return; }
		offset += count;
		boot_putc('K');
	}
	/* Publish CPU stores before fetching the freshly received instructions.
	 * Flushing also makes the checksum cover the external SDRAM contents.
	 */
	unsigned line = soc_read32(SOC_DCACHE_LINE);
	if (line) {
		if (line != 32) { boot_recovery(); return; }
		for (uint32_t offset = 0; offset < header.bytes; offset += line)
			__asm__ volatile("cbo.flush (%0)" :: "r"(dst + offset) : "memory");
	}
	__asm__ volatile("fence iorw, iorw" ::: "memory");
	if (boot_crc(dst, header.bytes) != header.crc) { boot_recovery(); return; }
	boot_putc('G'); boot_uart_drain();
	/* Give the host time to restore 115200 before the application banner. */
	uint32_t start = soc_read32(CLINT_MTIME);
	while ((uint32_t) (soc_read32(CLINT_MTIME) - start) < hz / 10) {}
	boot_uart_init();
	boot_puts("App SDRAM (UART load, CRC verified)\n");
	boot_jump(header.load + header.entry);
}
