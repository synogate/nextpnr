// Bug 31: register a on the fifth input (I4) of a 5-input LUT tree. The only path inside clk is a -> q.
module top(input clk, input d, input [3:0] x, output reg q);
    reg a;
    wire y;
    CC_L2T5 #(.INIT_L02(4'b0110), .INIT_L03(4'b0110), .INIT_L11(4'b0110), .INIT_L20(4'b0110))
        lut (.I0(x[0]), .I1(x[1]), .I2(x[2]), .I3(x[3]), .I4(a), .O(y));   // variant: a on I0, x[0] on I4
    always @(posedge clk) begin
        a <= d;
        q <= y;
    end
endmodule
