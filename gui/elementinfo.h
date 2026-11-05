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

#ifndef ELEMENTINFO_H
#define ELEMENTINFO_H

#include <string>

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// Human-readable descriptions of chip elements and what is bound to them, for tooltips, menus
// and the status bar. Names are always available; the binding state is read only when withState
// is set (the caller must make sure the design is not being changed meanwhile, e.g. by holding
// the context mutex). multiline gives one fact per line (tooltips), otherwise a single line.
namespace ElementInfo {

const char *strengthName(PlaceStrength strength);

std::string belText(const Context *ctx, BelId bel, bool withState, bool multiline);
std::string wireText(const Context *ctx, WireId wire, bool withState, bool multiline);
std::string pipText(const Context *ctx, PipId pip, bool withState, bool multiline);
std::string groupText(const Context *ctx, GroupId group, bool multiline);

} // namespace ElementInfo

NEXTPNR_NAMESPACE_END

#endif // ELEMENTINFO_H
