// O1 (common code): a RAM and an IO output register clocked from the fabric (clk without a
// clock buffer), and another clock with a register-to-register path.
module top(input clk, input other, input d, input [7:0] a, output q, output q2);
    wire oclk, r;
    wire [19:0] dout;
    reg r1, r2, ff;
    CC_BUFG ob (.I(other), .O(oclk));
    always @(posedge oclk) begin r1 <= d; r2 <= ~r1; end
    assign q2 = r2;
    CC_BRAM_20K #(.RAM_MODE("TDP"), .A_RD_WIDTH(20), .A_WR_WIDTH(0)) ram (
        .A_CLK(clk), .A_EN(1'b1), .A_WE(1'b0), .A_ADDR({1'b0, a, 7'b0}), .A_DO(dout),
        .B_CLK(clk), .B_EN(1'b0), .B_WE(1'b0));
    always @(posedge clk) ff <= dout[0];
    CC_OBUF #(.FF_OBF(1)) qbuf (.A(ff), .O(q));
endmodule
