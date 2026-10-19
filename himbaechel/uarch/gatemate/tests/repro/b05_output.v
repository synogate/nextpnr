// Bug 5: registers in, logic out: d -> r is a path into registers, r -> o a path out to a pad.
module top(input clk, input [7:0] d, output o);
    reg [7:0] r;
    always @(posedge clk) r <= d;
    assign o = ^r;
endmodule
