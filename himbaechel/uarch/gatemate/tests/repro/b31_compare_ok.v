// Bug 31, variant: equality instead of less-than.
module top(input clk, input [7:0] ia, ib, output reg q);
    reg [7:0] a, b;
    always @(posedge clk) begin
        a <= ia;
        b <= ib;
        q <= a == b;         // equality: LUTs only
    end
endmodule
