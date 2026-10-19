// Bug 33: the carry between bits 1 and 2 of a chain, also used as a signal into register m.
module top(input clk, input [3:0] ia, ib, output reg m);
    reg [3:0] a, b;
    wire c0, c1, c2, c3;
    wire [3:0] s;
    CC_ADDF x0 (.A(a[0]), .B(b[0]), .CI(1'b0), .S(s[0]), .CO(c0));
    CC_ADDF x1 (.A(a[1]), .B(b[1]), .CI(c0), .S(s[1]), .CO(c1));
    CC_ADDF x2 (.A(a[2]), .B(b[2]), .CI(c1), .S(s[2]), .CO(c2));
    CC_ADDF x3 (.A(a[3]), .B(b[3]), .CI(c2), .S(s[3]), .CO(c3));
    always @(posedge clk) begin
        a <= ia;
        b <= ib;
        m <= ~c1;          // variant b33_mid_carry_ok.v: m <= s[1];
    end
endmodule
