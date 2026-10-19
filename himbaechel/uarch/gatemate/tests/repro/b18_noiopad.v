// Bug 18 via bug 19: synthesized with -noiopad, the RAM address comes straight from an input port.
module top(input clk, input [8:0] a, output [19:0] q);
    CC_BRAM_20K #(.RAM_MODE("TDP"), .A_RD_WIDTH(20), .A_WR_WIDTH(0)) ram (
        .A_CLK(clk), .A_EN(1'b1), .A_WE(1'b0), .A_ADDR({a, 7'b0}), .A_DO(q),
        .B_CLK(clk), .B_EN(1'b0), .B_WE(1'b0));
endmodule
