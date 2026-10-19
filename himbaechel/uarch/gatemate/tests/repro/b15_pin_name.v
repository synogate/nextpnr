// Bug 15: input c is pinned to IO_SA_A0 with PIN_NAME, the other IOs are left to nextpnr.
module top(input c_in, input [3:0] d, output [3:0] q);
    wire c;
    CC_IBUF #(.PIN_NAME("IO_SA_A0")) c_buf (.I(c_in), .Y(c));
    assign q = d ^ {4{c}};
endmodule
