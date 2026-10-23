// Bug 37, variant: the second bit adds a[1] instead of the first bit's sum.
module top(input [1:0] a, input b, input c, input ci, output [1:0] s, output co);
    wire c0;
    CC_ADDF x0 (.A(a[0]), .B(b), .CI(ci), .S(s[0]), .CO(c0));
    CC_ADDF x1 (.A(a[1]), .B(c), .CI(c0), .S(s[1]), .CO(co));
endmodule
