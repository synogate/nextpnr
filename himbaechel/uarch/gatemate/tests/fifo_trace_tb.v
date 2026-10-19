// Cross-check of the FIFO model in netlist_eval.cc against the yosys simulation model
// (techlibs/gatemate/cells_sim.v CC_FIFO_40K). Not part of the build. Both sides apply the same
// pseudo-random stimulus (GateMateBramSemanticsTest.DISABLED_fifo_trace prints the model's side);
// their outputs should be identical. For widths of 5 bits and more, compare flags and pointers only
// (cut -c1-38 of the second field): cells_sim.v lets consecutive words overlap there.
//
//   iverilog -DWIDTH=2 '-DMODE="SYNC"' -DSTEPS=3000 -DNORESET=0 -o t.vvp fifo_trace_tb.v \
//       $(yosys-config --datdir)/gatemate/cells_sim.v
//   vvp -n t.vvp | grep -E '^[0-9]+ ' > iverilog.txt
//   FIFO_TRACE_WIDTH=2 nextpnr-himbaechel-test --gtest_also_run_disabled_tests \
//       --gtest_filter='*DISABLED_fifo_trace' | grep -E '^[0-9]+ ' > model.txt
//   diff iverilog.txt model.txt
//
// Options: FIFO_TRACE_ASYNC=1 with MODE "ASYNC"; FIFO_TRACE_WIDTH=80 with WIDTH=80 (SDP, a FIFO
// that can fill up); FIFO_TRACE_STEPS with STEPS; FIFO_TRACE_NORESET=1 with NORESET=1.
`timescale 1ns/1ps
module tb;
  reg rclk, wclk, pop, push, rst_n, d0, d1;
  wire [39:0] do_a, do_b;
  wire full, empty, afull, aempty, rderr, wrerr;
  wire [15:0] rdptr, wrptr;
  localparam W = `WIDTH;
  localparam DEPTH = (W == 1) ? 32768 : (W == 2) ? 16384 : 512;
  CC_FIFO_40K #(.A_WIDTH(W), .B_WIDTH(W), .RAM_MODE(W == 80 ? "SDP" : "TDP"), .FIFO_MODE(`MODE),
                .ALMOST_FULL_OFFSET(DEPTH - 3), .ALMOST_EMPTY_OFFSET(2)) f (
    .A_DO(do_a), .B_DO(do_b), .A_CLK(rclk), .A_EN(pop), .A_DI(W == 80 ? {38'b0, d1, d0} : 40'b0), .B_DI(W == 80 ? 40'b0 : {38'b0, d1, d0}),
    .A_BM(W == 80 ? {40{1'b1}} : 40'b0), .B_BM(W == 80 ? 40'b0 : {40{1'b1}}), .B_CLK(wclk), .B_EN(push), .B_WE(1'b1), .F_RST_N(rst_n),
    .F_ALMOST_FULL_OFFSET(15'b0), .F_ALMOST_EMPTY_OFFSET(15'b0),
    .F_FULL(full), .F_EMPTY(empty), .F_ALMOST_FULL(afull), .F_ALMOST_EMPTY(aempty), .F_RD_ERROR(rderr),
    .F_WR_ERROR(wrerr), .F_RD_PTR(rdptr), .F_WR_PTR(wrptr));
  integer step;
  reg [31:0] lcg;
  reg [14:0] r;
  reg fill;
  initial begin
    lcg = 12345;
    for (step = 0; step < `STEPS; step = step + 1) begin
      lcg = lcg * 32'd1103515245 + 32'd12345;
      r = (lcg >> 16) & 15'h7fff;
      fill = ((step / (`STEPS / 6)) % 2) == 0;
      if (step == 0) begin rclk = 0; wclk = 0; end
      pop = fill ? (`NORESET ? (r[10:8] == 3'b111) : (r[3:2] == 2'b11)) : (r[3:2] != 2'b00);
      push = fill ? (`NORESET || r[5:4] != 2'b00) : (r[5:4] == 2'b11);
      rst_n = (step < 2) ? 1'b0 : (`NORESET || r[14:8] != 7'b0);
      d0 = r[6]; d1 = r[7];
      #1;
      rclk = r[0]; wclk = r[1];
      #1;
      $display("%0d %b%b%b%b%b%b%b%b%b r=%0d", step, full, empty, afull, aempty, rderr, wrerr, rdptr, wrptr, do_a[(W > 1 ? 1 : 0):0], r);
      #1;
    end
    $finish;
  end
endmodule
