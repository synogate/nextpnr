// Bug 23: a W-bit adder; operands shifted in from pins so the pin count stays small.
module top #(parameter W = 128) (input clk, input ai, input bi, output reg y);
    reg [W-1:0] a, b, s;
    always @(posedge clk) begin
        a <= {a[W-2:0], ai};
        b <= {b[W-2:0], bi};
        s <= a + b;
        y <= s[W-1];
    end
endmodule
