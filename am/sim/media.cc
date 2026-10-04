/* SPDX-License-Identifier: MPL-2.0 */
#include <riscv/abstract_device.h>
#include "media-device.h"

class am_media_t : public abstract_device_t {
public:
    reg_t size() override { return AM_SIM_SIZE; }
    void tick(reg_t) override { device.tick(); }
    bool load(reg_t offset, size_t len, uint8_t* bytes) override {
        return device.load(offset, len, bytes);
    }
    bool store(reg_t offset, size_t len, const uint8_t* bytes) override {
        return device.store(offset, len, bytes);
    }
private:
    AMMediaDevice device{"mGBA (Spike)"};
};

static am_media_t* parse(const void*, const sim_t*, reg_t* base, const std::vector<std::string>& args) {
	if (!args.empty()) throw std::runtime_error("am_media accepts no arguments");
	*base = AM_SIM_BASE;
	return new am_media_t;
}
static std::string dts(const sim_t*, const std::vector<std::string>&) { return ""; }
REGISTER_DEVICE(am_media, parse, dts)
