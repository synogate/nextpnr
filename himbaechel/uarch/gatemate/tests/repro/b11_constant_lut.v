// Bug 11: register q has the only constant enable/reset; LUT c has a constant function.
module top(input clk, input x, input y, input s, output o, output reg q);
    wire zero;
    CC_LUT2 #(.INIT(4'b0000)) c (.I0(x), .I1(y), .O(zero));
    CC_MX2 m (.D0(x), .D1(zero), .S0(s), .Y(o));        // o = s ? 0 : x
    always @(posedge clk) q <= x;
endmodule
