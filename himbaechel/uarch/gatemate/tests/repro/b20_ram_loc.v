// Bug 20: two RAMs placed with LOC in opposite corners of the RAM grid (columns 0..3, rows 0..7).
module top(input clk, input [8:0] a, output [1:0] q);
    wire [19:0] q0, q1;
    CC_BRAM_20K #(.LOC("D0X0Y0"), .RAM_MODE("TDP"), .A_RD_WIDTH(20), .A_WR_WIDTH(0)) ram0 (
        .A_CLK(clk), .A_EN(1'b1), .A_WE(1'b0), .A_ADDR({a, 7'b0}), .A_DO(q0),
        .B_CLK(clk), .B_EN(1'b0), .B_WE(1'b0));
    CC_BRAM_20K #(.LOC("D0X3Y7"), .RAM_MODE("TDP"), .A_RD_WIDTH(20), .A_WR_WIDTH(0)) ram1 (
        .A_CLK(clk), .A_EN(1'b1), .A_WE(1'b0), .A_ADDR({a, 7'b0}), .A_DO(q1),
        .B_CLK(clk), .B_EN(1'b0), .B_WE(1'b0));
    assign q = {q1[0], q0[0]};
endmodule
