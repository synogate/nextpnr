/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2020  gatecat <gatecat@ds0.me>
 *  Copyright (C) 2024  rowanG077 <goemansrowan@gmail.com>
 *
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

#include "log.h"
#include "nextpnr.h"

#include <algorithm>
#include <iterator>

NEXTPNR_NAMESPACE_BEGIN

struct SdcEntity
{
    enum EntityType
    {
        ENTITY_CELL,
        ENTITY_PORT,
        ENTITY_NET,
        ENTITY_PIN,
        ENTITY_CLOCK, // by its name (ClockConstraint::name or the clock net's name)
    } type;
    IdString name;
    IdString pin; // for cell pins only

    SdcEntity(EntityType type, IdString name) : type(type), name(name) {}
    SdcEntity(EntityType type, IdString name, IdString pin) : type(type), name(name), pin(pin) {}

    const std::string &to_string(Context *ctx) { return name.str(ctx); }

    CellInfo *get_cell(Context *ctx) const
    {
        if (type != ENTITY_CELL)
            return nullptr;
        return ctx->cells.at(name).get();
    }

    PortInfo *get_port(Context *ctx) const
    {
        if (type != ENTITY_PORT)
            return nullptr;
        return &ctx->ports.at(name);
    }

    NetInfo *get_net(Context *ctx) const
    {
        if (type == ENTITY_PIN) {
            CellInfo *cell = nullptr;
            if (ctx->cells.count(name)) {
                cell = ctx->cells.at(name).get();
            } else {
                return nullptr;
            }
            if (!cell->ports.count(pin))
                return nullptr;
            return cell->ports.at(pin).net;
        } else if (type == ENTITY_NET) {
            return ctx->nets.at(name).get();
        } else {
            return nullptr;
        }
    }
};

struct SdcValue
{
    SdcValue(const std::string &s) : is_string(true), str(s) {};
    SdcValue(const std::vector<SdcEntity> &l) : is_string(false), list(l) {};

    bool is_string;
    std::string str;             // simple string value
    std::vector<SdcEntity> list; // list of entities
};

// Whether a name matches a pattern with the wildcards * (any characters) and ? (one character) of get_cells and
// the like; brackets are bus indices, not character classes
static bool sdc_match(const char *pattern, const char *name)
{
    const char *star = nullptr, *resume = nullptr;
    while (*name) {
        if (*pattern == '*') {
            star = pattern++;
            resume = name;
        } else if (*pattern == '?' || *pattern == *name) {
            pattern++;
            name++;
        } else if (star) {
            pattern = star + 1;
            name = ++resume;
        } else {
            return false;
        }
    }
    while (*pattern == '*')
        pattern++;
    return !*pattern;
}

static bool sdc_is_pattern(const std::string &s) { return s.find_first_of("*?") != std::string::npos; }

struct SDCParser
{
    std::string buf;
    int pos = 0;
    int lineno = 1;
    // The text of the command being evaluated (for the timing exceptions that keep it)
    std::string command;
    Context *ctx;

    SDCParser(const std::string &buf, Context *ctx) : buf(buf), ctx(ctx) {};

    inline bool eof() const { return pos == int(buf.size()); }

    inline char peek() const { return buf.at(pos); }

    inline char get()
    {
        char c = buf.at(pos++);
        if (c == '\n')
            ++lineno;
        return c;
    }

    std::string get(int n)
    {
        std::string s = buf.substr(pos, n);
        pos += n;
        return s;
    }

    // If next char matches c, take it from the stream and return true
    bool check_get(char c)
    {
        if (peek() == c) {
            get();
            return true;
        } else {
            return false;
        }
    }

    // If next char matches any in chars, take it from the stream and return true
    bool check_get_any(const std::string &chrs)
    {
        char c = peek();
        if (chrs.find(c) != std::string::npos) {
            get();
            return true;
        } else {
            return false;
        }
    }

    inline void skip_blank(bool nl = false)
    {
        while (!eof() && check_get_any(nl ? " \t\n\r" : " \t"))
            ;
    }

