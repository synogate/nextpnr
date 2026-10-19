// Bug 26: a register whose clock enable comes from deep logic.
module top(input clk, input [15:0] ie, input d, output reg q);
    reg [15:0] e;
    always @(posedge clk) begin
        e <= ie;
        if (^e) q <= d;
    end
endmodule
