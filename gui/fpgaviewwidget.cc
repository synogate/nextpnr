/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Serge Bazanski <q3k@q3k.org>
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <math.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QWidget>

#include "QtImGui.h"
#include "imgui.h"

#include "editcommands.h"
#include "elementinfo.h"
#include "fpgaviewwidget.h"
#include "log.h"
#include "mainwindow.h"

#if QT_VERSION_MAJOR >= 6
#define EVENT_POS(event) ((event)->position())
#else
#define EVENT_POS(event) ((event)->pos())
#endif

NEXTPNR_NAMESPACE_BEGIN

FPGAViewWidget::FPGAViewWidget(QWidget *parent)
        : QOpenGLWidget(parent), movieSaving(false), ctx_(nullptr), paintTimer_(this), lineShader_(this),
          textShader_(this), zoom_(10.0f), rendererArgs_(new FPGAViewWidget::RendererArgs),
          rendererData_(new FPGAViewWidget::RendererData)
{
    TextShader::init_font();
    colors_.background = QColor("#000000");
    colors_.grid = QColor("#333");
    colors_.frame = QColor("#808080");
    colors_.hidden = QColor("#606060");
    colors_.inactive = QColor("#303030");
    colors_.active = QColor("#f0f0f0");
    colors_.routing = QColor("#686868");
    colors_.selected = QColor("#ff6600");
    colors_.hovered = QColor("#906030");
    colors_.highlight[0] = QColor("#6495ed");
    colors_.highlight[1] = QColor("#7fffd4");
    colors_.highlight[2] = QColor("#98fb98");
    colors_.highlight[3] = QColor("#ffd700");
    colors_.highlight[4] = QColor("#cd5c5c");
    colors_.highlight[5] = QColor("#fa8072");
    colors_.highlight[6] = QColor("#ff69b4");
    colors_.highlight[7] = QColor("#da70d6");
    colors_.overlay[OVERLAY_FREE] = QColor("#30d030");
    colors_.overlay[OVERLAY_SWAP] = QColor("#ff9900");
    colors_.overlay[OVERLAY_BLOCKED] = QColor("#c03030");
    colors_.overlay[OVERLAY_HOVER_BAD] = QColor("#ff2020");

    rendererArgs_->changed = false;
    rendererArgs_->gridChanged = false;
    rendererArgs_->zoomOutbound = true;

    connect(&paintTimer_, SIGNAL(timeout()), this, SLOT(update()));
    paintTimer_.start(1000 / 20); // paint GL 20 times per second

    renderRunner_ = std::unique_ptr<PeriodicRunner>(new PeriodicRunner(this, [this] { renderLines(); }));
    renderRunner_->start();
    renderRunner_->startTimer(1000 / 2); // render lines 2 times per second
    setMouseTracking(true);
    // Focus on click, so that Tab / Shift+Tab reach the view to cycle the hovered element
    setFocusPolicy(Qt::ClickFocus);
    clock_.start();

    displayBel_ = false;
    displayWire_ = false;
    displayPip_ = false;
    displayGroup_ = false;
}

FPGAViewWidget::~FPGAViewWidget() {}

void FPGAViewWidget::setEditStack(EditStack *stack)
{
    editStack_ = stack;
    if (stack == nullptr)
        return;
    connect(stack, &EditStack::designChanged, this, [this] {
        if (armedCell_ == IdString())
            return;
        if (ctx_ == nullptr || !ctx_->cells.count(armedCell_)) {
            disarmPlacement();
            return;
        }
        updateOverlay();
    });
    connect(stack, &EditStack::stateChanged, this, [this] {
        if (armedCell_ != IdString() && !editStack_->canEdit())
            disarmPlacement();
    });
}

void FPGAViewWidget::newContext(Context *ctx)
{
    // The old context may be gone already: disarm without touching it
    hoverCandidates_.clear();
    ctx_ = nullptr;
    disarmPlacement();
    ctx_ = ctx;
    {
        QMutexLocker lock(&rendererArgsLock_);

        rendererArgs_->gridChanged = true;
    }
    onSelectedArchItem(std::vector<DecalXY>(), false);
    for (int i = 0; i < 8; i++)
        onHighlightGroupChanged(std::vector<DecalXY>(), i);
    {
        QMutexLocker lock(&rendererArgsLock_);
        rendererArgs_->zoomOutbound = true;
    }
    pokeRenderer();
}

QSize FPGAViewWidget::minimumSizeHint() const { return QSize(320, 200); }

QSize FPGAViewWidget::sizeHint() const { return QSize(640, 480); }

void FPGAViewWidget::initializeGL()
{
    if (!lineShader_.compile(overlaySlot(OVERLAY_SLOTS))) {
        log_error("Could not compile shader.\n");
    }
    if (!textShader_.compile(archTextSlot(ARCH_LAYERS, 0, 0))) {
        log_error("Could not compile text shader.\n");
    }
    initializeOpenGLFunctions();
    QtImGui::initialize(this);
    glClearColor(colors_.background.red() / 255, colors_.background.green() / 255, colors_.background.blue() / 255,
                 1.0);
}

float FPGAViewWidget::PickedElement::distance(Context *ctx, float wx, float wy, float scale) const
{
    // Get DecalXY for this element.
    DecalXY dec = decal(ctx);

    // Coordinates within decal.
    float dx = wx - dec.x;
    float dy = wy - dec.y;

    auto graphics = ctx->getDecalGraphics(dec.decal);
    if (graphics.size() == 0)
        return -1;

    // TODO(q3k): For multi-line decals, find intersections and also calculate distance to them.

    // Go over its' GraphicElements, and calculate the distance to them.
    std::vector<float> distances;
    std::transform(
            graphics.begin(), graphics.end(), std::back_inserter(distances), [&](const GraphicElement &ge) -> float {
                if (!lodVisible(lodRange(ge), scale))
                    return -1;
                switch (ge.type) {
                case GraphicElement::TYPE_BOX: {
                    // If outside the box, return unit distance to closest border.
                    float outside_x = -1, outside_y = -1;
                    if (dx < ge.x1 || dx > ge.x2) {
                        outside_x = std::min(std::abs(dx - ge.x1), std::abs(dx - ge.x2));
                    }
                    if (dy < ge.y1 || dy > ge.y2) {
                        outside_y = std::min(std::abs(dy - ge.y1), std::abs(dy - ge.y2));
                    }
                    if (outside_x != -1 && outside_y != -1)
                        return std::min(outside_x, outside_y);

                    // If in box, return 0.
                    return 0;
                }
                case GraphicElement::TYPE_LOCAL_LINE:
                case GraphicElement::TYPE_LOCAL_ARROW:
                case GraphicElement::TYPE_LINE:
                case GraphicElement::TYPE_ARROW: {
                    // Return somewhat primitively calculated distance to segment.
                    // TODO(q3k): consider coming up with a better algorithm
                    QVector2D w;
                    if (ge.type == GraphicElement::TYPE_LOCAL_LINE || ge.type == GraphicElement::TYPE_LOCAL_ARROW) {
                        w = QVector2D(dx, dy);
                    } else {
                        w = QVector2D(wx, wy);
                    }
                    QVector2D a(ge.x1, ge.y1);
                    QVector2D b(ge.x2, ge.y2);
                    float dw = a.distanceToPoint(w) + b.distanceToPoint(w);
                    float dab = a.distanceToPoint(b);
                    return std::abs(dw - dab) / dab;
                }
                default:
                    // Not close to anything.
                    return -1;
                }
            });

    // Find smallest non -1 distance.
    // Find closest element.
    return *std::min_element(distances.begin(), distances.end(), [&](float a, float b) {
        if (a == -1)
            return false;
        if (b == -1)
            return true;
        return a < b;
    });
}

bool FPGAViewWidget::PickedElement::hitMetrics(Context *ctx, float wx, float wy, float scale, float thin, float &dist,
                                               float &size) const
{
    DecalXY dec = decal(ctx);
    float dx = wx - dec.x;
    float dy = wy - dec.y;
    bool any = false;
    float x0 = std::numeric_limits<float>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
    dist = std::numeric_limits<float>::infinity();
    for (auto &ge : ctx->getDecalGraphics(dec.decal)) {
        if (!lodVisible(lodRange(ge), scale))
            continue;
        float d;
        float ax = ge.x1, ay = ge.y1, bx = ge.x2, by = ge.y2;
        switch (ge.type) {
        case GraphicElement::TYPE_BOX: {
            float ox = std::max({0.0f, std::min(ax, bx) - dx, dx - std::max(ax, bx)});
            float oy = std::max({0.0f, std::min(ay, by) - dy, dy - std::max(ay, by)});
            d = std::sqrt(ox * ox + oy * oy);
            break;
        }
        case GraphicElement::TYPE_LINE:
        case GraphicElement::TYPE_ARROW:
            // Not relative to the decal
            ax -= dec.x;
            bx -= dec.x;
            ay -= dec.y;
            by -= dec.y;
            /* fall through */
        case GraphicElement::TYPE_LOCAL_LINE:
        case GraphicElement::TYPE_LOCAL_ARROW: {
            float vx = bx - ax, vy = by - ay;
            float len2 = vx * vx + vy * vy;
            float t = len2 > 0 ? std::max(0.0f, std::min(1.0f, ((dx - ax) * vx + (dy - ay) * vy) / len2)) : 0.0f;
            float px = ax + t * vx - dx, py = ay + t * vy - dy;
            d = std::sqrt(px * px + py * py);
            break;
        }
        default:
            continue;
        }
        any = true;
        dist = std::min(dist, d);
        x0 = std::min({x0, ax, bx});
        x1 = std::max({x1, ax, bx});
        y0 = std::min({y0, ay, by});
        y1 = std::max({y1, ay, by});
    }
    if (!any)
        return false;
    size = std::max(x1 - x0, thin) * std::max(y1 - y0, thin);
    return true;
}

