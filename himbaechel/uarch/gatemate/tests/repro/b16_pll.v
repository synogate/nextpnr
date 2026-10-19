// Bug 16: a PLL with LOW_JITTER and LOCK_REQ left at their defaults (1 in UG1001).
module top(input clk_in, input d, output reg q);
    wire clk;
    CC_PLL #(.REF_CLK("10.0"), .OUT_CLK("123.456"), .PERF_MD("SPEED")) pll (.CLK_REF(clk_in), .CLK0(clk));
    always @(posedge clk) q <= d;
endmodule
