/* SPDX-License-Identifier: MPL-2.0 */
#ifndef GBA_NEXT_INTERNAL_H
#define GBA_NEXT_INTERNAL_H
#include <gba-next/core.h>
struct GbnRv32;
/* Called only after the shared instruction-boundary checks. Zero requests
 * the independent interpreter; successful blocks restore complete state.
 * Bit 31 requests one interpreter instruction after the completed prefix. */
unsigned gbn_rv32_execute(struct GbnRv32*, uint32_t cap);
enum GbnStatus gbn_device_read(const struct Gbn*, uint32_t address, unsigned width, struct GbnAccess*);
enum GbnStatus gbn_device_write(struct Gbn*, uint32_t address, unsigned width, uint32_t value, uint32_t* cycles);
bool gbn_device_writable(const struct Gbn*, uint32_t address, unsigned width);
uint32_t gbn_open_bus(const struct Gbn*);
void gbn_update_irq(struct Gbn*, uint32_t when);
void gbn_irq_at(struct Gbn*, uint16_t mask, uint32_t when);
bool gbn_bus_writable(const struct Gbn*, uint32_t address, unsigned width);
void gbn_dma_write(struct Gbn*, unsigned offset, uint16_t value);
void gbn_dma_trigger(struct Gbn*, unsigned timing, uint32_t when);
enum GbnStatus gbn_dma_service(struct Gbn*);
void gbn_dma_fifo(struct Gbn*, unsigned fifo, uint32_t when);
uint16_t gbn_timer_read(const struct Gbn*, unsigned timer, uint32_t when);
void gbn_timer_write(struct Gbn*, unsigned timer, bool control, uint16_t value, uint16_t mask);
void gbn_timer_service(struct Gbn*, unsigned timer, uint32_t when);
bool gbn_audio_register(unsigned offset);
uint16_t gbn_audio_reg_read(const struct Gbn*, unsigned offset);
void gbn_audio_write(struct Gbn*, unsigned offset, unsigned width, uint32_t value);
void gbn_audio_service(struct Gbn*, uint32_t when);
void gbn_audio_frame(struct Gbn*, uint32_t when);
void gbn_audio_timer(struct Gbn*, unsigned timer, uint32_t when);
bool gbn_sio_register(unsigned offset);
enum GbnStatus gbn_sio_write(struct Gbn*, unsigned offset, unsigned width, uint32_t value);
enum GbnStatus gbn_sio_service(struct Gbn*, uint32_t when);
bool gbn_save_mapped(const struct Gbn*, uint32_t address, unsigned width);
uint32_t gbn_save_read(const struct Gbn*, uint32_t address, unsigned width);
void gbn_save_write(struct Gbn*, uint32_t address, unsigned width, uint32_t value);
void gbn_save_dma(struct Gbn*, uint32_t dest, unsigned width, uint32_t count);
unsigned gbn_save_wait(const struct Gbn*, uint32_t address);
void gbn_ppu_reference(struct Gbn*, unsigned offset);
enum GbnStatus gbn_ppu_line(struct Gbn*, unsigned y);
#endif