    // Return true if end of line (or file)
    inline bool skip_check_eol()
    {
        skip_blank(false);
        if (eof())
            return true;
        char c = peek();
        // Comments count as end of line
        if (c == '#') {
            get();
            while (!eof() && peek() != '\n' && peek() != '\r')
                get();
            return true;
        }
        if (c == ';') {
            // Forced end of line
            get();
            return true;
        }
        return (c == '\n' || c == '\r');
    }

    inline std::string get_str()
    {
        std::string s;
        skip_blank(false);
        if (eof())
            return "";

        bool in_quotes = false, in_braces = false, escaped = false;

        char c = get();

        if (c == '"')
            in_quotes = true;
        else if (c == '{')
            in_braces = true;
        else
            s += c;

        while (true) {
            if (eof()) {
                if (in_quotes || in_braces || escaped)
                    log_error("EOF while parsing string '%s'\n", s.c_str());
                else
                    break;
            }

            char c = peek();
            if (!in_quotes && !in_braces && !escaped && (std::isblank(c) || c == ']')) {
                break;
            }
            get();
            if (escaped) {
                s += c;
                escaped = false;
            } else if ((in_quotes && c == '"') || (in_braces && c == '}')) {
                break;
            } else if (c == '\\') {
                escaped = true;
            } else {
                s += c;
            }
        }

        return s;
    }

    SdcValue evaluate(const std::vector<SdcValue> &arguments)
    {
        NPNR_ASSERT(!arguments.empty());
        auto &arg0 = arguments.at(0);
        NPNR_ASSERT(arg0.is_string);
        const std::string &cmd = arg0.str;
        if (cmd == "get_ports")
            return cmd_get_ports(arguments);
        else if (cmd == "get_cells")
            return cmd_get_cells(arguments);
        else if (cmd == "get_nets")
            return cmd_get_nets(arguments);
        else if (cmd == "get_pins")
            return cmd_get_pins(arguments);
        else if (cmd == "create_clock")
            return cmd_create_clock(arguments);
        else if (cmd == "get_clocks")
            return cmd_get_clocks(arguments);
        else if (cmd == "set_false_path")
            return cmd_timing_exception(arguments, TimingException::FALSE_PATH);
        else if (cmd == "set_multicycle_path")
            return cmd_timing_exception(arguments, TimingException::MULTICYCLE);
        else
            log_error("Unsupported SDC command '%s'\n", cmd.c_str());
    }

    std::vector<SdcValue> get_arguments()
    {
        std::vector<SdcValue> args;
        while (!skip_check_eol()) {
            if (check_get('[')) {
                // Start of a sub-expression
                auto result = evaluate(get_arguments());
                NPNR_ASSERT(check_get(']'));
                args.push_back(result);
            } else if (peek() == ']') {
                break;
            } else {
                args.push_back(get_str());
            }
        }
        skip_blank(true);
        return args;
    }

