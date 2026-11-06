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

#include "guitest.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSpinBox>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QTreeView>
#include <cmath>
#include <cstdio>
#include <limits>

#ifdef NEXTPNR_GUITEST_QPA
#include <qpa/qwindowsysteminterface.h>
#endif

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "basewindow.h"
#include "designwidget.h"
#include "editcommands.h"
#include "fpgaviewwidget.h"
#include "pyinterpreter.h"
#include "pythontab.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
QElapsedTimer g_clock;
FILE *g_log = nullptr;

void writeLog(const QString &s)
{
    QByteArray b = (s + "\n").toUtf8();
    fwrite(b.constData(), 1, b.size(), stderr);
    fflush(stderr);
    if (g_log) {
        fwrite(b.constData(), 1, b.size(), g_log);
        fflush(g_log);
    }
}

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    static const char *names[] = {"debug", "warning", "critical", "fatal", "info"};
    writeLog(QString("[qt %1] %2").arg(names[int(type) < 5 ? int(type) : 0], msg));
}

QString plain(QString s) { return s.remove('&').trimmed(); }

#ifdef _WIN32
// A crash during a test: log the exception and the stack (symbols if a .pdb is next to the exe,
// else module + offset), then let the process die (no dialog: the launcher sets the error mode)
LONG WINAPI crashHandler(EXCEPTION_POINTERS *ep)
{
    static bool inside = false;
    if (inside)
        return EXCEPTION_CONTINUE_SEARCH;
    inside = true;
    char buf[1024];
    snprintf(buf, sizeof(buf), "[guitest] CRASH: exception 0x%08lx at %p", ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    writeLog(buf);
    HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *ep->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0)
            break;
        DWORD64 addr = frame.AddrPC.Offset;
        char module[MAX_PATH] = "?";
        DWORD64 base = SymGetModuleBase64(process, addr);
        if (base)
            GetModuleFileNameA(HMODULE(base), module, MAX_PATH);
        const char *mod = strrchr(module, '\\') ? strrchr(module, '\\') + 1 : module;
        alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 512];
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(symbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        if (SymFromAddr(process, addr, &disp, sym)) {
            if (SymGetLineFromAddr64(process, addr, &ldisp, &line))
                snprintf(buf, sizeof(buf), "  #%d %s!%s+0x%llx (%s:%lu)", i, mod, sym->Name,
                         (unsigned long long)disp, line.FileName, line.LineNumber);
            else
                snprintf(buf, sizeof(buf), "  #%d %s!%s+0x%llx", i, mod, sym->Name, (unsigned long long)disp);
        } else {
            snprintf(buf, sizeof(buf), "  #%d %s+0x%llx", i, mod, (unsigned long long)(addr - base));
        }
        writeLog(buf);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// PNG writer (this Qt may be built without PNG support): RGB, zlib stream from qCompress
quint32 crc32(const QByteArray &data)
{
    static quint32 table[256];
    static bool init = false;
    if (!init) {
        for (quint32 n = 0; n < 256; n++) {
            quint32 c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        init = true;
    }
    quint32 c = 0xffffffffu;
    for (char ch : data)
        c = table[(c ^ quint8(ch)) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

void putU32(QByteArray &out, quint32 v)
{
    for (int i = 3; i >= 0; i--)
        out.append(char((v >> (8 * i)) & 0xff));
}

void pngChunk(QByteArray &out, const char *type, const QByteArray &data)
{
    putU32(out, data.size());
    QByteArray td = QByteArray(type, 4) + data;
    out.append(td);
    putU32(out, crc32(td));
}

bool savePng(const QImage &image, const QString &file)
{
    QImage img = image.convertToFormat(QImage::Format_RGB888);
    QByteArray raw;
    raw.reserve((img.width() * 3 + 1) * img.height());
    for (int y = 0; y < img.height(); y++) {
        raw.append(char(0)); // filter: none
        raw.append(reinterpret_cast<const char *>(img.constScanLine(y)), img.width() * 3);
    }
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    QByteArray png(reinterpret_cast<const char *>(signature), 8), ihdr;
    putU32(ihdr, img.width());
    putU32(ihdr, img.height());
    // 8 bit, RGB, deflate, no filter method, no interlace
    ihdr.append(char(8)).append(char(2)).append(char(0)).append(char(0)).append(char(0));
    pngChunk(png, "IHDR", ihdr);
    pngChunk(png, "IDAT", qCompress(raw, 6).mid(4)); // without qCompress' length prefix
    pngChunk(png, "IEND", QByteArray());
    QFile f(file);
    return f.open(QIODevice::WriteOnly) && f.write(png) == png.size();
}

// Result of a step that has to wait for something
enum Result
{
    STEP_OK,
    STEP_FAIL,
    STEP_RETRY
};
Result g_result = STEP_OK;
} // namespace

void GuiTest::startIfRequested(BaseMainWindow *win)
{
    QString script = qEnvironmentVariable("NEXTPNR_GUI_TEST");
    if (script.isEmpty())
        return;
    QString logFile = qEnvironmentVariable("NEXTPNR_GUI_TEST_LOG");
    if (!logFile.isEmpty())
        g_log = fopen(logFile.toLocal8Bit().constData(), "w");
    qInstallMessageHandler(messageHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif
    new GuiTest(win, script);
}

GuiTest::GuiTest(BaseMainWindow *win, const QString &script) : QObject(win), win_(win)
{
    g_clock.start();
    QFile f(script);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        writeLog("[guitest] cannot read script " + script);
        QTimer::singleShot(0, [] { QCoreApplication::exit(2); });
        return;
    }
    lines_ = QString::fromUtf8(f.readAll()).split('\n');
    connect(win->editStack, &EditStack::message, this, [this](const QString &msg) {
        lastMessage_ = msg;
        log("  message: " + msg);
    });
    log("script " + script);
    // Started from the constructor: the first step runs once the event loop runs, i.e. after the
    // design is loaded and the window is shown
    scheduleNext(500);
}

void GuiTest::log(const QString &s) { writeLog(QString("[guitest %1] %2").arg(g_clock.elapsed(), 6).arg(s)); }

void GuiTest::scheduleNext(int ms) { QTimer::singleShot(ms, this, &GuiTest::step); }

void GuiTest::finish(int code)
{
    done_ = true;
    log(code == 0 ? "PASSED" : QString("FAILED (exit %1)").arg(code));
    if (g_log)
        fclose(g_log), g_log = nullptr;
    QTimer::singleShot(0, [code] { QCoreApplication::exit(code); });
}

void GuiTest::step()
{
    if (done_)
        return;
    while (pc_ < lines_.size()) {
        QString line = lines_.at(pc_).trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            pc_++;
            continue;
        }
        QStringList w = line.split(' ', Qt::SkipEmptyParts);
        if (waitKind_ == 0)
            log(QString("line %1: %2").arg(pc_ + 1).arg(line));
        nextDelay_ = -1;
        g_result = STEP_OK;
        error_.clear();
        if (!exec(w, line))
            g_result = g_result == STEP_RETRY ? STEP_RETRY : STEP_FAIL;
        if (g_result == STEP_RETRY) {
            if (waitKind_ == 0) {
                waitKind_ = 1;
                bool idle = w.at(0) == "waitidle";
                waitUntil_ = g_clock.elapsed() + (idle ? (w.size() > 1 ? w.at(1).toInt() : 600000) : 3000);
            }
            if (g_clock.elapsed() < waitUntil_) {
                scheduleNext(100);
                return;
            }
            if (error_.isEmpty())
                error_ = "timeout";
            g_result = STEP_FAIL;
        }
        waitKind_ = 0;
        if (g_result == STEP_FAIL) {
            log(QString("ERROR line %1: %2").arg(pc_ + 1).arg(error_));
            finish(1);
            return;
        }
        pc_++;
        if (done_)
            return;
        scheduleNext(nextDelay_ >= 0 ? nextDelay_ : delay_);
        return;
    }
    finish(0);
}

QWidget *GuiTest::view() { return win_->fpgaView; }

bool GuiTest::parseMods(const QStringList &w, int from, Qt::MouseButton &button, Qt::KeyboardModifiers &mods)
{
    button = Qt::LeftButton;
    mods = Qt::NoModifier;
    for (int i = from; i < w.size(); i++) {
        QString m = w.at(i).toLower();
        if (m == "left")
            button = Qt::LeftButton;
        else if (m == "right")
            button = Qt::RightButton;
        else if (m == "middle")
            button = Qt::MiddleButton;
        else if (m == "ctrl")
            mods |= Qt::ControlModifier;
        else if (m == "shift")
            mods |= Qt::ShiftModifier;
        else if (m == "alt")
            mods |= Qt::AltModifier;
        else {
            error_ = "unknown button/modifier " + w.at(i);
            return false;
        }
    }
    return true;
}

void GuiTest::mouse(QWidget *target, QPointF pos, Qt::MouseButtons state, Qt::MouseButton button, int type,
                    Qt::KeyboardModifiers mods)
{
    QWidget *top = target->window();
    QPointF local = target->mapTo(top, pos.toPoint());
#ifdef NEXTPNR_GUITEST_QPA
    QWindow *wh = top->windowHandle();
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::AsynchronousDelivery>(
            wh, local, wh->mapToGlobal(local), state, button, QEvent::Type(type), mods);
#else
    (void)local;
    auto *ev = new QMouseEvent(QEvent::Type(type), pos, target->mapToGlobal(pos), button, state, mods);
    QCoreApplication::postEvent(target, ev);
#endif
}

void GuiTest::click(QPointF pos, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool dbl)
{
    QWidget *v = view();
    mouse(v, pos, Qt::NoButton, Qt::NoButton, QEvent::MouseMove, mods);
    for (int i = 0; i < (dbl ? 2 : 1); i++) {
        mouse(v, pos, button, button, QEvent::MouseButtonPress, mods);
        mouse(v, pos, Qt::NoButton, button, QEvent::MouseButtonRelease, mods);
    }
}

void GuiTest::drag(QPointF a, QPointF b, Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    QWidget *v = view();
    mouse(v, a, Qt::NoButton, Qt::NoButton, QEvent::MouseMove, mods);
    mouse(v, a, button, button, QEvent::MouseButtonPress, mods);
    const int n = 10;
    for (int i = 1; i <= n; i++)
        mouse(v, a + (b - a) * (double(i) / n), button, Qt::NoButton, QEvent::MouseMove, mods);
    mouse(v, b, Qt::NoButton, button, QEvent::MouseButtonRelease, mods);
}

// World <-> widget coordinates of the device view (the mapping is affine: the camera looks straight
// down on the chip)
namespace {
struct ViewMap
{
    float ox, oy, xx, xy, yx, yy; // world = o + x * (xx, xy) + y * (yx, yy)
    QPointF toWidget(float wx, float wy) const
    {
        float dx = wx - ox, dy = wy - oy;
        float det = xx * yy - yx * xy;
        return QPointF((dx * yy - dy * yx) / det, (xx * dy - xy * dx) / det);
    }
};
} // namespace

bool GuiTest::elementPoint(const QString &kind, const QString &name, QPointF &pos)
{
    FPGAViewWidget *v = win_->fpgaView;
    Context *ctx = win_->getContext();
    using PE = FPGAViewWidget::PickedElement;
    std::unique_ptr<PE> target;
    std::string n = name.toStdString();
    DecalXY decal;
    if (kind == "bel" || kind == "cell") {
        BelId bel;
        if (kind == "cell") {
            auto it = ctx->cells.find(ctx->id(n));
            if (it == ctx->cells.end()) {
                error_ = "no cell " + name;
                return false;
            }
            bel = it->second->bel;
            if (bel == BelId()) {
                error_ = "cell " + name + " is not placed";
                return false;
            }
        } else {
            bel = ctx->getBelByNameStr(n);
        }
        if (bel == BelId()) {
            error_ = "no bel " + name;
            return false;
        }
        decal = ctx->getBelDecal(bel);
        target.reset(new PE(PE::fromBel(bel, decal.x, decal.y)));
    } else if (kind == "wire") {
        WireId wire = ctx->getWireByNameStr(n);
        if (wire == WireId()) {
            error_ = "no wire " + name;
            return false;
        }
        decal = ctx->getWireDecal(wire);
        target.reset(new PE(PE::fromWire(wire, decal.x, decal.y)));
    } else if (kind == "pip") {
        PipId pip = ctx->getPipByNameStr(n);
        if (pip == PipId()) {
            error_ = "no pip " + name;
            return false;
        }
        decal = ctx->getPipDecal(pip);
        target.reset(new PE(PE::fromPip(pip, decal.x, decal.y)));
    } else {
        error_ = "bad element kind " + kind;
        return false;
    }

    // Candidate points: centres, midpoints and quarter points of the decal's graphics, then a grid
    // over its bounding box
    std::vector<std::pair<float, float>> pts;
    float x0 = std::numeric_limits<float>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
    for (auto &el : ctx->getDecalGraphics(decal.decal)) {
        if (el.type != GraphicElement::TYPE_BOX && el.type != GraphicElement::TYPE_LINE &&
            el.type != GraphicElement::TYPE_ARROW && el.type != GraphicElement::TYPE_LOCAL_LINE &&
            el.type != GraphicElement::TYPE_LOCAL_ARROW)
            continue;
        float ax = decal.x + el.x1, ay = decal.y + el.y1, bx = decal.x + el.x2, by = decal.y + el.y2;
        for (float t : {0.5f, 0.25f, 0.75f, 0.1f, 0.9f})
            pts.emplace_back(ax + (bx - ax) * t, ay + (by - ay) * t);
        if (el.type == GraphicElement::TYPE_BOX)
            for (float t : {0.05f, 0.95f})
                for (float u : {0.05f, 0.95f})
                    pts.emplace_back(ax + (bx - ax) * t, ay + (by - ay) * u);
        x0 = std::min({x0, ax, bx}), x1 = std::max({x1, ax, bx});
        y0 = std::min({y0, ay, by}), y1 = std::max({y1, ay, by});
    }
    if (x0 > x1) {
        // No graphics of its own (e.g. a pip drawn as part of the details): around its position
        x0 = decal.x - 0.01f, x1 = decal.x + 0.01f, y0 = decal.y - 0.01f, y1 = decal.y + 0.01f;
    }
    const int grid = 12;
    for (int i = 0; i <= grid; i++)
        for (int j = 0; j <= grid; j++)
            pts.emplace_back(x0 + (x1 - x0) * i / grid, y0 + (y1 - y0) * j / grid);

    int W = v->width(), H = v->height();
    QVector4D o = v->mouseToWorldCoordinates(0, 0), a = v->mouseToWorldCoordinates(W, 0),
              b = v->mouseToWorldCoordinates(0, H);
    ViewMap m;
    m.ox = o.x(), m.oy = o.y();
    m.xx = (a.x() - o.x()) / W, m.xy = (a.y() - o.y()) / W;
    m.yx = (b.x() - o.x()) / H, m.yy = (b.y() - o.y()) / H;

    // Bring it into view (keeping the zoom) if its centre is not
    QRectF inner(QPointF(2, 2), QSizeF(W - 4, H - 4));
    QPointF c = m.toWidget((x0 + x1) / 2, (y0 + y1) / 2);
    if (!inner.contains(c)) {
        v->viewMove_.setToIdentity();
        v->viewMove_.translate(-(x0 + x1) / 2, -(y0 + y1) / 2);
        v->update();
        error_ = name + " not in view (panned to it)";
        g_result = STEP_RETRY;
        return false;
    }
    QPointF fallback(-1, -1);
    for (auto &p : pts) {
        QPointF s = m.toWidget(p.first, p.second);
        if (!inner.contains(s))
            continue;
        // Pick at the pixel the click will land on
        QPoint px(int(std::round(s.x())), int(std::round(s.y())));
        QVector4D wp = v->mouseToWorldCoordinates(px.x(), px.y());
        auto cands = v->pickCandidates(wp.x(), wp.y());
        if (cands.empty())
            continue;
        if (cands.front() == *target) {
            pos = px;
            return true;
        }
        if (fallback.x() < 0)
            for (auto &cd : cands)
                if (cd == *target)
                    fallback = px;
    }
    if (fallback.x() >= 0) {
        log(QString("  note: %1 is not the first candidate at any sampled point, using %2 %3")
                    .arg(name)
                    .arg(fallback.x())
                    .arg(fallback.y()));
        pos = fallback;
        return true;
    }
    // The in-view graphics may still be rendering: retry for a while
    error_ = "no point found where " + kind + " " + name + " can be picked (zoom closer?)";
    g_result = STEP_RETRY;
    return false;
}

bool GuiTest::zoomElement(const QString &kind, const QString &name, float dist)
{
    FPGAViewWidget *v = win_->fpgaView;
    Context *ctx = win_->getContext();
    std::string n = name.toStdString();
    DecalXY decal;
    if (kind == "cell") {
        auto it = ctx->cells.find(ctx->id(n));
        if (it == ctx->cells.end() || it->second->bel == BelId()) {
            error_ = "no placed cell " + name;
            return false;
        }
        decal = ctx->getBelDecal(it->second->bel);
    } else if (kind == "bel") {
        BelId bel = ctx->getBelByNameStr(n);
        if (bel == BelId()) {
            error_ = "no bel " + name;
            return false;
        }
        decal = ctx->getBelDecal(bel);
    } else if (kind == "wire") {
        WireId wire = ctx->getWireByNameStr(n);
        if (wire == WireId()) {
            error_ = "no wire " + name;
            return false;
        }
        decal = ctx->getWireDecal(wire);
    } else {
        PipId pip = ctx->getPipByNameStr(n);
        if (pip == PipId()) {
            error_ = "no pip " + name;
            return false;
        }
        decal = ctx->getPipDecal(pip);
    }
    FPGAViewWidget::PickQuadTree::BoundingBox bb;
    LineShaderData scratch;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        v->renderDecal(scratch, bb, decal);
    }
    float cx = decal.x, cy = decal.y, size = 0.01f;
    if (bb.x0() != std::numeric_limits<float>::infinity()) {
        cx = bb.x0() + bb.w() / 2, cy = bb.y0() + bb.h() / 2;
        size = std::max(bb.w(), bb.h());
    }
    v->viewMove_.setToIdentity();
    v->viewMove_.translate(-cx, -cy);
    v->zoom_ = dist > 0 ? dist : std::max(size * 3, v->zoomNear_);
    v->clampZoom();
    v->update();
    log(QString("  view centre %1 %2 distance %3").arg(cx).arg(cy).arg(v->zoom_));
    return true;
}

QMenu *GuiTest::popupMenu() { return qobject_cast<QMenu *>(QApplication::activePopupWidget()); }

QStringList GuiTest::statusTexts()
{
    return {win_->statusBar->currentMessage(), lastMessage_, win_->selectionLabel->toolTip(),
            win_->placementLabel->isVisible() ? win_->placementLabel->toolTip() : QString()};
}

bool GuiTest::grabWindow(const QString &file, bool viewOnly)
{
    QFileInfo fi(file);
    QDir().mkpath(fi.absolutePath());
    FPGAViewWidget *v = win_->fpgaView;
    QImage viewImg = v->grabFramebuffer();
    QImage img;
    if (viewOnly) {
        img = viewImg;
    } else {
        img = win_->grab().toImage();
        QPainter p(&img);
        // The GL view, composited in case grab() did not get its content
        QPoint at = v->mapTo(win_, QPoint(0, 0));
        p.drawImage(QRect(at, v->size()), viewImg);
        // An open popup menu (a window of its own)
        if (QWidget *popup = QApplication::activePopupWidget())
            p.drawPixmap(popup->mapToGlobal(QPoint(0, 0)) - win_->mapToGlobal(QPoint(0, 0)), popup->grab());
        // An open modal dialog
        if (QWidget *modal = QApplication::activeModalWidget())
            if (modal != win_)
                p.drawPixmap(modal->mapToGlobal(QPoint(0, 0)) - win_->mapToGlobal(QPoint(0, 0)), modal->grab());
    }
    bool ok = fi.suffix().compare("png", Qt::CaseInsensitive) == 0 ? savePng(img, fi.absoluteFilePath())
                                                                    : img.save(fi.absoluteFilePath());
    if (!ok) {
        error_ = "cannot save " + fi.absoluteFilePath();
        return false;
    }
    log(QString("  saved %1 (%2x%3)").arg(fi.absoluteFilePath()).arg(img.width()).arg(img.height()));
    return true;
}

bool GuiTest::runPython(const QString &code, QString &out)
{
    int err = 0;
    // A trailing newline completes compound statements (for ...: ...)
    out = QString::fromStdString(pyinterpreter_execute(code.toStdString() + "\n", &err)).trimmed();
    return err == 0;
}

bool GuiTest::exec(const QStringList &w, const QString &line)
{
    const QString cmd = w.at(0).toLower();
    // The rest of the line after the command word
    QString rest = line.mid(w.at(0).size()).trimmed();
    auto need = [&](int n) {
        if (w.size() < n + 1) {
            error_ = QString("%1 needs %2 argument(s)").arg(cmd).arg(n);
            return false;
        }
        return true;
    };
    FPGAViewWidget *v = win_->fpgaView;

    if (cmd == "delay") {
        if (!need(1))
            return false;
        delay_ = w.at(1).toInt();
        return true;
    }
    if (cmd == "wait") {
        if (!need(1))
            return false;
        nextDelay_ = w.at(1).toInt();
        return true;
    }
    if (cmd == "waitidle") {
        if (win_->editStack->busy()) {
            error_ = "task still running";
            g_result = STEP_RETRY;
        }
        return true;
    }
    if (cmd == "echo") {
        log("  " + rest);
        return true;
    }
    if (cmd == "quit") {
        finish(w.size() > 1 ? w.at(1).toInt() : 0);
        return true;
    }
    if (cmd == "resize") {
        if (!need(2))
            return false;
        win_->showNormal();
        win_->resize(w.at(1).toInt(), w.at(2).toInt());
        return true;
    }
    if (cmd == "shot" || cmd == "shotview") {
        if (!need(1))
            return false;
        return grabWindow(rest, cmd == "shotview");
    }
    if (cmd == "view") {
        if (!need(3))
            return false;
        v->viewMove_.setToIdentity();
        v->viewMove_.translate(-w.at(1).toFloat(), -w.at(2).toFloat());
        v->zoom_ = w.at(3).toFloat();
        v->clampZoom();
        v->update();
        nextDelay_ = std::max(delay_, 500); // let the in-view graphics render
        return true;
    }
    if (cmd == "zoomfit") {
        v->zoomOutbound();
        v->update();
        nextDelay_ = std::max(delay_, 500);
        return true;
    }
    if (cmd == "zoombel" || cmd == "zoomwire" || cmd == "zoompip" || cmd == "zoomcell") {
        if (!need(1))
            return false;
        nextDelay_ = std::max(delay_, 500);
        return zoomElement(cmd.mid(4), w.at(1), w.size() > 2 ? w.at(2).toFloat() : 0);
    }
    if (cmd == "move" || cmd == "click" || cmd == "dclick" || cmd == "press" || cmd == "release") {
        if (!need(2))
            return false;
        Qt::MouseButton button;
        Qt::KeyboardModifiers mods;
        if (!parseMods(w, 3, button, mods))
            return false;
        QPointF p(w.at(1).toDouble(), w.at(2).toDouble());
        if (cmd == "move") {
            mouse(view(), p, pressed_, Qt::NoButton, QEvent::MouseMove, mods);
        } else if (cmd == "press") {
            pressed_ |= button;
            mouse(view(), p, pressed_, button, QEvent::MouseButtonPress, mods);
        } else if (cmd == "release") {
            pressed_ &= ~Qt::MouseButtons(button);
            mouse(view(), p, pressed_, button, QEvent::MouseButtonRelease, mods);
        } else
            click(p, button, mods, cmd == "dclick");
        return true;
    }
    if (cmd.startsWith("click") || cmd.startsWith("hover")) {
        if (!need(1))
            return false;
        QString kind = cmd.mid(5);
        Qt::MouseButton button;
        Qt::KeyboardModifiers mods;
        if (!parseMods(w, 2, button, mods))
            return false;
        QPointF p;
        if (!elementPoint(kind, w.at(1), p))
            return false;
        log(QString("  at %1 %2").arg(p.x()).arg(p.y()));
        if (cmd.startsWith("hover"))
            mouse(view(), p, Qt::NoButton, Qt::NoButton, QEvent::MouseMove, mods);
        else
            click(p, button, mods);
        return true;
    }
    if (cmd == "drag") {
        if (!need(4))
            return false;
        Qt::MouseButton button;
        Qt::KeyboardModifiers mods;
        if (!parseMods(w, 5, button, mods))
            return false;
        drag(QPointF(w.at(1).toDouble(), w.at(2).toDouble()), QPointF(w.at(3).toDouble(), w.at(4).toDouble()),
             button, mods);
        return true;
    }
    if (cmd == "dragbel") {
        if (!need(2))
            return false;
        Qt::MouseButton button;
        Qt::KeyboardModifiers mods;
        if (!parseMods(w, 3, button, mods))
            return false;
        QPointF a, b;
        if (!elementPoint("bel", w.at(1), a) || !elementPoint("bel", w.at(2), b))
            return false;
        log(QString("  from %1 %2 to %3 %4").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y()));
        drag(a, b, button, mods);
        return true;
    }
    if (cmd == "key") {
        if (!need(1))
            return false;
        QKeySequence seq = QKeySequence::fromString(rest);
        if (seq.isEmpty()) {
            error_ = "bad key sequence " + rest;
            return false;
        }
        // An action with this shortcut (shortcuts need the window to be the active one, which it
        // may not be on a hidden desktop)
        if (popupMenu() == nullptr && QApplication::activeModalWidget() == nullptr) {
            for (QAction *a : win_->findChildren<QAction *>()) {
                if (a->isEnabled() && a->shortcuts().contains(seq)) {
                    log("  shortcut of action '" + plain(a->text()) + "'");
                    QTimer::singleShot(0, a, &QAction::trigger);
                    return true;
                }
            }
        }
        for (int i = 0; i < seq.count(); i++) {
            QKeyCombination kc = seq[i];
            QWidget *target = QApplication::activePopupWidget();
            if (!target)
                target = QApplication::activeModalWidget();
            if (!target)
                target = QApplication::focusWidget();
            if (!target)
                target = win_;
            QString text;
            int key = int(kc.key());
            if (!(kc.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier)) && key < 0x80 && key >= 0x20)
                text = (kc.keyboardModifiers() & Qt::ShiftModifier) ? QString(QChar(key)).toUpper()
                                                                     : QString(QChar(key)).toLower();
            QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyPress, key, kc.keyboardModifiers(), text));
            QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyRelease, key, kc.keyboardModifiers(), text));
        }
        return true;
    }
    if (cmd == "type") {
        QWidget *target = QApplication::focusWidget();
        if (!target) {
            error_ = "no focus widget";
            return false;
        }
        for (QChar ch : rest) {
            QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch)));
            QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyRelease, 0, Qt::NoModifier, QString(ch)));
        }
        return true;
    }
    if (cmd == "focus") {
        if (!need(1))
            return false;
        QWidget *target = nullptr;
        if (w.at(1) == "view")
            target = v;
        else if (w.at(1) == "console")
            target = win_->console;
        else if (w.at(1) == "tree") {
            for (QTreeView *t : win_->designview->findChildren<QTreeView *>())
                if (t->isVisible()) {
                    target = t;
                    break;
                }
        }
        if (!target) {
            error_ = "no widget " + w.at(1);
            return false;
        }
        win_->activateWindow();
        target->setFocus(Qt::OtherFocusReason);
        return true;
    }
    if (cmd == "action") {
        if (!need(1))
            return false;
        QAction *found = nullptr, *prefix = nullptr;
        int prefixMatches = 0;
        for (QAction *a : win_->findChildren<QAction *>()) {
            QString t = plain(a->text());
            if (t.compare(rest, Qt::CaseInsensitive) == 0) {
                found = a;
                break;
            }
            if (t.startsWith(rest, Qt::CaseInsensitive))
                prefix = a, prefixMatches++;
        }
        if (!found && prefixMatches == 1)
            found = prefix;
        if (!found) {
            error_ = prefixMatches > 1 ? "ambiguous action " + rest : "no action " + rest;
            return false;
        }
        if (!found->isEnabled()) {
            error_ = "action '" + plain(found->text()) + "' is disabled";
            return false;
        }
        QTimer::singleShot(0, found, &QAction::trigger);
        return true;
    }
    if (cmd == "selectcell" || cmd == "selectnet") {
        if (!need(1))
            return false;
        Context *ctx = win_->getContext();
        IdString id = ctx->id(w.at(1).toStdString());
        if (cmd == "selectcell") {
            if (!ctx->cells.count(id)) {
                error_ = "no cell " + w.at(1);
                return false;
            }
            // Like a user's click on the row: showCell reveals and selects it (without arming, as
            // "Show in tree" does), then the row is clicked (with Ctrl: added to the old selection)
            Qt::MouseButton button;
            Qt::KeyboardModifiers mods;
            if (!parseMods(w, 2, button, mods))
                return false;
            QTreeView *tree = nullptr;
            QModelIndexList before;
            for (QTreeView *t : win_->designview->findChildren<QTreeView *>())
                if (t->isVisible())
                    tree = t;
            if (tree && mods.testFlag(Qt::ControlModifier))
                before = tree->selectionModel()->selectedIndexes();
            win_->designview->showCell(id);
            tree = nullptr;
            for (QTreeView *t : win_->designview->findChildren<QTreeView *>())
                if (t->isVisible())
                    tree = t;
            if (!tree) {
                error_ = "no visible tree";
                return false;
            }
            QModelIndex mi = tree->currentIndex();
            tree->scrollTo(mi);
            tree->selectionModel()->clearSelection();
            for (const QModelIndex &b : before)
                tree->selectionModel()->select(b, QItemSelectionModel::Select | QItemSelectionModel::Rows);
            QRect r = tree->visualRect(mi);
            if (!mi.isValid() || r.isEmpty()) {
                error_ = "row of " + w.at(1) + " not visible in the tree";
                return false;
            }
            QPointF p(r.left() + std::min(20, r.width() / 2), r.center().y());
            mouse(tree->viewport(), p, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress, mods);
            mouse(tree->viewport(), p, Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease, mods);
        } else {
            if (!ctx->nets.count(id)) {
                error_ = "no net " + w.at(1);
                return false;
            }
            win_->designview->showNet(id);
        }
        return true;
    }
    if (cmd == "menu" || cmd == "assert_menu" || cmd == "assert_menu_disabled" || cmd == "assert_nomenu") {
        if (!need(1))
            return false;
        QMenu *menu = popupMenu();
        if (!menu || !menu->isVisible()) {
            error_ = "no popup menu open";
            g_result = STEP_RETRY;
            return false;
        }
        QStringList path = rest.split('>');
        QString want = path.first().trimmed();
        QAction *hit = nullptr;
        QStringList entries;
        for (QAction *a : menu->actions()) {
            if (a->isSeparator())
                continue;
            entries << plain(a->text());
            if (!hit && plain(a->text()).startsWith(want, Qt::CaseInsensitive))
                hit = a;
        }
        if (cmd == "assert_nomenu") {
            if (hit) {
                error_ = "menu has entry '" + plain(hit->text()) + "'";
                return false;
            }
            return true;
        }
        if (!hit) {
            error_ = "no menu entry '" + want + "' in [" + entries.join(" | ") + "]";
            return false;
        }
        if (cmd == "assert_menu" || cmd == "assert_menu_disabled") {
            log("  entry '" + plain(hit->text()) + "' " + (hit->isEnabled() ? "enabled" : "disabled") +
                (hit->toolTip().isEmpty() || hit->toolTip() == hit->text() ? "" : ", tooltip: " + hit->toolTip()));
            if ((cmd == "assert_menu_disabled") == hit->isEnabled()) {
                error_ = QString("menu entry is %1").arg(hit->isEnabled() ? "enabled" : "disabled");
                return false;
            }
            return true;
        }
        if (!hit->isEnabled()) {
            error_ = "menu entry '" + plain(hit->text()) + "' is disabled";
            return false;
        }
        if (path.size() > 1 && hit->menu()) {
            // Open the submenu, the inserted next step picks in it
            menu->setActiveAction(hit);
            hit->menu()->popup(menu->mapToGlobal(menu->actionGeometry(hit).topRight()));
            lines_.insert(pc_ + 1, "menu " + path.mid(1).join(">"));
            return true;
        }
        log("  picking '" + plain(hit->text()) + "' of [" + entries.join(" | ") + "]");
        QPointer<QMenu> mp(menu);
        QTimer::singleShot(0, [mp, hit] {
            if (!mp)
                return;
            mp->setActiveAction(hit);
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(mp, &press);
        });
        return true;
    }
    if (cmd == "menuclose") {
        while (QWidget *p = QApplication::activePopupWidget())
            p->close();
        return true;
    }
    if (cmd == "dialog" || cmd == "dialog_cancel") {
        QWidget *modal = QApplication::activeModalWidget();
        QDialog *dlg = qobject_cast<QDialog *>(modal);
        if (!dlg) {
            error_ = "no dialog open";
            g_result = STEP_RETRY;
            return false;
        }
        log("  dialog '" + dlg->windowTitle() + "'");
        if (cmd == "dialog_cancel") {
            QTimer::singleShot(0, dlg, &QDialog::reject);
            return true;
        }
        if (auto *in = qobject_cast<QInputDialog *>(dlg)) {
            if (!rest.isEmpty()) {
                if (in->inputMode() == QInputDialog::IntInput)
                    in->setIntValue(rest.toInt());
                else if (in->inputMode() == QInputDialog::DoubleInput)
                    in->setDoubleValue(rest.toDouble());
                else
                    in->setTextValue(rest);
            }
        }
        else if (!rest.isEmpty()) {
            // Any other dialog: the values go into its spin boxes, in order
            QStringList values = rest.split(' ', Qt::SkipEmptyParts);
            QList<QSpinBox *> spins = dlg->findChildren<QSpinBox *>();
            for (int i = 0; i < values.size() && i < spins.size(); i++)
                spins.at(i)->setValue(values.at(i).toInt());
        }
        if (auto *mb = qobject_cast<QMessageBox *>(dlg))
            log("  message box: " + mb->text());
        QTimer::singleShot(0, dlg, &QDialog::accept);
        return true;
    }
    if (cmd == "check") {
        if (!need(2))
            return false;
        bool on = w.last().compare("on", Qt::CaseInsensitive) == 0;
        QString want = QStringList(w.mid(1, w.size() - 2)).join(' ');
        for (QAbstractButton *b : win_->findChildren<QAbstractButton *>()) {
            if (!b->isCheckable() || !b->isVisible() || !plain(b->text()).startsWith(want, Qt::CaseInsensitive))
                continue;
            if (b->isChecked() != on)
                b->click();
            log("  '" + plain(b->text()) + "' " + (b->isChecked() ? "on" : "off"));
            return true;
        }
        error_ = "no visible checkable button '" + want + "'";
        return false;
    }
    if (cmd == "status") {
        QStringList t = statusTexts();
        log("  message:   " + t.at(0));
        log("  last msg:  " + t.at(1));
        log("  selection: " + t.at(2));
        log("  placement: " + t.at(3));
        return true;
    }
    if (cmd == "assert_status" || cmd == "assert_nostatus") {
        if (!need(1))
            return false;
        bool found = false;
        for (auto &t : statusTexts())
            if (t.contains(rest, Qt::CaseInsensitive))
                found = true;
        if (found != (cmd == "assert_status")) {
            error_ = QString("status %1 '%2'; texts: [%3]")
                             .arg(found ? "contains" : "does not contain", rest, statusTexts().join(" | "));
            return false;
        }
        return true;
    }
    if (cmd == "py" || cmd == "assert_py") {
        if (!need(1))
            return false;
        QString out;
        bool ok = runPython(cmd == "py" ? rest : "assert (" + rest + ")", out);
        if (!out.isEmpty())
            log("  py: " + out);
        if (!ok) {
            error_ = cmd == "py" ? "python error" : "assertion failed: " + rest;
            return false;
        }
        return true;
    }
    error_ = "unknown command " + cmd;
    return false;
}

NEXTPNR_NAMESPACE_END
