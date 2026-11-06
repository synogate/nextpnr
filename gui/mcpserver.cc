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

#include "mcpserver.h"
#include <QJsonDocument>
#include <QMetaObject>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>
#include "version.h"

NEXTPNR_NAMESPACE_BEGIN

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace {

// The protocol versions the server speaks, the latest first
const char *const protocol_versions[] = {"2025-11-25", "2025-06-18", "2025-03-26"};

// Whether a Host or Origin header names this computer: a page elsewhere (DNS rebinding) must not
// reach the server through the user's browser
bool is_local(std::string value)
{
    auto scheme = value.find("://");
    if (scheme != std::string::npos)
        value = value.substr(scheme + 3);
    std::string host;
    if (!value.empty() && value[0] == '[')
        host = value.substr(0, value.find(']') + 1);
    else
        host = value.substr(0, value.find(':'));
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

QJsonObject rpc_result(const QJsonValue &id, const QJsonValue &result)
{
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

QJsonObject rpc_error(const QJsonValue &id, int code, const QString &message)
{
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

} // namespace

namespace {
struct Session;
}

struct McpServer::Impl
{
    McpServer *owner;
    // Shared with the replies of tools still running: they post to it, also once it stopped
    std::shared_ptr<asio::io_context> ioc = std::make_shared<asio::io_context>();
    tcp::acceptor acceptor{*ioc};
    std::thread thread;
    // The open connections (on the server's thread)
    std::vector<std::weak_ptr<Session>> sessions;

    void accept();
    void close();
};

namespace {

// One connection: requests one after the other, a tool's reply may come later
struct Session : std::enable_shared_from_this<Session>
{
    std::weak_ptr<asio::io_context> ioc;
    QJsonArray tools;
    QString instructions;
    McpServer::Call call;
    QObject *owner;

    beast::tcp_stream stream;
    beast::flat_buffer buffer;
    std::optional<http::request_parser<http::string_body>> parser;
    http::request<http::string_body> request;
    http::response<http::string_body> response;

    Session(tcp::socket socket, std::weak_ptr<asio::io_context> ioc, const QJsonArray &tools,
            const QString &instructions, const McpServer::Call &call, QObject *owner)
            : ioc(ioc), tools(tools), instructions(instructions), call(call), owner(owner), stream(std::move(socket))
    {
    }

    void read()
    {
        parser.emplace();
        parser->body_limit(16 * 1024 * 1024);
        // An idle connection is closed after a while (the client opens a new one)
        stream.expires_after(std::chrono::minutes(5));
        http::async_read(stream, buffer, *parser,
                         [self = shared_from_this()](beast::error_code ec, size_t) { self->handle(ec); });
    }

    void send(http::status status, const std::string &body = "", const char *type = nullptr)
    {
        response = {};
        response.version(request.version());
        response.result(status);
        response.set(http::field::server, "nextpnr");
        if (type)
            response.set(http::field::content_type, type);
        if (status == http::status::method_not_allowed)
            response.set(http::field::allow, "POST");
        response.body() = body;
        response.keep_alive(request.keep_alive());
        response.prepare_payload();
        stream.expires_never();
        http::async_write(stream, response, [self = shared_from_this()](beast::error_code ec, size_t) {
            if (ec || !self->response.keep_alive()) {
                beast::error_code ignored;
                self->stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
                return;
            }
            self->read();
        });
    }

    void sendJson(const QJsonObject &message)
    {
        send(http::status::ok, QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString(),
             "application/json");
    }

    void handle(beast::error_code ec)
    {
        if (ec)
            return; // closed, timed out or not HTTP
        stream.expires_never();
        request = parser->release();

        if (!is_local(std::string(request[http::field::host])) ||
            (request.count(http::field::origin) && !is_local(std::string(request[http::field::origin]))))
            return send(http::status::forbidden);
        std::string target(request.target());
        target = target.substr(0, target.find('?'));
        if (target != "/mcp" && target != "/")
            return send(http::status::not_found);
        // No stream for messages from the server (GET), and no sessions to end (DELETE)
        if (request.method() != http::verb::post)
            return send(http::status::method_not_allowed);

        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(request.body()), &parseError);
        if (parseError.error != QJsonParseError::NoError)
            return send(http::status::bad_request,
                        QJsonDocument(rpc_error(QJsonValue::Null, -32700, "Parse error")).toJson().toStdString(),
                        "application/json");
        if (!doc.isObject())
            return send(http::status::bad_request,
                        QJsonDocument(rpc_error(QJsonValue::Null, -32600, "Batches are not supported"))
                                .toJson()
                                .toStdString(),
                        "application/json");
        QJsonObject message = doc.object();
        // Notifications and responses: taken, nothing to answer
        if (!message.contains("id") || !message.contains("method"))
            return send(http::status::accepted);

        QJsonValue id = message["id"];
        QString method = message["method"].toString();
        QJsonObject params = message["params"].toObject();
        if (method == "initialize") {
            QString version = protocol_versions[0];
            for (auto v : protocol_versions)
                if (params["protocolVersion"].toString() == v)
                    version = v;
            QJsonObject result{
                    {"protocolVersion", version},
                    {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                    {"serverInfo", QJsonObject{{"name", "nextpnr"}, {"version", GIT_DESCRIBE_STR}}},
                    {"instructions", instructions},
            };
            return sendJson(rpc_result(id, result));
        }
        if (method == "ping")
            return sendJson(rpc_result(id, QJsonObject{}));
        if (method == "tools/list")
            return sendJson(rpc_result(id, QJsonObject{{"tools", tools}}));
        if (method == "tools/call") {
            QString name = params["name"].toString();
            bool known = false;
            for (auto tool : tools)
                known |= tool.toObject()["name"].toString() == name;
            if (!known)
                return sendJson(rpc_error(id, -32602, "Unknown tool: " + name));
            // The reply comes on the GUI thread, maybe much later: back to this thread with it. If
            // the server stopped meanwhile, it stays in the stopped io_context and goes with it.
            McpServer::Reply reply = [self = shared_from_this(), ioc = ioc.lock(), id](const McpServer::Result &result) {
                asio::post(*ioc, [self, id, result]() {
                    self->sendJson(
                            rpc_result(id, QJsonObject{{"content", result.content}, {"isError", result.error}}));
                });
            };
            QMetaObject::invokeMethod(
                    owner, [call = call, name, args = params["arguments"].toObject(), reply]() { call(name, args, reply); },
                    Qt::QueuedConnection);
            return;
        }
        sendJson(rpc_error(id, -32601, "Method not found: " + method));
    }
};

} // namespace

McpServer::McpServer(QJsonArray tools, QString instructions, Call call, QObject *parent)
        : QObject(parent), tools(tools), instructions(instructions), call(call)
{
}

McpServer::~McpServer() { stop(); }

McpServer::Result McpServer::text(const QString &text, bool error)
{
    Result result;
    result.content.append(QJsonObject{{"type", "text"}, {"text", text}});
    result.error = error;
    return result;
}

void McpServer::Impl::accept()
{
    acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (ec)
            return; // closed
        auto session = std::make_shared<Session>(std::move(socket), ioc, owner->tools, owner->instructions,
                                                 owner->call, owner);
        sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [](auto &s) { return s.expired(); }),
                       sessions.end());
        sessions.push_back(session);
        session->read();
        accept();
    });
}

void McpServer::Impl::close()
{
    beast::error_code ignored;
    acceptor.close(ignored);
    for (auto &weak : sessions)
        if (auto session = weak.lock())
            session->stream.socket().close(ignored);
    sessions.clear();
}

bool McpServer::start(int port, std::string &error)
{
    stop();
    auto server = std::make_shared<Impl>();
    server->owner = this;
    try {
        tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
        server->acceptor.open(endpoint.protocol());
        server->acceptor.bind(endpoint);
        server->acceptor.listen();
    } catch (const boost::system::system_error &e) {
        error = e.what();
        return false;
    }
    server->accept();
    server->thread = std::thread([ioc = server->ioc]() {
        for (;;) {
            try {
                ioc->run();
                return;
            } catch (const std::exception &) {
                // (a request that went wrong; the others go on)
            }
        }
    });
    impl = server;
    return true;
}

void McpServer::stop()
{
    if (!impl)
        return;
    // Closing all connections leaves the server's thread without work, and it ends
    asio::post(*impl->ioc, [server = impl.get()]() { server->close(); });
    impl->thread.join();
    impl = nullptr;
}

NEXTPNR_NAMESPACE_END
