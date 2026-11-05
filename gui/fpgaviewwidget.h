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

#ifndef MAPGLWIDGET_H
#define MAPGLWIDGET_H

#include <QMainWindow>
#include <QMutex>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>
#include <boost/optional.hpp>

#include "designwidget.h"
#include "lineshader.h"
#include "nextpnr.h"
#include "quadtree.h"
#include "textshader.h"

NEXTPNR_NAMESPACE_BEGIN

class PeriodicRunner : public QThread
{
    Q_OBJECT
  private:
    QMutex mutex_;
    QWaitCondition condition_;
    bool abort_;
    std::function<void()> target_;
    QTimer timer_;

  public:
    explicit PeriodicRunner(QObject *parent, std::function<void()> target)
            : QThread(parent), abort_(false), target_(target), timer_(this)
    {
        connect(&timer_, &QTimer::timeout, this, &PeriodicRunner::poke);
    }

    void run(void) override
    {
        for (;;) {
            QMutexLocker locker(&mutex_);
            condition_.wait(&mutex_);
            if (abort_) {
                return;
            }
            target_();
        }
    }

    void startTimer(int msecs) { timer_.start(msecs); }

    ~PeriodicRunner()
    {
        {
            QMutexLocker locker(&mutex_);
            abort_ = true;
            condition_.wakeOne();
        }
        wait();
    }

    void poke(void) { condition_.wakeOne(); }
};

class FPGAViewWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

  public:
    FPGAViewWidget(QWidget *parent = 0);
    ~FPGAViewWidget();

  protected:
    // Qt callbacks.
    void initializeGL() Q_DECL_OVERRIDE;
    void paintGL() Q_DECL_OVERRIDE;
    void resizeGL(int width, int height) Q_DECL_OVERRIDE;
    void mousePressEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    void mouseMoveEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    void wheelEvent(QWheelEvent *event) Q_DECL_OVERRIDE;
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;
    void leaveEvent(QEvent *event) override;

  public Q_SLOTS:
    void newContext(Context *ctx);
    void onSelectedArchItem(std::vector<DecalXY> decals, bool keep);
    void onHighlightGroupChanged(std::vector<DecalXY> decals, int group);
    void onHoverItemChanged(DecalXY decal);
    void pokeRenderer(void);
    void zoomIn();
    void zoomOut();
    void zoomSelected();
    void zoomOutbound();
    void enableDisableDecals(bool bels, bool wires, bool pips, bool groups);
    void movieStart(QString dir, long frameSkip, bool skipSame);
    void movieStop();
  Q_SIGNALS:
    void clickedBel(BelId bel, bool add);
    void clickedWire(WireId wire, bool add);
    void clickedPip(PipId pip, bool add);

  private:
    QString movieDir;
    long currentMovieFrame;
    long currentFrameSkip;
    long movieCounter;
    bool movieSaving;
    bool movieSkipSame;
    QImage movieLastImage;
    const float zoomNear_ = 0.05f; // do not zoom closer than this
    float zoomFar_ = 10.0f;        // do not zoom further than this
    const float zoomLvl1_ = 1.0f;
    const float zoomLvl2_ = 5.0f;

    struct PickedElement
    {
        ElementType type;

        // These are not in an union (and thus this structure is very verbose
        // and somewhat heavy) because the Id types are typedef'd to StringIds
        // in the generic architecture. Once that changes (or we get an AnyId
        // construct from Arches), this should go away.
        BelId bel;
        WireId wire;
        PipId pip;
        GroupId group;

        float x, y; // Decal X and Y

        PickedElement(ElementType type, float x, float y) : type(type), x(x), y(y) {}

        static PickedElement fromBel(BelId bel, float x, float y)
        {
            PickedElement e(ElementType::BEL, x, y);
            e.bel = bel;
            return e;
        }
        static PickedElement fromWire(WireId wire, float x, float y)
        {
            PickedElement e(ElementType::WIRE, x, y);
            e.wire = wire;
            return e;
        }
        static PickedElement fromPip(PipId pip, float x, float y)
        {
            PickedElement e(ElementType::PIP, x, y);
            e.pip = pip;
            return e;
        }
        static PickedElement fromGroup(GroupId group, float x, float y)
        {
            PickedElement e(ElementType::GROUP, x, y);
            e.group = group;
            return e;
        }

        PickedElement(const PickedElement &other) : type(other.type)
        {
            switch (type) {
            case ElementType::BEL:
                bel = other.bel;
                break;
            case ElementType::WIRE:
                wire = other.wire;
                break;
            case ElementType::PIP:
                pip = other.pip;
                break;
            case ElementType::GROUP:
                group = other.group;
                break;
            default:
                NPNR_ASSERT_FALSE("Invalid ElementType");
            }
        }

        PickedElement &operator=(const PickedElement &other) = default;

        DecalXY decal(Context *ctx) const
        {
            DecalXY decal;
            switch (type) {
            case ElementType::BEL:
                decal = ctx->getBelDecal(bel);
                break;
            case ElementType::WIRE:
                decal = ctx->getWireDecal(wire);
                break;
            case ElementType::PIP:
                decal = ctx->getPipDecal(pip);
                break;
            case ElementType::GROUP:
                decal = ctx->getGroupDecal(group);
                break;
            default:
                NPNR_ASSERT_FALSE("Invalid ElementType");
            }
            return decal;
        }
        // Distance to the elements visible at `scale` (pixels per coordinate unit), or -1
        float distance(Context *ctx, float wx, float wy, float scale) const;
    };
    using PickQuadTree = QuadTree<float, PickedElement>;

    Context *ctx_;
    QTimer paintTimer_;
    std::unique_ptr<PeriodicRunner> renderRunner_;

    QPoint lastDragPos_;
    LineShader lineShader_;
    TextShader textShader_;

    // Levels of detail, in pixels per coordinate unit. An element's min_scale and max_scale are
    // rounded down to a level, and the graphics from the Arch are kept per range of levels: from
    // level lo up to, not including, level hi (LOD_LEVELS: no upper limit).
    static constexpr int LOD_LEVELS = 7;
    static constexpr float lodScale_[LOD_LEVELS] = {0, 10, 25, 60, 150, 400, 1000};
    static constexpr int LOD_RANGES = LOD_LEVELS * (LOD_LEVELS + 1);
    static int lodLevel(float scale);
    // The range of an element, or -1 if it is never drawn
    static int lodRange(const GraphicElement &el);
    static bool lodVisible(int range, float scale);
    // The styles of Arch graphics: STYLE_FRAME, STYLE_INACTIVE and STYLE_ACTIVE
    static constexpr int ARCH_STYLES = 3;
    static int archStyle(GraphicElement::style_t style);
    // The finest levels of detail, from VIEW_LEVEL on, are drawn only for the area in view (and
    // around it), and drawn again when the view leaves that area: for the whole chip, there may be
    // far too many of them to keep.
    static constexpr int VIEW_LEVEL = 4;
    static bool viewDependent(const GraphicElement &el) { return lodLevel(el.min_scale) >= VIEW_LEVEL; }
    // The Arch graphics are in two layers: those drawn regardless of the view, and those in view
    static constexpr int ARCH_LAYERS = 2;
    // Buffer slots of the Arch graphics: after the per-style slots for lines, from 0 for text
    static int archLineSlot(int layer, int range, int style)
    {
        return GraphicElement::STYLE_MAX + archTextSlot(layer, range, style);
    }
    static int archTextSlot(int layer, int range, int style)
    {
        return (layer * LOD_RANGES + range) * ARCH_STYLES + style;
    }
    int archGeneration_ = 0;
    // The area and the finest level of detail the in-view layer was last asked for (GUI thread)
    PickQuadTree::BoundingBox viewRequested_;
    int viewRequestedLevel_ = -1;
    // The area of the chip shown
    PickQuadTree::BoundingBox visibleRect();
    void requestView();
    QMatrix4x4 viewMove_;
    float zoom_;

    bool displayBel_;
    bool displayWire_;
    bool displayPip_;
    bool displayGroup_;

    struct
    {
        QColor background;
        QColor grid;
        QColor frame;
        QColor hidden;
        QColor inactive;
        QColor active;
        QColor selected;
        QColor hovered;
        QColor highlight[8];
    } colors_;

    struct RendererArgs
    {
        // Decals that he user selected.
        std::vector<DecalXY> selectedDecals;
        // Decals that the user highlighted.
        std::vector<DecalXY> highlightedDecals[8];
        // Decals that the user's mouse is hovering in.
        DecalXY hoveredDecal;
        // Whether to render the above three or skip it.
        bool changed;
        // Whether to render grid or skip it.
        bool gridChanged;

        // Flags for rendering.
        bool zoomOutbound;
        // The area and the finest level of detail to draw the in-view layer for, and whether they
        // changed
        PickQuadTree::BoundingBox viewRect;
        int viewLevel = LOD_LEVELS - 1;
        bool viewChanged = false;
        // Hint text
        std::string hintText;
        // cursor pos
        int x, y;
    };
    std::unique_ptr<RendererArgs> rendererArgs_;
    QMutex rendererArgsLock_;

    struct ArchLayer
    {
        // By range of levels of detail and style (see lodRange, archStyle)
        LineShaderData gfx[LOD_RANGES][ARCH_STYLES];
        TextShaderData text[LOD_RANGES][ARCH_STYLES];
        void setGeneration(int generation)
        {
            for (int range = 0; range < LOD_RANGES; range++)
                for (int style = 0; style < ARCH_STYLES; style++)
                    gfx[range][style].last_render = text[range][style].last_render = generation;
        }
    };

    struct RendererData
    {
        LineShaderData gfxGrid;
        // Arch graphics: those drawn regardless of the view, and those for the area viewRect only
        ArchLayer arch, view;
        PickQuadTree::BoundingBox viewRect;
        // The decals with graphics drawn only in view, with the bounding box of those graphics
        std::vector<std::pair<PickQuadTree::BoundingBox, DecalXY>> viewDecals;
        // The detail decals (see ArchAPI::getDetailDecals), with the area they cover
        std::vector<std::pair<PickQuadTree::BoundingBox, DecalXY>> detailDecals;
        LineShaderData gfxSelected;
        LineShaderData gfxHovered;
        LineShaderData gfxHighlighted[8];
        // Global bounding box of data from Arch.
        PickQuadTree::BoundingBox bbGlobal;
        // Bounding box of selected items.
        PickQuadTree::BoundingBox bbSelected;
        // Quadtree for picking objects.
        std::unique_ptr<PickQuadTree> qt;
    };
    std::unique_ptr<RendererData> rendererData_;
    QMutex rendererDataLock_;

    void clampZoom();
    void zoomToBB(const PickQuadTree::BoundingBox &bb, float margin, bool clamp);
    void zoom(int level);
    void renderLines(void);
    void renderGraphicElement(LineShaderData &out, PickQuadTree::BoundingBox &bb, const GraphicElement &el, float x,
                              float y);
    void renderDecal(LineShaderData &out, PickQuadTree::BoundingBox &bb, const DecalXY &decal);
    // Draw the graphics of a decal that are drawn regardless of the view (view false), adding the
    // bounding box of the others to viewBB, or those drawn only in view (view true), up to a level
    // of detail
    void renderArchDecal(ArchLayer &layer, PickQuadTree::BoundingBox &bb, const DecalXY &decal, bool view,
                         PickQuadTree::BoundingBox *viewBB, int maxLevel = LOD_LEVELS - 1);
    // Draw the in-view layer for an area up to a level of detail, from the decals of viewDecals in it
    void renderView(ArchLayer &layer, const std::vector<std::pair<PickQuadTree::BoundingBox, DecalXY>> &decals,
                    const PickQuadTree::BoundingBox &rect, int maxLevel);
    void populateQuadTree(RendererData *data, const DecalXY &decal, const PickedElement &element);
    boost::optional<PickedElement> pickElement(float worldx, float worldy);
    QVector4D mouseToWorldCoordinates(int x, int y);
    QVector4D mouseToWorldDimensions(float x, float y);
    QMatrix4x4 getProjection(void);
    void update_vbos();
};

NEXTPNR_NAMESPACE_END

#endif
