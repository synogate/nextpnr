// Timing exceptions for the Exceptions tab and the slack details of the Paths tab (with designs/multicycle.sdc):
// four operand registers into an adder tree, whose result res is taken every second cycle (a multicycle path),
// and a counter (a false path from it)
module top (input clk, input [15:0] din, input [1:0] sel, output reg [15:0] dout, output led);
    reg [15:0] op [0:3];
    reg [15:0] res;
    reg [3:0] cnt;
    // (an internal counter: the HDL name of an output port goes to the net of its pad)
    assign led = cnt[3];
    always @(posedge clk) begin
        op[sel] <= din;
        if (cnt[0])
            res <= ((op[0] + op[1]) ^ (op[2] + op[3])) + (op[0] ^ op[3]);
        dout <= res;
        cnt <= cnt + 1'b1;
    end
endmodule
