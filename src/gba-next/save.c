/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"
#include <string.h>

enum { IDLE, COMMAND, ADDRESS_WRITE, ADDRESS_READ, DATA_WRITE, STOP_WRITE, STOP_READ, DATA_READ };

enum GbnSaveType gbn_detect_save(const uint8_t* rom, uint32_t size) {
	if (!rom) return GBN_SAVE_NONE;
	static const struct { const char* text; uint8_t length; enum GbnSaveType type; } tags[] = {
		{"EEPROM_V", 8, GBN_SAVE_EEPROM}, {"SRAM_V", 6, GBN_SAVE_SRAM}, {"SRAM_F_V", 8, GBN_SAVE_SRAM},
		{"FLASH1M_V", 9, GBN_SAVE_FLASH128}, {"FLASH512_V", 10, GBN_SAVE_FLASH64}, {"FLASH_V", 7, GBN_SAVE_FLASH64}
	};
	for (uint32_t i = 0; i < size; ++i) if (rom[i] == 'E' || rom[i] == 'S' || rom[i] == 'F') {
		for (unsigned t = 0; t < sizeof(tags) / sizeof(*tags); ++t)
			if (size - i >= tags[t].length && !memcmp(rom + i, tags[t].text, tags[t].length)) return tags[t].type;
	}
	return GBN_SAVE_NONE;
}

bool gbn_attach_save(struct Gbn* m, struct GbnSave* s, uint8_t* data, uint32_t capacity, enum GbnSaveType type) {
	uint32_t size;
	switch (type) {
	case GBN_SAVE_NONE: m->save = NULL; gbn_cancel(m, GBN_EVENT_SAVE); return true;
	case GBN_SAVE_EEPROM: size = 512; if (capacity < 8192) return false; break;
	case GBN_SAVE_SRAM: size = 0x8000; break;
	case GBN_SAVE_FLASH64: size = 0x10000; break;
	case GBN_SAVE_FLASH128: size = 0x20000; break;
	default: return false;
	}
	if (!s || !data || capacity < size) return false;
	memset(s, 0, sizeof(*s));
	s->data = data; s->capacity = capacity; s->size = size; s->type = type; s->address_bits = 6;
	m->save = s;
	gbn_cancel(m, GBN_EVENT_SAVE);
	return true;
}

bool gbn_save_mapped(const struct Gbn* m, uint32_t address, unsigned width) {
	if (!m->save) return false;
	if (m->save->type == GBN_SAVE_EEPROM) return width == 2 && address >> 24 == 13 &&
		(m->rom_size <= 0x1000000 || (address & 0xffffff) >= 0xffff00);
	return (address >> 24) == 14 || (address >> 24) == 15;
}

unsigned gbn_save_wait(const struct Gbn* m, uint32_t address) {
	static const uint8_t waits[] = {4, 3, 2, 8};
	return address >> 24 == 13 ? m->rom_nonseq[2] : waits[m->waitcnt & 3];
}

static void settle(struct Gbn* m, uint32_t cycles) {
	m->save->busy = true;
	m->save->busy_until = m->now + cycles;
	gbn_schedule(m, GBN_EVENT_SAVE, cycles, 0x70);
}

uint32_t gbn_save_read(const struct Gbn* m, uint32_t address, unsigned width) {
	struct GbnSave* s = m->save;
	bool busy = s->busy && (int32_t) (m->now - s->busy_until) < 0;
	if (s->type == GBN_SAVE_EEPROM) {
		if (s->phase != DATA_READ) return !busy;
		unsigned bit = s->bits++;
		uint32_t value = bit < 4 ? 0 : s->data[s->address + (bit - 4) / 8] >> (7 - ((bit - 4) & 7)) & 1;
		if (s->bits == 68) { s->phase = IDLE; s->bits = 0; }
		return value;
	}
	uint32_t offset = address & 0xffff;
	uint8_t byte;
	if (s->type == GBN_SAVE_SRAM) byte = s->data[offset & (s->size - 1)];
	else {
		byte = s->data[(uint32_t) s->bank * 0x10000 + offset];
		if (s->flash_command == 0x90 && offset < 2) byte = (uint8_t) ((s->type == GBN_SAVE_FLASH128 ? 0x1362u : 0x1b32u) >> (offset * 8));
		else if (busy && (s->settling == 0xffff || offset >> 12 == s->settling)) byte = (byte ^ 0x80) & 0x80;
	}
	return width == 4 ? (uint32_t) byte * 0x01010101u : width == 2 ? byte * 0x101u : byte;
}

