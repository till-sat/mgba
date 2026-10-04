// SPDX-License-Identifier: MPL-2.0
// Simulation-only AXI bridge. Normal requests continue to the existing SoC.
// 0x40000000..0x400fffff is the AM media extension, serviced by host DPI.
// One read and one write may be outstanding; AW and W are independent.
module axi_media_bridge (
    input clk, rst,
    input s_arvalid, output s_arready,
    input [31:0] s_araddr, input [7:0] s_arlen,
    input [2:0] s_arsize, s_arprot, input [1:0] s_arburst,
    input [3:0] s_arid,
    output s_rvalid, input s_rready, output [31:0] s_rdata,
    output [1:0] s_rresp, output s_rlast, output [3:0] s_rid,
    input s_awvalid, output s_awready,
    input [31:0] s_awaddr, input [7:0] s_awlen,
    input [2:0] s_awsize, s_awprot, input [1:0] s_awburst,
    input [3:0] s_awid,
    input s_wvalid, output s_wready, input [31:0] s_wdata,
    input [3:0] s_wstrb, input s_wlast,
    output s_bvalid, input s_bready, output [1:0] s_bresp, output [3:0] s_bid,

    output m_arvalid, input m_arready,
    output [31:0] m_araddr, output [7:0] m_arlen,
    output [2:0] m_arsize, m_arprot, output [1:0] m_arburst,
    output [3:0] m_arid,
    input m_rvalid, output m_rready, input [31:0] m_rdata,
    input [1:0] m_rresp, input m_rlast, input [3:0] m_rid,
    output m_awvalid, input m_awready,
    output [31:0] m_awaddr, output [7:0] m_awlen,
    output [2:0] m_awsize, m_awprot, output [1:0] m_awburst,
    output [3:0] m_awid,
    output m_wvalid, input m_wready, output [31:0] m_wdata,
    output [3:0] m_wstrb, output m_wlast,
    input m_bvalid, output m_bready, input [1:0] m_bresp, input [3:0] m_bid
);
    import "DPI-C" function int am_rtl_read(input int unsigned address, output int unsigned value);
    import "DPI-C" function int am_rtl_write(input int unsigned address, input int unsigned value);

    wire ar_media = s_araddr[31:20] == 12'h400;
    wire aw_media = s_awaddr[31:20] == 12'h400;
    reg rd_active, rd_media;
    reg [7:0] rd_left;
    reg [31:0] rd_data;
    reg [1:0] rd_resp;
    reg [3:0] rd_id;
    reg wr_active, wr_media, wr_done, wr_valid;
    reg [31:0] wr_addr;
    reg [1:0] wr_resp;
    reg [3:0] wr_id;
    int unsigned value;
    int response;

    assign m_arvalid = s_arvalid && !rd_active && !ar_media;
    assign s_arready = !rd_active && (ar_media || m_arready);
    assign m_araddr = s_araddr;
    assign m_arlen = s_arlen;
    assign m_arsize = s_arsize;
    assign m_arprot = s_arprot;
    assign m_arburst = s_arburst;
    assign m_arid = s_arid;
    assign s_rvalid = rd_active && (rd_media || m_rvalid);
    assign s_rdata = rd_media ? rd_data : m_rdata;
    assign s_rresp = rd_media ? rd_resp : m_rresp;
    assign s_rlast = rd_media ? rd_left == 0 : m_rlast;
    assign s_rid = rd_media ? rd_id : m_rid;
    assign m_rready = s_rready && rd_active && !rd_media;

    assign m_awvalid = s_awvalid && !wr_active && !aw_media;
    assign s_awready = !wr_active && (aw_media || m_awready);
    assign m_awaddr = s_awaddr;
    assign m_awlen = s_awlen;
    assign m_awsize = s_awsize;
    assign m_awprot = s_awprot;
    assign m_awburst = s_awburst;
    assign m_awid = s_awid;
    // An AXI slave may wait for WVALID before asserting AWREADY. Present
    // normal write data from the live AW decode even before AW is accepted.
    wire live_write = !wr_active && s_awvalid && !aw_media;
    assign m_wvalid = s_wvalid && !wr_done && ((wr_active && !wr_media) || live_write);
    assign s_wready = !wr_done && (wr_active ? (wr_media || m_wready) : (live_write && m_wready));
    assign m_wdata = s_wdata;
    assign m_wstrb = s_wstrb;
    assign m_wlast = s_wlast;
    assign s_bvalid = wr_active && (wr_media ? wr_done : m_bvalid);
    assign s_bresp = wr_media ? wr_resp : m_bresp;
    assign s_bid = wr_media ? wr_id : m_bid;
    assign m_bready = s_bready && wr_active && !wr_media;

    always @(posedge clk) begin
        if (rst) begin
            rd_active <= 0;
            rd_media <= 0;
            rd_left <= 0;
            rd_data <= 0;
            rd_resp <= 0;
            rd_id <= 0;
            wr_active <= 0;
            wr_media <= 0;
            wr_done <= 0;
            wr_valid <= 0;
            wr_addr <= 0;
            wr_resp <= 0;
            wr_id <= 0;
        end else begin
            if (s_arvalid && s_arready) begin
                rd_active <= 1;
                rd_media <= ar_media;
                rd_left <= s_arlen;
                rd_id <= s_arid;
                if (ar_media) begin
                    rd_data <= 0;
                    rd_resp <= 2;
                    if (s_arlen == 0 && s_arsize == 2 && s_araddr[1:0] == 0) begin
                        response = am_rtl_read(s_araddr, value);
                        rd_data <= value;
                        rd_resp <= response[1:0];
                    end
                end
            end
            if (s_rvalid && s_rready) begin
                if (s_rlast) rd_active <= 0;
                else if (rd_media) rd_left <= rd_left - 1'b1;
            end
            if (s_awvalid && s_awready) begin
                wr_active <= 1;
                wr_media <= aw_media;
                wr_valid <= s_awlen == 0 && s_awsize == 2 && s_awaddr[1:0] == 0;
                wr_addr <= s_awaddr;
                wr_id <= s_awid;
                wr_resp <= 2;
            end
            if (s_wvalid && s_wready) begin
                if (s_wlast) wr_done <= 1;
                if (wr_active && wr_media && wr_valid && s_wlast && s_wstrb == 4'hf) begin
                    response = am_rtl_write(wr_addr, s_wdata);
                    wr_resp <= response[1:0];
                end
            end
            if (s_bvalid && s_bready) begin
                wr_active <= 0;
                wr_done <= 0;
            end
        end
    end
endmodule
