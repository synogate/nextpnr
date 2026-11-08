/*
 *  nextpnr -- Next Generation Place and Route
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

#ifndef AUTO_NAMES_H
#define AUTO_NAMES_H

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// Give the cells that synthesis named automatically ("$abc$123$...", "$auto$ff.cc:...") the name of the net they
// drive, and name each net after the best of its aliases (HDL signal names before names a tool generated, like
// GHDL's "n1234_o"). A cell that drives only nets without a public name (inside an abc cone, a carry chain) is named
// after the cell it feeds: "<sink>$<sink port>". Run by the frontend right after the netlist is imported, unless the
// setting frontend/renameAutoCells is false (--no-rename-auto-cells).
void rename_auto_cells(Context *ctx);

NEXTPNR_NAMESPACE_END

#endif
