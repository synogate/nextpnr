// N picorv32 SoCs; their LEDs are XORed so all logic stays
module multi #(parameter N = 1) (input clk, input [N-1:0] din, output [7:0] led);
    wire [7:0] leds [0:N-1];
    genvar i;
    generate for (i = 0; i < N; i = i + 1) begin : core
        attosoc soc(.clk(clk), .din(din[i]), .led(leds[i]));
    end endgenerate
    integer j;
    reg [7:0] x;
    always @* begin
        x = 0;
        for (j = 0; j < N; j = j + 1) x = x ^ leds[j];
    end
    assign led = x;
endmodule
