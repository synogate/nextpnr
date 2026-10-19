// Bug 1, variant: AND(x, y), two different inputs.
module top(input x, input y, output o);
    CC_LUT2 #(.INIT(4'b1000)) lut (.I0(x), .I1(y), .O(o));
endmodule
