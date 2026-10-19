// Bug 35: a register on the inverted global clock.
module top(input clk_in, input d, output reg q);
    wire clk, nclk;
    CC_BUFG bufg (.I(clk_in), .O(clk));
    CC_LUT1 #(.INIT(2'b01)) inv (.I0(clk), .O(nclk));
    always @(posedge nclk) q <= d;
endmodule
