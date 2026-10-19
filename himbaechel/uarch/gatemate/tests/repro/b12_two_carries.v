// Bug 12: the carry out of x0 feeds the carry inputs of x1 and of y0.
module top(input [1:0] a, b, c, output [1:0] s, t, output co, cy);
    wire c0, c1, d0;
    CC_ADDF x0 (.A(a[0]), .B(b[0]), .CI(1'b0), .S(s[0]), .CO(c0));
    CC_ADDF x1 (.A(a[1]), .B(b[1]), .CI(c0), .S(s[1]), .CO(co));
    CC_ADDF y0 (.A(c[0]), .B(c[1]), .CI(c0), .S(t[0]), .CO(d0));
    CC_ADDF y1 (.A(a[0]), .B(c[1]), .CI(d0), .S(t[1]), .CO(cy));
endmodule
