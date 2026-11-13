// InkPDF - the page canvas (PDF rendering + ink + notes)
#pragma once
#include "model.h"
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPdfWriter>
#include <QPageSize>
#include <QCache>

class Canvas : public QAbstractScrollArea {
public:
    QPdfDocument doc;
    QString pdfPath;
    Tool tool = Pen;
    QColor color = QColor("#d32f2f");
    float sizes[ToolCount];          // 1..60 per tool
    int stab = 3;                    // pen stabilizer 0..10 (0 = raw input)
    bool night = false, dirty = false, notesVisible = true;
    QMap<int, QVector<Stroke>> strokes;
    int curLayer = 0;
    bool layerVis[3] = {true, true, true};
    QColor defNoteColor;             // invalid = layer color
    int defTheme = 0;
    std::function<void()> changed;
    QWidget *overlay = nullptr;      // small handle kept at the top-center

    static constexpr qreal kUnit = 0.0006;  // size 1 = 0.06% of page width

    explicit Canvas(QWidget *parent = nullptr) : QAbstractScrollArea(parent) {
        setLayoutDirection(Qt::LeftToRight);
        for (float &v : sizes) v = 6;
        sizes[Marker] = 36; sizes[Highlighter] = 36; sizes[Eraser] = 16; sizes[Laser] = 14;
        cache.setMaxCost(300 * 1024);  // ~300 MB of page bitmaps (cost in KB)
        viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
        viewport()->setAttribute(Qt::WA_TabletTracking);
        viewport()->setMouseTracking(true);
        viewport()->setCursor(Qt::CrossCursor);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), [this] { viewport()->update(); });
        connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), [this] { viewport()->update(); });
        clock.start();
        laserTimer.setInterval(16);
        connect(&laserTimer, &QTimer::timeout, this, [this] { pruneLaser(); });
        autosave.setInterval(20000);
        connect(&autosave, &QTimer::timeout, this, [this] { saveIfDirty(); });
        autosave.start();
        animTimer.setInterval(16);
        connect(&animTimer, &QTimer::timeout, this, [this] { stepAnim(); });
    }

    // ------------------------------------------------------------ document
    bool load(const QString &path) {
        saveIfDirty();
        if (doc.load(path) != QPdfDocument::Error::None) return false;
        pdfPath = path;
        strokes.clear(); undoS.clear(); redoS.clear(); cache.clear();
        nextId = 1; activeId = -1; nd = ND_None; drawing = false; laserDown = false;
        loadInk();
        relayout();
        verticalScrollBar()->setValue(0);
        viewport()->update();
        return true;
    }
    void setZoom(qreal z) {
        z = qBound(0.5, z, 6.0);
        qreal fy = contentH > 0 ? (verticalScrollBar()->value() + viewport()->height() / 2.0) / contentH : 0;
        qreal fx = contentW > 0 ? (horizontalScrollBar()->value() + viewport()->width() / 2.0) / contentW : 0;
        zoom = z;
        relayout();
        verticalScrollBar()->setValue(int(fy * contentH - viewport()->height() / 2.0));
        horizontalScrollBar()->setValue(int(fx * contentW - viewport()->width() / 2.0));
        viewport()->update();
    }
    qreal zoomLevel() const { return zoom; }

    void saveIfDirty() { if (dirty) saveInk(); }
    void markDirty() { dirty = true; if (changed) changed(); }

    // ------------------------------------------------------------ undo / redo
    void undo() {
        if (Stroke *n = findNote(activeId))
            if ((n->open || n->pinned) && !n->ink.isEmpty()) {
                n->redoInk.append(n->ink.takeLast()); markDirty(); viewport()->update(); return;
            }
        if (undoS.isEmpty()) return;
        int g = undoS.last().group;
        while (!undoS.isEmpty() && undoS.last().group == g) {
            Act a = undoS.takeLast();
            auto &v = strokes[a.page];
            if (a.added) { if (a.idx < v.size()) v.remove(a.idx); }
            else v.insert(qMin(a.idx, v.size()), a.s);
            redoS.append(a);
        }
        markDirty(); viewport()->update();
    }
    void redo() {
        if (Stroke *n = findNote(activeId))
            if ((n->open || n->pinned) && !n->redoInk.isEmpty()) {
                n->ink.append(n->redoInk.takeLast()); markDirty(); viewport()->update(); return;
            }
        if (redoS.isEmpty()) return;
        int g = redoS.last().group;
        while (!redoS.isEmpty() && redoS.last().group == g) {
            Act a = redoS.takeLast();
            auto &v = strokes[a.page];
            if (a.added) v.insert(qMin(a.idx, v.size()), a.s);
            else if (a.idx < v.size()) v.remove(a.idx);
            undoS.append(a);
        }
        markDirty(); viewport()->update();
    }

    // ------------------------------------------------------------ notes api
    QVector<NoteRef> collectNotes(bool withHighlights) const {
        QVector<NoteRef> out;
        for (auto it = strokes.cbegin(); it != strokes.cend(); ++it) {
            QVector<NoteRef> pg;
            for (const Stroke &s : it.value())
                if ((s.kind == 4 || (withHighlights && (s.kind == 1 || s.kind == 5))) && !s.pts.isEmpty())
                    pg.append(NoteRef{it.key(), s.pts[0], s.kind, s.layer, s.text, s.id});
            std::sort(pg.begin(), pg.end(), [](const NoteRef &a, const NoteRef &b) { return a.pt.y() < b.pt.y(); });
            out += pg;
        }
        return out;
    }
    void gotoPoint(int page, QPointF n) {
        if (page < 0 || page >= rects.size()) return;
        const QRectF &r = rects[page];
        verticalScrollBar()->setValue(int(r.top() + n.y() * r.width() - viewport()->height() / 3.0));
        viewport()->update();
    }
    void setAllMasks(bool revealed) {
        for (auto it = strokes.begin(); it != strokes.end(); ++it)
            for (Stroke &s : it.value()) if (s.kind == 3) s.revealed = revealed;
        viewport()->update();
    }
    void openNoteById(int id) {
        if (Stroke *n = findNote(id)) { n->open = true; activeId = id; startAnim(); viewport()->update(); }
    }
    void closeAllNotes() {
        for (auto it = strokes.begin(); it != strokes.end(); ++it)
            for (Stroke &s : it.value()) if (s.kind == 4) { s.open = false; s.pinned = false; }
        markDirty(); startAnim();
    }
    QString statusText() const {
        if (rects.isEmpty()) return QString();
        qreal mid = verticalScrollBar()->value() + viewport()->height() / 2.0;
        int pg = rects.size() - 1;
        for (int i = 0; i < rects.size(); ++i) if (rects[i].bottom() + 5 >= mid) { pg = i; break; }
        return FA("صفحه ") + QString::number(pg + 1) + FA(" از ") + QString::number(rects.size()) + "    " + QString::number(int(zoom * 100)) + "%";
    }

    // ------------------------------------------------------------ save / export
    void saveInk() {
        if (pdfPath.isEmpty()) return;
        auto putPts = [](const Ink &k) {
            QJsonArray a;
            for (int i = 0; i < k.pts.size(); ++i) { a.append(k.pts[i].x()); a.append(k.pts[i].y()); a.append(double(prAt(k, i))); }
            return a;
        };
        QJsonObject pages;
        for (auto it = strokes.cbegin(); it != strokes.cend(); ++it) {
            if (it.value().isEmpty()) continue;
            QJsonArray arr;
            for (const Stroke &s : it.value()) {
                QJsonObject o;
                o["k"] = s.kind;
                o["c"] = s.color.name(QColor::HexArgb);
                o["w"] = s.width; o["l"] = s.layer; o["t"] = s.text; o["bw"] = s.bw; o["bh"] = s.bh;
                if (s.kind == 4) {
                    o["id"] = s.id; o["pin"] = s.pinned; o["th"] = s.theme;
                    if (s.ncolor.isValid()) o["nc"] = s.ncolor.name(QColor::HexArgb);
                    QJsonArray ia;
                    for (const Ink &k : s.ink) {
                        QJsonObject io; io["c"] = k.color.name(QColor::HexArgb); io["w"] = k.width; io["p"] = putPts(k);
                        ia.append(io);
                    }
                    o["ink"] = ia;
                }
                o["p"] = putPts(s);
                arr.append(o);
            }
            pages[QString::number(it.key())] = arr;
        }
        QJsonObject root; root["v"] = 2; root["pages"] = pages;
        QSaveFile f(pdfPath + ".ink.json");  // the original PDF is never modified
        if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(root).toJson(QJsonDocument::Compact)); f.commit(); }
        dirty = false;
    }

    bool exportPdf(const QString &out, int noteMode = 0) {
        if (doc.status() != QPdfDocument::Status::Ready) return false;
        QPdfWriter w(out);
        w.setResolution(144);
        w.setPageMargins(QMarginsF(0, 0, 0, 0));
        QPainter p;
        for (int i = 0; i < doc.pageCount(); ++i) {
            QSizeF sz = doc.pagePointSize(i);
            w.setPageSize(QPageSize(sz, QPageSize::Point, QString(), QPageSize::ExactMatch));
            if (i == 0) { if (!p.begin(&w)) return false; } else w.newPage();
            QSize px(qRound(sz.width() * 2), qRound(sz.height() * 2));  // 144 dpi
            QImage im = doc.render(i, px * 1.5);
            p.fillRect(QRect(QPoint(0, 0), px), Qt::white);
            p.drawImage(QRect(QPoint(0, 0), px), im);
            QRectF r(0, 0, px.width(), px.height());
            for (const Stroke &s : strokes.value(i)) if (s.kind != 3 && s.kind != 4 && layerVis[s.layer]) drawStroke(p, s, r);
            if (noteMode)
                for (const Stroke &s : strokes.value(i))
                    if (s.kind == 4 && layerVis[s.layer]) { Stroke t = s; t.anim = noteMode == 2 ? 1.f : 0.f; drawNote(p, t, r); }
        }
        p.end();
        return true;
    }