// The head of an arrow in a decal's graphics: two short lines back from its end, appended to heads.
// A path drawn as several arrows gets one head, at its end: none where another line of the decal
// continues. The head's size follows the arrow's length, up to a limit, and it is shown only from
// the zoom on where it is a few pixels long.
static void arrowHead(const std::vector<GraphicElement> &graphics, const GraphicElement &el,
                      std::vector<GraphicElement> &heads)
{
    if (el.type != GraphicElement::TYPE_ARROW && el.type != GraphicElement::TYPE_LOCAL_ARROW)
        return;
    const float eps = 1e-4f, max_length = 0.003f, min_pixels = 4;
    for (auto &other : graphics) {
        if (&other == &el ||
            (other.type != GraphicElement::TYPE_LINE && other.type != GraphicElement::TYPE_ARROW &&
             other.type != GraphicElement::TYPE_LOCAL_LINE && other.type != GraphicElement::TYPE_LOCAL_ARROW))
            continue;
        for (auto [x, y] : {std::pair<float, float>(other.x1, other.y1), {other.x2, other.y2}})
            if (std::abs(x - el.x2) < eps && std::abs(y - el.y2) < eps)
                return;
    }
    float dx = el.x2 - el.x1, dy = el.y2 - el.y1, length = std::hypot(dx, dy);
    if (length == 0)
        return;
    float h = std::min(0.3f * length, max_length), w = 0.35f * h;
    dx /= length, dy /= length;
    GraphicElement head = el;
    head.type = el.type == GraphicElement::TYPE_ARROW ? GraphicElement::TYPE_LINE : GraphicElement::TYPE_LOCAL_LINE;
    head.min_scale = std::max(el.min_scale, min_pixels / h);
    for (float side : {-1.0f, 1.0f}) {
        head.x1 = el.x2 - dx * h - dy * w * side;
        head.y1 = el.y2 - dy * h + dx * w * side;
        heads.push_back(head);
    }
}

void FPGAViewWidget::renderGraphicElement(LineShaderData &out, PickQuadTree::BoundingBox &bb, const GraphicElement &el,
                                          float x, float y)
{
    if (el.type == GraphicElement::TYPE_BOX) {
        auto line = PolyLine(true);
        line.point(x + el.x1, y + el.y1);
        line.point(x + el.x2, y + el.y1);
        line.point(x + el.x2, y + el.y2);
        line.point(x + el.x1, y + el.y2);
        line.build(out);

        bb.setX0(std::min(bb.x0(), x + el.x1));
        bb.setY0(std::min(bb.y0(), y + el.y1));
        bb.setX1(std::max(bb.x1(), x + el.x2));
        bb.setY1(std::max(bb.y1(), y + el.y2));
        return;
    }

    if (el.type == GraphicElement::TYPE_LINE || el.type == GraphicElement::TYPE_ARROW ||
        el.type == GraphicElement::TYPE_LOCAL_LINE || el.type == GraphicElement::TYPE_LOCAL_ARROW) {
        PolyLine(x + el.x1, y + el.y1, x + el.x2, y + el.y2).build(out);
        bb.setX0(std::min(bb.x0(), x + el.x1));
        bb.setY0(std::min(bb.y0(), y + el.y1));
        bb.setX1(std::max(bb.x1(), x + el.x2));
        bb.setY1(std::max(bb.y1(), y + el.y2));
        return;
    }
}

void FPGAViewWidget::renderDecal(LineShaderData &out, PickQuadTree::BoundingBox &bb, const DecalXY &decal)
{
    if (decal.decal == DecalId())
        return;

    float offsetX = decal.x;
    float offsetY = decal.y;

    // Selected and highlighted items are drawn in full detail at any zoom: without the elements
    // that stand in for details when zoomed out, and, for a used item, without the inactive
    // elements of its decal (the background, such as unused connections inside a bel).
    auto graphics = ctx_->getDecalGraphics(decal.decal);
    bool active = std::any_of(graphics.begin(), graphics.end(),
                              [](const GraphicElement &el) {
                                  return el.style == GraphicElement::STYLE_ACTIVE ||
                                         el.style == GraphicElement::STYLE_ROUTING;
                              });
    std::vector<GraphicElement> heads;
    for (auto &el : graphics)
        arrowHead(graphics, el, heads);
    graphics.insert(graphics.end(), heads.begin(), heads.end());
    for (auto &el : graphics) {
        if (std::isinf(el.max_scale) && !(active && el.style == GraphicElement::STYLE_INACTIVE))
            renderGraphicElement(out, bb, el, offsetX, offsetY);
    }
}

int FPGAViewWidget::archStyle(GraphicElement::style_t style)
{
    switch (style) {
    case GraphicElement::STYLE_FRAME:
        return 0;
    case GraphicElement::STYLE_INACTIVE:
        return 1;
    case GraphicElement::STYLE_ROUTING:
        return 2;
    case GraphicElement::STYLE_ACTIVE:
        return 3;
    default:
        return -1;
    }
}

int FPGAViewWidget::lodLevel(float scale)
{
    int level = 0;
    while (level + 1 < LOD_LEVELS && lodScale_[level + 1] <= scale)
        level++;
    return level;
}

int FPGAViewWidget::lodRange(const GraphicElement &el)
{
    int lo = lodLevel(el.min_scale);
    int hi = std::isinf(el.max_scale) ? LOD_LEVELS : lodLevel(el.max_scale);
    return hi > lo ? lo * (LOD_LEVELS + 1) + hi : -1;
}

bool FPGAViewWidget::lodVisible(int range, float scale)
{
    if (range < 0)
        return false;
    int lo = range / (LOD_LEVELS + 1), hi = range % (LOD_LEVELS + 1);
    return scale >= lodScale_[lo] && (hi == LOD_LEVELS || scale < lodScale_[hi]);
}

void FPGAViewWidget::renderArchDecal(ArchLayer &layer, PickQuadTree::BoundingBox &bb, const DecalXY &decal, bool view,
                                     PickQuadTree::BoundingBox *viewBB, int maxLevel)
{
    float offsetX = decal.x;
    float offsetY = decal.y;

    auto graphics = ctx_->getDecalGraphics(decal.decal);
    std::vector<GraphicElement> heads;
    for (auto &el : graphics)
        arrowHead(graphics, el, heads);
    graphics.insert(graphics.end(), heads.begin(), heads.end());
    for (auto &el : graphics) {
        int style = archStyle(el.style), range = lodRange(el);
        if (style < 0 || range < 0)
            continue;
        if (viewDependent(el) != view) {
            if (viewBB != nullptr) {
                viewBB->setX0(std::min({viewBB->x0(), offsetX + el.x1, offsetX + el.x2}));
                viewBB->setY0(std::min({viewBB->y0(), offsetY + el.y1, offsetY + el.y2}));
                viewBB->setX1(std::max({viewBB->x1(), offsetX + el.x1, offsetX + el.x2}));
                viewBB->setY1(std::max({viewBB->y1(), offsetY + el.y1, offsetY + el.y2}));
            }
            continue;
        }
        if (lodLevel(el.min_scale) > maxLevel)
            continue;
        if (el.type == GraphicElement::TYPE_LABEL)
            TextShader::build(layer.text[range][style], el.text, offsetX + el.x1, offsetY + el.y1, offsetX + el.x2,
                              offsetY + el.y2);
        else
            renderGraphicElement(layer.gfx[range][style], bb, el, offsetX, offsetY);
    }
}

void FPGAViewWidget::renderView(ArchLayer &layer,
                                const std::vector<std::pair<PickQuadTree::BoundingBox, DecalXY>> &decals,
                                const PickQuadTree::BoundingBox &rect, int maxLevel)
{
    PickQuadTree::BoundingBox bb;
    for (auto &d : decals) {
        const auto &b = d.first;
        if (b.x1() >= rect.x0() && b.x0() <= rect.x1() && b.y1() >= rect.y0() && b.y0() <= rect.y1())
            renderArchDecal(layer, bb, d.second, true, nullptr, maxLevel);
    }
}

FPGAViewWidget::PickQuadTree::BoundingBox FPGAViewWidget::visibleRect()
{
    QVector4D a = mouseToWorldCoordinates(0, 0), b = mouseToWorldCoordinates(width(), height());
    PickQuadTree::BoundingBox r(a.x(), a.y(), b.x(), b.y());
    r.fixup();
    return r;
}

