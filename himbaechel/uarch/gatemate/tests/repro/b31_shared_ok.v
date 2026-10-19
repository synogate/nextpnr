// Bug 31, variant: LUT s drives only LUT t (no register q1).
module top(input clk, input d, input x, input y, output reg q2);
    reg a;
    wire s, t;
    CC_LUT2 #(.INIT(4'b0110)) s_lut (.I0(a), .I1(x), .O(s));
    CC_LUT2 #(.INIT(4'b1000)) t_lut (.I0(s), .I1(y), .O(t));
    always @(posedge clk)  a <= d;
    always @(posedge clk)  q2 <= t;
endmodule