protected:
    void resizeEvent(QResizeEvent *) override {
        relayout();
        if (overlay) overlay->move((viewport()->width() - overlay->width()) / 2, 2);
    }
    void wheelEvent(QWheelEvent *e) override {
        if (e->modifiers() & Qt::ControlModifier) {
            setZoom(zoom * (e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1));
            e->accept();
        } else QAbstractScrollArea::wheelEvent(e);
    }

    void paintEvent(QPaintEvent *e) override {
        QPainter p(viewport());
        p.fillRect(e->rect(), night ? QColor(22, 24, 30) : QColor(46, 51, 64));
        const QPointF o = origin();
        const qreal dpr = devicePixelRatioF();
        const QRectF dirtyR = e->rect();
        for (int i = 0; i < rects.size(); ++i) {
            QRectF r = rects[i].translated(o);
            if (r.bottom() < dirtyR.top()) continue;
            if (r.top() > dirtyR.bottom()) break;
            int W = qRound(r.width() * dpr), H = qRound(r.height() * dpr);
            quint64 key = (quint64(i) << 32) | quint32(W);
            QImage im;
            if (QImage *c = cache.object(key)) im = *c;
            else {
                im = doc.render(i, QSize(W, H));
                im.setDevicePixelRatio(dpr);
                cache.insert(key, new QImage(im), int(im.sizeInBytes() / 1024));
            }
            p.fillRect(r.adjusted(-2, 1, 3, 6), QColor(0, 0, 0, 22));  // page shadow
            p.fillRect(r.adjusted(-1, 1, 2, 3), QColor(0, 0, 0, 45));
            p.fillRect(r, Qt::white);  // some Qt versions render pages with a transparent background
            p.drawImage(r.topLeft(), im);
            if (night) {
                p.save(); p.setCompositionMode(QPainter::CompositionMode_Difference);
                p.fillRect(r, Qt::white); p.restore();
            }
            p.save();
            p.setClipRect(r);
            auto it = strokes.constFind(i);
            if (it != strokes.cend())
                for (const Stroke &s : it.value()) {
                    if (s.kind == 4 || !layerVis[s.layer]) continue;
                    QRectF nb = strokeBounds(s);  // skip strokes outside the repaint area
                    QRectF sb(r.x() + nb.x() * r.width(), r.y() + nb.y() * r.width(), nb.width() * r.width(), nb.height() * r.width());
                    if (!sb.intersects(dirtyR)) continue;
                    drawStroke(p, s, r);
                }
            if (drawing && curPage == i && activeTool != Eraser) drawStroke(p, cur, r);
            if (it != strokes.cend() && notesVisible)
                for (int pass = 0; pass < 2; ++pass)
                    for (const Stroke &s : it.value()) {
                        if (s.kind != 4 || !layerVis[s.layer] || (s.id == activeId) != (pass == 1)) continue;
                        drawNote(p, s, r, (nd == ND_Write && s.id == ndId) ? &liveInk : nullptr);
                    }
            p.restore();
        }
        p.setRenderHint(QPainter::Antialiasing);
        if (hoverOn && tool == Eraser) {  // eraser size preview
            p.setBrush(QColor(255, 255, 255, 40)); p.setPen(QPen(QColor(255, 255, 255, 200), 1.2));
            p.drawEllipse(hover, eraserRadius(), eraserRadius());
        }
        // laser pointer: fading trail + the live dot while pressed
        qint64 now = clock.elapsed();
        const qreal R = 3 + sizes[Laser] * 0.5;
        for (const auto &lp : laser) {
            qreal age = (now - lp.second) / 600.0;
            if (age >= 1) continue;
            p.setPen(Qt::NoPen); p.setBrush(QColor(255, 30, 30, int(200 * (1 - age))));
            p.drawEllipse(lp.first, R * (1 - age) + 1.5, R * (1 - age) + 1.5);
        }
        if (laserDown) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 40, 40, 70)); p.drawEllipse(laserPos, R * 2.0, R * 2.0);
            p.setBrush(QColor(255, 30, 30)); p.drawEllipse(laserPos, R, R);
            p.setBrush(QColor(255, 255, 255, 200)); p.drawEllipse(laserPos, R * 0.35, R * 0.35);
        }
    }

    void tabletEvent(QTabletEvent *e) override {
        QPointF pos = e->position();
        bool er = e->pointerType() == QPointingDevice::PointerType::Eraser;
        float pr = e->pressure() > 0 ? float(e->pressure()) : 0.5f;
        switch (e->type()) {
        case QEvent::TabletPress: begin(pos, pr, er); break;
        case QEvent::TabletMove: move(pos, pr, er); break;
        case QEvent::TabletRelease: end(); break;
        default: break;
        }
        e->accept();
    }
    // Mouse is supported too, but touch-synthesized mouse events are ignored (simple palm rejection)
    void mousePressEvent(QMouseEvent *e) override {
        if (e->source() != Qt::MouseEventNotSynthesized || e->button() != Qt::LeftButton) return;
        begin(e->position(), 0.5f, false);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (e->source() != Qt::MouseEventNotSynthesized) return;
        move(e->position(), 0.5f, false);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->source() != Qt::MouseEventNotSynthesized) return;
        end();
    }
    void leaveEvent(QEvent *) override { hoverOn = false; viewport()->update(); }

