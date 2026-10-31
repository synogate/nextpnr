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

// The tools of the MCP server (see mcpserver.h): an assistant works in the GUI's Python console,
// runs the flow steps, and sees and sets what the user sees.

#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include "basewindow.h"
#include "designwidget.h"
#include "fpgaviewwidget.h"
#include "log.h"
#include "pythontab.h"
#include "timing.h"

#include <algorithm>
#include <climits>

NEXTPNR_NAMESPACE_BEGIN

namespace {

const char *mcp_instructions =
        R"(This is the nextpnr GUI the user has open, shared with you: the user watches the device view while
you work, and every change you make to the design shows there at once. You place and route by hand
together: the user says what they want, often pointing at things selected in the GUI, and you change
the design accordingly.

- python runs code in the GUI's Python console (the user sees it there, with its output). `ctx` is
  the design's Context, the nextpnr module is imported (STRENGTH_USER and the other names are
  global), and variables stay from one call to the next. Bels, wires and pips go in and come out as
  name strings (e.g. ctx.getBelLocation(name), ctx.getPipsDownhill(wire)). ctx.cells[name] and
  ctx.nets[name] are cells and nets; iterating over them gives pairs, with the name in .first and
  the cell or net in .second. dir() and help() show what there is.
- Bind what you place or route by hand with STRENGTH_USER, so that the placer and router keep it:
  ctx.bindBel(bel, ctx.cells[name], STRENGTH_USER); a route is the source wire bound with
  ctx.bindWire(wire, net, STRENGTH_USER), then each pip toward the sinks with
  ctx.bindPip(pip, net, STRENGTH_USER). ctx.unbindBel/unbindWire/unbindPip undo them.
- run_step runs a step of the flow as the toolbar does (Pack, Place, Route and the architecture's
  own); the user sees it run. Do not call ctx.pack(), ctx.place() or ctx.route() in python.
- get_selection tells what the user has selected ("this bel", "that net"); select, highlight and
  view point the user at things; screenshot shows you the device view as the user sees it.
- options lists and sets the options of the flow (those of the Options dialog and the
  architecture's --vopt ones); save_json writes the design; timing gives Fmax, the critical paths
  and the slack histogram, or the routes of one net with their delays; find looks up cells, nets
  or bels by a regular expression.
- Python code runs on the GUI's thread: the GUI stands still until it ends. Run long searches
  (over the whole chip database, say) in a separate process of this program without the GUI
  (--device ... --run script.py), and bring the result back.
- The netlist: ctx.createCell/removeCell/renameCell, ctx.createNet/removeNet/renameNet,
  ctx.connectPort(net, cell, port)/disconnectPort(cell, port), ctx.connectPorts/movePort/copyPort
  (cell, port, other, other_port), ctx.removePort/renamePort, ctx.copyBelPorts(cell, bel); a cell
  in a cluster first out of it with ctx.removeFromCluster(cell) (where there is one).
- More in Python: ctx.getSetting/setSetting/getSettings, ctx.writeJson(file), ctx.readSdc(file),
  ctx.getNetinfoSinkWire(net, user, i), ctx.getNetinfoRouteDelay(net, user); on some
  architectures ctx.getCellCluster(cell) and ctx.getClusterChildren(root).)";

QJsonObject tool(const char *name, const char *description, QJsonObject properties = {}, QJsonArray required = {})
{
    return QJsonObject{
            {"name", name},
            {"description", description},
            {"inputSchema", QJsonObject{{"type", "object"}, {"properties", properties}, {"required", required}}},
    };
}

QJsonObject elements_schema()
{
    QJsonObject element{
            {"type", "object"},
            {"properties",
             QJsonObject{
                     {"type", QJsonObject{{"type", "string"},
                                          {"enum", QJsonArray{"bel", "wire", "pip", "cell", "net"}}}},
                     {"name", QJsonObject{{"type", "string"}}},
             }},
            {"required", QJsonArray{"type", "name"}},
    };
    return QJsonObject{{"type", "array"}, {"items", element}};
}

