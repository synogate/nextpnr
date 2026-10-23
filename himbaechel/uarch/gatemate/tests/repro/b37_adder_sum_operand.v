// Bug 37: a 2 bit ripple-carry chain whose second bit adds the first bit's sum (s[0]) to c: an
// ordinary design without a loop (s[1] = s[0] + c + carry of bit 0).
module top(input [1:0] a, input b, input c, input ci, output [1:0] s, output co);
    wire c0;
    CC_ADDF x0 (.A(a[0]), .B(b), .CI(ci), .S(s[0]), .CO(c0));
    CC_ADDF x1 (.A(s[0]), .B(c), .CI(c0), .S(s[1]), .CO(co));
endmodule
