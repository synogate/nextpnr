// Bug 30, variant: the same with an input pad.
module top(input clk, input b, output reg q);
    always @(posedge clk) q <= ~b;
endmodule
