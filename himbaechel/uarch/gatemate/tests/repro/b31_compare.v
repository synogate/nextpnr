// Bug 31: the result of a < b is a carry out. The only path inside clk is a, b -> q.
module top(input clk, input [7:0] ia, ib, output reg q);
    reg [7:0] a, b;
    always @(posedge clk) begin
        a <= ia;
        b <= ib;
        q <= a < b;          // variant b31_compare_ok.v: q <= a == b;
    end
endmodule
