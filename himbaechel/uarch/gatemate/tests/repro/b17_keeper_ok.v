// Bug 17, variant: the keeper alone.
module top(input a, output o);
    wire y;
    CC_IBUF #(.KEEPER(1)) a_buf (.I(a), .Y(y));
    assign o = ~y;
endmodule
