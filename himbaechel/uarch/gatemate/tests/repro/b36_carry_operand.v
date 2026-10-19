// Bug 36: x0..x3: a 4 bit ripple-carry chain. a_single: a 1 bit adder that takes the carry
// between bits 1 and 2 of the chain (c1) as an operand, with its own carry input ci2.
module top(input [3:0] a, input [3:0] b, input b2, input ci2,
           output [3:0] s, output co, output s2, output co2);
    wire c0, c1, c2;
    CC_ADDF a_single (.A(c1), .B(b2), .CI(ci2), .S(s2), .CO(co2));
    CC_ADDF x0 (.A(a[0]), .B(b[0]), .CI(1'b0), .S(s[0]), .CO(c0));
    CC_ADDF x1 (.A(a[1]), .B(b[1]), .CI(c0), .S(s[1]), .CO(c1));
    CC_ADDF x2 (.A(a[2]), .B(b[2]), .CI(c1), .S(s[2]), .CO(c2));
    CC_ADDF x3 (.A(a[3]), .B(b[3]), .CI(c2), .S(s[3]), .CO(co));
endmodule
