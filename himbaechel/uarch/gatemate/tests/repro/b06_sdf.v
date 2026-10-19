// Bug 6: one register between two pads, written out as SDF.
module top(input clk, input d, output q);
    reg r;
    always @(posedge clk) r <= ~d;
    assign q = r;
endmodule
