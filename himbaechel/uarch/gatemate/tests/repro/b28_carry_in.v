// Bug 28: an adder with a carry input from a pin (the carry enters the chain through the CP lines).
module top(input clk, input [7:0] ia, ib, input ci, output reg [8:0] s);
    reg [7:0] a, b;
    always @(posedge clk) begin
        a <= ia;
        b <= ib;
        s <= a + b + ci;
    end
endmodule
