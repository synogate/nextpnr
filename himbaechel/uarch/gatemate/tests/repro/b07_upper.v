// Bug 7: eight registers; the placer puts some into the upper half of a CPE.
module top(input clk, input [7:0] d, output [7:0] q);
    reg [7:0] r;
    always @(posedge clk) r <= d;
    assign q = ~r;
endmodule
