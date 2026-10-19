// Bug 2: AND(x, 1) = x
module top(input x, output o);
    CC_LUT2 #(.INIT(4'b1000)) lut (.I0(x), .I1(1'b1), .O(o));
endmodule
