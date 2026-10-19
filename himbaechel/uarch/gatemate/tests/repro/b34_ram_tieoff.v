// Bug 34: a 1024 x 16 bit memory, which yosys maps to a 20K block RAM in 20-bit mode.
module top(input clk, input we, input [9:0] waddr, raddr, input [15:0] d, output reg [15:0] q);
    reg [15:0] mem [0:1023];
    always @(posedge clk) begin
        if (we) mem[waddr] <= d;
        q <= mem[raddr];
    end
endmodule
