// Two clocks for the Clocks tab: a counter on clk_a, one on the falling edge of clk_b and a register stage from
// clk_a into clk_b (a path between unrelated clocks)
module top (input clk_a, input clk_b, input rst, output [3:0] led_a, output [3:0] led_b);
    reg [15:0] ca = 0;
    always @(posedge clk_a)
        if (rst) ca <= 0; else ca <= ca + 1'b1;

    reg [11:0] cb = 0;
    always @(negedge clk_b)
        if (rst) cb <= 0; else cb <= cb + 1'b1;

    reg [3:0] sync = 0;
    always @(posedge clk_b)
        sync <= ca[15:12] ^ cb[3:0];

    assign led_a = ca[15:12];
    assign led_b = sync;
endmodule
