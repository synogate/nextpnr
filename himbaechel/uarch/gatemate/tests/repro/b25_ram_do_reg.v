// Bug 25: the same RAM with its output register (A_DO_REG = 1).
module top(input clk, input [7:0] ia, input x, output reg y);
    wire [19:0] dout;
    CC_BRAM_20K #(.RAM_MODE("TDP"), .A_RD_WIDTH(20), .A_WR_WIDTH(0), .A_DO_REG(1'b1))
        ram (.A_CLK(clk), .A_EN(1'b1), .A_WE(1'b0), .A_ADDR({1'b0, ia, 7'b0}), .A_DO(dout),
             .B_CLK(clk), .B_EN(1'b0), .B_WE(1'b0));
    always @(posedge clk) begin
        y <= dout[0] ^ x;
    end
endmodule
