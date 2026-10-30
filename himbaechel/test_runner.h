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

#ifndef HIMBAECHEL_TEST_RUNNER_H
#define HIMBAECHEL_TEST_RUNNER_H

#include <functional>
#include <string>
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

// Work done once before the tests when they run in parallel (see test_main.cc), for all the processes
// that run them: `make` does the work, such as filling the caches a uarch keeps between contexts, and
// writes what the tests share to `file`; each process that runs tests then reads it with `load`.
// Declare one as a static object.
struct TestWarmUp
{
    TestWarmUp(std::function<void(const std::string &file)> make, std::function<void(const std::string &file)> load);
};

NEXTPNR_NAMESPACE_END

#endif
