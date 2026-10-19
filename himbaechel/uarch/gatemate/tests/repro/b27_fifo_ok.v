// Bug 27, variant: the data output instead of the empty flag.
module top(input clk, input push, input pop, input d, output reg q);
    wire empty;
    wire [39:0] dout;
    CC_FIFO_40K #(.RAM_MODE("TDP"), .FIFO_MODE("SYNC"), .A_WIDTH(1), .B_WIDTH(1)) fifo (
        .A_CLK(clk), .B_CLK(clk), .A_EN(pop), .B_EN(push), .B_WE(1'b1), .F_RST_N(1'b1),
        .B_DI({39'b0, d}), .B_BM({39'b0, 1'b1}), .F_EMPTY(empty), .A_DO(dout));
    always @(posedge clk) q <= ~dout[0];    // data output instead of the flag
endmodule
