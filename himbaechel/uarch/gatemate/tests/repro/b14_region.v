// Bug 14: the logic of instance u is constrained to a region (b14_region.ccf), and its LUT drives
// the output pad o directly. (Synthesized with -scopename; LUTs mapped by abc lose the scope, so
// u instantiates its LUT.)
module sub(input a, input b, output y);
    CC_LUT2 #(.INIT(4'b0110)) lut (.I0(a), .I1(b), .O(y));
endmodule
module top(input a, input b, output o);
    sub u (.a(a), .b(b), .y(o));       // variant b14_region_ok.v: o = ~(y & a)
endmodule
