/* SPDX-License-Identifier: MPL-2.0 */
/* Private diagnosis only. Spike mem_t pages are separate host allocations. */
#ifndef GBN_DIAG_GUEST_COPY_H
#define GBN_DIAG_GUEST_COPY_H
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
template<class Memory>
static void gbn_copy_guest_pages(Memory* sim, void* output, uint32_t address, size_t bytes) {
    if (bytes > (UINT64_C(1) << 32) - address)
        throw std::runtime_error("Guest snapshot wraps RV32 address space");
    auto* dst = static_cast<unsigned char*>(output);
    while (bytes) {
        size_t chunk = std::min(bytes, size_t(4096 - (address & 4095)));
        const char* src = sim->addr_to_mem(address);
        if (!src) throw std::runtime_error("Guest snapshot outside RAM");
        memcpy(dst, src, chunk);
        address += uint32_t(chunk); dst += chunk; bytes -= chunk;
    }
}
#endif