QJsonArray mcp_tools()
{
    return QJsonArray{
            tool("python",
                 "Run Python code in the GUI's console (the user sees it), with the Context locked; the "
                 "device view shows the changes right after. Prints and the value of a final expression "
                 "come back. Not while a flow step runs.",
                 QJsonObject{{"code", QJsonObject{{"type", "string"}, {"description", "Python statements"}}}},
                 QJsonArray{"code"}),
            tool("run_step",
                 "Run a step of the flow, as its toolbar button does (e.g. Pack, Place, Route; the "
                 "architecture may have more), and wait for it to end; returns its log. Without step: "
                 "the steps there are, and those that can run now.",
                 QJsonObject{{"step", QJsonObject{{"type", "string"}}}}),
            tool("options",
                 "The options of the flow, as the Options dialog has them (with their keys, values, "
                 "ranges and whether their step has run already); set: change some first, by key or "
                 "label (null: the default). A key that is no option of the dialog is set as the "
                 "architecture's --vopt (a string, or true for a flag; null or false unsets it). They "
                 "take effect in the steps that run after.",
                 QJsonObject{{"set", QJsonObject{{"type", "object"},
                                                 {"description", "e.g. {\"target_freq\": 50, \"as-partition\": true, "
                                                                 "\"partition-region\": \"10,10,21,21\"}"}}}}),
            tool("save_json", "Write the design, as it is now, to a JSON file (as File > Save JSON does).",
                 QJsonObject{{"path", QJsonObject{{"type", "string"}}}}, QJsonArray{"path"}),
            tool("timing",
                 "Timing analysis of the design as it is now: Fmax per clock, the critical path of each "
                 "(segment by segment) and the slack histogram. With net: the route to each of its "
                 "sinks instead, pip by pip, with the delays.",
                 QJsonObject{{"net", QJsonObject{{"type", "string"}}},
                             {"paths", QJsonObject{{"type", "boolean"}, {"description", "critical paths (true)"}}}}),
            tool("find",
                 "Cells, nets or bels whose names match a regular expression (a search anywhere in the "
                 "name), with where they are.",
                 QJsonObject{{"pattern", QJsonObject{{"type", "string"}}},
                             {"type", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"cell", "net", "bel"}}}},
                             {"limit", QJsonObject{{"type", "integer"}, {"description", "at most (200)"}}}},
                 QJsonArray{"pattern"}),
            tool("get_selection",
                 "What the user has selected in the GUI (in the device view or the design trees), with "
                 "what is bound to it."),
            tool("select", "Select these in the GUI, as the user would by clicking; zoom: show them.",
                 QJsonObject{{"items", elements_schema()}, {"zoom", QJsonObject{{"type", "boolean"}}}},
                 QJsonArray{"items"}),
            tool("highlight",
                 "Highlight these in a colour group (0 to 7), or take them out of their group (-1); "
                 "clear: all highlights first.",
                 QJsonObject{{"items", elements_schema()},
                             {"group", QJsonObject{{"type", "integer"}, {"minimum", -1}, {"maximum", 7}}},
                             {"clear", QJsonObject{{"type", "boolean"}}}},
                 QJsonArray{"items", "group"}),
            tool("view",
                 "The area the device view shows (x0, y0, x1, y1 in its coordinates); with area, show "
                 "that area first. To show elements, select them with zoom instead.",
                 QJsonObject{{"area", QJsonObject{{"type", "array"},
                                                  {"items", QJsonObject{{"type", "number"}}},
                                                  {"minItems", 4},
                                                  {"maxItems", 4}}}}),
            tool("screenshot", "An image of the device view as the user sees it now; window: of the whole GUI window.",
                 QJsonObject{{"max_size", QJsonObject{{"type", "integer"},
                                                      {"description", "longest side in pixels (1568)"}}},
                             {"window", QJsonObject{{"type", "boolean"}}}}),
    };
}