public:
    // ------------------------------------------------------------ private-ish state
    QVector<QRectF> rects;  // content coordinates
    qreal zoom = 1.0, contentH = 0, contentW = 0;
    QCache<quint64, QImage> cache;
    bool drawing = false;
    Tool activeTool = Pen;
    int curPage = -1, group = 0, groupCounter = 0;
    Stroke cur;
    QVector<Act> undoS, redoS;
    QPointF smooth, rawLast, hlStart;
    qint64 lastSel = 0;
    bool hlMoved = false;
    // laser
    QVector<QPair<QPointF, qint64>> laser;
    QElapsedTimer clock;
    QTimer laserTimer, autosave, animTimer;
    bool laserDown = false;
    QPointF laserPos;
    // hover (eraser preview)
    QPointF hover; bool hoverOn = false;
    // notes
    enum NoteDrag { ND_None, ND_Icon, ND_Move, ND_Resize, ND_Write, ND_Erase };
    NoteDrag nd = ND_None;
    int ndId = 0, ndPage = -1, activeId = -1, nextId = 1;
    QPointF ndStart, ndOrig, rawInk;
    qint64 ndTime = 0;
    Ink liveInk;

    qreal eraserRadius() const { return 4 + sizes[Eraser] * 0.9; }
    qreal widthFor(Tool t) const { return sizes[t] * kUnit; }

    QPointF origin() const {
        qreal x0 = qMax(0.0, (viewport()->width() - contentW) / 2.0);
        return QPointF(x0 - horizontalScrollBar()->value(), -verticalScrollBar()->value());
    }
    void relayout() {
        rects.clear();
        if (doc.status() != QPdfDocument::Status::Ready) return;
        qreal W = qMax(50.0, (viewport()->width() - 24) * zoom);
        qreal y = 12;
        for (int i = 0; i < doc.pageCount(); ++i) {
            QSizeF s = doc.pagePointSize(i);
            qreal h = s.width() > 0 ? W * s.height() / s.width() : W * 1.414;
            rects.append(QRectF(12, y, W, h));
            y += h + 10;
        }
        contentH = y + 2; contentW = W + 24;
        verticalScrollBar()->setRange(0, qMax(0, int(contentH - viewport()->height())));
        verticalScrollBar()->setPageStep(viewport()->height());
        verticalScrollBar()->setSingleStep(60);
        horizontalScrollBar()->setRange(0, qMax(0, int(contentW - viewport()->width())));
        horizontalScrollBar()->setPageStep(viewport()->width());
    }
    int pageAt(QPointF vp) const {
        QPointF o = origin();
        for (int i = 0; i < rects.size(); ++i) {
            QRectF r = rects[i].translated(o);
            if (r.top() > viewport()->height() + 5) break;
            if (r.contains(vp)) return i;
        }
        return -1;
    }
    QRectF pageRect(int i) const {
        if (i < 0 || i >= rects.size()) return QRectF(0, 0, 1, 1);
        return rects[i].translated(origin());
    }
    QPointF norm(QPointF vp, int pg) const {
        QRectF r = pageRect(pg);
        return QPointF(qBound(0.0, (vp.x() - r.x()) / r.width(), 1.0),
                       qBound(0.0, (vp.y() - r.y()) / r.width(), r.height() / r.width()));
    }
    void setHover(QPointF p) {
        QRectF a = hoverRect();
        hover = p; hoverOn = true;
        if (tool == Eraser) viewport()->update((a | hoverRect()).toAlignedRect());
    }
    QRectF hoverRect() const {
        if (!hoverOn || tool != Eraser) return QRectF();
        qreal r = eraserRadius() + 3;
        return QRectF(hover.x() - r, hover.y() - r, 2 * r, 2 * r);
    }

    // ---- laser
    void addLaser(QPointF p) {
        laser.append({p, clock.elapsed()});
        if (!laserTimer.isActive()) laserTimer.start();
        viewport()->update();
    }
    void pruneLaser() {
        qint64 now = clock.elapsed();
        while (!laser.isEmpty() && now - laser.first().second > 600) laser.removeFirst();
        if (laser.isEmpty() && !laserDown) laserTimer.stop();
        viewport()->update();
    }

    // ---- input
    void begin(QPointF vp, float pr, bool eraserTip) {
        Tool t = eraserTip ? Eraser : tool;
        setHover(vp);
        if (t == Laser) { laserDown = true; laserPos = vp; addLaser(vp); return; }
        int pg = pageAt(vp);
        if (pg < 0) return;
        if (noteBegin(pg, vp, pr, t)) return;
        if (t != Eraser && t != MaskT) {  // tap on a hidden mask reveals it
            QPointF q = norm(vp, pg);
            auto &sv = strokes[pg];
            for (int i = sv.size() - 1; i >= 0; --i) {
                Stroke &m = sv[i];
                if (m.kind == 3 && m.pts.size() >= 2 && !m.revealed && layerVis[m.layer] && QRectF(m.pts[0], m.pts[1]).normalized().contains(q)) {
                    m.revealed = true;
                    viewport()->update(pageRect(pg).toAlignedRect());
                    return;
                }
            }
        }
        if (t == Note) { if (notesVisible) createNote(pg, vp); return; }
        activeId = -1;
        curPage = pg; drawing = true; activeTool = t; hlMoved = false;
        if (t == Eraser) { group = ++groupCounter; eraseAt(vp); return; }
        cur = Stroke();
        cur.layer = curLayer;
        cur.color = color;
        cur.width = widthFor(t);
        QPointF np = norm(vp, pg);
        smooth = np; rawLast = np; hlStart = np;
        switch (t) {
        case Marker: case Highlighter: cur.kind = 1; cur.color.setAlpha(90); break;
        case Line: cur.kind = 2; break;
        case Arrow: cur.kind = 6; break;
        case DArrow: cur.kind = 7; break;
        case Ellipse: cur.kind = 8; break;
        case Rect: cur.kind = 9; break;
        case MaskT: cur.kind = 3; break;
        default: cur.kind = 0; break;
        }
        cur.pts << np; cur.pr << pr;
        if (cur.kind >= 2) { cur.pts << np; cur.pr << pr; }  // two-point shapes
        if (t == Highlighter) { rawPath.clear(); rawPr.clear(); rawPath << np; rawPr << pr; }
    }

    void move(QPointF vp, float pr, bool eraserTip) {
        setHover(vp);
        if (laserDown) { laserPos = vp; addLaser(vp); return; }
        if (nd != ND_None) { noteMove(vp, pr); return; }
        if (!drawing) return;
        if (activeTool == Eraser) { eraseAt(vp); return; }
        QPointF np = norm(vp, curPage);
        QRectF r = pageRect(curPage);
        rawLast = np;
        QPointF pnt = np;
        if (stab > 0 && (activeTool == Pen || activeTool == Marker || activeTool == Highlighter)) {
            smooth += (np - smooth) * (1.0 - stab * 0.085); pnt = smooth;
        }
        if (activeTool == Highlighter) {
            if (std::hypot(pnt.x() - rawPath.last().x(), pnt.y() - rawPath.last().y()) * r.width() >= 0.8) { rawPath << pnt; rawPr << pr; }
            if (std::hypot(np.x() - hlStart.x(), np.y() - hlStart.y()) * r.width() > 4) hlMoved = true;
            qint64 now = clock.elapsed();
            if (hlMoved && now - lastSel > 25) { lastSel = now; updateTextHighlight(); }
            viewport()->update(r.toAlignedRect());
            return;
        }
        if (cur.kind >= 2) { cur.pts[1] = np; viewport()->update(r.toAlignedRect()); return; }
        QPointF last = cur.pts.last();
        if (std::hypot(pnt.x() - last.x(), pnt.y() - last.y()) * r.width() < 0.8) return;
        cur.pts << pnt; cur.pr << pr;
        if (cur.kind == 1) { viewport()->update(r.toAlignedRect()); return; }
        QPointF a(r.x() + last.x() * r.width(), r.y() + last.y() * r.width());  // pen: repaint only the last segment
        QPointF b(r.x() + pnt.x() * r.width(), r.y() + pnt.y() * r.width());
        qreal m = cur.width * r.width() * 1.6 + 8;
        viewport()->update(QRectF(a, b).normalized().adjusted(-m, -m, m, m).toAlignedRect());
    }

    void end() {
        if (laserDown) { laserDown = false; viewport()->update(); return; }
        if (nd != ND_None) { noteEnd(); return; }
        if (!drawing) return;
        drawing = false;
        bool keep = true;
        QRectF r = pageRect(curPage);
        if (activeTool == Highlighter) {
            if (hlMoved) updateTextHighlight();
            keep = hlMoved && (cur.kind == 5 || cur.pts.size() >= 2);
        } else if (activeTool == MaskT) {
            QPointF d = cur.pts[1] - cur.pts[0];
            keep = std::abs(d.x()) > 0.01 && std::abs(d.y()) > 0.005;
        } else if (activeTool != Eraser && cur.kind >= 2) {  // shapes need some size
            QPointF d = cur.pts[1] - cur.pts[0];
            keep = std::hypot(d.x(), d.y()) * r.width() > 4;
        } else if (stab > 0 && (cur.kind == 0 || cur.kind == 1) && !cur.pts.isEmpty()) {  // catch up with the pen tip
            if (std::hypot(rawLast.x() - cur.pts.last().x(), rawLast.y() - cur.pts.last().y()) * r.width() > 1) {
                cur.pts << rawLast; cur.pr << prAt(cur, cur.pts.size() - 2);
            }
        }
        if (!keep) { cur = Stroke(); viewport()->update(); return; }
        if (activeTool != Eraser && !cur.pts.isEmpty()) {
            auto &v = strokes[curPage];
            undoS.append({true, curPage, int(v.size()), cur, ++groupCounter});
            v.append(cur);
            redoS.clear();
            markDirty();
        }
        cur = Stroke();
        viewport()->update();
    }

    // select the text under the drag (snaps to the PDF text layer); falls back to a free marker stroke
    void updateTextHighlight() {
        QSizeF ps = doc.pagePointSize(curPage);
        const qreal k = ps.width();
        Stroke marker;  // free stroke built from the raw path
        marker.kind = 1; marker.layer = curLayer; marker.color = color; marker.color.setAlpha(90);
        marker.width = widthFor(Highlighter); marker.pts = rawPath; marker.pr = rawPr;
        if (k <= 0) { cur = marker; return; }
        QPdfSelection sel = doc.getSelection(curPage, hlStart * k, rawLast * k);
        QList<QPolygonF> polys = sel.isValid() ? sel.bounds() : QList<QPolygonF>();
        if (polys.isEmpty()) { cur = marker; return; }  // no text layer here (scanned page, figure...)
        Stroke h;
        h.kind = 5; h.layer = curLayer; h.color = color; h.color.setAlpha(100);
        for (const QPolygonF &poly : polys) {
            QRectF br = poly.boundingRect().adjusted(-0.6, -0.9, 0.6, 0.9);
            h.pts << br.topLeft() / k << br.topRight() / k << br.bottomRight() / k << br.bottomLeft() / k;
        }
        cur = h;
    }
    QVector<QPointF> rawPath; QVector<float> rawPr;

    void eraseAt(QPointF vp) {
        int pg = pageAt(vp);
        if (pg < 0 || pg != curPage) return;
        QRectF r = pageRect(pg);
        QPointF q = norm(vp, pg);
        qreal rad = eraserRadius() / r.width();
        auto &v = strokes[pg];
        for (int i = v.size() - 1; i >= 0; --i) {
            const Stroke &s = v[i];
            if (!layerVis[s.layer]) continue;
            if (hitStroke(s, q, rad)) {
                undoS.append({false, pg, i, s, group});
                v.remove(i);
                redoS.clear();
                markDirty();
                viewport()->update(r.toAlignedRect());
            }
        }
    }

    // ---- sticky notes
    Stroke *findNote(int id) {
        if (id <= 0) return nullptr;
        for (auto it = strokes.begin(); it != strokes.end(); ++it)
            for (Stroke &s : it.value()) if (s.kind == 4 && s.id == id) return &s;
        return nullptr;
    }
    void startAnim() { if (!animTimer.isActive()) animTimer.start(); }
    void stepAnim() {
        bool moving = false;
        for (auto it = strokes.begin(); it != strokes.end(); ++it)
            for (Stroke &s : it.value()) {
                if (s.kind != 4) continue;
                float tgt = (s.open || s.pinned) ? 1.f : 0.f, d = tgt - s.anim;
                if (std::abs(d) > 0.001f) { s.anim = std::abs(d) <= 0.09f ? tgt : s.anim + (d > 0 ? 0.09f : -0.09f); moving = true; }
            }
        viewport()->update();
        if (!moving) animTimer.stop();
    }
    QPointF inkPt(const NoteGeom &g, const QRectF &r, QPointF vp) const {
        return QPointF((vp.x() - g.card.x()) / r.width(), (vp.y() - g.card.y()) / r.width());
    }
    void eraseInk(Stroke &s, const NoteGeom &g, QPointF vp) {
        QRectF r = pageRect(ndPage);
        QPointF q = inkPt(g, r, vp);
        qreal rad = eraserRadius() / r.width();
        for (int i = s.ink.size() - 1; i >= 0; --i) {
            const Ink &k = s.ink[i];
            bool hit = k.pts.size() == 1 && distSeg(q, k.pts[0], k.pts[0]) < rad + k.width / 2;
            for (int j = 1; j < k.pts.size() && !hit; ++j) hit = distSeg(q, k.pts[j - 1], k.pts[j]) < rad + k.width / 2;
            if (hit) s.ink.remove(i);
        }
        viewport()->update(g.card.toAlignedRect());
    }
    void editNoteText(int id) {
        Stroke *n = findNote(id);
        if (!n) return;
        bool ok = false;
        QString t = QInputDialog::getMultiLineText(this, FA("متن یادداشت"), FA("متن یادداشت:"), n->text, &ok);
        n = findNote(id);
        if (ok && n) { n->text = t.trimmed(); markDirty(); viewport()->update(); }
    }
    void setNoteStyle(int id, QColor c, int theme, bool setColor) {
        Stroke *n = findNote(id);
        if (!n) return;
        if (setColor) { n->ncolor = c; defNoteColor = c; }
        if (theme >= 0) { n->theme = theme; defTheme = theme; }
        markDirty(); viewport()->update();
    }
    void noteStyleMenu(int id) {
        Stroke *n = findNote(id);
        if (!n) return;
        QMenu m(this);
        QMenu *cm = m.addMenu(FA("رنگ نوت"));
        struct C { const char *name; const char *hex; };
        static const C cols[] = {{"پیش‌فرض لایه", ""}, {"طلایی", "#f2b632"}, {"آبی", "#3d9be0"}, {"قرمز", "#e4524c"},
                                 {"سبز", "#3fa66b"}, {"بنفش", "#8e5bd6"}, {"صورتی", "#e86fa5"}, {"فیروزه‌ای", "#21b3b3"}, {"خاکستری", "#6c7589"}};
        for (const C &c : cols) {
            QPixmap pm(16, 16); pm.fill(c.hex[0] ? QColor(c.hex) : layerColor(n->layer));
            QAction *a = cm->addAction(QIcon(pm), FA(c.name));
            QColor col = c.hex[0] ? QColor(c.hex) : QColor();
            connect(a, &QAction::triggered, this, [this, id, col] { setNoteStyle(id, col, -1, true); });
        }
        QAction *custom = cm->addAction(FA("رنگ دلخواه…"));
        connect(custom, &QAction::triggered, this, [this, id] {
            QColor c = QColorDialog::getColor(Qt::white, this, FA("رنگ نوت"));
            if (c.isValid()) setNoteStyle(id, c, -1, true);
        });
        QMenu *tm = m.addMenu(FA("تم کاغذ"));
        for (int i = 0; i < kThemeCount; ++i) {
            QPixmap pm(16, 16); pm.fill(QColor(kThemes[i].paper));
            QAction *a = tm->addAction(QIcon(pm), FA(kThemes[i].name));
            a->setCheckable(true); a->setChecked(n->theme == i);
            connect(a, &QAction::triggered, this, [this, id, i] { setNoteStyle(id, QColor(), i, false); });
        }
        m.exec(QCursor::pos());
    }
    void createNote(int pg, QPointF vp) {
        QRectF r = pageRect(pg);
        Stroke n; n.kind = 4; n.id = nextId++; n.layer = curLayer; n.color = color;
        n.ncolor = defNoteColor; n.theme = defTheme;
        n.pts << norm(vp, pg); n.pr << 0.5f;
        qreal ph = r.height() / r.width();
        qreal x = n.pts[0].x() + 0.035;
        if (x + n.bw > 0.99) x = n.pts[0].x() - 0.035 - n.bw;
        x = qBound(0.01, x, qMax(0.01, 0.99 - n.bw));
        qreal y = qBound(0.01, n.pts[0].y() - 0.02, qMax(0.01, ph - n.bh - 0.01));
        n.pts << QPointF(x, y); n.pr << 0.5f;
        n.open = true;
        auto &v = strokes[pg];
        undoS.append({true, pg, int(v.size()), n, ++groupCounter});
        v.append(n);
        redoS.clear();
        activeId = n.id;
        markDirty(); startAnim();
    }
    bool noteBegin(int pg, QPointF vp, float pr, Tool t) {
        if (!notesVisible) return false;
        QRectF r = pageRect(pg);
        auto it = strokes.find(pg);
        if (it == strokes.end()) return false;
        QVector<Stroke> &v = it.value();
        int hit = -1;
        for (int pass = 1; pass >= 0 && hit < 0; --pass)  // the active note is drawn on top
            for (int i = v.size() - 1; i >= 0; --i) {
                const Stroke &s = v[i];
                if (s.kind != 4 || s.pts.size() < 2 || !layerVis[s.layer] || (s.id == activeId) != (pass == 1)) continue;
                if ((s.open || s.pinned) && noteGeom(s, r).card.contains(vp)) { hit = i; break; }
            }
        if (hit >= 0) {
            Stroke &s = v[hit];
            NoteGeom g = noteGeom(s, r);
            activeId = s.id; ndId = s.id; ndPage = pg; ndStart = vp;
            const int id = s.id;
            auto in = [&](const QRectF &b) { return b.adjusted(-5, -5, 5, 5).contains(vp); };
            if (in(g.closeB)) { s.pinned = false; s.open = false; markDirty(); startAnim(); return true; }
            if (in(g.pinB)) { s.pinned = !s.pinned; markDirty(); viewport()->update(); return true; }
            if (in(g.typeB)) { QTimer::singleShot(0, this, [this, id] { editNoteText(id); }); return true; }
            if (in(g.styleB)) { QTimer::singleShot(0, this, [this, id] { noteStyleMenu(id); }); return true; }
            if (g.grip.contains(vp)) { nd = ND_Resize; ndOrig = QPointF(s.bw, s.bh); return true; }
            if (g.header.contains(vp)) { nd = ND_Move; ndOrig = s.pts[1]; return true; }
            if (t == Eraser) { nd = ND_Erase; eraseInk(s, g, vp); return true; }
            nd = ND_Write;
            liveInk = Ink(); liveInk.color = color; liveInk.width = widthFor(Pen) * 0.8;
            liveInk.pts << inkPt(g, r, vp); liveInk.pr << pr;
            smooth = liveInk.pts[0]; rawInk = smooth;
            return true;
        }
        if (t != Eraser && t != MaskT) {
            for (int i = v.size() - 1; i >= 0; --i) {
                const Stroke &s = v[i];
                if (s.kind == 4 && s.pts.size() >= 2 && layerVis[s.layer] && noteGeom(s, r).icon.adjusted(-4, -4, 4, 4).contains(vp)) {
                    nd = ND_Icon; ndId = s.id; ndPage = pg; ndStart = vp; ndTime = clock.elapsed();
                    return true;
                }
            }
        }
        return false;
    }
    void noteMove(QPointF vp, float pr) {
        Stroke *s = findNote(ndId);
        if (!s || s->pts.size() < 2) { nd = ND_None; return; }
        QRectF r = pageRect(ndPage);
        const qreal W = r.width(), ph = r.height() / W;
        switch (nd) {
        case ND_Icon:
            if (std::hypot(vp.x() - ndStart.x(), vp.y() - ndStart.y()) > 8) nd = ND_None;  // not a tap
            return;
        case ND_Move: {
            QPointF d((vp.x() - ndStart.x()) / W, (vp.y() - ndStart.y()) / W);
            s->pts[1] = QPointF(qBound(0.0, ndOrig.x() + d.x(), qMax(0.0, 1 - s->bw)), qBound(0.0, ndOrig.y() + d.y(), qMax(0.0, ph - s->bh)));
            break;
        }
        case ND_Resize: {
            qreal dx = (vp.x() - ndStart.x()) / W, dy = (vp.y() - ndStart.y()) / W;
            s->bw = qBound(0.18, ndOrig.x() + dx, qMax(0.18, 1 - s->pts[1].x()));
            s->bh = qBound(0.12, ndOrig.y() + dy, qMax(0.12, ph - s->pts[1].y()));
            break;
        }
        case ND_Write: {
            if (liveInk.pts.isEmpty()) { nd = ND_None; return; }
            NoteGeom g = noteGeom(*s, r);
            QPointF q = inkPt(g, r, vp);
            rawInk = q;
            if (stab > 0) { smooth += (q - smooth) * (1.0 - stab * 0.085); q = smooth; }
            if (std::hypot(q.x() - liveInk.pts.last().x(), q.y() - liveInk.pts.last().y()) * W < 0.8) return;
            liveInk.pts << q; liveInk.pr << pr;
            viewport()->update(g.card.adjusted(-6, -6, 6, 6).toAlignedRect());
            return;
        }
        case ND_Erase: {
            NoteGeom g = noteGeom(*s, r);
            if (g.body.contains(vp)) eraseInk(*s, g, vp);
            return;
        }
        default: return;
        }
        viewport()->update(r.toAlignedRect());
    }
    void noteEnd() {
        NoteDrag d = nd; nd = ND_None;
        Stroke *s = findNote(ndId);
        if (!s) return;
        switch (d) {
        case ND_Icon:
            if (clock.elapsed() - ndTime < 450) {  // quick tap toggles the card
                if (s->open || s->pinned) { if (s->pinned) { s->pinned = false; markDirty(); } s->open = false; }
                else s->open = true;
                activeId = s->id; startAnim();
            }
            break;
        case ND_Write:
            if (!liveInk.pts.isEmpty()) {
                if (stab > 0 && std::hypot(rawInk.x() - liveInk.pts.last().x(), rawInk.y() - liveInk.pts.last().y()) * pageRect(ndPage).width() > 1) {
                    liveInk.pts << rawInk; liveInk.pr << prAt(liveInk, liveInk.pts.size() - 2);
                }
                s->ink.append(liveInk); s->redoInk.clear(); markDirty();
            }
            liveInk = Ink();
            break;
        case ND_Move: case ND_Resize: case ND_Erase: markDirty(); break;
        default: break;
        }
        viewport()->update();
    }

    // ---- load
    void loadInk() {
        QFile f(pdfPath + ".ink.json");
        if (!f.open(QIODevice::ReadOnly)) return;
        auto getPts = [](const QJsonArray &a, Ink &k) {
            for (int j = 0; j + 2 < a.size(); j += 3) {
                k.pts << QPointF(a.at(j).toDouble(), a.at(j + 1).toDouble());
                k.pr << float(a.at(j + 2).toDouble(0.5));
            }
        };
        QJsonObject pages = QJsonDocument::fromJson(f.readAll()).object().value("pages").toObject();
        for (auto it = pages.begin(); it != pages.end(); ++it) {
            QVector<Stroke> v;
            for (const QJsonValue &sv : it.value().toArray()) {
                QJsonObject o = sv.toObject();
                Stroke s;
                s.kind = o["k"].toInt(); s.color = QColor(o["c"].toString()); s.width = o["w"].toDouble(0.0035);
                s.layer = qBound(0, o["l"].toInt(), 2); s.text = o["t"].toString();
                s.bw = qMax(0.18, o["bw"].toDouble(0.34)); s.bh = qMax(0.14, o["bh"].toDouble(0.24));
                getPts(o["p"].toArray(), s);
                if (s.pts.isEmpty()) continue;
                if (s.kind == 4) {
                    if (s.pts.size() < 2) s.pts << (s.pts[0] + QPointF(0.04, 0.0));
                    s.id = o["id"].toInt(); if (s.id >= nextId) nextId = s.id + 1;
                    s.pinned = o["pin"].toBool(); s.anim = s.pinned ? 1.f : 0.f;
                    s.theme = qBound(0, o["th"].toInt(), kThemeCount - 1);
                    if (o.contains("nc")) s.ncolor = QColor(o["nc"].toString());
                    for (const QJsonValue &iv : o["ink"].toArray()) {
                        QJsonObject io = iv.toObject(); Ink k;
                        k.color = QColor(io["c"].toString()); k.width = io["w"].toDouble(0.003);
                        getPts(io["p"].toArray(), k);
                        if (!k.pts.isEmpty()) s.ink << k;
                    }
                } else if ((s.kind >= 2 && s.kind != 5 && s.pts.size() < 2) || s.kind < 0 || s.kind > 9) continue;
                v << s;
            }
            strokes[it.key().toInt()] = v;
        }
        for (auto it2 = strokes.begin(); it2 != strokes.end(); ++it2)
            for (Stroke &s : it2.value()) if (s.kind == 4 && s.id <= 0) s.id = nextId++;
    }
};
