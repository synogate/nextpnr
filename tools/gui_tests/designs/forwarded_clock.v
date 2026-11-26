// A source synchronous interface for the Paths tab (with designs/forwarded_clock.sdc): the design puts its clock out
// at clk_out, and the device outside takes dout and sends din against that clock
module top (input clk, input [7:0] din, output reg [7:0] dout, output clk_out);
    reg [7:0] stage;
    assign clk_out = clk;
    always @(posedge clk) begin
        stage <= din + 8'd3;
        dout <= stage ^ {stage[3:0], stage[7:4]};
    end
endmodule
