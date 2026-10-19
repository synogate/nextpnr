// Bug 31: LUT s drives register q1 and LUT t. q1 is on another clock, so the only path inside clk is a -> s -> t -> q2.
module top(input clk, input clk2, input d, input x, input y, output reg q1, output reg q2);
    reg a;
    wire s, t;
    CC_LUT2 #(.INIT(4'b0110)) s_lut (.I0(a), .I1(x), .O(s));
    CC_LUT2 #(.INIT(4'b1000)) t_lut (.I0(s), .I1(y), .O(t));
    always @(posedge clk)  a <= d;
    always @(posedge clk)  q2 <= t;
    always @(posedge clk2) q1 <= s;      // variant b31_shared_ok.v: without q1
endmodule