void FPGAViewWidget::requestView()
{
    auto v = visibleRect();
    const auto &r = viewRequested_;
    int level = lodLevel(1.0f / mouseToWorldDimensions(1, 0).x());
    if (v.x0() >= r.x0() && v.x1() <= r.x1() && v.y0() >= r.y0() && v.y1() <= r.y1() && level <= viewRequestedLevel_)
        return;
    // The visible area and as much again on each side, to pan a bit without drawing again; up to
    // the level of detail shown now (zooming in further asks again, for a smaller area)
    viewRequested_ = PickQuadTree::BoundingBox(v.x0() - v.w(), v.y0() - v.h(), v.x1() + v.w(), v.y1() + v.h());
    viewRequestedLevel_ = level;
    {
        QMutexLocker lock(&rendererArgsLock_);
        rendererArgs_->viewRect = viewRequested_;
        rendererArgs_->viewLevel = level;
        rendererArgs_->viewChanged = true;
    }
    pokeRenderer();
}

void FPGAViewWidget::populateQuadTree(RendererData *data, const DecalXY &decal, const PickedElement &element)
{
    float x = decal.x;
    float y = decal.y;

    // The graphics drawn only in view (possibly many) get one box for all of them
    PickQuadTree::BoundingBox viewBB;
    for (auto &el : ctx_->getDecalGraphics(decal.decal)) {
        if (el.style == GraphicElement::STYLE_HIDDEN || el.style == GraphicElement::STYLE_FRAME) {
            continue;
        }
        if (viewDependent(el)) {
            viewBB.setX0(std::min({viewBB.x0(), x + el.x1, x + el.x2}));
            viewBB.setY0(std::min({viewBB.y0(), y + el.y1, y + el.y2}));
            viewBB.setX1(std::max({viewBB.x1(), x + el.x1, x + el.x2}));
            viewBB.setY1(std::max({viewBB.y1(), y + el.y1, y + el.y2}));
            continue;
        }

        bool res = true;
        if (el.type == GraphicElement::TYPE_BOX) {
            // Boxes are bounded by themselves.
            res = data->qt->insert(PickQuadTree::BoundingBox(x + el.x1, y + el.y1, x + el.x2, y + el.y2), element);
        }

        if (el.type == GraphicElement::TYPE_LINE || el.type == GraphicElement::TYPE_ARROW ||
            el.type == GraphicElement::TYPE_LOCAL_LINE || el.type == GraphicElement::TYPE_LOCAL_ARROW) {
            // Lines are bounded by their AABB slightly enlarged.
            float x0 = x + el.x1;
            float y0 = y + el.y1;
            float x1 = x + el.x2;
            float y1 = y + el.y2;
            if (x1 < x0)
                std::swap(x0, x1);
            if (y1 < y0)
                std::swap(y0, y1);

            x0 -= 0.01;
            y0 -= 0.01;
            x1 += 0.01;
            y1 += 0.01;

            res = data->qt->insert(PickQuadTree::BoundingBox(x0, y0, x1, y1), element);
        }

        if (!res) {
            NPNR_ASSERT_FALSE("populateQuadTree: could not insert element");
        }
    }
    if (viewBB.x0() <= viewBB.x1() && !data->qt->insert(viewBB, element)) {
        NPNR_ASSERT_FALSE("populateQuadTree: could not insert element");
    }
}

QMatrix4x4 FPGAViewWidget::getProjection(void)
{
    QMatrix4x4 matrix;

    const float aspect = float(width()) / float(height());
    matrix.perspective(90, aspect, zoomNear_ - 0.01f, zoomFar_ + 0.01f);
    return matrix;
}

