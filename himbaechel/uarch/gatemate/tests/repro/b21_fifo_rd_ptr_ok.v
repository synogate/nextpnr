// Bug 21, variant: read pointer bit 3, which has a CPE of its own.
module top(input clk, input push, input pop, input d, output q, output empty, output ptr2);
    wire [39:0] dout;
    wire [15:0] rd_ptr;
    CC_FIFO_40K #(.RAM_MODE("TDP"), .FIFO_MODE("SYNC"), .A_WIDTH(1), .B_WIDTH(1)) fifo (
        .A_CLK(clk), .B_CLK(clk), .A_EN(pop), .B_EN(push), .B_WE(1'b1), .F_RST_N(1'b1),
        .B_DI({39'b0, d}), .B_BM({39'b0, 1'b1}), .A_DO(dout), .F_EMPTY(empty), .F_RD_PTR(rd_ptr));
    assign q = dout[0];
    assign ptr2 = rd_ptr[3];
endmodule