// PNG without the image plugin (that a Qt build may lack): rows unfiltered, deflated with qCompress
uint32_t crc32(const QByteArray &data)
{
    static uint32_t table[256];
    if (table[1] == 0)
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    uint32_t c = 0xffffffffu;
    for (char b : data)
        c = table[(c ^ uint8_t(b)) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

QByteArray png(const QImage &image)
{
    QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    QByteArray rows;
    for (int y = 0; y < rgb.height(); y++) {
        rows.append('\0');
        rows.append(reinterpret_cast<const char *>(rgb.constScanLine(y)), rgb.width() * 3);
    }
    auto be32 = [](QByteArray &out, uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.append(char(v >> shift));
    };
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    auto chunk = [&](const char *type, const QByteArray &data) {
        be32(out, data.size());
        QByteArray typed = QByteArray(type, 4) + data;
        out += typed;
        be32(out, crc32(typed));
    };
    QByteArray header;
    be32(header, rgb.width());
    be32(header, rgb.height());
    header.append("\x08\x02\x00\x00\x00", 5); // 8 bit RGB
    chunk("IHDR", header);
    chunk("IDAT", qCompress(rows, 6).mid(4)); // (after the length qCompress puts first: zlib)
    chunk("IEND", QByteArray());
    return out;
}

std::vector<DesignWidget::Element> elements(const QJsonArray &items, QString &error)
{
    std::vector<DesignWidget::Element> list;
    for (auto value : items) {
        QJsonObject item = value.toObject();
        ElementType type = DesignWidget::elementTypeByName(item["type"].toString());
        if (type == ElementType::NONE)
            error += "unknown type: " + item["type"].toString() + "\n";
        else
            list.push_back({type, item["name"].toString().toStdString()});
    }
    return list;
}

QString view_text(FPGAViewWidget *view)
{
    QRectF r = view->viewArea();
    return QString("The view shows x %1 to %2, y %3 to %4.")
            .arg(r.left(), 0, 'f', 3)
            .arg(r.right(), 0, 'f', 3)
            .arg(r.top(), 0, 'f', 3)
            .arg(r.bottom(), 0, 'f', 3);
}

QString missing_text(const std::vector<std::string> &missing)
{
    QString text;
    for (auto &name : missing)
        text += "not found: " + QString::fromStdString(name) + "\n";
    return text;
}

QJsonArray location(Context *ctx, BelId bel)
{
    Loc loc = ctx->getBelLocation(bel);
    return QJsonArray{loc.x, loc.y, loc.z};
}

QString ns(Context *ctx, delay_t delay) { return QString::number(ctx->getDelayNS(delay), 'f', 3); }

const char *step_name(PnrStep step)
{
    static const char *names[] = {"General", "Pack", "Place", "Route", "Repair", "Finish"};
    return names[int(step)];
}

// An option's value as text
QString option_text(const PnrOption &o, const QVariant &v)
{
    if (o.kind == PnrOption::Flag)
        return v.toBool() ? "on" : "off";
    if ((o.kind == PnrOption::Int || o.kind == PnrOption::Float) && !o.special.isEmpty() && v.toDouble() <= o.min)
        return QString("%1 (%2)").arg(v.toString(), o.special);
    if (o.kind == PnrOption::Int || o.kind == PnrOption::Float)
        return QString::number(v.toDouble(), 'g', 10);
    return '"' + v.toString() + '"';
}

// The value given for an option, as the option takes it; an invalid QVariant with the reason in error
QVariant option_value(const PnrOption &o, const QJsonValue &value, QString &error)
{
    if (value.isNull())
        return o.def;
    switch (o.kind) {
    case PnrOption::Flag:
        if (value.isBool())
            return value.toBool();
        break;
    case PnrOption::Int:
    case PnrOption::Float: {
        if (!value.isDouble())
            break;
        double v = value.toDouble();
        if (o.max > o.min && (v < o.min || v > o.max)) {
            error = QString("%1: %2 is out of %3 to %4")
                            .arg(o.key, QString::number(v, 'g', 10), QString::number(o.min, 'g', 10),
                                 QString::number(o.max, 'g', 10));
            return QVariant();
        }
        return o.kind == PnrOption::Int ? QVariant(int(v)) : QVariant(v);
    }
    case PnrOption::Choice:
        if (value.isString() && o.choices.contains(value.toString()))
            return value.toString();
        error = QString("%1: one of %2").arg(o.key, o.choices.join(", "));
        return QVariant();
    default:
        if (value.isString())
            return value.toString();
    }
    error = QString("%1: wrong type of value").arg(o.key);
    return QVariant();
}

// The route of a net to a sink wire, from its source: the pips, with their delays
QString route_text(Context *ctx, NetInfo *net, WireId sink)
{
    std::vector<PipId> pips;
    WireId wire = sink;
    for (int guard = 0; guard < 100000; guard++) {
        auto found = net->wires.find(wire);
        if (found == net->wires.end())
            return "    not routed to it\n";
        PipId pip = found->second.pip;
        if (pip == PipId())
            break;
        pips.push_back(pip);
        wire = ctx->getPipSrcWire(pip);
    }
    std::reverse(pips.begin(), pips.end());
    QString text = QString("    from %1 (wire %2 ns)\n")
                           .arg(ctx->nameOfWire(wire), ns(ctx, ctx->getWireDelay(wire).maxDelay()));
    delay_t total = ctx->getWireDelay(wire).maxDelay();
    for (PipId pip : pips) {
        WireId dst = ctx->getPipDstWire(pip);
        delay_t pip_delay = ctx->getPipDelay(pip).maxDelay(), wire_delay = ctx->getWireDelay(dst).maxDelay();
        total += pip_delay + wire_delay;
        text += QString("    %1 %2 ns, wire %3 ns, total %4%5\n")
                        .arg(ctx->nameOfPip(pip), ns(ctx, pip_delay), ns(ctx, wire_delay), ns(ctx, total),
                             ctx->isPipInverting(pip) ? " (inverting)" : "");
    }
    return text;
}

// Long output: its start and end
QString shortened(const std::string &text, size_t max = 40000)
{
    if (text.size() <= max)
        return QString::fromStdString(text);
    return QString::fromStdString(text.substr(0, max / 4)) +
           QString("\n[... %1 characters left out ...]\n").arg(text.size() - max) +
           QString::fromStdString(text.substr(text.size() - max * 3 / 4));
}

} // namespace

bool BaseMainWindow::startMcpServer(int port)
{
    if (!mcp)
        mcp = new McpServer(
                mcp_tools(), mcp_instructions,
                [this](const QString &name, const QJsonObject &args, McpServer::Reply reply) {
                    mcpCall(name, args, reply);
                },
                this);
    std::string error;
    if (!mcp->start(port, error)) {
        log_warning("The MCP server cannot listen on port %d: %s\n", port, error.c_str());
        actionMcp->setChecked(false);
        return false;
    }
    mcpPort = port;
    log_info("MCP server on http://127.0.0.1:%d/mcp; for Claude Code:\n"
             "    claude mcp add --transport http nextpnr http://127.0.0.1:%d/mcp\n",
             port, port);
    actionMcp->setChecked(true);
    actionMcp->setToolTip(QString("MCP server running on http://127.0.0.1:%1/mcp (click to stop it)").arg(port));
    return true;
}

void BaseMainWindow::toggleMcpServer(bool on)
{
    if (on && !(mcp && mcp->isRunning()))
        startMcpServer(mcpPort);
    else if (!on && mcp && mcp->isRunning()) {
        mcp->stop();
        actionMcp->setToolTip("Start the MCP server (for an assistant such as Claude Code)");
        log_info("MCP server stopped.\n");
    }
}

void BaseMainWindow::mcpTaskEnded(const char *outcome, bool ok)
{
    taskRunning = false;
    if (!mcpTaskReply)
        return;
    McpServer::Reply reply = std::move(mcpTaskReply);
    mcpTaskReply = nullptr;
    reply(McpServer::text(QString(outcome) + "\n\n" + shortened(mcpTaskLog, 12000), !ok));
}

void BaseMainWindow::mcpCall(const QString &name, const QJsonObject &args, McpServer::Reply reply)
{
    // An error in a tool is its answer (the client waits for one)
    try {
        mcpTool(name, args, reply);
    } catch (const std::exception &e) {
        reply(McpServer::text(QString("Internal error: ") + e.what(), true));
    }
}

void BaseMainWindow::mcpTool(const QString &name, const QJsonObject &args, McpServer::Reply reply)
{
    if (name == "python") {
        if (taskRunning)
            return reply(McpServer::text("A flow step is running; try again when it ended.", true));
        QString code = args["code"].toString();
        // They take the lock on the Context, which the code runs with
        static const QRegularExpression flow("\\.\\s*(pack|place|route)\\s*\\(");
        if (flow.match(code).hasMatch())
            return reply(McpServer::text("Run pack, place and route with run_step.", true));
        size_t cells = ctx->cells.size(), nets = ctx->nets.size();
        bool error = false;
        std::string out;
        {
            std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
            std::lock_guard<std::mutex> lock(ctx->mutex);
            out = console->run(code.toStdString(), error);
            // The architecture's data on cells and nets (indices, port caches) after the netlist changed
            static const QRegularExpression edits("\\b(create|remove|rename)(Cell|Net)\\b|\\b(dis)?connectPorts?\\b|"
                                                  "\\b(move|copy|remove|rename)Port\\b|\\badd(Input|Output|Inout)\\b");
            if (ctx->cells.size() != cells || ctx->nets.size() != nets || edits.match(code).hasMatch())
                ctx->assignArchInfo();
        }
        ctx->refreshUi();
        // The trees only when there are other cells or nets: rebuilding them loses the selection
        static const QRegularExpression renamed("\\brename(Cell|Net)\\s*\\(");
        if (ctx->cells.size() != cells || ctx->nets.size() != nets || renamed.match(code).hasMatch())
            Q_EMIT updateTreeView();
        return reply(McpServer::text(out.empty() ? "(no output)" : shortened(out), error));
    }

    if (name == "run_step") {
        if (taskRunning || mcpTaskReply)
            return reply(McpServer::text("A flow step is running already.", true));
        QString step = args["step"].toString();
        QAction *action = nullptr;
        QStringList all, available;
        for (auto a : menuDesign->actions()) {
            if (a->isSeparator() || a->menu() || a == actionOptions || a == actionExecutePy || a == actionMcp)
                continue;
            QString text = a->text().remove('&');
            all.append(text);
            if (a->isEnabled())
                available.append(text);
            if (text.compare(step, Qt::CaseInsensitive) == 0)
                action = a;
        }
        QString steps = available.isEmpty() ? "none" : available.join(", ");
        if (step.isEmpty())
            return reply(McpServer::text(QString("The steps: %1.\nThose that can run now: %2.%3")
                                                 .arg(all.join(", "), steps,
                                                      taskRunning ? "\nA step is running." : "")));
        if (!action || !action->isEnabled())
            return reply(McpServer::text(
                    QString("%1 cannot run now; the steps that can: %2.").arg(step, steps), true));
        mcpTaskReply = reply;
        mcpTaskLog.clear();
        int started = tasksStarted;
        action->trigger();
        // A step that did not start a task (the user said no to a dialog, say) gets an answer too
        QTimer::singleShot(2000, this, [this, started]() {
            if (mcpTaskReply && tasksStarted == started && !taskRunning)
                mcpTaskEnded("The step did not start.", false);
        });
        return;
    }

    if (name == "get_selection") {
        QJsonArray list;
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        auto net_name = [this](NetInfo *net) { return net ? QJsonValue(net->name.c_str(ctx.get())) : QJsonValue(); };
        for (auto &e : designview->selectedElements()) {
            QJsonObject item{{"type", DesignWidget::elementTypeName(e.type)}, {"name", QString::fromStdString(e.name)}};
            if (e.type == ElementType::BEL) {
                BelId bel = ctx->getBelByNameStr(e.name);
                CellInfo *cell = ctx->getBoundBelCell(bel);
                item["bel_type"] = ctx->getBelType(bel).c_str(ctx.get());
                item["location"] = location(ctx.get(), bel);
                item["cell"] = cell ? QJsonValue(cell->name.c_str(ctx.get())) : QJsonValue();
            } else if (e.type == ElementType::WIRE) {
                item["net"] = net_name(ctx->getBoundWireNet(ctx->getWireByNameStr(e.name)));
            } else if (e.type == ElementType::PIP) {
                item["net"] = net_name(ctx->getBoundPipNet(ctx->getPipByNameStr(e.name)));
            } else if (e.type == ElementType::CELL) {
                auto found = ctx->cells.find(ctx->id(e.name));
                if (found == ctx->cells.end())
                    continue;
                CellInfo *cell = found->second.get();
                item["cell_type"] = cell->type.c_str(ctx.get());
                item["bel"] = cell->bel != BelId() ? QJsonValue(ctx->nameOfBel(cell->bel)) : QJsonValue();
                if (cell->bel != BelId())
                    item["location"] = location(ctx.get(), cell->bel);
                if (cell->cluster != ClusterId())
                    item["cluster_root"] = ctx->getClusterRootCell(cell->cluster)->name.c_str(ctx.get());
            } else if (e.type == ElementType::NET) {
                auto found = ctx->nets.find(ctx->id(e.name));
                if (found == ctx->nets.end())
                    continue;
                NetInfo *net = found->second.get();
                if (net->driver.cell)
                    item["driver"] = QString("%1.%2").arg(net->driver.cell->name.c_str(ctx.get()),
                                                          net->driver.port.c_str(ctx.get()));
                item["users"] = int(net->users.entries());
                item["routed_wires"] = int(net->wires.size());
            }
            list.append(item);
        }
        return reply(McpServer::text(list.isEmpty() ? QString("Nothing is selected.")
                                                    : QString(QJsonDocument(list).toJson(QJsonDocument::Indented))));
    }

    if (name == "select" || name == "highlight") {
        QString error;
        auto list = elements(args["items"].toArray(), error);
        std::vector<std::string> missing;
        if (name == "select") {
            designview->selectElements(list, missing);
            if (args["zoom"].toBool() && list.size() > missing.size())
                fpgaView->zoomSelected();
        } else {
            designview->highlightElements(list, args["group"].toInt(-1), args["clear"].toBool(), missing);
        }
        error += missing_text(missing);
        return reply(McpServer::text(error.isEmpty() ? "Done." : error, !missing.empty() && missing.size() == list.size()));
    }

    if (name == "view") {
        QJsonArray area = args["area"].toArray();
        if (area.size() == 4)
            fpgaView->zoomToArea(area[0].toDouble(), area[1].toDouble(), area[2].toDouble(), area[3].toDouble());
        return reply(McpServer::text(view_text(fpgaView)));
    }

    if (name == "screenshot") {
        QImage image = args["window"].toBool() ? grab().toImage() : fpgaView->grabFramebuffer();
        int max = args["max_size"].toInt(1568);
        if (max > 0 && std::max(image.width(), image.height()) > max)
            image = image.scaled(max, max, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        McpServer::Result result = McpServer::text(view_text(fpgaView));
        result.content.append(QJsonObject{{"type", "image"},
                                          {"data", QString::fromLatin1(png(image).toBase64())},
                                          {"mimeType", "image/png"}});
        return reply(result);
    }

    if (name == "options") {
        if (taskRunning)
            return reply(McpServer::text("A flow step is running; try again when it ended.", true));
        std::vector<PnrOption> options = pnrOptions();
        PnrStep open = nextStep();
        auto editable = [open](const PnrOption &o) { return o.step == PnrStep::General || o.step >= open; };
        QString errors;
        QStringList changed;
        QJsonObject set = args["set"].toObject();
        for (auto it = set.begin(); it != set.end(); ++it) {
            QString key = it.key();
            auto o = std::find_if(options.begin(), options.end(), [&key](const PnrOption &o) {
                return o.key == key || o.label.compare(key, Qt::CaseInsensitive) == 0;
            });
            if (o == options.end()) {
                QString error;
                if (setArchOption(key.toStdString(), it.value(), error))
                    changed << QString("--vopt %1: %2").arg(key, it.value().isString() ? it.value().toString()
                                                                    : it.value().toBool()       ? "on"
                                                                                                : "off");
                else
                    errors += error + "\n";
                continue;
            }
            if (!editable(*o) || !o->set) {
                errors += QString("%1: cannot change any more, its step has run\n").arg(key);
                continue;
            }
            QString error;
            QVariant v = option_value(*o, it.value(), error);
            if (!v.isValid()) {
                errors += error + "\n";
                continue;
            }
            o->set(v);
            changed << QString("%1: %2").arg(o->label, option_text(*o, o->get ? o->get() : v));
        }
        if (!changed.isEmpty()) {
            optionsChanged();
            for (auto &line : changed)
                log("Option %s\n", line.toUtf8().constData());
        }
        QString text;
        if (!changed.isEmpty())
            text += "Set: " + changed.join("; ") + "\n\n";
        text += QString("The next step: %1. key = value [step, group / label]:\n").arg(step_name(open));
        for (auto &o : options) {
            text += QString("%1 = %2  [%3, %4 / %5]").arg(o.key, option_text(o, o.get ? o.get() : o.def),
                                                         step_name(o.step), o.group, o.label);
            if (o.kind == PnrOption::Choice)
                text += " one of " + o.choices.join(", ");
            else if ((o.kind == PnrOption::Int || o.kind == PnrOption::Float) && o.max > o.min)
                text += QString(" %1 to %2").arg(QString::number(o.min, 'g', 10), QString::number(o.max, 'g', 10));
            else if (o.kind == PnrOption::OpenFile || o.kind == PnrOption::SaveFile)
                text += " a file";
            text += " - " + o.help;
            if (!editable(o))
                text += " (its step has run)";
            text += "\n";
        }
        text += "Other keys are set as --vopt options of the architecture.";
        return reply(McpServer::text(errors.isEmpty() ? text : errors + "\n" + text, !errors.isEmpty()));
    }

    if (name == "save_json") {
        if (taskRunning)
            return reply(McpServer::text("A flow step is running; try again when it ended.", true));
        QString path = QFileInfo(args["path"].toString()).absoluteFilePath();
        if (!writeJson(path.toStdString()))
            return reply(McpServer::text("Cannot write " + path, true));
        return reply(McpServer::text("Written to " + path));
    }

    if (name == "timing") {
        if (taskRunning)
            return reply(McpServer::text("A flow step is running; try again when it ended.", true));
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        Context *c = ctx.get();
        QString text;
        if (args.contains("net")) {
            QString net_name = args["net"].toString();
            auto found = ctx->nets.find(ctx->id(net_name.toStdString()));
            if (found == ctx->nets.end())
                return reply(McpServer::text("No net " + net_name, true));
            NetInfo *net = found->second.get();
            if (!net->driver.cell)
                return reply(McpServer::text(net_name + " has no driver.", true));
            text = QString("%1, driven by %2.%3, %4 users:\n")
                           .arg(net_name, net->driver.cell->name.c_str(c), net->driver.port.c_str(c))
                           .arg(int(net->users.entries()));
            for (auto &user : net->users) {
                text += QString("to %1.%2: estimate %3 ns")
                                .arg(user.cell->name.c_str(c), user.port.c_str(c), ns(c, ctx->predictArcDelay(net, user)));
                if (!net->wires.empty())
                    text += QString(", routed %1 ns").arg(ns(c, ctx->getNetinfoRouteDelay(net, user)));
                text += "\n";
                if (net->wires.empty())
                    continue;
                size_t count = ctx->getNetinfoSinkWireCount(net, user);
                for (size_t i = 0; i < count; i++)
                    text += route_text(c, net, ctx->getNetinfoSinkWire(net, user, i));
            }
            if (net->wires.empty())
                text += "(not routed)\n";
            return reply(McpServer::text(shortened(text.toStdString())));
        }

        TimingAnalyser tmg(c);
        tmg.setup_only = false;
        tmg.with_clock_skew = true;
        bool paths = args["paths"].toBool(true);
        tmg.setup(false, true, true);
        auto &result = tmg.get_timing_result();
        if (result.clock_fmax.empty())
            text += "No Fmax: no paths within a clock domain (is the clock driven and constrained?).\n";
        for (auto &clock : result.clock_fmax)
            text += QString("Fmax %1: %2 MHz (constraint %3 MHz)%4\n")
                            .arg(clock.first.c_str(c))
                            .arg(clock.second.achieved, 0, 'f', 2)
                            .arg(clock.second.constraint, 0, 'f', 2)
                            .arg(clock.second.achieved < clock.second.constraint ? " FAILS" : "");
        auto event = [c](const ClockEvent &e) {
            if (e.clock == IdString())
                return QString("ports");
            return QString("%1 %2").arg(e.edge == RISING_EDGE ? "rising" : "falling", e.clock.c_str(c));
        };
        auto path_text = [c, event](const CriticalPath &path) {
            QString t = QString("  from %1 to %2, budget %3 ns\n")
                                .arg(event(path.clock_pair.start), event(path.clock_pair.end), ns(c, path.max_delay));
            delay_t total = 0;
            for (auto &s : path.segments) {
                total += s.delay;
                t += QString("  %1 %2 ns (total %3) %4.%5 -> %6.%7%8\n")
                             .arg(CriticalPath::Segment::type_to_str(s.type).c_str(), ns(c, s.delay), ns(c, total),
                                  s.from.first.c_str(c), s.from.second.c_str(c), s.to.first.c_str(c),
                                  s.to.second.c_str(c))
                             .arg(s.net != IdString() ? QString(" net ") + s.net.c_str(c) : QString());
            }
            return t;
        };
        if (paths) {
            for (auto &p : result.clock_paths)
                text += QString("\nCritical path of %1:\n").arg(p.first.c_str(c)) + path_text(p.second);
            for (auto &p : result.xclock_paths)
                text += "\nCritical path between clocks:\n" + path_text(p);
        }
        if (!result.slack_histogram.empty()) {
            int lo = INT_MAX, hi = INT_MIN;
            unsigned negative = 0, total = 0;
            for (auto &h : result.slack_histogram) {
                lo = std::min(lo, h.first);
                hi = std::max(hi, h.first);
                total += h.second;
                if (h.first < 0)
                    negative += h.second;
            }
            const int bins = 20;
            int size = std::max(1, (hi - lo + bins) / bins);
            std::vector<unsigned> count(bins);
            for (auto &h : result.slack_histogram)
                count[std::min(bins - 1, (h.first - lo) / size)] += h.second;
            text += QString("\nSlack histogram (ns; %1 of %2 endpoints negative):\n").arg(negative).arg(total);
            for (int i = 0; i < bins; i++)
                if (count[i])
                    text += QString("  %1 to %2: %3\n")
                                    .arg((lo + i * size) / 1000.0, 0, 'f', 3)
                                    .arg((lo + (i + 1) * size) / 1000.0, 0, 'f', 3)
                                    .arg(count[i]);
        }
        return reply(McpServer::text(shortened(text.toStdString())));
    }

    if (name == "find") {
        QRegularExpression re(args["pattern"].toString());
        if (!re.isValid())
            return reply(McpServer::text("Invalid pattern: " + re.errorString(), true));
        QString type = args["type"].toString("cell");
        int limit = args["limit"].toInt(200), found = 0;
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        Context *c = ctx.get();
        QString text;
        auto match = [&](const char *name) { return re.match(QString::fromUtf8(name)).hasMatch(); };
        if (type == "cell") {
            for (auto &cell : ctx->cells) {
                CellInfo *ci = cell.second.get();
                if (!match(cell.first.c_str(c)) || found++ >= limit)
                    continue;
                text += QString("%1 (%2)").arg(cell.first.c_str(c), ci->type.c_str(c));
                if (ci->bel != BelId()) {
                    Loc loc = ctx->getBelLocation(ci->bel);
                    text += QString(" at %1 (%2, %3, %4)").arg(ctx->nameOfBel(ci->bel)).arg(loc.x).arg(loc.y).arg(loc.z);
                }
                text += "\n";
            }
        } else if (type == "net") {
            for (auto &net : ctx->nets) {
                NetInfo *ni = net.second.get();
                if (!match(net.first.c_str(c)) || found++ >= limit)
                    continue;
                text += QString("%1: %2 users").arg(net.first.c_str(c)).arg(int(ni->users.entries()));
                if (ni->driver.cell)
                    text += QString(", from %1.%2").arg(ni->driver.cell->name.c_str(c), ni->driver.port.c_str(c));
                text += ni->wires.empty() ? ", not routed\n" : ", routed\n";
            }
        } else if (type == "bel") {
            for (BelId bel : ctx->getBels()) {
                const char *bel_name = ctx->nameOfBel(bel);
                if (!match(bel_name) || found++ >= limit)
                    continue;
                Loc loc = ctx->getBelLocation(bel);
                CellInfo *cell = ctx->getBoundBelCell(bel);
                text += QString("%1 (%2) at (%3, %4, %5)%6\n")
                                .arg(bel_name, ctx->getBelType(bel).c_str(c))
                                .arg(loc.x)
                                .arg(loc.y)
                                .arg(loc.z)
                                .arg(cell ? QString(": ") + cell->name.c_str(c) : QString());
            }
        } else {
            return reply(McpServer::text("type is cell, net or bel", true));
        }
        if (found > limit)
            text += QString("... %1 more\n").arg(found - limit);
        return reply(McpServer::text(found ? text : QString("Nothing matches.")));
    }

    reply(McpServer::text("Unknown tool: " + name, true));
}

NEXTPNR_NAMESPACE_END
