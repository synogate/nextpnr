// Bug 17: keeper and pull-up together, which UG1001 says are mutually exclusive.
module top(input a, output o);
    wire y;
    CC_IBUF #(.KEEPER(1), .PULLUP(1)) a_buf (.I(a), .Y(y));
    assign o = ~y;
endmodule
