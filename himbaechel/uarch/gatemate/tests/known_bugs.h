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

#ifndef GATEMATE_KNOWN_BUGS_H
#define GATEMATE_KNOWN_BUGS_H

// Known bugs that some tests deliberately steer around, so that they do not mask other problems.
//
// Every such restriction is temporary and limits coverage. Each one is guarded by the entry
// below for the bug it avoids; once a bug is fixed, delete its entry and the compiler points at
// every restriction to remove. Each bug also has dedicated tests (listed here) that fail until it
// is fixed, independent of these entries.
enum class KnownBug
{
};

// Whether tests still avoid the given bug. Kept as a function so that a restriction reads as
// `if (avoid(KnownBug::X))` at the place where coverage is limited.
constexpr bool avoid(KnownBug) { return true; }

#endif
