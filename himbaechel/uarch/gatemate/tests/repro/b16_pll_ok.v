// Bug 16, variant: LOW_JITTER and LOCK_REQ set to their documented defaults explicitly.
module top(input clk_in, input d, output reg q);
    wire clk;
    CC_PLL #(.REF_CLK("10.0"), .OUT_CLK("123.456"), .PERF_MD("SPEED"), .LOW_JITTER(1), .LOCK_REQ(1)) pll (.CLK_REF(clk_in), .CLK0(clk));
    always @(posedge clk) q <= d;
endmodule