void FPGAViewWidget::paintGL()
{
    auto gl = QOpenGLContext::currentContext()->functions();
#if defined(__APPLE__)
    const qreal retinaScale = devicePixelRatio();
#else
    const qreal retinaScale = devicePixelRatioF();
#endif
    gl->glViewport(0, 0, width() * retinaScale, height() * retinaScale);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    QMatrix4x4 matrix = getProjection();
    matrix.translate(0.0f, 0.0f, -zoom_);

    matrix *= viewMove_;

    // Calculate world thickness to achieve a screen 1px/1.1px line.
    float thick1Px = mouseToWorldDimensions(1, 0).x();
    float thick11Px = mouseToWorldDimensions(1.1, 0).x();
    float thick2Px = mouseToWorldDimensions(2, 0).x();

    {
        QMutexLocker locker(&rendererDataLock_);
        // Must be called from a thread holding the OpenGL context
        update_vbos();
    }

    // Render the grid.
    lineShader_.draw(GraphicElement::STYLE_GRID, colors_.grid, thick1Px, matrix);

    // Render Arch graphics: the levels of detail for the current zoom, lines first, then text.
    float scale = 1.0f / thick1Px; // pixels per coordinate unit
    if (scale >= lodScale_[VIEW_LEVEL])
        requestView();
    const QColor *archColors[ARCH_STYLES] = {&colors_.frame, &colors_.inactive, &colors_.routing, &colors_.active};
    for (int style = 0; style < ARCH_STYLES; style++)
        for (int layer = 0; layer < ARCH_LAYERS; layer++)
            for (int range = 0; range < LOD_RANGES; range++)
                if (lodVisible(range, scale))
                    lineShader_.draw(archLineSlot(layer, range, style), *archColors[style], thick11Px, matrix);
    gl->glEnable(GL_BLEND);
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int style = 0; style < ARCH_STYLES; style++)
        for (int layer = 0; layer < ARCH_LAYERS; layer++)
            for (int range = 0; range < LOD_RANGES; range++)
                if (lodVisible(range, scale))
                    textShader_.draw(archTextSlot(layer, range, style), *archColors[style], matrix);
    gl->glDisable(GL_BLEND);

    // Valid-site overlay of the cell armed for placement
    for (int i = 0; i < OVERLAY_SLOTS; i++)
        lineShader_.draw(overlaySlot(i), colors_.overlay[i], i == OVERLAY_HOVER_BAD ? thick2Px : thick11Px, matrix);

    // Draw highlighted items.
    for (int i = 0; i < 8; i++) {
        GraphicElement::style_t style = (GraphicElement::style_t)(GraphicElement::STYLE_HIGHLIGHTED0 + i);
        lineShader_.draw(style, colors_.highlight[i], thick11Px, matrix);
    }

    lineShader_.draw(GraphicElement::STYLE_SELECTED, colors_.selected, thick11Px, matrix);
    lineShader_.draw(GraphicElement::STYLE_HOVER, colors_.hovered, thick2Px, matrix);

    if (movieSaving) {
        if (movieCounter == currentFrameSkip) {
            QMutexLocker lock(&rendererArgsLock_);
            movieCounter = 0;
            QImage image = grabFramebuffer();
            if (!movieSkipSame || movieLastImage != image) {
                currentMovieFrame++;

                QString number = QString("movie_%1.png").arg(currentMovieFrame, 5, 10, QChar('0'));

                QFileInfo fileName = QFileInfo(QDir(movieDir), number);
                QImageWriter imageWriter(fileName.absoluteFilePath(), "png");
                imageWriter.write(image);
                movieLastImage = image;
            }
        } else {
            movieCounter++;
        }
    }

    // Render ImGui
    QtImGui::newFrame();
    drawOverlays();
    QMutexLocker lock(&rendererArgsLock_);
    if (!(rendererArgs_->hoveredDecal == DecalXY()) && rendererArgs_->hintText.size() > 0 &&
        clock_.elapsed() - hoverSince_ >= TOOLTIP_DELAY_MS) {
        ImGui::SetNextWindowPos(ImVec2(rendererArgs_->x, rendererArgs_->y));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(rendererArgs_->hintText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::Render();
}

void FPGAViewWidget::pokeRenderer(void) { renderRunner_->poke(); }

void FPGAViewWidget::enableDisableDecals(bool bels, bool wires, bool pips, bool groups)
{
    displayBel_ = bels;
    displayWire_ = wires;
    displayPip_ = pips;
    displayGroup_ = groups;
}

void FPGAViewWidget::renderLines(void)
{
    if (ctx_ == nullptr)
        return;

    // Data from Context needed to render all decals.
    std::vector<std::pair<DecalXY, BelId>> belDecals;
    std::vector<std::pair<DecalXY, WireId>> wireDecals;
    std::vector<std::pair<DecalXY, PipId>> pipDecals;
    std::vector<std::pair<DecalXY, GroupId>> groupDecals;
    std::vector<std::pair<DecalXY, BoundingBox>> detailDecals;
    bool decalsChanged = false;
    // Drawing a decal can read the netlist (an architecture draws a used bel by its cell's
    // configuration): hold the Context while rendering, so that what changes the netlist under the
    // lock waits for it.
    std::unique_lock<std::mutex> lock_ui(ctx_->ui_mutex);
    std::lock_guard<std::mutex> lock(ctx_->mutex);
    {
        // Take the UI mutex only to copy over all we need as fast as we can.

        // For now, collapse any decal changes into change of all decals.
        // TODO(q3k): fix this
        if (ctx_->allUiReload) {
            ctx_->allUiReload = false;
            decalsChanged = true;
        }
        if (ctx_->frameUiReload) {
            ctx_->frameUiReload = false;
            decalsChanged = true;
        }
        if (ctx_->belUiReload.size() > 0) {
            ctx_->belUiReload.clear();
            decalsChanged = true;
        }
        if (ctx_->wireUiReload.size() > 0) {
            ctx_->wireUiReload.clear();
            decalsChanged = true;
        }
        if (ctx_->pipUiReload.size() > 0) {
            ctx_->pipUiReload.clear();
            decalsChanged = true;
        }
        if (ctx_->groupUiReload.size() > 0) {
            ctx_->groupUiReload.clear();
            decalsChanged = true;
        }

        // Local copy of decals, taken as fast as possible to not block the P&R.
        if (decalsChanged) {
            if (displayBel_) {
                for (auto bel : ctx_->getBels()) {
                    belDecals.push_back({ctx_->getBelDecal(bel), bel});
                }
            }
            if (ctx_->guiDrawsUnusedRouting()) {
                if (displayWire_) {
                    for (auto wire : ctx_->getWires()) {
                        wireDecals.push_back({ctx_->getWireDecal(wire), wire});
                    }
                }
                if (displayPip_) {
                    for (auto pip : ctx_->getPips()) {
                        pipDecals.push_back({ctx_->getPipDecal(pip), pip});
                    }
                }
            } else if (displayWire_ || displayPip_) {
                for (auto &net : ctx_->nets) {
                    for (auto &wire : net.second->wires) {
                        if (displayWire_)
                            wireDecals.push_back({ctx_->getWireDecal(wire.first), wire.first});
                        if (displayPip_ && wire.second.pip != PipId())
                            pipDecals.push_back({ctx_->getPipDecal(wire.second.pip), wire.second.pip});
                    }
                }
            }
            if (displayGroup_) {
                for (auto group : ctx_->getGroups()) {
                    groupDecals.push_back({ctx_->getGroupDecal(group), group});
                }
            }
            detailDecals = ctx_->getDetailDecals();
        }
    }
    lock_ui.unlock();

    // Arguments from the main UI thread on what we should render.
    std::vector<DecalXY> selectedDecals;
    DecalXY hoveredDecal;
    std::vector<DecalXY> highlightedDecals[8];
    std::vector<DecalXY> overlayDecals[OVERLAY_SETS];
    DecalXY overlayHoverBad;
    bool overlayChanged;
    bool highlightedOrSelectedChanged;
    bool gridChanged;
    PickQuadTree::BoundingBox viewRect;
    int viewLevel;
    bool viewChanged;
    {
        // Take the renderer arguments lock, copy over all we need.
        QMutexLocker lock(&rendererArgsLock_);

        selectedDecals = rendererArgs_->selectedDecals;
        hoveredDecal = rendererArgs_->hoveredDecal;

        for (int i = 0; i < 8; i++)
            highlightedDecals[i] = rendererArgs_->highlightedDecals[i];

        overlayChanged = rendererArgs_->overlayChanged;
        rendererArgs_->overlayChanged = false;
        if (overlayChanged)
            for (int i = 0; i < OVERLAY_SETS; i++)
                overlayDecals[i] = rendererArgs_->overlayDecals[i];
        overlayHoverBad = rendererArgs_->overlayHoverBad;

        highlightedOrSelectedChanged = rendererArgs_->changed;
        gridChanged = rendererArgs_->gridChanged;
        rendererArgs_->changed = false;
        rendererArgs_->gridChanged = false;
        viewRect = rendererArgs_->viewRect;
        viewLevel = rendererArgs_->viewLevel;
        viewChanged = rendererArgs_->viewChanged;
        rendererArgs_->viewChanged = false;
    }

    // Render decals if necessary.
    if (decalsChanged) {
        auto data = std::unique_ptr<FPGAViewWidget::RendererData>(new FPGAViewWidget::RendererData);
        // Reset bounding box.
        data->bbGlobal.clear();

        // Draw Bels.
        if (displayBel_) {
            for (auto const &decal : belDecals) {
                PickQuadTree::BoundingBox viewBB;
                renderArchDecal(data->arch, data->bbGlobal, decal.first, false, &viewBB);
                if (viewBB.x0() <= viewBB.x1())
                    data->viewDecals.push_back({viewBB, decal.first});
            }
        }
        // Draw Wires.
        if (displayWire_) {
            for (auto const &decal : wireDecals) {
                PickQuadTree::BoundingBox viewBB;
                renderArchDecal(data->arch, data->bbGlobal, decal.first, false, &viewBB);
                if (viewBB.x0() <= viewBB.x1())
                    data->viewDecals.push_back({viewBB, decal.first});
            }
        }
        // Draw Pips.
        if (displayPip_) {
            for (auto const &decal : pipDecals) {
                PickQuadTree::BoundingBox viewBB;
                renderArchDecal(data->arch, data->bbGlobal, decal.first, false, &viewBB);
                if (viewBB.x0() <= viewBB.x1())
                    data->viewDecals.push_back({viewBB, decal.first});
            }
        }
        // Draw Groups.
        if (displayGroup_) {
            for (auto const &decal : groupDecals) {
                PickQuadTree::BoundingBox viewBB;
                renderArchDecal(data->arch, data->bbGlobal, decal.first, false, &viewBB);
                if (viewBB.x0() <= viewBB.x1())
                    data->viewDecals.push_back({viewBB, decal.first});
            }
        }

        // Details, drawn only in view: from the tiles they cover
        for (auto const &decal : detailDecals) {
            const BoundingBox &tiles = decal.second;
            PickQuadTree::BoundingBox area(ctx_->getGuiColumnX(tiles.x0), tiles.y0, ctx_->getGuiColumnX(tiles.x1) + 1,
                                           tiles.y1 + 1);
            data->viewDecals.push_back({area, decal.first});
            data->detailDecals.push_back({area, decal.first});
        }

        // Bounding box should be calculated by now.
        NPNR_ASSERT(data->bbGlobal.w() != 0);
        NPNR_ASSERT(data->bbGlobal.h() != 0);

        // Enlarge the bounding box slightly for the picking - when we insert
        // elements into it, we enlarge their bounding boxes slightly, so
        // we need to give ourselves some sagery margin here.
        auto bb = data->bbGlobal;
        bb.setX0(bb.x0() - 1);
        bb.setY0(bb.y0() - 1);
        bb.setX1(bb.x1() + 1);
        bb.setY1(bb.y1() + 1);

        // Populate picking quadtree.
        data->qt = std::unique_ptr<PickQuadTree>(new PickQuadTree(bb));
        for (auto const &decal : belDecals) {
            populateQuadTree(data.get(), decal.first,
                             PickedElement::fromBel(decal.second, decal.first.x, decal.first.y));
        }
        for (auto const &decal : wireDecals) {
            populateQuadTree(data.get(), decal.first,
                             PickedElement::fromWire(decal.second, decal.first.x, decal.first.y));
        }
        for (auto const &decal : pipDecals) {
            populateQuadTree(data.get(), decal.first,
                             PickedElement::fromPip(decal.second, decal.first.x, decal.first.y));
        }
        for (auto const &decal : groupDecals) {
            populateQuadTree(data.get(), decal.first,
                             PickedElement::fromGroup(decal.second, decal.first.x, decal.first.y));
        }

        // Swap over.
        {
            QMutexLocker lock(&rendererDataLock_);

            // If we're not re-rendering any highlights/selections, let's
            // copy them over from the current object.
            data->gfxGrid = rendererData_->gfxGrid;
            if (!highlightedOrSelectedChanged) {
                data->gfxSelected = rendererData_->gfxSelected;
                data->gfxHovered = rendererData_->gfxHovered;
                for (int i = 0; i < 8; i++)
                    data->gfxHighlighted[i] = rendererData_->gfxHighlighted[i];
            }
            // The overlay is re-rendered below when it changed
            for (int i = 0; i < OVERLAY_SLOTS; i++)
                data->gfxOverlay[i] = rendererData_->gfxOverlay[i];
            data->arch.setGeneration(++archGeneration_);
            rendererData_ = std::move(data);
        }
    }
    // Render the graphics drawn only in view, for a new area or after the decals changed.
    if ((viewChanged || decalsChanged) && viewRect.x0() <= viewRect.x1()) {
        std::vector<std::pair<PickQuadTree::BoundingBox, DecalXY>> decals;
        {
            QMutexLocker lock(&rendererDataLock_);
            for (auto &d : rendererData_->viewDecals) {
                const auto &b = d.first;
                if (b.x1() >= viewRect.x0() && b.x0() <= viewRect.x1() && b.y1() >= viewRect.y0() &&
                    b.y0() <= viewRect.y1())
                    decals.push_back(d);
            }
        }
        auto view = std::unique_ptr<ArchLayer>(new ArchLayer);
        renderView(*view, decals, viewRect, viewLevel);
        QMutexLocker lock(&rendererDataLock_);
        rendererData_->view = std::move(*view);
        rendererData_->view.setGeneration(++archGeneration_);
        rendererData_->viewRect = viewRect;
    }
    if (gridChanged) {
        QMutexLocker locker(&rendererDataLock_);
        rendererData_->gfxGrid.clear();
        // Render grid.
        // The edges of the tile columns (where the Arch spreads them out, both edges of each)
        int columns = ctx_->getGridDimX();
        float right = ctx_->getGuiColumnX(columns - 1) + 1.0f;
        for (int x = 0; x < columns; x++) {
            float left = ctx_->getGuiColumnX(x);
            if (x == 0 || left != ctx_->getGuiColumnX(x - 1) + 1.0f)
                PolyLine(left, 0.0f, left, 1.0f * ctx_->getGridDimY()).build(rendererData_->gfxGrid);
            PolyLine(left + 1.0f, 0.0f, left + 1.0f, 1.0f * ctx_->getGridDimY()).build(rendererData_->gfxGrid);
        }
        for (float i = 0.0f; i < 1.0f * ctx_->getGridDimY() + 1; i += 1.0f) {
            PolyLine(ctx_->getGuiColumnX(0), i, right, i).build(rendererData_->gfxGrid);
        }
        rendererData_->bbGlobal.setX0(-1.0f);
        rendererData_->bbGlobal.setY0(-1.0f);
        rendererData_->bbGlobal.setX1(right + 1.0f);
        rendererData_->bbGlobal.setY1(1.0f * (ctx_->getGridDimY() + 1));
        rendererData_->gfxGrid.last_render++;
    }
    if (highlightedOrSelectedChanged) {
        QMutexLocker locker(&rendererDataLock_);

        // Whether the currently being hovered decal is also selected.
        bool hoveringSelected = false;
        // Render selected.
        rendererData_->bbSelected.clear();
        rendererData_->gfxSelected.clear();
        for (auto &decal : selectedDecals) {
            if (decal == hoveredDecal)
                hoveringSelected = true;
            renderDecal(rendererData_->gfxSelected, rendererData_->bbSelected, decal);
        }
        rendererData_->gfxSelected.last_render++;

        // Render hovered.
        rendererData_->gfxHovered.clear();
        if (!hoveringSelected) {
            renderDecal(rendererData_->gfxHovered, rendererData_->bbGlobal, hoveredDecal);
        }
        rendererData_->gfxHovered.last_render++;

        // Render highlighted.
        for (int i = 0; i < 8; i++) {
            rendererData_->gfxHighlighted[i].clear();
            for (auto &decal : highlightedDecals[i]) {
                renderDecal(rendererData_->gfxHighlighted[i], rendererData_->bbGlobal, decal);
            }
            rendererData_->gfxHighlighted[i].last_render++;
        }

        rendererData_->gfxOverlay[OVERLAY_HOVER_BAD].clear();
        renderDecal(rendererData_->gfxOverlay[OVERLAY_HOVER_BAD], rendererData_->bbGlobal, overlayHoverBad);
        rendererData_->gfxOverlay[OVERLAY_HOVER_BAD].last_render++;
    }
    if (overlayChanged) {
        // Rendered outside the data lock: the overlay of a common cell type covers much of the chip
        LineShaderData gfx[OVERLAY_SETS];
        PickQuadTree::BoundingBox bb;
        for (int i = 0; i < OVERLAY_SETS; i++)
            for (auto &decal : overlayDecals[i])
                renderDecal(gfx[i], bb, decal);
        QMutexLocker locker(&rendererDataLock_);
        for (int i = 0; i < OVERLAY_SETS; i++) {
            int last = rendererData_->gfxOverlay[i].last_render;
            rendererData_->gfxOverlay[i] = std::move(gfx[i]);
            rendererData_->gfxOverlay[i].last_render = last + 1;
        }
    }

    bool zoomSelected = false;
    {
        QMutexLocker lock(&rendererArgsLock_);

        if (rendererArgs_->zoomOutbound) {
            zoomOutbound();
            rendererArgs_->zoomOutbound = false;
        }
        // Only once no change of the selection is pending: then bbSelected is the selection's
        if (rendererArgs_->zoomSelected && !rendererArgs_->changed) {
            rendererArgs_->zoomSelected = false;
            zoomSelected = true;
        }
    }
    if (zoomSelected) {
        QMutexLocker lock(&rendererDataLock_);
        if (rendererData_->bbSelected.x0() != std::numeric_limits<float>::infinity())
            zoomToBB(rendererData_->bbSelected, 0.5f, true);
    }
}

void FPGAViewWidget::movieStart(QString dir, long frameSkip, bool skipSame)
{
    QMutexLocker locker(&rendererArgsLock_);
    movieLastImage = QImage();
    movieSkipSame = skipSame;
    movieDir = dir;
    currentMovieFrame = 0;
    movieCounter = 0;
    currentFrameSkip = frameSkip;
    movieSaving = true;
}

void FPGAViewWidget::movieStop()
{
    QMutexLocker locker(&rendererArgsLock_);
    movieSaving = false;
}

void FPGAViewWidget::onSelectedArchItem(std::vector<DecalXY> decals, bool keep)
{
    {
        QMutexLocker locker(&rendererArgsLock_);
        if (keep) {
            std::copy(decals.begin(), decals.end(), std::back_inserter(rendererArgs_->selectedDecals));
        } else {
            rendererArgs_->selectedDecals = decals;
        }
        rendererArgs_->changed = true;
    }
    pokeRenderer();
}

void FPGAViewWidget::onHighlightGroupChanged(std::vector<DecalXY> decals, int group)
{
    {
        QMutexLocker locker(&rendererArgsLock_);
        rendererArgs_->highlightedDecals[group] = decals;
        rendererArgs_->changed = true;
    }
    pokeRenderer();
}

void FPGAViewWidget::onHoverItemChanged(DecalXY decal)
{
    QMutexLocker locked(&rendererArgsLock_);
    rendererArgs_->hoveredDecal = decal;
    rendererArgs_->changed = true;
    pokeRenderer();
}

void FPGAViewWidget::resizeGL(int width, int height) {}

std::vector<FPGAViewWidget::PickedElement> FPGAViewWidget::pickCandidates(float worldx, float worldy)
{
    std::vector<PickedElement> result;
    if (ctx_ == nullptr)
        return result;
    // Get elements from renderer whose BBs correspond to the pick.
    std::vector<PickedElement> elems;
    {
        QMutexLocker locker(&rendererDataLock_);
        if (rendererData_->qt == nullptr) {
            return result;
        }
        elems = rendererData_->qt->get(worldx, worldy);
        // The pips drawn in the details (which are not in the quadtree), where they are shown
        if (1.0f / mouseToWorldDimensions(1, 0).x() >= lodScale_[VIEW_LEVEL]) {
            for (auto &d : rendererData_->detailDecals) {
                if (!d.first.contains(worldx, worldy))
                    continue;
                const DecalXY &decal = d.second;
                for (PipId pip : ctx_->getDetailPips(decal.decal, worldx - decal.x, worldy - decal.y)) {
                    DecalXY pd = ctx_->getPipDecal(pip);
                    elems.push_back(PickedElement::fromPip(pip, pd.x, pd.y));
                }
            }
        }
    }

    if (elems.size() == 0) {
        return result;
    }

    // Everything drawn (at this zoom) within a few pixels of the cursor, smallest first: big boxes
    // (planes, tiles) cover the small things in them.
    float scale = 1.0f / mouseToWorldDimensions(1, 0).x();
    float reach = mouseToWorldDimensions(4, 0).x();
    float thin = mouseToWorldDimensions(1, 0).x();
    struct Hit
    {
        size_t index;
        float dist, size;
    };
    std::vector<Hit> hits;
    for (size_t i = 0; i < elems.size(); i++) {
        bool dup = false;
        for (auto &h : hits)
            if (elems[h.index] == elems[i])
                dup = true;
        if (dup)
            continue;
        float dist, size;
        if (elems[i].hitMetrics(ctx_, worldx, worldy, scale, thin, dist, size) && dist <= reach)
            hits.push_back(Hit{i, dist, size});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.size != b.size)
            return a.size < b.size;
        return a.dist < b.dist;
    });
    for (auto &h : hits)
        result.push_back(elems[h.index]);
    if (!result.empty())
        return result;

    // Nothing that close: the closest element, as before
    using ElemDist = std::pair<const PickedElement *, float>;
    std::vector<ElemDist> distances;
    std::transform(elems.begin(), elems.end(), std::back_inserter(distances), [&](const PickedElement &e) -> ElemDist {
        return std::make_pair(&e, e.distance(ctx_, worldx, worldy, scale));
    });

    // Find closest non -1 element.
    auto closest = std::min_element(distances.begin(), distances.end(), [&](const ElemDist &a, const ElemDist &b) {
        if (a.second == -1)
            return false;
        if (b.second == -1)
            return true;
        return a.second < b.second;
    });

    // All out of reach?
    if (closest->second >= 0)
        result.push_back(*(closest->first));
    return result;
}

