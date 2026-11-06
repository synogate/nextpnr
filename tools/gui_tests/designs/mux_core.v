// A small partition for the save / load tests (tools/gui_tests/saveload_*.txt): the 008-counter with a
// registered 4:1 multiplexer (a CC_MX4, packed as a cluster of the two halves of a CPE) and a register fed by a
// LUT (packed as a LUT / register cluster). Synthesized without IO pads, run with --vopt as-partition.
module core (
    input  clk,
    input  rst,
    input  [1:0] sel,
    input  [3:0] d,
    output reg q,
    output reg p,
    output [7:0] led
);
    reg [28:0] counter = 0;
    always @(posedge clk) begin
        if (~rst)
            counter <= 0;
        else
            counter <= counter + 1;
    end
    assign led = ~counter[28:21];

    wire m;
    CC_MX4 mux (.D0(d[0]), .D1(d[1]), .D2(d[2]), .D3(d[3]), .S0(sel[0]), .S1(sel[1]), .Y(m));
    always @(posedge clk) begin
        q <= m;
        p <= (d[0] & sel[1]) ^ m;
    end
endmodule
