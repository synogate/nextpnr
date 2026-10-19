// Bug 35, variant: the same register on the falling edge (yosys sets CLK_INV on the register).
module top(input clk_in, input d, output reg q);
    wire clk;
    CC_BUFG bufg (.I(clk_in), .O(clk));
    always @(negedge clk) q <= d;
endmodule
