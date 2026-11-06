# Partitions: placing and routing a design in parts

A partition is a module placed and routed on its own. Another design can then use it as a finished block. The
partition's cells and the routing inside it are taken over, optionally moved by whole tiles. The placer keeps
them where they are and places only the rest of the design. After that, by default, the partition is part of the
design like any other logic: the passes after placement (the placement and routing repairs, LUT input swaps,
driver replication) and the router may change its cells and routing too, where that helps. How much of it is
taken over and what may change can be set per partition (see Options). One partition file can be used several
times, at different places.

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

   or, without a CCF, with `--vopt partition-region=X0,Y0,X1,Y1` in bel coordinates (`X..Y..` of the bel names),
   for example `partition-region=10,10,21,21` (or `X10,Y10,X21,Y21`), which takes all cells the CCF does not
   floorplan.

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

## In the GUI

The Partitions tab beside the design view sets all of this up, before packing (the options are set when Pack is
pressed; those given on the command line are filled in at the start):

- Implement this design as a partition: placed only or not, and the region, dragged open in the device view with
  Pick. After Finish (or after Place, placed only), Save JSON writes the partition.
- Import partitions: per blackbox instance of the loaded design, the file, the offset and the options load, place
  and route, and `partition-open` for all. The device view shows where each partition goes, its area and the
  tiles of its cells, in red where it can not go. With Pick, the partition follows the mouse, snapped to the
  nearest offset it can be moved by without landing on the cells of another partition; a click puts it there.
  Once packed, the cells of each partition are highlighted in its colour.

## Options

Each partition takes options as `,key=value` after the file (and the offset). `--vopt partition-<key>=<value>`
sets them for all partitions, and the options of one partition override that:

```
--vopt partition=u_core:core_pnr.json@4,0,place=locked,route=repair
--vopt partition=u_gpu:gpu_pnr.json,load=placement --vopt partition-route=locked
```

| Key | Values | |
|---|---|---|
| `load` | `routing` (default), `placement` | `placement` takes only the placement from the file; the partition's nets are routed with the design. A file written with `partition-unrouted` has no routing, so `placement` is the default there and `routing` is an error. |
| `place` | `repair` (default), `locked` | The placer always keeps the cells where they are. With `repair` the passes after placement may move them (as far as the clusters allow), with `locked` they stay where they are. |
| `route` | `open` (default), `repair`, `locked` | `open`: the routing is the router's starting point; it keeps what does not conflict, and other nets may pass through the partition's wires, which the router then negotiates. `repair`: locked while routing (no other net uses its wires, the router does not change it), then open to the repairs after routing. `locked`: stays as loaded; the LUT input swaps and driver replication before routing leave the cells on such nets alone too. |

The fixed routing of the partition's multipliers stays locked with every option.

On the dmg_soc CPU partition (same step 1 for all), step 2 gave:

| Options | clk0 | Router | Whole run | Partition pips changed |
|---|---|---|---|---|
| `place=locked,route=locked` | 23.13 MHz | 51 s, 155 iterations | 270 s | none |
| `route=repair` | 24.44 MHz | 47 s, 155 iterations | 412 s | 1 % |
| `route=open` | 24.50 MHz | 46 s, 94 iterations | 800 s | 29 % |
| `load=placement` | 24.75 MHz | 59 s, 221 iterations | 503 s | (all routed again) |

The router takes about the same time with every option; the time goes into the repairs after routing, which
have more to work on the more of the partition they may change.

## Placed only

With `--vopt partition-unrouted` (which implies `as-partition`) and `--no-route`, step 1 stops after placing and
writes the partition without routing. The stand-ins, buffers and constant drivers are removed after placing
instead of after routing. Run with routing, step 1 stops with an error. On import, such a partition brings only
its placement, and all of its nets are routed with the design. Moving, the keep-out while placing and
`partition-open` work as for a routed partition. Without routing of its own, nothing of the partition closes in
the free tiles of its area, so `partition-open` opens all of them.