std::string FPGAViewWidget::elementText(const PickedElement &e, bool multiline)
{
    if (ctx_ == nullptr)
        return "";
    // The binding state only when the design is not being changed (a running pass holds the lock)
    bool locked = ctx_->mutex.try_lock();
    std::string text;
    switch (e.type) {
    case ElementType::BEL:
        text = ElementInfo::belText(ctx_, e.bel, locked, multiline);
        break;
    case ElementType::WIRE:
        text = ElementInfo::wireText(ctx_, e.wire, locked, multiline);
        break;
    case ElementType::PIP:
        text = ElementInfo::pipText(ctx_, e.pip, locked, multiline);
        break;
    case ElementType::GROUP:
        text = ElementInfo::groupText(ctx_, e.group, multiline);
        break;
    default:
        break;
    }
    if (locked)
        ctx_->mutex.unlock();
    else if (multiline && e.type != ElementType::GROUP)
        text += "\n(busy: binding state not shown)";
    return text;
}

void FPGAViewWidget::emitClicked(const PickedElement &e, bool add)
{
    if (e.type == ElementType::BEL) {
        clickedBel(e.bel, add);
    } else if (e.type == ElementType::WIRE) {
        clickedWire(e.wire, add);
    } else if (e.type == ElementType::PIP) {
        clickedPip(e.pip, add);
    }
}

