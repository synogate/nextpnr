// Bug 30: a bidirectional pad read into a register.
module top(input clk, input a, input t, inout b, output reg q);
    assign b = t ? 1'bz : a;
    always @(posedge clk) q <= ~b;
endmodule
