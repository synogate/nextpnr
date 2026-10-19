// Bug 1: AND(x, x) = x
module top(input x, output o);
    CC_LUT2 #(.INIT(4'b1000)) lut (.I0(x), .I1(x), .O(o));
endmodule
