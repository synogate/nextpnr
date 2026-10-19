// Bug 9: two cascaded 40K RAMs with 40-bit ports (nextpnr's own message: cascades only 1 bit wide).
module top(input clk, input we, input [15:0] addr, input [39:0] d, output [39:0] q);
    wire a_co, b_co;
    CC_BRAM_40K #(.CAS("LOWER"), .RAM_MODE("TDP"), .A_RD_WIDTH(40), .A_WR_WIDTH(40)) lower (
        .A_CLK(clk), .A_EN(1'b1), .A_WE(we), .A_ADDR(addr), .A_DI(d), .A_BM({40{1'b1}}),
        .A_CO(a_co), .B_CO(b_co), .B_CLK(clk), .B_EN(1'b0), .B_WE(1'b0));
    CC_BRAM_40K #(.CAS("UPPER"), .RAM_MODE("TDP"), .A_RD_WIDTH(40), .A_WR_WIDTH(40)) upper (
        .A_CLK(clk), .A_EN(1'b1), .A_WE(we), .A_ADDR(addr), .A_DI(d), .A_BM({40{1'b1}}),
        .A_CI(a_co), .B_CI(b_co), .A_DO(q), .B_CLK(clk), .B_EN(1'b0), .B_WE(1'b0));
endmodule
