// A small design with hierarchy for the Hierarchy dock test (tools/gui_tests/hierarchy.txt): two counters of
// different width, one of them inside a further instance, and an XOR of their outputs at the top level.
// Synthesized flattened (synth_gatemate -run :vlog keeps the dotted net names the dock goes by).
module counter #(parameter W = 8) (input clk, input rst, output [3:0] q);
    reg [W-1:0] count = 0;
    always @(posedge clk)
        if (~rst)
            count <= 0;
        else
            count <= count + 1;
    assign q = count[W-1:W-4];
endmodule

module slow_block (input clk, input rst, output [3:0] q);
    counter #(.W(24)) u_cnt (.clk(clk), .rst(rst), .q(q));
endmodule

module top (input clk, input rst, output [3:0] led);
    wire [3:0] a, b;
    counter #(.W(12)) u_fast (.clk(clk), .rst(rst), .q(a));
    slow_block u_slow (.clk(clk), .rst(rst), .q(b));
    assign led = a ^ b;
endmodule
