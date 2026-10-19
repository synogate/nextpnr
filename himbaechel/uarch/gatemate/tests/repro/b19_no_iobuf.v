// Bug 19: synthesized with -noiopad: input p drives logic directly.
module top(input p, output o);
    assign o = ~p;
endmodule
