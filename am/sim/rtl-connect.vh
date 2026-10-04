// SPDX-License-Identifier: MPL-2.0
// Included by the optional simulation media integration after the AXI downsizer.
    wire b_arvalid;
    wire b_arready;
    wire [31:0] b_araddr;
    wire [7:0] b_arlen;
    wire [2:0] b_arsize;
    wire [2:0] b_arprot;
    wire [1:0] b_arburst;
    wire [3:0] b_arid;
    wire b_rvalid;
    wire b_rready;
    wire [31:0] b_rdata;
    wire [1:0] b_rresp;
    wire b_rlast;
    wire [3:0] b_rid;
    wire b_awvalid;
    wire b_awready;
    wire [31:0] b_awaddr;
    wire [7:0] b_awlen;
    wire [2:0] b_awsize;
    wire [2:0] b_awprot;
    wire [1:0] b_awburst;
    wire [3:0] b_awid;
    wire b_wvalid;
    wire b_wready;
    wire [31:0] b_wdata;
    wire [3:0] b_wstrb;
    wire b_wlast;
    wire b_bvalid;
    wire b_bready;
    wire [1:0] b_bresp;
    wire [3:0] b_bid;
    axi_media_bridge u_am_media (
        .clk (`AM_MEDIA_CLOCK), .rst (`AM_MEDIA_RESET),
        .s_arvalid (`AM_MEDIA_UP(arvalid)),
        .s_arready (`AM_MEDIA_UP(arready)),
        .s_araddr (`AM_MEDIA_UP(araddr)),
        .s_arlen (`AM_MEDIA_UP(arlen)),
        .s_arsize (`AM_MEDIA_UP(arsize)),
        .s_arprot (`AM_MEDIA_UP(arprot)),
        .s_arburst (`AM_MEDIA_UP(arburst)),
        .s_arid (`AM_MEDIA_UP(arid)),
        .s_rvalid (`AM_MEDIA_UP(rvalid)),
        .s_rready (`AM_MEDIA_UP(rready)),
        .s_rdata (`AM_MEDIA_UP(rdata)),
        .s_rresp (`AM_MEDIA_UP(rresp)),
        .s_rlast (`AM_MEDIA_UP(rlast)),
        .s_rid (`AM_MEDIA_UP(rid)),
        .s_awvalid (`AM_MEDIA_UP(awvalid)),
        .s_awready (`AM_MEDIA_UP(awready)),
        .s_awaddr (`AM_MEDIA_UP(awaddr)),
        .s_awlen (`AM_MEDIA_UP(awlen)),
        .s_awsize (`AM_MEDIA_UP(awsize)),
        .s_awprot (`AM_MEDIA_UP(awprot)),
        .s_awburst (`AM_MEDIA_UP(awburst)),
        .s_awid (`AM_MEDIA_UP(awid)),
        .s_wvalid (`AM_MEDIA_UP(wvalid)),
        .s_wready (`AM_MEDIA_UP(wready)),
        .s_wdata (`AM_MEDIA_UP(wdata)),
        .s_wstrb (`AM_MEDIA_UP(wstrb)),
        .s_wlast (`AM_MEDIA_UP(wlast)),
        .s_bvalid (`AM_MEDIA_UP(bvalid)),
        .s_bready (`AM_MEDIA_UP(bready)),
        .s_bresp (`AM_MEDIA_UP(bresp)),
        .s_bid (`AM_MEDIA_UP(bid)),
        .m_arvalid (`AM_MEDIA_DOWN(arvalid)),
        .m_arready (`AM_MEDIA_DOWN(arready)),
        .m_araddr (`AM_MEDIA_DOWN(araddr)),
        .m_arlen (`AM_MEDIA_DOWN(arlen)),
        .m_arsize (`AM_MEDIA_DOWN(arsize)),
        .m_arprot (`AM_MEDIA_DOWN(arprot)),
        .m_arburst (`AM_MEDIA_DOWN(arburst)),
        .m_arid (`AM_MEDIA_DOWN(arid)),
        .m_rvalid (`AM_MEDIA_DOWN(rvalid)),
        .m_rready (`AM_MEDIA_DOWN(rready)),
        .m_rdata (`AM_MEDIA_DOWN(rdata)),
        .m_rresp (`AM_MEDIA_DOWN(rresp)),
        .m_rlast (`AM_MEDIA_DOWN(rlast)),
        .m_rid (`AM_MEDIA_DOWN(rid)),
        .m_awvalid (`AM_MEDIA_DOWN(awvalid)),
        .m_awready (`AM_MEDIA_DOWN(awready)),
        .m_awaddr (`AM_MEDIA_DOWN(awaddr)),
        .m_awlen (`AM_MEDIA_DOWN(awlen)),
        .m_awsize (`AM_MEDIA_DOWN(awsize)),
        .m_awprot (`AM_MEDIA_DOWN(awprot)),
        .m_awburst (`AM_MEDIA_DOWN(awburst)),
        .m_awid (`AM_MEDIA_DOWN(awid)),
        .m_wvalid (`AM_MEDIA_DOWN(wvalid)),
        .m_wready (`AM_MEDIA_DOWN(wready)),
        .m_wdata (`AM_MEDIA_DOWN(wdata)),
        .m_wstrb (`AM_MEDIA_DOWN(wstrb)),
        .m_wlast (`AM_MEDIA_DOWN(wlast)),
        .m_bvalid (`AM_MEDIA_DOWN(bvalid)),
        .m_bready (`AM_MEDIA_DOWN(bready)),
        .m_bresp (`AM_MEDIA_DOWN(bresp)),
        .m_bid (`AM_MEDIA_DOWN(bid))
    );
