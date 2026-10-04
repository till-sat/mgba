/* SPDX-License-Identifier: MPL-2.0 */
/* Functional model of the polled devices used by this AM port, not SoC RTL. */
#include <riscv/abstract_device.h>
#include <riscv/sim.h>
#include <am.h>
#include "../src/protosoc/platform.h"
#include <stdexcept>

class am_protosoc_t : public abstract_device_t {
public:
	explicit am_protosoc_t(uint32_t buttons) : gpio_input(buttons) {}
	reg_t size() override { return 0xe000; }
	bool load(reg_t offset, size_t len, uint8_t* bytes) override {
		offset += 0x2000;
		if (len != 4 || (offset & 3)) return false;
		uint32_t value;
		if (offset >= 0x2000 && offset < 0x3000) {
			switch (offset - 0x2000) {
			case 0: value = gpio_output; break;
			case 4: value = gpio_input | am_input_read().buttons; break;
			case 8: value = segments; break;
			case 0x10: value = 0; break;
			case 0x14: value = gpio_enable; break;
			default: value = 0; break;
			}
		} else if (offset >= 0xf000 && offset < 0x10000) {
			switch (offset - 0xf000) {
			case 0: value = AM_SOC_SDRAM; break;
			case 0x0c: value = scratch; break;
			case 0x10: value = 0x50534f43; break;
			case 0x14: value = AM_SOC_MAP_VERSION; break;
			case 0x28: value = AM_SOC_SDRAM_SIZE; break;
			case 0x38: value = sim_t::CPU_HZ / sim_t::INSNS_PER_RTC_TICK; break;
			default: value = 0; break;
			}
		} else return false;
		for (unsigned i = 0; i < 4; ++i) bytes[i] = value >> (8 * i);
		return true;
	}
	bool store(reg_t offset, size_t len, const uint8_t* bytes) override {
		offset += 0x2000;
		if (len != 4 || (offset & 3)) return false;
		uint32_t value = 0;
		for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes[i]) << (8 * i);
		if (offset >= 0x2000 && offset < 0x3000) {
			if (offset == 0x2000) gpio_output = value & 0xffff;
			if (offset == 0x2008) segments = value;
			if (offset == 0x2014) gpio_enable = value & 0xffff;
			return true;
		}
		if (offset == 0xf00c) { scratch = value; return true; }
		return false;
	}
private:
	uint32_t gpio_input;
	uint32_t gpio_output = 0, segments = 0, gpio_enable = 0, scratch = 0;
};

static am_protosoc_t* parse(const void*, const sim_t*, reg_t* base, const std::vector<std::string>& args) {
	*base = AM_SOC_GPIO;
	uint32_t buttons = 0;
	if (args.size() > 1) throw std::runtime_error("am_protosoc accepts one optional GPIO input mask");
	if (!args.empty()) {
		size_t end;
		unsigned long value = std::stoul(args[0], &end, 0);
		if (end != args[0].size() || value > 0xffff) throw std::runtime_error("Invalid GPIO input mask");
		buttons = value;
	}
	return new am_protosoc_t(buttons);
}
static std::string dts(const sim_t*, const std::vector<std::string>&) { return ""; }
REGISTER_DEVICE(am_protosoc, parse, dts)
