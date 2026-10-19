// Bug 17, variant: the pull-up alone.
module top(input a, output o);
    wire y;
    CC_IBUF #(.PULLUP(1)) a_buf (.I(a), .Y(y));
    assign o = ~y;
endmodule
