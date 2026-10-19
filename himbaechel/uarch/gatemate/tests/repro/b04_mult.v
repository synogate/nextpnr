// Bug 4: a multiplier whose product registers do not all have the same reset.
module top(input clk, input rst, input [1:0] a, b, output reg [3:0] p);
    wire [3:0] prod;
    CC_MULT #(.A_WIDTH(2), .B_WIDTH(2), .P_WIDTH(4)) mult (.A(a), .B(b), .P(prod));
    always @(posedge clk) {p[3], p[2], p[0]} <= {prod[3], prod[2], prod[0]};
    always @(posedge clk or posedge rst) if (rst) p[1] <= 1'b0; else p[1] <= prod[1];
endmodule