void FPGAViewWidget::updateHover(int x, int y)
{
    if (picking_)
        return; // (nothing hovered while picking)
    auto world = mouseToWorldCoordinates(x, y);
    std::vector<PickedElement> cands = pickCandidates(world.x(), world.y());
    if (cands != hoverCandidates_) {
        hoverCandidates_ = std::move(cands);
        hoverIndex_ = 0;
    }
    {
        QMutexLocker locked(&rendererArgsLock_);
        rendererArgs_->x = x;
        rendererArgs_->y = y;
    }
    applyHover();
}

void FPGAViewWidget::applyHover(bool immediate)
{
    if (hoverCandidates_.empty()) {
        QMutexLocker locked(&rendererArgsLock_);
        rendererArgs_->hoveredDecal = DecalXY();
        rendererArgs_->overlayHoverBad = DecalXY();
        rendererArgs_->changed = true;
        rendererArgs_->hintText = "";
        pokeRenderer();
        return;
    }
    hoverIndex_ = std::max(0, std::min(hoverIndex_, int(hoverCandidates_.size()) - 1));
    const PickedElement &e = hoverCandidates_.at(hoverIndex_);
    std::string text = elementText(e, true);
    if (hoverCandidates_.size() > 1)
        text += "\n[" + std::to_string(hoverIndex_ + 1) + "/" + std::to_string(hoverCandidates_.size()) +
                " here: Tab / click again for the next, right-click for a list]";
    DecalXY decal = e.decal(ctx_);
    // Placing the armed cell: what a click on the bel here would do (the bel the click would use)
    DecalXY hoverBad;
    if (armedCell_ != IdString()) {
        BelId bel = candidateBel(hoverCandidates_);
        if (bel != BelId()) {
            if (bel != hoverCheckedBel_) {
                hoverCheckedBel_ = bel;
                hoverCheck_ = placementHint(bel, hoverCheckFailed_);
            }
            text += "\n" + hoverCheck_;
            if (hoverCheckFailed_)
                hoverBad = ctx_->getBelDecal(bel);
        }
    }
    {
        QMutexLocker locked(&rendererArgsLock_);
        rendererArgs_->overlayHoverBad = hoverBad;
        if (immediate)
            hoverSince_ = clock_.elapsed() - TOOLTIP_DELAY_MS;
        else if (!(rendererArgs_->hoveredDecal == decal))
            hoverSince_ = clock_.elapsed();
        rendererArgs_->hoveredDecal = decal;
        rendererArgs_->changed = true;
        rendererArgs_->hintText = text;
    }
    pokeRenderer();
    update();
}

void FPGAViewWidget::showSelectMenu(const QPoint &pos)
{
    updateHover(pos.x(), pos.y());
    if (hoverCandidates_.empty())
        return;
    std::vector<PickedElement> cands = hoverCandidates_;
    QMenu menu(this);
    QAction *title = menu.addAction(QString("%1 element(s) here").arg(cands.size()));
    title->setEnabled(false);
    menu.addSeparator();
    int shown = std::min(int(cands.size()), int(MENU_MAX_CANDIDATES));
    for (int i = 0; i < shown; i++) {
        QAction *action = menu.addAction(QString("Select ") + QString::fromStdString(elementText(cands[i], false)));
        connect(action, &QAction::triggered, this, [this, cands, i] {
            emitClicked(cands[i], QApplication::keyboardModifiers().testFlag(Qt::ControlModifier));
        });
        connect(action, &QAction::hovered, this, [this, cands, i] {
            if (hoverCandidates_ == cands) {
                hoverIndex_ = i;
                applyHover(true);
            }
        });
    }
    if (int(cands.size()) > shown) {
        QAction *more = menu.addAction(QString("... %1 more").arg(cands.size() - shown));
        more->setEnabled(false);
    }
    BelId bel = candidateBel(cands);
    if (bel != BelId())
        addBelEditActions(menu, bel);
    menu.exec(mapToGlobal(pos));
}

void FPGAViewWidget::mousePressEvent(QMouseEvent *event)
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse)
        return;

    bool shift = QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    bool ctrl = QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
    bool btn_right = event->buttons() & Qt::RightButton;
    bool btn_mid = event->buttons() & Qt::MiddleButton;
    bool btn_left = event->buttons() & Qt::LeftButton;

    if (btn_right || btn_mid || (btn_left && shift)) {
        lastDragPos_ = event->pos();
    }
    if (picking_ && btn_left && !shift) {
        auto world = mouseToWorldCoordinates(EVENT_POS(event).x(), EVENT_POS(event).y());
        Q_EMIT picked(PICK_PRESS, world.x(), world.y());
        return;
    }
    if (event->button() == Qt::RightButton) {
        rightPressPos_ = event->pos();
        rightDragged_ = false;
    }
    if (btn_left && !shift) {
        QPoint pos = event->pos();
        auto world = mouseToWorldCoordinates(EVENT_POS(event).x(), EVENT_POS(event).y());
        std::vector<PickedElement> cands = pickCandidates(world.x(), world.y());
        if (armedCell_ != IdString()) {
            // Placing: the click goes to the bel here, the selection stays as it is
            BelId bel = candidateBel(cands);
            if (bel == BelId()) {
                if (editStack_)
                    editStack_->notify(QString("Placing %1: click on a bel (Esc to cancel)")
                                               .arg(armedCell_.c_str(ctx_)));
                return;
            }
            placeArmedAt(bel);
            return;
        }
        if (cands.empty()) {
            hoverCandidates_.clear();
            clickCycleArmed_ = false;
            applyHover();
            // If we clicked on empty space and aren't holding down ctrl,
            // clear the selection.
            if (!ctrl) {
                QMutexLocker locked(&rendererArgsLock_);
                rendererArgs_->selectedDecals.clear();
                rendererArgs_->changed = true;
                pokeRenderer();
            }
            return;
        }

        if (cands != hoverCandidates_) {
            hoverCandidates_ = std::move(cands);
            hoverIndex_ = 0;
        } else if (clickCycleArmed_ && (pos - lastClickPos_).manhattanLength() <= 3) {
            // Clicked again at the same spot: the next element there
            hoverIndex_ = (hoverIndex_ + 1) % int(hoverCandidates_.size());
        }
        clickCycleArmed_ = true;
        lastClickPos_ = pos;
        applyHover(true);
        emitClicked(hoverCandidates_.at(hoverIndex_), ctrl);
    }
}

void FPGAViewWidget::mouseMoveEvent(QMouseEvent *event)
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse)
        return;

    bool shift = QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    bool btn_right = event->buttons() & Qt::RightButton;
    bool btn_mid = event->buttons() & Qt::MiddleButton;
    bool btn_left = event->buttons() & Qt::LeftButton;

    if (btn_right || btn_mid || (btn_left && shift)) {
        const int dx = EVENT_POS(event).x() - lastDragPos_.x();
        const int dy = EVENT_POS(event).y() - lastDragPos_.y();
        lastDragPos_ = event->pos();

        auto world = mouseToWorldDimensions(dx, dy);
        viewMove_.translate(world.x(), -world.y());

        if ((event->pos() - rightPressPos_).manhattanLength() > 3)
            rightDragged_ = true;
        update();
        return;
    }

    if (picking_) {
        auto world = mouseToWorldCoordinates(EVENT_POS(event).x(), EVENT_POS(event).y());
        Q_EMIT picked(btn_left ? PICK_DRAG : PICK_MOVE, world.x(), world.y());
        update();
        return;
    }

    if ((event->pos() - lastClickPos_).manhattanLength() > 3)
        clickCycleArmed_ = false;
    updateHover(EVENT_POS(event).x(), EVENT_POS(event).y());
}

void FPGAViewWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (picking_) {
        if (event->button() == Qt::LeftButton) {
            auto world = mouseToWorldCoordinates(EVENT_POS(event).x(), EVENT_POS(event).y());
            Q_EMIT picked(PICK_RELEASE, world.x(), world.y());
        }
        return;
    }
    // A right click without dragging (dragging pans the view): choose among the elements here
    if (event->button() == Qt::RightButton && !rightDragged_ &&
        (event->pos() - rightPressPos_).manhattanLength() <= 3) {
        ImGuiIO &io = ImGui::GetIO();
        if (io.WantCaptureMouse)
            return;
        showSelectMenu(event->pos());
    }
}

