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

#include <QElapsedTimer>
#include <QMainWindow>
#include <QMenu>
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

class EditStack;

class FPGAViewWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

  public:
    FPGAViewWidget(QWidget *parent = 0);
    ~FPGAViewWidget();
    // Command layer for manual edits (see editcommands.h); owned by BaseMainWindow.
    void setEditStack(EditStack *stack);
    // The cell armed for placement by click (empty if none)
    IdString armedCell() const { return armedCell_; }

  protected:
    // Qt callbacks.
    void initializeGL() Q_DECL_OVERRIDE;
    void paintGL() Q_DECL_OVERRIDE;
    void resizeGL(int width, int height) Q_DECL_OVERRIDE;
    void mousePressEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    void mouseMoveEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    void mouseReleaseEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    // Tab / Shift+Tab cycle through the elements under the cursor
    bool event(QEvent *event) Q_DECL_OVERRIDE;
    void wheelEvent(QWheelEvent *event) Q_DECL_OVERRIDE;
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) Q_DECL_OVERRIDE;

  public:
    // Rectangles drawn over the view (world coordinates), with a label in the upper left corner
    struct Overlay
    {
        float x0, y0, x1, y1;
        QColor color;
        QString label;
        bool fill = false;
    };
    void setOverlays(std::vector<Overlay> overlays);
    // While picking, the left mouse button picks positions (the picked signal, with a PickEvent)
    // instead of selecting; Escape cancels (pickCanceled)
    enum PickEvent
    {
        PICK_MOVE,
        PICK_PRESS,
        PICK_DRAG,
        PICK_RELEASE
    };
    void setPicking(bool on);
    bool isPicking() const { return picking_; }
    // The area shown, in world coordinates, and showing an area
    QRectF viewArea();
    void zoomToArea(float x0, float y0, float x1, float y1);

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
    // Placement by click (edit mode): arm a cell (a cluster member arms its cluster root); while
    // armed, the next left click on a bel places / moves / swaps it there (see addPlaceCell) and
    // the bels it may go to are coloured. An empty name disarms.
    void armCell(IdString cell);
    void disarmPlacement();
    // Centre the view on the bounding box of these bels (keeping the zoom)
    void centerOnBels(std::vector<BelId> bels);
  Q_SIGNALS:
    void clickedBel(BelId bel, bool add);
    void clickedWire(WireId wire, bool add);
    void clickedPip(PipId pip, bool add);
    void picked(int event, float x, float y);
    void pickCanceled();
    // Status text while a cell is armed for placement, empty when disarmed
    void placementStatus(QString text);
    // "Show cell in tree" from the context menu
    void showCellRequested(IdString cell);
    // Arrow key in edit mode: move the selection one step in this direction (grid units)
    void nudgeRequested(int dx, int dy);

  private:
    std::vector<Overlay> overlays_;
    bool picking_ = false;
    void drawOverlays();

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

        PickedElement(const PickedElement &other) : type(other.type), x(other.x), y(other.y)
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
        bool operator==(const PickedElement &other) const
        {
            if (type != other.type)
                return false;
            switch (type) {
            case ElementType::BEL:
                return bel == other.bel;
            case ElementType::WIRE:
                return wire == other.wire;
            case ElementType::PIP:
                return pip == other.pip;
            case ElementType::GROUP:
                return group == other.group;
            default:
                return false;
            }
        }
        bool operator!=(const PickedElement &other) const { return !(*this == other); }

        // Distance to the elements visible at `scale` (pixels per coordinate unit), or -1
        float distance(Context *ctx, float wx, float wy, float scale) const;
        // The distance in coordinate units from the point to the closest graphics visible at
        // `scale` (0 inside a box), and the size of those graphics (area of their bounding box,
        // thin dimensions counted as `thin`). False if nothing is visible.
        bool hitMetrics(Context *ctx, float wx, float wy, float scale, float thin, float &dist,
                        float &size) const;
    };
    using PickQuadTree = QuadTree<float, PickedElement>;

    // The elements under the cursor, the one hovered (and picked by a click) among them
    std::vector<PickedElement> hoverCandidates_;
    int hoverIndex_ = 0;
    // Repeated clicks at (nearly) the same spot cycle through the candidates
    QPoint lastClickPos_;
    bool clickCycleArmed_ = false;
    // Right button: a click without dragging opens the "Select ..." menu
    QPoint rightPressPos_;
    bool rightDragged_ = false;
    // Since when (in ms of clock_) the hovered element is hovered, for the tooltip delay; under
    // rendererArgsLock_
    QElapsedTimer clock_;
    qint64 hoverSince_ = 0;
    static constexpr int TOOLTIP_DELAY_MS = 300;
    static constexpr int MENU_MAX_CANDIDATES = 20;

    std::vector<PickedElement> pickCandidates(float worldx, float worldy);
    // Update hoverCandidates_ for the cursor at (x, y); keep hoverIndex_ if the candidates are the same
    void updateHover(int x, int y);
    // Make hoverCandidates_[hoverIndex_] the hovered element (decal, tooltip); the tooltip at once
    // if immediate (cycling), else after a delay when the hovered element changed
    void applyHover(bool immediate = false);
    std::string elementText(const PickedElement &e, bool multiline);
    void emitClicked(const PickedElement &e, bool add);
    void showSelectMenu(const QPoint &pos);
    // Edit actions on a bel and the cell bound to it, for the right-click menu
    void addBelEditActions(QMenu &menu, BelId bel);

    // Placement by click
    IdString armedCell_;
    // The bels the armed cell type may go to (type and region)
    std::vector<BelId> armedBels_;
    // The result of a trial placement of the armed cell at the hovered bel (computed once per bel)
    BelId hoverCheckedBel_;
    std::string hoverCheck_;
    bool hoverCheckFailed_ = false;
    // Recompute the valid-site overlay from the current design state
    void updateOverlay();
    // Tooltip line for placing the armed cell at bel (trial run), failed set if it would be refused
    std::string placementHint(BelId bel, bool &failed);
    // Returns "" on success, else the reason (also shown in the status bar)
    std::string placeArmedAt(BelId bel);
    // Drag & drop (edit mode): a left press on a bel with an unlocked placed cell, then moving the
    // mouse more than DRAG_DISTANCE pixels, arms that cell while dragging; releasing over a bel
    // places it there (as a click would while armed), releasing elsewhere cancels
    bool dragPending_ = false, dragging_ = false;
    QPoint dragStartPos_;
    IdString dragCell_;
    BelId dragFromBel_;
    static constexpr int DRAG_DISTANCE = 6;
    // The bel the armed cluster root goes to when the dragged cell is dropped at bel (the same
    // offset for the whole cluster when a non-root member is dragged)
    BelId dropTarget(BelId bel);
    void finishDrag(const QPoint &pos);
    // Bel among the candidates under the cursor: the hovered one if it is a bel, else the first
    BelId candidateBel(const std::vector<PickedElement> &cands);
    // Valid-site overlay: free, occupied by a cell that would be swapped, blocked (locked cell,
    // cluster, bel unavailable); and the hovered bel if the trial placement there fails
    static constexpr int OVERLAY_FREE = 0, OVERLAY_SWAP = 1, OVERLAY_BLOCKED = 2, OVERLAY_HOVER_BAD = 3;
    static constexpr int OVERLAY_SETS = 3, OVERLAY_SLOTS = 4;
    // Line shader slots of the overlay, after those of the Arch graphics
    static int overlaySlot(int i) { return archLineSlot(ARCH_LAYERS, 0, 0) + i; }

    Context *ctx_;
    EditStack *editStack_ = nullptr;
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
    // The styles of Arch graphics, in the order they are drawn: STYLE_FRAME, STYLE_INACTIVE,
    // STYLE_ROUTING and STYLE_ACTIVE
    static constexpr int ARCH_STYLES = 4;
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
        QColor routing;
        QColor selected;
        QColor hovered;
        QColor highlight[8];
        QColor overlay[OVERLAY_SLOTS];
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
        // Zoom to the selection once it is drawn (its bounding box comes from drawing it)
        bool zoomSelected = false;
        // The area and the finest level of detail to draw the in-view layer for, and whether they
        // changed
        PickQuadTree::BoundingBox viewRect;
        int viewLevel = LOD_LEVELS - 1;
        bool viewChanged = false;
        // Valid-site overlay of the armed cell (OVERLAY_FREE ... OVERLAY_BLOCKED), and whether it
        // changed (rendered on its own: it may be large)
        std::vector<DecalXY> overlayDecals[OVERLAY_SETS];
        bool overlayChanged = false;
        // The hovered bel when placing the armed cell there would fail (rendered with `changed`)
        DecalXY overlayHoverBad;
        // Hint text
        std::string hintText;
        // cursor pos
        int x = 0, y = 0;
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
        LineShaderData gfxOverlay[OVERLAY_SLOTS];
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
    QVector4D mouseToWorldCoordinates(int x, int y);
    QVector4D mouseToWorldDimensions(float x, float y);
    QMatrix4x4 getProjection(void);
    void update_vbos();
};

NEXTPNR_NAMESPACE_END

#endif
