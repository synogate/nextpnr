// Bug 14, variant: the instance's output goes through another LUT before the pad.
module sub(input a, input b, output y);
    CC_LUT2 #(.INIT(4'b0110)) lut (.I0(a), .I1(b), .O(y));
endmodule
module top(input a, input b, output o);
    wire y;
    sub u (.a(a), .b(b), .y(y));
    assign o = ~(y & a);
endmodule
