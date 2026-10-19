// Bug 8: ten clocks on the two-die CCGM1A2, which has 8 global clock buffers with --vopt strategy=full.
module top(input [9:0] clk, input [9:0] d, output reg [9:0] q);
    genvar i;
    for (i = 0; i < 10; i = i + 1)
        always @(posedge clk[i]) q[i] <= d[i];
endmodule
