// Bug 31, variant: register a on input I0 instead of I4.
module top(input clk, input d, input [3:0] x, output reg q);
    reg a;
    wire y;
    CC_L2T5 #(.INIT_L02(4'b0110), .INIT_L03(4'b0110), .INIT_L11(4'b0110), .INIT_L20(4'b0110))
        lut (.I0(a), .I1(x[1]), .I2(x[2]), .I3(x[3]), .I4(x[0]), .O(y));
    always @(posedge clk) begin
        a <= d;
        q <= y;
    end
endmodule