void FPGAViewWidget::keyPressEvent(QKeyEvent *event)
{
    if (picking_ && event->key() == Qt::Key_Escape) {
        setPicking(false);
        Q_EMIT pickCanceled();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void FPGAViewWidget::setPicking(bool on)
{
    picking_ = on;
    if (on) {
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::StrongFocus);
        setFocus();
        // (nothing hovered while picking)
        QMutexLocker locked(&rendererArgsLock_);
        rendererArgs_->hoveredDecal = DecalXY();
        rendererArgs_->hintText = "";
        rendererArgs_->changed = true;
    } else
        unsetCursor();
    pokeRenderer();
    update();
}

void FPGAViewWidget::setOverlays(std::vector<Overlay> overlays)
{
    overlays_ = std::move(overlays);
    update();
}

void FPGAViewWidget::drawOverlays()
{
    if (overlays_.empty())
        return;
    // World to screen: the view maps the plane linearly
    QVector4D a = mouseToWorldCoordinates(0, 0), b = mouseToWorldCoordinates(width(), height());
    if (a.x() == b.x() || a.y() == b.y())
        return;
    auto screen = [&](float x, float y) {
        return ImVec2((x - a.x()) / (b.x() - a.x()) * width(), (y - a.y()) / (b.y() - a.y()) * height());
    };
    ImDrawList *list = ImGui::GetOverlayDrawList();
    for (auto &o : overlays_) {
        ImVec2 p0 = screen(o.x0, o.y0), p1 = screen(o.x1, o.y1);
        ImVec2 min(std::min(p0.x, p1.x), std::min(p0.y, p1.y)), max(std::max(p0.x, p1.x), std::max(p0.y, p1.y));
        if (max.x < 0 || max.y < 0 || min.x > width() || min.y > height())
            continue;
        QColor c = o.color;
        if (o.fill)
            list->AddRectFilled(min, max, IM_COL32(c.red(), c.green(), c.blue(), c.alpha()));
        else
            list->AddRect(min, max, IM_COL32(c.red(), c.green(), c.blue(), c.alpha()), 0.0f, 0, 2.0f);
        if (!o.label.isEmpty() && max.x - min.x > 20) {
            QByteArray text = o.label.toUtf8();
            list->AddText(ImVec2(min.x + 4, min.y + 3), IM_COL32(c.red(), c.green(), c.blue(), 255),
                          text.constData());
        }
    }
}

bool FPGAViewWidget::event(QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) &&
            !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            // Do not move the focus away: cycle the hovered element
            int n = int(hoverCandidates_.size());
            if (n > 1) {
                bool back = ke->key() == Qt::Key_Backtab || (ke->modifiers() & Qt::ShiftModifier);
                hoverIndex_ = (hoverIndex_ + (back ? n - 1 : 1)) % n;
                applyHover(true);
            }
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

// Invert the projection matrix to calculate screen/mouse to world/grid
// coordinates.
QVector4D FPGAViewWidget::mouseToWorldCoordinates(int x, int y)
{
    auto projection = getProjection();

    QMatrix4x4 vp;
    vp.viewport(0, 0, width(), height());

    QVector4D vec(x, y, 1, 1);
    vec = vp.inverted() * vec;
    vec = projection.inverted() * QVector4D(vec.x(), vec.y(), -1, 1);

    // Hic sunt dracones.
    // TODO(q3k): grab a book, remind yourself linear algebra and undo this
    // operation properly.
    QVector3D ray = vec.toVector3DAffine();
    ray.normalize();
    ray.setX((ray.x() / -ray.z()) * zoom_);
    ray.setY((ray.y() / ray.z()) * zoom_);
    ray.setZ(1.0);

    vec = viewMove_.inverted() * QVector4D(ray.x(), ray.y(), ray.z(), 1.0);
    vec.setZ(0);

    return vec;
}

QVector4D FPGAViewWidget::mouseToWorldDimensions(float x, float y)
{
    QMatrix4x4 p = getProjection();
    p.translate(0.0f, 0.0f, -zoom_);
    QVector2D unit = p.map(QVector4D(1, 1, 0, 1)).toVector2DAffine();

    float sx = (((float)x) / (width() / 2));
    float sy = (((float)y) / (height() / 2));
    return QVector4D(sx / unit.x(), sy / unit.y(), 0, 1);
}

void FPGAViewWidget::wheelEvent(QWheelEvent *event)
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse)
        return;

    QPoint degree = event->angleDelta() / 8;

    if (!degree.isNull()) {
        zoom(degree.y());
        // what is under the mouse now
        updateHover(event->position().x(), event->position().y());
    }
}

void FPGAViewWidget::zoom(int level)
{
    if (zoom_ < zoomLvl1_) {
        zoom_ -= level / 500.0;
    } else if (zoom_ < zoomLvl2_) {
        zoom_ -= level / 100.0;
    } else {
        zoom_ -= level / 10.0;
    }

    if (zoom_ < zoomNear_)
        zoom_ = zoomNear_;
    else if (zoom_ > zoomFar_)
        zoom_ = zoomFar_;
    update();
}

void FPGAViewWidget::clampZoom()
{
    if (zoom_ < zoomNear_)
        zoom_ = zoomNear_;
    else if (zoom_ > zoomFar_)
        zoom_ = zoomFar_;
}

void FPGAViewWidget::zoomIn() { zoom(10); }

void FPGAViewWidget::zoomOut() { zoom(-10); }

void FPGAViewWidget::zoomToBB(const PickQuadTree::BoundingBox &bb, float margin, bool clamp)
{
    if (fabs(bb.w()) < 0.00005 && fabs(bb.h()) < 0.00005)
        return;

    viewMove_.setToIdentity();
    viewMove_.translate(-(bb.x0() + bb.w() / 2), -(bb.y0() + bb.h() / 2));

    // Our FOV is π/2, so distance for camera to see a plane of width H is H/2.
    // We add 1 unit to cover half a unit of extra space around.
    float distance_w = bb.w() / 2 + margin;
    float distance_h = bb.h() / 2 + margin;
    zoom_ = std::max(distance_w, distance_h);
    if (clamp)
        clampZoom();
}

QRectF FPGAViewWidget::viewArea()
{
    auto r = visibleRect();
    return QRectF(QPointF(r.x0(), r.y0()), QPointF(r.x1(), r.y1()));
}

void FPGAViewWidget::zoomToArea(float x0, float y0, float x1, float y1)
{
    PickQuadTree::BoundingBox bb(x0, y0, x1, y1);
    bb.fixup();
    zoomToBB(bb, 0, true);
    update();
}

void FPGAViewWidget::zoomSelected()
{
    // The renderer zooms at the end of its next pass: a selection that has just changed is drawn
    // only then, and the zoom is to its bounding box
    {
        QMutexLocker lock(&rendererArgsLock_);
        rendererArgs_->zoomSelected = true;
    }
    pokeRenderer();
}

void FPGAViewWidget::zoomOutbound()
{
    {
        QMutexLocker lock(&rendererDataLock_);
        zoomToBB(rendererData_->bbGlobal, 1.0f, false);
        zoomFar_ = zoom_;
    }
}

void FPGAViewWidget::leaveEvent(QEvent *event)
{
    hoverCandidates_.clear();
    clickCycleArmed_ = false;
    QMutexLocker locked(&rendererArgsLock_);
    rendererArgs_->hoveredDecal = DecalXY();
    rendererArgs_->changed = true;
    rendererArgs_->hintText = "";
    pokeRenderer();
}

BelId FPGAViewWidget::candidateBel(const std::vector<PickedElement> &cands)
{
    if (cands == hoverCandidates_ && hoverIndex_ >= 0 && hoverIndex_ < int(cands.size()) &&
        cands.at(hoverIndex_).type == ElementType::BEL)
        return cands.at(hoverIndex_).bel;
    for (auto &c : cands)
        if (c.type == ElementType::BEL)
            return c.bel;
    return BelId();
}

void FPGAViewWidget::armCell(IdString cell)
{
    if (cell == IdString()) {
        disarmPlacement();
        return;
    }
    QString why;
    if (editStack_ == nullptr || ctx_ == nullptr || !editStack_->canEdit(&why)) {
        if (editStack_)
            editStack_->notify("Cannot place: " + why);
        return;
    }
    auto fnd = ctx_->cells.find(cell);
    if (fnd == ctx_->cells.end())
        return;
    CellInfo *ci = fnd->second.get();
    if (ci->cluster != ClusterId())
        ci = ctx_->getClusterRootCell(ci->cluster);
    if (ci->name == armedCell_)
        return;
    armedCell_ = ci->name;
    size_t group = 1;
    {
        std::lock_guard<std::mutex> lock_ui(ctx_->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx_->mutex);
        armedBels_.clear();
        for (BelId bel : ctx_->getBels())
            if (ctx_->isValidBelForCellType(ci->type, bel) && ci->testRegion(bel))
                armedBels_.push_back(bel);
        if (ci->cluster != ClusterId())
            group = placementGroup(ctx_, ci).size();
    }
    hoverCheckedBel_ = BelId();
    QString what = QString::fromStdString(ci->name.str(ctx_));
    if (group > 1)
        what = QString("cluster %1 (%2 cells)").arg(what).arg(group);
    Q_EMIT placementStatus(QString("%1 %2 [%3]: click a bel (green: free, orange: swap, red: blocked) - Esc "
                                   "cancels")
                                   .arg(ci->bel != BelId() ? "Moving" : "Placing")
                                   .arg(what)
                                   .arg(ci->type.c_str(ctx_)));
    updateOverlay();
    applyHover();
}

