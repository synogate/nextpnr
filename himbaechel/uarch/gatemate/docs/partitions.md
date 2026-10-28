# Partitions: placing and routing a design in parts

A partition is a module placed and routed on its own. Another design can then use it as a finished block. The
partition's cells and the routing inside it are taken over as they are, locked, and optionally moved by whole
tiles. Only the rest of the design and the connections to the partition are placed and routed. One partition
file can be used several times, at different places.

## Flow

1. Synthesize the partition as its own top level, without IO pads, constrain it to a region, and place and route
   it with `--vopt as-partition`:

   ```
   yosys -p "read_verilog top.v; synth_gatemate -top core -noiopad; write_json core.json"
   nextpnr-himbaechel --device CCGM1A1 --json core.json --vopt ccf=core.ccf --sdc core.sdc \
       --vopt as-partition --write core_pnr.json
   ```

   with the region in `core.ccf` (CCF coordinates, which are the bel coordinates minus 2):

   ```
   start floorplanning
   top* ; pB : x0y24x31y127
   end floorplanning
   ```

   The region is the partition's area. Its cells are placed inside, and the routing it keeps stays inside too,
   with a margin of two tiles (the switch boxes around the CPEs at the edge). Without a region, the area is
   wherever the cells and routing end up, and that can be most of the device.

   The top-level ports become nets without IO pads. In the result, an input has no driver and an output has no
   users, and neither is routed. While the partition is placed and routed, a cell outside the area stands in
   for the outside on each port, and clock inputs keep their global buffer. This way the routing inside the
   partition leaves ways across its edge to each port's sinks and drivers, and the clock's dedicated routing
   stays free. After routing, the stand-ins, the buffers, the constant drivers and their routing are removed.
   No bitstream is written for a partition. Give the clocks constraints (`--sdc`, on the port nets), since
   the partition's own timing is optimised only here.

2. Instantiate the partition as a blackbox in the design. Synthesis does not know which ports of a blackbox are
   clocks, so mark them, or no global buffer goes on a clock that only the partition uses. Blackbox modules need
   `=` in the selection:

   ```
   read_verilog top.v
   blackbox core
   setattr -set clkbuf_sink 1 =core/w:clk
   synth_gatemate -top top
   ```

   Then import it with `--vopt partition=<instance>:<file>[@dx,dy]`, once per instance:

   ```
   nextpnr-himbaechel --device CCGM1A1 --json top.json --vopt ccf=board.ccf \
       --vopt partition=u_core:core_pnr.json \
       --vopt partition=u_core2:core_pnr.json@0,-32 --vopt out=top.txt
   ```

   The instance is replaced with the partition's cells, named `<instance>.<cell>`. The ports join the nets on
   the instance's ports, and those nets are routed together with the rest of the design. That includes the
   clocks and constants: the global buffers, the constant drivers and the top-level IOs belong to the importing
   design. Ports that are one net inside the partition (two outputs of the same signal, or a feedthrough) are
   made one net outside too. Ports left unconnected on the instance stay unconnected.

## Moving

`@dx,dy` moves the partition by dx tiles in X and dy tiles in Y (the coordinates of the bel names, `X..Y..`).
Every cell has to land on the same kind of tile it came from, so the same resource is used there. Otherwise the
import stops and lists the cells that would not fit. A net whose pips land on another kind of tile is routed
again, and the log reports how many there are (`--verbose` names them and gives the reason). In practice:

- CPE logic: dx + dy must be a multiple of 4, because the big and small switch boxes alternate diagonally.
  Examples are (4, 0), (0, 4), (2, 2), (1, 3), (3, -3) and (4, 8).
- A partition with cells in the RAM columns (X35, X67, X99 and X131 on the CCGM1A1, which includes the CPEs
  feeding a RAM): dx must be a multiple of 32 and dy a multiple of 16. This also holds when the placer only put
  some CPE logic there.
- A partition has to stay on one die.

## What the importing design keeps out of

- The rectangle of each partition: its area, or where its cells are if they reach further. The placer puts no
  cell there except cells fixed by packing (the CPEs next to the IO pads), because a cell in a gap would be
  closed in by the partition's locked routing. A compact area wastes the least.
- The RAM sites whose CPEs fall into that rectangle, even those the partition does not use. A RAM uses CPEs
  from 6 tiles left to 4 tiles right of its column, so a partition area within that distance of a RAM column
  takes the RAM sites along it. Keep partitions without RAMs away from the RAM columns, or the rest of the
  design may run out of RAM sites: on the dmg_soc (23 of 32 sites used), a CPU partition next to X35 took the
  six sites there, and moving the other partition next to X99 then left too few for the rest.
- The CPEs next to the IOs and the clocking (whichever the design uses): a partition never places cells on
  them or routes into them, but placing its area along the IO ring still narrows the routing there.

With `--vopt partition-open` the design may also place cells in the free tiles of a partition's rectangle, but
only where the partition's locked routing leaves a way into every CPE input there: a flood over the free wires,
starting from the outputs of all bels outside the partitions' cells, has to reach each input. Without it, a
partition without region closes most of the device. With it, on the dmg_soc, a partition of everything but
the pixel pipeline left 10041 of 10402 tiles of its rectangle open; the closed ones hold bridges of its
routing. Multipliers never go into tiles that the locked routing of a partition passes through, since their
fixed routing could not get through there.

## Timing

The connections into and out of a partition are routed like any other, close to their best: on the dmg_soc
the two on the critical path that crossed the CPU partition's edge took 0.19 ns more than on the empty chip.
What a partition costs is placement. In step 1 its cells are placed without knowing where the logic they
connect to will be, and the design around keeps out of its rectangle. On the dmg_soc, with the CPU and the
pixel pipeline as partitions, the 33 MHz clock went from 31.1 MHz (flat) to about 25 MHz, the critical path
being the same one with longer connections around the CPU. Floorplan partitions where their neighbours will
be, and register the signals that cross their edges where timing matters.

In step 1 a port's stand-in is not clocked, so paths that end or start at a port are not timed there. Logic
in the partition that feeds an IO register of the design is then placed and sized as if it had all the time,
and far from the IO. On the dmg_soc, with everything but the pixel pipeline as one partition without region,
step 1 reached 30.7 MHz (flat 31.1), but in the whole design the critical path became one from a RAM in the
partition, 15.8 ns through its logic, to a DDR output register of the HyperBus PHY on the falling edge (half
a period), with 4.7 ns from the partition to the IO: 21.4 MHz. Keep logic that feeds IO registers out of
partitions, or register it at the partition's edge.

## Limits

- Partitions have no inout ports, no IO buffers of their own and no PLLs. Up to 4 clock inputs are allowed,
  and each must be a global clock in the importing design. RAMs of the partition are reconnected to the
  global clock lines the importing design uses for those clocks.
- The timing inside a partition is optimised only when the partition is placed and routed on its own. Moving it
  changes only the connections to the rest of the design.
- Loading a placed and routed design to place and route it again is not supported. Implement it as a partition
  instead.
