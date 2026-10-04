/* SPDX-License-Identifier: MPL-2.0 */
/* AXI channel independence, response backpressure and invalid media accesses. */
#include "Vaxi_media_bridge.h"
#include <cstdio>
#include <cstdlib>

static unsigned reads, writes, last_address, last_value;
extern "C" int am_rtl_read(unsigned address, unsigned* value) {
	++reads;
	*value = address ^ 0x12345678u;
	return 0;
}
extern "C" int am_rtl_write(unsigned address, unsigned value) {
	++writes;
	last_address = address;
	last_value = value;
	return 0;
}

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
	Vaxi_media_bridge bus;
	auto tick = [&] { bus.clk = 0; bus.eval(); bus.clk = 1; bus.eval(); bus.clk = 0; bus.eval(); };
	bus.rst = 1;
	tick();
	bus.rst = 0;
	bus.s_arsize = bus.s_awsize = 2;
	bus.s_arburst = bus.s_awburst = 1;
	bus.s_arid = 3;
	bus.s_awid = 5;
	bus.s_wstrb = 15;
	bus.s_wlast = 1;

	bus.s_araddr = 0x40000000;
	bus.s_arvalid = 1;
	bus.eval();
	CHECK(bus.s_arready && !bus.m_arvalid);
	tick();
	bus.s_arvalid = 0;
	bus.s_araddr = 0;
	for (int i = 0; i < 4; ++i) {
		tick();
		CHECK(bus.s_rvalid && bus.s_rdata == 0x52345678 && bus.s_rid == 3 && bus.s_rlast);
	}
	CHECK(reads == 1);
	bus.s_rready = 1;
	tick();
	CHECK(!bus.s_rvalid);
	bus.s_arvalid = 1;
	bus.s_araddr = 0x40000001;
	bus.s_arsize = 0;
	tick();
	bus.s_arvalid = 0;
	CHECK(bus.s_rvalid && bus.s_rresp == 2 && reads == 1);
	tick();

	bus.s_arsize = 2;
	bus.s_araddr = 0xa0001234;
	bus.s_arvalid = 1;
	bus.eval();
	CHECK(bus.m_arvalid && bus.m_araddr == 0xa0001234 && !bus.s_arready);
	bus.m_arready = 1;
	tick();
	bus.s_arvalid = 0;
	bus.m_rvalid = bus.m_rlast = 1;
	bus.m_rdata = 0x87654321;
	bus.m_rid = 3;
	bus.s_rready = 0;
	bus.eval();
	CHECK(bus.s_rvalid && bus.s_rdata == 0x87654321 && !bus.m_rready);
	tick();
	bus.s_rready = 1;
	tick();
	bus.m_rvalid = 0;

	// A downstream target can wait for WVALID before accepting AW.
	bus.s_awaddr = 0xa0000000;
	bus.s_awvalid = bus.s_wvalid = 1;
	bus.s_wdata = 0x11223344;
	bus.m_wready = 1;
	bus.eval();
	CHECK(bus.m_awvalid && bus.m_wvalid && bus.m_wdata == 0x11223344);
	bus.m_awready = bus.m_wvalid;
	tick();
	bus.s_awvalid = bus.s_wvalid = 0;
	bus.m_bvalid = 1;
	bus.m_bid = 5;
	bus.eval();
	CHECK(bus.s_bvalid && bus.s_bid == 5);
	tick();
	CHECK(bus.s_bvalid);
	bus.s_bready = 1;
	tick();
	bus.m_bvalid = 0;

	// W may also be accepted a cycle before AW.
	bus.s_awvalid = bus.s_wvalid = 1;
	bus.m_awready = 0;
	tick();
	bus.s_wvalid = 0;
	bus.eval();
	CHECK(bus.m_awvalid && !bus.m_wvalid);
	bus.m_awready = 1;
	tick();
	bus.s_awvalid = 0;
	bus.m_bvalid = 1;
	tick();
	bus.m_bvalid = 0;

	bus.s_awaddr = 0x40001000;
	bus.s_awvalid = bus.s_wvalid = 1;
	bus.s_wdata = 0xabcdef;
	tick();
	bus.s_awvalid = 0;
	bus.s_awaddr = 0;
	tick();
	bus.s_wvalid = 0;
	CHECK(bus.s_bvalid && bus.s_bresp == 0 && writes == 1);
	CHECK(last_address == 0x40001000 && last_value == 0xabcdef);
	tick();
	// A following normal W-before-AW must not reuse the previous media decode.
	bus.s_awaddr = 0xa0000000;
	bus.s_awvalid = bus.s_wvalid = 1;
	bus.m_awready = 0;
	tick();
	CHECK(writes == 1);
	bus.s_wvalid = 0;
	bus.m_awready = 1;
	tick();
	bus.s_awvalid = 0;
	bus.m_bvalid = 1;
	tick();
	bus.m_bvalid = 0;
	bus.s_awaddr = 0x40001000;
	bus.s_awvalid = bus.s_wvalid = 1;
	bus.s_wstrb = 1;
	tick();
	bus.s_awvalid = 0;
	tick();
	bus.s_wvalid = 0;
	CHECK(bus.s_bvalid && bus.s_bresp == 2 && writes == 1);
	tick();
	std::puts("PASS: RTL media AXI read/write independence, response backpressure and invalid transfers");
}
