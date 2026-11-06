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

#ifndef MCPSERVER_H
#define MCPSERVER_H

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// A Model Context Protocol server, so that an assistant such as Claude Code can work in the GUI
// with the user: the streamable HTTP transport (with plain JSON responses) on 127.0.0.1. The
// protocol runs on a thread of its own, the tools on the GUI thread.
class McpServer : public QObject
{
    Q_OBJECT

  public:
    // The outcome of a tool: MCP content blocks
    struct Result
    {
        QJsonArray content;
        bool error = false;
    };
    using Reply = std::function<void(const Result &)>;
    // Runs a tool on the GUI thread; it calls the reply once, right away or later
    using Call = std::function<void(const QString &name, const QJsonObject &args, Reply reply)>;

    // tools: as tools/list gives them; instructions: what the client is told about the server
    McpServer(QJsonArray tools, QString instructions, Call call, QObject *parent = nullptr);
    ~McpServer();

    // Listens on the port; false, with the reason in error, if it cannot
    bool start(int port, std::string &error);
    void stop();
    bool isRunning() const { return impl != nullptr; }

    static Result text(const QString &text, bool error = false);

  private:
    struct Impl;
    std::shared_ptr<Impl> impl;
    QJsonArray tools;
    QString instructions;
    Call call;
};

NEXTPNR_NAMESPACE_END

#endif // MCPSERVER_H
