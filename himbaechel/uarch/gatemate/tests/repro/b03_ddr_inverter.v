// Bug 3: an inverted DDR output, and a plain output packed after it (IOs are packed in name order).
module top(input clk, input d0, input d1, input sel, input p, output ddr_out, output plain);
    wire q;
    CC_ODDR oddr (.D0(d0), .D1(d1), .CLK(clk), .DDR(sel), .Q(q));
    assign ddr_out = ~q;
    assign plain = p;
endmodule
