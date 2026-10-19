// Bug 3, variant: the same without the inverter after the DDR output.
module top(input clk, input d0, input d1, input sel, input p, output ddr_out, output plain);
    wire q;
    CC_ODDR oddr (.D0(d0), .D1(d1), .CLK(clk), .DDR(sel), .Q(q));
    assign ddr_out = q;
    assign plain = p;
endmodule
