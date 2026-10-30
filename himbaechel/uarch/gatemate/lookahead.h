/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2024  The Project Peppercorn Authors.
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifndef GATEMATE_LOOKAHEAD_H
#define GATEMATE_LOOKAHEAD_H

#include <functional>
#include <iostream>
#include <memory>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// Delay estimates from shortest paths on the empty chip, for the router's lookahead and the placer.
//
// Routed delay grows with the number of routing muxes a connection passes, not with distance:
// neighbouring CPEs are 0.5 to 1.3 ns apart, and each further tile adds only some 35 ps. Which plane
// a signal arrives on matters too, because each CPE input belongs to one plane. So the tables hold,
// for every wire within `radius` tiles of a CPE, the least delay from that wire to each CPE input
// (IN1..IN8, CLK, EN, SR) and from each CPE output (OUT1, OUT2) to that wire. The wiring repeats
// with x mod 2 and (x + y) mod 4, so one table per such phase covers the chip. Beyond the radius,
// the value at the nearest offset of the same phase grows linearly with the remaining distance.
struct GateMateLookahead
{
    // avail filters out pips that can never be used; `key` names the chip, timing and avail, see
    // keep_built
    void build(Context *ctx, int radius, std::function<bool(PipId)> avail, const std::string &key = "");
    // Keep the tables built with a key for the rest of the process, and copy them instead of
    // building again for the same key and radius (the tests create a context for each test)
    static bool keep_built;
    // Write the kept tables to a stream, or add those of a stream to the kept tables (for the tests, which
    // build them once for all their processes)
    static void save_kept(std::ostream &out);
    static void load_kept(std::istream &in);
    bool built() const { return values && !values->empty(); }

    // Delay from src to dst if dst is a pin behind a CPE input or src a pin before a CPE output,
    // else -1
    delay_t estimate(WireId src, WireId dst) const;

  private:
    static constexpr int PHASES = 8;
    static constexpr uint16_t MISSING = 0xffff;
    // Delay of a wire that cannot reach the pin within the table
    static constexpr int UNREACHABLE = 20000;

    struct Hop
    {
        int anchor, delay;
    };
    // Per tile type and wire: the anchors of its CPE that reach it or that it reaches, with the delay
    struct Hops
    {
        std::vector<int> start; // wire index -> first hop, size wires + 1
        std::vector<Hop> hops;
    };

    Context *ctx = nullptr;
    int radius = 0, span = 0;
    std::vector<IdString> anchor_names; // CPE inputs first, then outputs
    int inputs = 0;
    std::vector<Hops> to_pin;   // per tile type: anchor (input) -> pin
    std::vector<Hops> from_pin; // per tile type: pin -> anchor (output)
    // Per tile type, between any two wires of the CPE: the wires between an anchor and a pin are
    // not in the tables
    struct Inside
    {
        std::vector<int> id; // wire index -> row, or -1 outside the CPE
        int size = 0;
        std::vector<int> delay; // size * size, -1 if unreachable
    };
    std::vector<Inside> inside;

    // Slot (phase, anchor, offset) holds the delays of all wires of tile type slot_type[(phase, offset)]
    std::vector<int> slot_type;
    std::vector<int64_t> slot_base;
    // (shared by the copies of the tables, see keep_built)
    std::shared_ptr<std::vector<uint16_t>> values;
    // remap[from_type][to_type][wire]: the wire of the same name in another tile type, or -1
    std::vector<std::vector<std::vector<int>>> remap;
    float slope_x = 0, slope_y = 0; // ps per tile beyond the radius

    int phase(int x, int y) const { return (x & 1) * 4 + ((x + y) & 3); }
    // Delay between anchor k of the CPE in anchor_tile and wire w, or -1 if unknown
    int lookup(int k, int anchor_tile, WireId w) const;
};

NEXTPNR_NAMESPACE_END

#endif