    SdcValue cmd_get_nets(const std::vector<SdcValue> &arguments)
    {
        std::vector<SdcEntity> nets;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_nets expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_nets (line %d)\n", s.c_str(), lineno);
            if (sdc_is_pattern(s)) {
                // (nets by their aliases too, each once)
                pool<IdString> found;
                for (auto &alias : ctx->net_aliases)
                    if (sdc_match(s.c_str(), alias.first.c_str(ctx)) && found.insert(alias.second).second)
                        nets.emplace_back(SdcEntity::ENTITY_NET, alias.second);
                for (auto &net : ctx->nets)
                    if (sdc_match(s.c_str(), net.first.c_str(ctx)) && found.insert(net.first).second)
                        nets.emplace_back(SdcEntity::ENTITY_NET, net.first);
                if (found.empty())
                    log_warning("get_nets argument '%s' matched no objects.\n", s.c_str());
                continue;
            }
            IdString id = ctx->id(s);
            if (ctx->nets.count(id) || ctx->net_aliases.count(id))
                nets.emplace_back(SdcEntity::ENTITY_NET, ctx->net_aliases.count(id) ? ctx->net_aliases.at(id) : id);
            else
                log_warning("get_nets argument '%s' matched no objects.\n", s.c_str());
        }
        return nets;
    }

    SdcValue cmd_get_clocks(const std::vector<SdcValue> &arguments)
    {
        // The clocks given so far: the names of the clock constraints and of the nets they are on
        pool<IdString> names;
        for (auto &net : ctx->nets)
            if (net.second->clkconstr) {
                names.insert(net.first);
                if (net.second->clkconstr->name != IdString())
                    names.insert(net.second->clkconstr->name);
            }
        std::vector<SdcEntity> clocks;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_clocks expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_clocks (line %d)\n", s.c_str(), lineno);
            bool found = false;
            for (IdString name : names)
                if (sdc_is_pattern(s) ? sdc_match(s.c_str(), name.c_str(ctx)) : name == ctx->id(s)) {
                    clocks.emplace_back(SdcEntity::ENTITY_CLOCK, name);
                    found = true;
                }
            if (!found)
                log_warning("get_clocks argument '%s' matched no clock (line %d).\n", s.c_str(), lineno);
        }
        return clocks;
    }

    SdcValue cmd_get_ports(const std::vector<SdcValue> &arguments)
    {
        std::vector<SdcEntity> ports;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_ports expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_ports (line %d)\n", s.c_str(), lineno);
            if (sdc_is_pattern(s)) {
                for (auto &port : ctx->ports)
                    if (sdc_match(s.c_str(), port.first.c_str(ctx)))
                        ports.emplace_back(SdcEntity::ENTITY_PORT, port.first);
                continue;
            }
            IdString id = ctx->id(s);
            if (ctx->ports.count(id))
                ports.emplace_back(SdcEntity::ENTITY_PORT, id);
        }
        return ports;
    }

    SdcValue cmd_get_cells(const std::vector<SdcValue> &arguments)
    {
        std::vector<SdcEntity> cells;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_cells expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_cells (line %d)\n", s.c_str(), lineno);
            if (sdc_is_pattern(s)) {
                size_t before = cells.size();
                for (auto &cell : ctx->cells)
                    if (sdc_match(s.c_str(), cell.first.c_str(ctx)))
                        cells.emplace_back(SdcEntity::ENTITY_CELL, cell.first);
                if (cells.size() == before)
                    log_warning("get_cells argument '%s' matched no objects.\n", s.c_str());
                continue;
            }
            IdString id = ctx->id(s);
            if (ctx->cells.count(id))
                cells.emplace_back(SdcEntity::ENTITY_CELL, id);
            else
                log_warning("get_cells argument '%s' matched no objects.\n", s.c_str());
        }
        return cells;
    }

    SdcValue cmd_get_pins(const std::vector<SdcValue> &arguments)
    {
        std::vector<SdcEntity> pins;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_pins expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_pins (line %d)\n", s.c_str(), lineno);
            auto pos = s.rfind('/');
            if (pos == std::string::npos)
                log_error("expected / in cell pin name '%s' (line %d)\n", s.c_str(), lineno);
            if (sdc_is_pattern(s)) {
                std::string cell_pattern = s.substr(0, pos), pin_pattern = s.substr(pos + 1);
                size_t before = pins.size();
                for (auto &cell : ctx->cells) {
                    if (!sdc_match(cell_pattern.c_str(), cell.first.c_str(ctx)))
                        continue;
                    for (auto &port : cell.second->ports)
                        if (port.second.net != nullptr && sdc_match(pin_pattern.c_str(), port.first.c_str(ctx)))
                            pins.emplace_back(SdcEntity::ENTITY_PIN, cell.first, port.first);
                }
                if (pins.size() == before)
                    log_warning("get_pins argument '%s' matched no objects.\n", s.c_str());
                continue;
            }
            pins.emplace_back(SdcEntity::ENTITY_PIN, ctx->id(s.substr(0, pos)), ctx->id(s.substr(pos + 1)));
            if (pins.back().get_net(ctx) == nullptr) {
                log_warning("cell pin '%s' not found\n", s.c_str());
                pins.pop_back();
            }
        }
        return pins;
    }

    SdcValue cmd_create_clock(const std::vector<SdcValue> &arguments)
    {
        float period = 10;
        std::string name;
        std::vector<NetInfo *> nets;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (arg.is_string) {
                std::string s = arg.str;
                if (s == "-period") {
                    i++;
                    auto &val = arguments.at(i);
                    if (!val.is_string)
                        log_error("expecting string argument to -period (line %d)\n", lineno);
                    try {
                        period = std::stof(val.str);
                    } catch (std::exception &e) {
                        log_error("invalid argument '%s' to -period (line %d)\n", val.str.c_str(), lineno);
                    }
                } else if (s == "-name") {
                    i++;
                    if (i >= int(arguments.size()) || !arguments.at(i).is_string)
                        log_error("expecting string argument to -name (line %d)\n", lineno);
                    name = arguments.at(i).str;
                } else {
                    log_error("unsupported argument '%s' to create_clock\n", s.c_str());
                }
            } else {
                for (const auto &ety : arg.list) {
                    NetInfo *net = nullptr;
                    if (ety.type == SdcEntity::ENTITY_PIN)
                        net = ety.get_net(ctx);
                    else if (ety.type == SdcEntity::ENTITY_NET)
                        net = ctx->nets.at(ety.name).get();
                    else if (ety.type == SdcEntity::ENTITY_PORT)
                        net = ctx->ports.at(ety.name).net;
                    else
                        log_error("create_clock applies only to cells, cell pins, or IO ports (line %d)\n", lineno);
                    if (net != nullptr)
                        nets.push_back(net);
                }
            }
        }
        for (NetInfo *net : nets) {
            ctx->addClock(net->name, 1000.0f / period);
            if (!name.empty() && net->clkconstr)
                net->clkconstr->name = ctx->id(name);
        }
        return std::string{};
    }

    // set_multicycle_path <multiplier> [-setup | -hold] [-start | -end] [-from <objects>] [-to <objects>]
    // set_false_path [-setup | -hold] [-from <objects>] [-to <objects>]
    SdcValue cmd_timing_exception(const std::vector<SdcValue> &arguments, TimingException::Type type)
    {
        const char *cmd = type == TimingException::MULTICYCLE ? "set_multicycle_path" : "set_false_path";
        TimingException exc;
        exc.type = type;
        exc.source = command;
        exc.line = lineno;
        bool have_multiplier = false, have_setup = false, have_hold = false, have_start = false, have_end = false;
        bool have_from = false, have_to = false;
        auto points = [&](const SdcValue &val, const char *option) {
            if (val.is_string)
                log_error("%s %s expects objects of get_cells, get_pins, get_nets, get_ports or get_clocks, not '%s' "
                          "(line %d)\n",
                          cmd, option, val.str.c_str(), lineno);
            std::vector<TimingPathPoint> result;
            for (auto &ety : val.list) {
                TimingPathPoint p;
                p.name = ety.name;
                switch (ety.type) {
                case SdcEntity::ENTITY_CELL:
                    p.type = TimingPathPoint::CELL;
                    break;
                case SdcEntity::ENTITY_PIN:
                    p.type = TimingPathPoint::PIN;
                    p.pin = ety.pin;
                    break;
                case SdcEntity::ENTITY_NET:
                    p.type = TimingPathPoint::NET;
                    break;
                case SdcEntity::ENTITY_PORT: {
                    // (the net of the port)
                    NetInfo *net = ctx->ports.at(ety.name).net;
                    if (net == nullptr)
                        continue;
                    p.type = TimingPathPoint::NET;
                    p.name = net->name;
                    break;
                }
                case SdcEntity::ENTITY_CLOCK:
                    p.type = TimingPathPoint::CLOCK;
                    break;
                }
                result.push_back(p);
            }
            return result;
        };
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("unexpected objects in %s, expecting -from or -to before them (line %d)\n", cmd, lineno);
            const std::string &s = arg.str;
            if (s == "-setup") {
                have_setup = true;
            } else if (s == "-hold") {
                have_hold = true;
            } else if (s == "-start" && type == TimingException::MULTICYCLE) {
                have_start = true;
            } else if (s == "-end" && type == TimingException::MULTICYCLE) {
                have_end = true;
            } else if (s == "-from" || s == "-to") {
                if (++i >= int(arguments.size()))
                    log_error("missing objects after %s in %s (line %d)\n", s.c_str(), cmd, lineno);
                auto &list = s == "-from" ? exc.from : exc.to;
                auto more = points(arguments.at(i), s.c_str());
                list.insert(list.end(), more.begin(), more.end());
                (s == "-from" ? have_from : have_to) = true;
            } else if (s.at(0) == '-' || type != TimingException::MULTICYCLE) {
                log_error("unsupported argument '%s' to %s (line %d)\n", s.c_str(), cmd, lineno);
            } else {
                if (have_multiplier)
                    log_error("%s expects one multiplier, got '%s' too (line %d)\n", cmd, s.c_str(), lineno);
                try {
                    size_t used = 0;
                    exc.multiplier = std::stoi(s, &used);
                    if (used != s.size())
                        throw std::invalid_argument(s);
                } catch (std::exception &e) {
                    log_error("invalid multiplier '%s' to %s (line %d)\n", s.c_str(), cmd, lineno);
                }
                have_multiplier = true;
            }
        }
        if (type == TimingException::MULTICYCLE && !have_multiplier)
            log_error("set_multicycle_path needs a multiplier (line %d)\n", lineno);
        if (have_setup && have_hold)
            log_error("%s takes -setup or -hold, not both (line %d)\n", cmd, lineno);
        if (have_start && have_end)
            log_error("set_multicycle_path takes -start or -end, not both (line %d)\n", lineno);
        if (type == TimingException::MULTICYCLE) {
            exc.hold = have_hold;
            exc.setup = !have_hold;
            if (exc.multiplier < (exc.hold ? 0 : 1))
                log_error("set_multicycle_path multiplier %d is too small for %s (line %d)\n", exc.multiplier,
                          exc.hold ? "hold" : "setup", lineno);
            exc.start = have_start || (exc.hold && !have_end);
        } else {
            exc.setup = !have_hold;
            exc.hold = !have_setup;
        }
        // A list given that matched nothing must not stand for every path
        if ((have_from && exc.from.empty()) || (have_to && exc.to.empty())) {
            log_warning("%s: the objects of -%s matched nothing, constraint ignored (line %d)\n", cmd,
                        (have_from && exc.from.empty()) ? "from" : "to", lineno);
            return std::string{};
        }
        if (!have_from && !have_to)
            log_warning("%s without -from or -to applies to every path (line %d)\n", cmd, lineno);
        if (type == TimingException::MULTICYCLE)
            log_info("Multicycle path (line %d): %s %d (-%s), from %d object(s), to %d object(s)\n", lineno,
                     exc.hold ? "hold" : "setup", exc.multiplier, exc.start ? "start" : "end", int(exc.from.size()),
                     int(exc.to.size()));
        else
            log_warning("False path (line %d), from %d object(s), to %d object(s): not applied by the timing "
                        "analysis yet (the GUI lists the paths it covers)\n",
                        lineno, int(exc.from.size()), int(exc.to.size()));
        ctx->timing_exceptions.push_back(exc);
        return std::string{};
    }

    // The text of a command from the start of its line to its end, without a comment after it
    std::string command_text(int start, int end)
    {
        std::string text = buf.substr(start, end - start);
        int depth = 0;
        bool quoted = false;
        for (size_t i = 0; i < text.size(); i++) {
            char c = text.at(i);
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                quoted = !quoted;
            } else if (!quoted && (c == '{' || c == '[')) {
                depth++;
            } else if (!quoted && (c == '}' || c == ']')) {
                depth--;
            } else if (!quoted && depth <= 0 && (c == '#' || c == ';')) {
                text.resize(i);
                break;
            }
        }
        size_t last = text.find_last_not_of(" \t\r\n");
        text.resize(last == std::string::npos ? 0 : last + 1);
        return text;
    }

    void operator()()
    {
        while (!eof()) {
            skip_blank(true);
            int line = lineno, start = pos;
            auto args = get_arguments();
            if (args.empty())
                continue;
            command = command_text(start, pos);
            // (messages of the command name its line, not the next one get_arguments went on to)
            int next_line = lineno;
            lineno = line;
            evaluate(args);
            lineno = next_line;
        }
    }
};

void Context::read_sdc(std::istream &in)
{
    std::string buf(std::istreambuf_iterator<char>(in), {});
    SDCParser(buf, getCtx())();
}

NEXTPNR_NAMESPACE_END