void gbn_save_dma(struct Gbn* m, uint32_t dest, unsigned width, uint32_t count) {
	if (!gbn_save_mapped(m, dest, width) || m->save->type != GBN_SAVE_EEPROM || m->save->phase != IDLE) return;
	if (count == 17 || count == 81) { m->save->address_bits = 14; m->save->size = 8192; }
	else if (count == 9 || count == 73) { m->save->address_bits = 6; m->save->size = 512; }
}

static void eeprom_write(struct Gbn* m, unsigned bit) {
	struct GbnSave* s = m->save;
	if (s->busy && (int32_t) (m->now - s->busy_until) < 0) return;
	switch (s->phase) {
	case IDLE: if (bit) s->phase = COMMAND; break;
	case COMMAND: s->phase = bit ? ADDRESS_READ : ADDRESS_WRITE; s->address = 0; s->bits = 0; break;
	case ADDRESS_READ: case ADDRESS_WRITE:
		s->address = (s->address << 1) | bit;
		if (++s->bits == s->address_bits) {
			s->address = (s->address & (s->size / 8 - 1)) * 8;
			s->phase = s->phase == ADDRESS_READ ? STOP_READ : DATA_WRITE;
			s->bits = 0;
			memset(s->pending, 0, sizeof(s->pending));
		}
		break;
	case DATA_WRITE:
		s->pending[s->bits / 8] |= (uint8_t) (bit << (7 - (s->bits & 7)));
		if (++s->bits == 64) s->phase = STOP_WRITE;
		break;
	case STOP_WRITE:
		if (!bit) { memcpy(s->data + s->address, s->pending, 8); s->dirty = true; settle(m, 115000); }
		s->phase = IDLE; s->bits = 0;
		break;
	case STOP_READ:
		s->phase = bit ? IDLE : DATA_READ; s->bits = 0;
		break;
	default: s->phase = bit ? COMMAND : IDLE; s->bits = 0; break;
	}
}

static void flash_write(struct Gbn* m, uint16_t address, uint8_t value) {
	struct GbnSave* s = m->save;
	if (s->flash_command == 0xa0) {
		s->data[(uint32_t) s->bank * 0x10000 + address] &= value;
		s->dirty = true; s->flash_command = 0; s->settling = address >> 12; settle(m, 650);
		return;
	}
	if (value == 0xf0) { s->flash_command = s->flash_unlock = 0; return; }
	if (s->flash_command == 0xb0) {
		if (!address && value < 2 && (value == 0 || s->capacity >= 0x20000)) {
			s->bank = value;
			if (value) { s->type = GBN_SAVE_FLASH128; s->size = 0x20000; }
		}
		s->flash_command = 0;
		return;
	}
	if (!s->flash_unlock) { if (address == 0x5555 && value == 0xaa) s->flash_unlock = 1; return; }
	if (s->flash_unlock == 1) { s->flash_unlock = address == 0x2aaa && value == 0x55 ? 2 : 0; return; }
	s->flash_unlock = 0;
	if (s->flash_command == 0x80) {
		if (address == 0x5555 && value == 0x10) {
			memset(s->data, 255, s->size); s->dirty = true; s->settling = 0xffff; settle(m, 30000);
		}
		else if (value == 0x30) {
			memset(s->data + (uint32_t) s->bank * 0x10000 + (address & 0xf000), 255, 0x1000);
			s->dirty = true; s->settling = address >> 12; settle(m, 30000);
		}
		s->flash_command = 0;
	} else if (address == 0x5555 && (value == 0xa0 || value == 0x80 || value == 0x90 || value == 0xb0)) s->flash_command = value;
}

void gbn_save_write(struct Gbn* m, uint32_t address, unsigned width, uint32_t value) {
	struct GbnSave* s = m->save;
	if (s->type == GBN_SAVE_EEPROM) { eeprom_write(m, value & 1); return; }
	uint8_t byte = (uint8_t) (value >> ((address & (width - 1)) * 8));
	if (s->type == GBN_SAVE_SRAM) { s->data[address & (s->size - 1)] = byte; s->dirty = true; }
	else flash_write(m, (uint16_t) address, byte);
}