void FPGAViewWidget::disarmPlacement()
{
    if (armedCell_ == IdString())
        return;
    armedCell_ = IdString();
    armedBels_.clear();
    hoverCheckedBel_ = BelId();
    Q_EMIT placementStatus(QString());
    updateOverlay();
    applyHover();
}

void FPGAViewWidget::updateOverlay()
{
    std::vector<DecalXY> sets[OVERLAY_SETS];
    hoverCheckedBel_ = BelId();
    if (armedCell_ != IdString() && ctx_ != nullptr) {
        std::lock_guard<std::mutex> lock_ui(ctx_->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx_->mutex);
        auto fnd = ctx_->cells.find(armedCell_);
        if (fnd != ctx_->cells.end()) {
            CellInfo *ci = fnd->second.get();
            bool clustered = ci->cluster != ClusterId();
            for (BelId bel : armedBels_) {
                if (bel == ci->bel)
                    continue; // shown by the selection, if selected
                CellInfo *bound = ctx_->getBoundBelCell(bel);
                int set;
                if (bound != nullptr) {
                    if (clustered && bound->cluster == ci->cluster)
                        continue;
                    set = (clustered || bound->cluster != ClusterId() || isCellLocked(bound)) ? OVERLAY_BLOCKED
                                                                                              : OVERLAY_SWAP;
                } else {
                    set = ctx_->checkBelAvail(bel) ? OVERLAY_FREE : OVERLAY_BLOCKED;
                }
                sets[set].push_back(ctx_->getBelDecal(bel));
            }
        }
    }
    {
        QMutexLocker locker(&rendererArgsLock_);
        for (int i = 0; i < OVERLAY_SETS; i++)
            rendererArgs_->overlayDecals[i] = std::move(sets[i]);
        rendererArgs_->overlayChanged = true;
        rendererArgs_->overlayHoverBad = DecalXY();
        rendererArgs_->changed = true;
    }
    pokeRenderer();
}

std::string FPGAViewWidget::placementHint(BelId bel, bool &failed)
{
    failed = false;
    if (editStack_ == nullptr || ctx_ == nullptr || armedCell_ == IdString())
        return "";
    auto fnd = ctx_->cells.find(armedCell_);
    if (fnd == ctx_->cells.end())
        return "";
    CellInfo *ci = fnd->second.get();
    if (!ctx_->isValidBelForCellType(ci->type, bel))
        return "(not a site for " + ci->type.str(ctx_) + ")";
    if (ci->bel == bel)
        return "(" + ci->name.str(ctx_) + " is here)";
    IdString cell = armedCell_;
    std::string err = editStack_->dryRun([&](EditBatch &b) { return addPlaceCell(b, cell, bel); });
    if (!err.empty()) {
        failed = true;
        return "Cannot place here: " + err;
    }
    return "Click: " + describePlaceCell(ctx_, cell, bel).toStdString();
}

void FPGAViewWidget::placeArmedAt(BelId bel)
{
    if (editStack_ == nullptr || ctx_ == nullptr || armedCell_ == IdString())
        return;
    IdString cell = armedCell_;
    auto fnd = ctx_->cells.find(cell);
    if (fnd == ctx_->cells.end()) {
        disarmPlacement();
        return;
    }
    if (fnd->second->bel == bel) {
        editStack_->notify(QString("%1 is already at %2").arg(cell.c_str(ctx_)).arg(ctx_->nameOfBel(bel)));
        disarmPlacement();
        return;
    }
    QString text = describePlaceCell(ctx_, cell, bel);
    std::string err = editStack_->run(text, [&](EditBatch &b) { return addPlaceCell(b, cell, bel); });
    if (!err.empty())
        return; // reason shown in the status bar; stay armed
    disarmPlacement();
    // Select the cell where it is now
    fnd = ctx_->cells.find(cell);
    if (fnd != ctx_->cells.end() && fnd->second->bel != BelId())
        emitClicked(PickedElement::fromBel(fnd->second->bel, 0, 0), false);
}

void FPGAViewWidget::addBelEditActions(QMenu &menu, BelId bel)
{
    if (editStack_ == nullptr || ctx_ == nullptr || editStack_->busy())
        return;
    QString why;
    bool can_edit = editStack_->canEdit(&why);
    menu.setToolTipsVisible(true);
    auto editAction = [&](const QString &text) {
        QAction *action = menu.addAction(text);
        action->setEnabled(can_edit);
        if (!can_edit)
            action->setToolTip(why);
        return action;
    };
    menu.addSeparator();
    QAction *title = menu.addAction(QString("Bel %1").arg(ctx_->nameOfBel(bel)));
    title->setEnabled(false);
    if (armedCell_ != IdString()) {
        QAction *place = editAction(describePlaceCell(ctx_, armedCell_, bel) + "\tclick");
        connect(place, &QAction::triggered, this, [this, bel] { placeArmedAt(bel); });
    }
    CellInfo *bound = ctx_->getBoundBelCell(bel);
    if (bound == nullptr)
        return;
    IdString name = bound->name;
    std::vector<IdString> group;
    for (CellInfo *c : placementGroup(ctx_, bound))
        group.push_back(c->name);
    QString what = QString::fromStdString(name.str(ctx_));
    if (group.size() > 1)
        what = QString("cluster %1 (%2 cells)").arg(group.front().c_str(ctx_)).arg(group.size());

    QAction *move = editAction(QString("Move %1...\tP").arg(what));
    connect(move, &QAction::triggered, this, [this, name] { armCell(name); });

    QAction *unplace = editAction(QString("Unplace %1").arg(what));
    connect(unplace, &QAction::triggered, this, [this, group, what] {
        editStack_->run("Unplace " + what, [&](EditBatch &b) -> std::string {
            for (IdString c : group) {
                auto fnd = ctx_->cells.find(c);
                if (fnd == ctx_->cells.end() || fnd->second->bel == BelId())
                    continue;
                std::string err = b.add(std::make_unique<UnbindBelCmd>(fnd->second->bel));
                if (!err.empty())
                    return err;
            }
            return {};
        });
    });

    bool locked = isCellLocked(bound);
    QAction *lock = editAction(QString("%1 %2 [%3]")
                                       .arg(locked ? "Unlock" : "Lock")
                                       .arg(what)
                                       .arg(ElementInfo::strengthName(bound->belStrength)));
    lock->setStatusTip(locked ? "Set the placement strength to STRONG: placers and swaps may move it"
                              : "Set the placement strength to USER: kept by the placers, never swapped away");
    connect(lock, &QAction::triggered, this, [this, group, what, locked] {
        PlaceStrength strength = locked ? STRENGTH_MANUAL : STRENGTH_MANUAL_LOCKED;
        editStack_->run((locked ? "Unlock " : "Lock ") + what, [&](EditBatch &b) -> std::string {
            for (IdString c : group) {
                std::string err = b.add(std::make_unique<SetCellStrengthCmd>(c, strength));
                if (!err.empty())
                    return err;
            }
            return {};
        });
    });

    QAction *show = menu.addAction(QString("Show %1 in tree").arg(name.c_str(ctx_)));
    connect(show, &QAction::triggered, this, [this, name] { Q_EMIT showCellRequested(name); });
}

void FPGAViewWidget::update_vbos()
{
    lineShader_.update_vbos(GraphicElement::STYLE_GRID, rendererData_->gfxGrid);

    for (int layer = 0; layer < ARCH_LAYERS; layer++) {
        const ArchLayer &data = layer == 0 ? rendererData_->arch : rendererData_->view;
        for (int range = 0; range < LOD_RANGES; range++) {
            for (int style = 0; style < ARCH_STYLES; style++) {
                lineShader_.update_vbos(archLineSlot(layer, range, style), data.gfx[range][style]);
                textShader_.update_vbos(archTextSlot(layer, range, style), data.text[range][style]);
            }
        }
    }

    for (int i = 0; i < 8; i++) {
        GraphicElement::style_t style = (GraphicElement::style_t)(GraphicElement::STYLE_HIGHLIGHTED0 + i);
        lineShader_.update_vbos(style, rendererData_->gfxHighlighted[i]);
    }

    for (int i = 0; i < OVERLAY_SLOTS; i++)
        lineShader_.update_vbos(overlaySlot(i), rendererData_->gfxOverlay[i]);

    lineShader_.update_vbos(GraphicElement::STYLE_SELECTED, rendererData_->gfxSelected);
    lineShader_.update_vbos(GraphicElement::STYLE_HOVER, rendererData_->gfxHovered);
}

NEXTPNR_NAMESPACE_END