Step 1 is faster and the router is free to route the partition around the logic of the importing design. The
partition's internal timing is then only estimated in step 1, and step 2 routes more. On the dmg_soc CPU
partition, step 1 took 60 s, and step 2 reached 24.75 MHz on clk0, about the same as with the routed partition
(see Options). `load=placement` does the same with a routed file.

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

- While placing, the rectangle of each partition: its area, or where its cells are if they reach further. The
  placer puts no cell there except cells fixed by packing (the CPEs next to the IO pads), because a cell in a
  gap would have the router take the partition's routing apart. A compact area wastes the least. The passes
  after placement may use the free places there.
- The RAM sites whose CPEs fall into that rectangle, even those the partition does not use. A RAM uses CPEs
  from 6 tiles left to 4 tiles right of its column, so a partition area within that distance of a RAM column
  takes the RAM sites along it. Keep partitions without RAMs away from the RAM columns, or the rest of the
  design may run out of RAM sites: on the dmg_soc (23 of 32 sites used), a CPU partition next to X35 took the
  six sites there, and moving the other partition next to X99 then left too few for the rest.
- The CPEs next to the IOs and the clocking (whichever the design uses): a partition never places cells on
  them or routes into them, but placing its area along the IO ring still narrows the routing there.

With `--vopt partition-open` the placer may also put cells in the free tiles of a partition's rectangle, but
only where the partition's routing leaves a way into every CPE input there: a flood over the free wires,
starting from the outputs of all bels outside the partitions' cells, has to reach each input. Without it, a
partition without region closes most of the device. With it, on the dmg_soc, a partition of everything but
the pixel pipeline left 10041 of 10402 tiles of its rectangle open; the closed ones hold bridges of its
routing. Multipliers never go into tiles that the routing of a partition passes through, since their fixed
routing could not get through there. The fixed routing of the partition's own multipliers stays locked.

## Timing

The connections into and out of a partition are routed like any other, close to their best: on the dmg_soc
the two on the critical path that crossed the CPU partition's edge took 0.19 ns more than on the empty chip.
What a partition costs is placement. In step 1 its cells are placed without knowing where the logic they
connect to will be, and the design around keeps out of its rectangle. On the dmg_soc, with the CPU and the
pixel pipeline as partitions, the 33 MHz clock went from 31.1 MHz (flat) to about 25 MHz, the critical path
being the same one with longer connections around the CPU. Floorplan partitions where their neighbours will
be, and register the signals that cross their edges where timing matters.

In step 1 the paths through the ports are timed with half a period. A port's stand-in is a register clocked
on the edge opposite to the registers the port reaches through the partition's logic (the most common clock
and edge among them, if that clock is a clock input of the partition), so the logic between the port and
those registers gets half the period, and the other half is left for what the importing design adds outside.
An output driven straight by a register (or a RAM) keeps an unclocked stand-in, as do ports reaching no
register on a clock input; paths through them are not timed in step 1. The log names how many stand-ins are
clocked (`--verbose` lists them).

Without that reserve, the logic between a port and the partition's registers was placed and routed as if it
had all the time. On the dmg_soc, with everything but the pixel pipeline as one partition without region,
step 1 reached 30.7 MHz (flat 31.1), but in the whole design the critical path became one from a RAM in the
partition, 15.8 ns through its logic, to a DDR output register of the HyperBus PHY on the falling edge (half
a period), with 4.7 ns from the partition to the IO: 21.4 MHz. Half a period is a guess: logic that feeds IO
registers on the other edge, or long paths outside, still need registers at the partition's edge.

## Limits

- Partitions have no inout ports, no IO buffers of their own and no PLLs. Up to 4 clock inputs are allowed,
  and each must be a global clock in the importing design. RAMs of the partition are reconnected to the
  global clock lines the importing design uses for those clocks.
- The placement inside a partition is made when it is placed and routed on its own; the importing design only
  repairs it (the repairs move single LUT trees and registers, not clusters such as chains or multipliers).
- The clusters of the partition (registers merged with their LUTs, chains, multipliers) are stored with it
  since the passes after placement respect them; the cells of partition files written before that stay locked.
- Loading a placed and routed design to place and route it again is not supported. Implement it as a partition
  instead.
