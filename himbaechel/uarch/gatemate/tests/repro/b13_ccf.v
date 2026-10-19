// Bug 13: two outputs; their pin settings are in b13_ccf.ccf.
module top(input a, output x, output y);
    assign x = a;
    assign y = ~a;
endmodule
