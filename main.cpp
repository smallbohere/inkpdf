// InkPDF - lightweight PDF reader with stylus ink for teaching (Windows, C++17 / Qt 6)
#include <QtWidgets>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QPageSize>
#include <QCache>
#include <cmath>
#include <functional>

#define FA(s) QString::fromUtf8(s)

enum Tool { Pen, Highlighter, Eraser, Line, Laser, MaskT, Note };

struct Ink {
    int kind = 0;  // 0 pen, 1 highlighter, 2 line, 3 mask, 4 note
    QColor color;
    qreal width = 0.0035;           // fraction of page width
    QVector<QPointF> pts;           // normalized to page width (note: pts[0]=icon anchor, pts[1]=card top-left)
    QVector<float> pr;              // pressure per point
};
struct Stroke : Ink {
    int layer = 0;                  // 0 base, 1 extra, 2 exam tips
    QString text;                   // typed note text
    qreal bw = 0.34, bh = 0.24;     // note card size (fraction of page width)
    bool revealed = false;          // mask state (runtime only)
    int id = 0;                     // note id
    bool pinned = false;            // note stays open
    bool open = false;              // runtime
    float anim = 0;                 // runtime open animation 0..1
    QVector<Ink> ink, redoInk;      // handwriting inside the note (coords relative to card top-left)
};
struct NoteRef { int page; QPointF pt; int kind; int layer; QString text; int id; };
struct Act { bool added; int page; int idx; Stroke s; int group; };

static qreal distSeg(QPointF p, QPointF a, QPointF b) {
    QPointF ab = b - a;
    qreal l2 = ab.x() * ab.x() + ab.y() * ab.y();
    qreal t = l2 <= 0 ? 0 : qBound(0.0, ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / l2, 1.0);
    QPointF c = a + ab * t;
    return std::hypot(p.x() - c.x(), p.y() - c.y());
}

static const qreal kNoteFont = 0.017;
static bool isRtl(const QString &t) {
    for (QChar c : t) if (c.unicode() >= 0x0590 && c.unicode() <= 0x08FF) return true;
    return false;
}

static void drawStroke(QPainter &p, const Stroke &s, const QRectF &r) {
    const qreal W = r.width();
    const int n = s.pts.size();
    if (n == 0) return;
    auto P = [&](int i) { return QPointF(r.x() + s.pts[i].x() * W, r.y() + s.pts[i].y() * W); };
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (s.kind == 3 && n >= 2) {
        QRectF m = QRectF(P(0), P(1)).normalized();
        if (s.revealed) {
            p.setPen(QPen(QColor(40, 90, 160, 160), 1.5, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRect(m);
        } else {
            p.setPen(Qt::NoPen); p.setBrush(QColor(35, 70, 120)); p.drawRoundedRect(m, 4, 4);
            p.setPen(Qt::white);
            QFont f = p.font(); f.setPixelSize(qMax(10, int(qMin(m.height(), m.width()) * 0.6))); p.setFont(f);
            p.drawText(m, Qt::AlignCenter, QString::fromUtf8("؟"));
        }
    } else if (s.kind == 0) {
        if (n == 1) {
            p.setPen(Qt::NoPen); p.setBrush(s.color);
            p.drawEllipse(P(0), s.width * W * 0.5, s.width * W * 0.5);
        }
        for (int i = 1; i < n; ++i) {
            float pr = (s.pr[i] + s.pr[i - 1]) * 0.5f;
            p.setPen(QPen(s.color, s.width * W * (0.4 + 1.2 * pr), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(P(i - 1), P(i));
        }
    } else if (s.kind <= 2) {
        QPainterPath path;
        path.moveTo(P(0));
        if (s.kind == 2 || n < 3) {
            for (int i = 1; i < n; ++i) path.lineTo(P(i));
            if (n == 1) path.lineTo(P(0) + QPointF(0.1, 0));
        } else {
            for (int i = 1; i < n - 1; ++i) {
                QPointF mid = (P(i) + P(i + 1)) * 0.5;
                path.quadTo(P(i), mid);
            }
            path.lineTo(P(n - 1));
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.color, s.width * W, Qt::SolidLine, s.kind == 1 ? Qt::FlatCap : Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    }
    p.restore();
}

static QColor layerColor(int l) {
    static const char *c[3] = {"#f2b632", "#3d9be0", "#e4524c"};
    return QColor(c[qBound(0, l, 2)]);
}
struct NoteGeom { QRectF icon, card, header, body, closeB, pinB, typeB, grip; };
static NoteGeom noteGeom(const Stroke &s, const QRectF &r) {
    NoteGeom g;
    const qreal W = r.width();
    qreal iw = qBound(20.0, W * 0.032, 34.0);
    QPointF a(r.x() + s.pts[0].x() * W, r.y() + s.pts[0].y() * W);
    g.icon = QRectF(a.x() - iw / 2, a.y() - iw * 0.55, iw, iw * 1.1);
    g.card = QRectF(r.x() + s.pts[1].x() * W, r.y() + s.pts[1].y() * W, s.bw * W, s.bh * W);
    qreal hh = qMax(22.0, W * 0.034);
    g.header = QRectF(g.card.x(), g.card.y(), g.card.width(), hh);
    g.body = QRectF(g.card.x(), g.card.y() + hh, g.card.width(), g.card.height() - hh);
    qreal bs = hh * 0.62, gap = hh * 0.18, y = g.card.y() + (hh - bs) / 2, x = g.card.x() + hh * 0.25;
    g.closeB = QRectF(x, y, bs, bs); x += bs + gap;
    g.pinB = QRectF(x, y, bs, bs); x += bs + gap;
    g.typeB = QRectF(x, y, bs, bs);
    g.grip = QRectF(g.card.right() - 16, g.card.bottom() - 16, 16, 16);
    return g;
}

static void drawNote(QPainter &p, const Stroke &s, const QRectF &r, const Ink *live = nullptr) {
    if (s.pts.size() < 2) return;
    const qreal W = r.width();
    NoteGeom g = noteGeom(s, r);
    const QColor lc = layerColor(s.layer);
    const qreal e = 1 - std::pow(1 - qBound(0.0f, s.anim, 1.0f), 3);  // ease-out
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (e > 0.01) {
        QRectF c(g.icon.center() + (g.card.topLeft() - g.icon.center()) * e,
                 g.icon.center() + (g.card.bottomRight() - g.icon.center()) * e);
        c = c.normalized();
        p.setOpacity(e);
        QPointF ac = g.icon.center();
        QPointF nearest(qBound(c.left(), ac.x(), c.right()), qBound(c.top(), ac.y(), c.bottom()));
        p.setPen(QPen(lc.darker(115), 1.6, Qt::DashLine));
        p.drawLine(ac, nearest);
        p.setPen(Qt::NoPen);
        for (int i = 5; i >= 1; --i) {  // soft shadow
            p.setBrush(QColor(0, 0, 0, 12));
            p.drawRoundedRect(c.adjusted(-i * 1.6, -i * 1.6 + 3, i * 1.6, i * 1.6 + 3), 10 + i, 10 + i);
        }
        p.setBrush(QColor(255, 252, 240)); p.setPen(QPen(lc, 1.2));
        p.drawRoundedRect(c, 10, 10);
        p.save();
        QPainterPath clip; clip.addRoundedRect(c, 10, 10);
        p.setClipPath(clip);
        QRectF hr(c.x(), c.y(), c.width(), g.header.height() * e);
        QLinearGradient hg(hr.topLeft(), hr.bottomLeft());
        hg.setColorAt(0, lc.lighter(118)); hg.setColorAt(1, lc);
        p.fillRect(hr, hg);
        p.restore();
        if (e > 0.97) {
            // header buttons + title
            auto circ = [&](const QRectF &b, int a) { p.setPen(Qt::NoPen); p.setBrush(QColor(255, 255, 255, a)); p.drawEllipse(b); };
            circ(g.closeB, 70);
            p.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap));
            QPointF cc = g.closeB.center(); qreal d = g.closeB.width() * 0.22;
            p.drawLine(cc + QPointF(-d, -d), cc + QPointF(d, d)); p.drawLine(cc + QPointF(-d, d), cc + QPointF(d, -d));
            circ(g.pinB, s.pinned ? 235 : 70);
            p.setPen(QPen(s.pinned ? lc.darker(140) : QColor(Qt::white), 1.8, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            QPointF pc = g.pinB.center(); qreal pd = g.pinB.width();
            p.drawEllipse(pc + QPointF(0, -pd * 0.13), pd * 0.14, pd * 0.14);
            p.drawLine(pc + QPointF(0, pd * 0.02), pc + QPointF(0, pd * 0.28));
            circ(g.typeB, 70);
            p.setPen(Qt::white);
            QFont tf = p.font(); tf.setBold(true); tf.setPixelSize(qMax(8, int(g.typeB.height() * 0.72))); p.setFont(tf);
            p.drawText(g.typeB, Qt::AlignCenter, QStringLiteral("T"));
            QFont hf = p.font(); hf.setBold(true); hf.setPixelSize(qMax(9, int(g.header.height() * 0.48))); p.setFont(hf);
            p.drawText(g.header.adjusted(8, 0, -12, 0), Qt::AlignVCenter | Qt::AlignRight | Qt::AlignAbsolute, FA("یادداشت"));
            // body: typed text, hint, handwriting
            QFont bf = p.font(); bf.setBold(false); bf.setPixelSize(qMax(9, int(W * 0.017))); p.setFont(bf);
            const bool empty = s.text.isEmpty() && s.ink.isEmpty() && !live;
            if (!s.text.isEmpty()) {
                p.setPen(QColor(40, 40, 40));
                p.drawText(g.body.adjusted(10, 8, -10, -8), Qt::TextWordWrap | Qt::AlignTop | Qt::AlignAbsolute | (isRtl(s.text) ? Qt::AlignRight : Qt::AlignLeft), s.text);
            } else if (empty) {
                p.setPen(QColor(150, 150, 150));
                p.drawText(g.body.adjusted(12, 8, -12, -8), Qt::TextWordWrap | Qt::AlignCenter, FA("با قلم بنویس یا از دکمه T متن تایپ کن"));
            }
            p.save();
            p.setClipRect(g.body);
            auto drawInk = [&](const Ink &k) {
                Stroke t; static_cast<Ink &>(t) = k; t.kind = 0;
                drawStroke(p, t, QRectF(g.card.x(), g.card.y(), W, r.height()));
            };
            for (const Ink &k : s.ink) drawInk(k);
            if (live) drawInk(*live);
            p.restore();
            p.setPen(QPen(lc.darker(110), 1.4, Qt::SolidLine, Qt::RoundCap));  // resize grip
            for (int i = 1; i <= 3; ++i)
                p.drawLine(QPointF(g.card.right() - 4 - i * 4, g.card.bottom() - 4), QPointF(g.card.right() - 4, g.card.bottom() - 4 - i * 4));
        }
    }
    // icon (a small folded sheet)
    p.setOpacity(1 - 0.4 * e);
    p.setPen(Qt::NoPen);
    QRectF ic = g.icon; qreal f = ic.width() * 0.28;
    p.setBrush(QColor(0, 0, 0, 45)); p.drawRoundedRect(ic.translated(1.5, 2.2), 4, 4);
    QPainterPath sheet;
    sheet.moveTo(ic.topLeft()); sheet.lineTo(ic.right() - f, ic.top()); sheet.lineTo(ic.right(), ic.top() + f);
    sheet.lineTo(ic.bottomRight()); sheet.lineTo(ic.bottomLeft()); sheet.closeSubpath();
    QLinearGradient gr(ic.topLeft(), ic.bottomRight());
    gr.setColorAt(0, lc.lighter(125)); gr.setColorAt(1, lc);
    p.setBrush(gr); p.setPen(QPen(lc.darker(130), 1)); p.drawPath(sheet);
    QPainterPath fold;
    fold.moveTo(ic.right() - f, ic.top()); fold.lineTo(ic.right() - f, ic.top() + f); fold.lineTo(ic.right(), ic.top() + f); fold.closeSubpath();
    p.setBrush(lc.darker(125)); p.drawPath(fold);
    const bool has = !s.text.isEmpty() || !s.ink.isEmpty();
    p.setPen(QPen(QColor(255, 255, 255, has ? 235 : 120), 1.6, Qt::SolidLine, Qt::RoundCap));
    qreal lx0 = ic.left() + ic.width() * 0.2, lx1 = ic.right() - ic.width() * 0.2;
    for (int i = 0; i < (has ? 3 : 1); ++i) {
        qreal y = ic.top() + ic.height() * (0.45 + 0.17 * i);
        p.drawLine(QPointF(lx0, y), QPointF(i == 2 ? (lx0 + lx1) / 2 : lx1, y));
    }
    p.restore();
}

class Canvas : public QAbstractScrollArea {
public:
    QPdfDocument doc;
    QString pdfPath;
    Tool tool = Pen;
    QColor color = QColor("#d32f2f");
    qreal penScale = 1.0;
    bool night = false, dirty = false;
    QMap<int, QVector<Stroke>> strokes;
    int curLayer = 0;
    bool layerVis[3] = {true, true, true};
    std::function<void()> changed;
    bool notesVisible = true;

    explicit Canvas(QWidget *parent = nullptr) : QAbstractScrollArea(parent) {
        setLayoutDirection(Qt::LeftToRight);
        cache.setMaxCost(300 * 1024);  // ~300 MB of page bitmaps (cost in KB)
        viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
        viewport()->setAttribute(Qt::WA_TabletTracking);
        viewport()->setMouseTracking(true);
        viewport()->setCursor(Qt::CrossCursor);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), [this] { viewport()->update(); });
        connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), [this] { viewport()->update(); });
        laserClock.start();
        laserTimer.setInterval(16);
        connect(&laserTimer, &QTimer::timeout, this, [this] { pruneLaser(); });
        autosave.setInterval(20000);
        connect(&autosave, &QTimer::timeout, this, [this] { saveIfDirty(); });
        autosave.start();
        animTimer.setInterval(16);
        connect(&animTimer, &QTimer::timeout, this, [this] { stepAnim(); });
    }

    bool load(const QString &path) {
        saveIfDirty();
        if (doc.load(path) != QPdfDocument::Error::None) return false;
        pdfPath = path;
        strokes.clear(); undoS.clear(); redoS.clear(); cache.clear(); nextId = 1; activeId = -1; nd = ND_None;
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

    void saveIfDirty() { if (dirty) saveInk(); }
    void markDirty() { dirty = true; if (changed) changed(); }

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

    QVector<NoteRef> collectNotes(bool withHighlights) const {
        QVector<NoteRef> out;
        for (auto it = strokes.cbegin(); it != strokes.cend(); ++it) {
            QVector<NoteRef> pg;
            for (const Stroke &s : it.value())
                if ((s.kind == 4 || (withHighlights && s.kind == 1)) && !s.pts.isEmpty())
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

    void saveInk() {
        if (pdfPath.isEmpty()) return;
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
                    o["id"] = s.id; o["pin"] = s.pinned;
                    QJsonArray ia;
                    for (const Ink &k : s.ink) {
                        QJsonObject io; io["c"] = k.color.name(QColor::HexArgb); io["w"] = k.width;
                        QJsonArray ka;
                        for (int j = 0; j < k.pts.size(); ++j) { ka.append(k.pts[j].x()); ka.append(k.pts[j].y()); ka.append(double(k.pr[j])); }
                        io["p"] = ka; ia.append(io);
                    }
                    o["ink"] = ia;
                }
                QJsonArray pa;
                for (int i = 0; i < s.pts.size(); ++i) { pa.append(s.pts[i].x()); pa.append(s.pts[i].y()); pa.append(double(s.pr[i])); }
                o["p"] = pa;
                arr.append(o);
            }
            pages[QString::number(it.key())] = arr;
        }
        QJsonObject root; root["v"] = 1; root["pages"] = pages;
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
            for (const Stroke &s : strokes.value(i)) if (s.kind != 3 && layerVis[s.layer]) drawStroke(p, s, r);
            if (noteMode)
                for (const Stroke &s : strokes.value(i))
                    if (s.kind == 4 && layerVis[s.layer]) { Stroke t = s; t.anim = noteMode == 2 ? 1.f : 0.f; drawNote(p, t, r); }
        }
        p.end();
        return true;
    }

protected:
    void resizeEvent(QResizeEvent *) override { relayout(); }

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
        for (int i = 0; i < rects.size(); ++i) {
            QRectF r = rects[i].translated(o);
            if (r.bottom() < e->rect().top()) continue;
            if (r.top() > e->rect().bottom()) break;
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
            if (it != strokes.cend()) for (const Stroke &s : it.value()) if (layerVis[s.layer]) drawStroke(p, s, r);
            if (drawing && curPage == i && activeTool != Eraser) drawStroke(p, cur, r);
            if (it != strokes.cend() && notesVisible)
                for (int pass = 0; pass < 2; ++pass)
                    for (const Stroke &s : it.value()) {
                        if (s.kind != 4 || !layerVis[s.layer] || (s.id == activeId) != (pass == 1)) continue;
                        drawNote(p, s, r, (nd == ND_Write && s.id == ndId) ? &liveInk : nullptr);
                    }
            p.restore();
        }
        // laser pointer trail
        qint64 now = laserClock.elapsed();
        p.setRenderHint(QPainter::Antialiasing);
        for (const auto &lp : laser) {
            qreal age = (now - lp.second) / 700.0;
            if (age >= 1) continue;
            QColor c(255, 30, 30, int(255 * (1 - age)));
            p.setPen(Qt::NoPen); p.setBrush(c);
            p.drawEllipse(lp.first, 9 * (1 - age) + 2, 9 * (1 - age) + 2);
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

private:
    QVector<QRectF> rects;  // content coordinates
    qreal zoom = 1.0, contentH = 0, contentW = 0;
    QCache<quint64, QImage> cache;
    bool drawing = false;
    Tool activeTool = Pen;
    int curPage = -1, group = 0;
    Stroke cur;
    QVector<Act> undoS, redoS;
    QVector<QPair<QPointF, qint64>> laser;
    QElapsedTimer laserClock;
    QTimer laserTimer, autosave;

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
            qreal h = W * s.height() / s.width();
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
    QRectF pageRect(int i) const { return rects[i].translated(origin()); }
    QPointF norm(QPointF vp, int pg) const {
        QRectF r = pageRect(pg);
        return QPointF(qBound(0.0, (vp.x() - r.x()) / r.width(), 1.0),
                       qBound(0.0, (vp.y() - r.y()) / r.width(), r.height() / r.width()));
    }

    void addLaser(QPointF p) {
        laser.append({p, laserClock.elapsed()});
        if (!laserTimer.isActive()) laserTimer.start();
        viewport()->update();
    }
    void pruneLaser() {
        qint64 now = laserClock.elapsed();
        while (!laser.isEmpty() && now - laser.first().second > 700) laser.removeFirst();
        if (laser.isEmpty()) laserTimer.stop();
        viewport()->update();
    }

    void begin(QPointF vp, float pr, bool eraser) {
        Tool t = eraser ? Eraser : tool;
        if (t == Laser) { addLaser(vp); return; }
        int pg = pageAt(vp);
        if (pg < 0) return;
        if (noteBegin(pg, vp, pr, t)) return;
        if (t != Eraser && t != MaskT) {  // tap on a hidden mask reveals it
            QPointF q = norm(vp, pg);
            auto &sv = strokes[pg];
            for (int i = sv.size() - 1; i >= 0; --i) {
                Stroke &m = sv[i];
                if (m.kind == 3 && !m.revealed && layerVis[m.layer] && QRectF(m.pts[0], m.pts[1]).normalized().contains(q)) {
                    m.revealed = true;
                    viewport()->update(pageRect(pg).toAlignedRect());
                    return;
                }
            }
        }
        if (t == Note) { if (notesVisible) createNote(pg, vp); return; }
        activeId = -1;
        curPage = pg; drawing = true; activeTool = t;
        if (t == Eraser) { group = ++groupCounter; eraseAt(vp); return; }
        cur = Stroke();
        cur.kind = t == Highlighter ? 1 : t == Line ? 2 : t == MaskT ? 3 : 0;
        cur.layer = curLayer;
        cur.color = color;
        if (t == Highlighter) { cur.color.setAlpha(90); cur.width = 0.022 * penScale; }
        else cur.width = 0.0035 * penScale;
        cur.pts << norm(vp, pg); cur.pr << pr;
        if (t == Line || t == MaskT) { cur.pts << cur.pts[0]; cur.pr << pr; }
    }

    void move(QPointF vp, float pr, bool eraser) {
        if (tool == Laser && !eraser) { addLaser(vp); return; }
        if (nd != ND_None) { noteMove(vp, pr); return; }
        if (!drawing) return;
        if (activeTool == Eraser) { eraseAt(vp); return; }
        QPointF np = norm(vp, curPage);
        QRectF r = pageRect(curPage);
        if (activeTool == Line || activeTool == MaskT) { cur.pts[1] = np; viewport()->update(r.toAlignedRect()); return; }
        QPointF last = cur.pts.last();
        if (std::hypot(np.x() - last.x(), np.y() - last.y()) * r.width() < 0.8) return;
        cur.pts << np; cur.pr << pr;
        if (activeTool == Highlighter) { viewport()->update(r.toAlignedRect()); return; }
        QPointF a(r.x() + last.x() * r.width(), r.y() + last.y() * r.width());  // pen: repaint only the last segment
        QRectF dr = QRectF(a, vp).normalized().adjusted(-12, -12, 12, 12);
        viewport()->update(dr.toAlignedRect());
    }

    void end() {
        if (nd != ND_None) { noteEnd(); return; }
        if (!drawing) return;
        drawing = false;
        bool keep = true;
        if (activeTool == MaskT) {
            QPointF d = cur.pts[1] - cur.pts[0];
            keep = std::abs(d.x()) > 0.01 && std::abs(d.y()) > 0.005;
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

    void eraseAt(QPointF vp) {
        int pg = pageAt(vp);
        if (pg < 0 || pg != curPage) return;
        QRectF r = pageRect(pg);
        QPointF q = norm(vp, pg);
        qreal rad = 12.0 / r.width();
        auto &v = strokes[pg];
        for (int i = v.size() - 1; i >= 0; --i) {
            const Stroke &s = v[i];
            if (!layerVis[s.layer]) continue;
            bool hit = false;
            if (s.kind == 3) {
                hit = QRectF(s.pts[0], s.pts[1]).normalized().adjusted(-rad, -rad, rad, rad).contains(q);
            } else if (s.kind == 4) {
                hit = std::hypot(q.x() - s.pts[0].x(), q.y() - s.pts[0].y()) < rad * 1.3;
            } else {
                if (s.pts.size() == 1) hit = distSeg(q, s.pts[0], s.pts[0]) < rad + s.width / 2;
                for (int k = 1; k < s.pts.size() && !hit; ++k)
                    hit = distSeg(q, s.pts[k - 1], s.pts[k]) < rad + s.width / 2;
            }
            if (hit) {
                undoS.append({false, pg, i, s, group});
                v.remove(i);
                redoS.clear();
                markDirty();
                viewport()->update(r.toAlignedRect());
            }
        }
    }

    void loadInk() {
        QFile f(pdfPath + ".ink.json");
        if (!f.open(QIODevice::ReadOnly)) return;
        QJsonObject pages = QJsonDocument::fromJson(f.readAll()).object().value("pages").toObject();
        for (auto it = pages.begin(); it != pages.end(); ++it) {
            QVector<Stroke> v;
            for (const QJsonValue &sv : it.value().toArray()) {
                QJsonObject o = sv.toObject();
                Stroke s;
                s.kind = o["k"].toInt(); s.color = QColor(o["c"].toString()); s.width = o["w"].toDouble(0.0035);
                s.layer = qBound(0, o["l"].toInt(), 2); s.text = o["t"].toString();
                s.bw = qMax(0.18, o["bw"].toDouble(0.34)); s.bh = qMax(0.14, o["bh"].toDouble(0.24));
                if (s.kind == 4) {
                    s.id = o["id"].toInt(); if (s.id >= nextId) nextId = s.id + 1;
                    s.pinned = o["pin"].toBool(); s.anim = s.pinned ? 1.f : 0.f;
                    for (const QJsonValue &iv : o["ink"].toArray()) {
                        QJsonObject io = iv.toObject(); Ink k;
                        k.color = QColor(io["c"].toString()); k.width = io["w"].toDouble(0.003);
                        QJsonArray ka = io["p"].toArray();
                        for (int j = 0; j + 2 < ka.size(); j += 3) { k.pts << QPointF(ka[j].toDouble(), ka[j + 1].toDouble()); k.pr << float(ka[j + 2].toDouble(0.5)); }
                        s.ink << k;
                    }
                }
                QJsonArray pa = o["p"].toArray();
                for (int i = 0; i + 2 < pa.size(); i += 3) {
                    s.pts << QPointF(pa[i].toDouble(), pa[i + 1].toDouble());
                    s.pr << float(pa[i + 2].toDouble(0.5));
                }
                v << s;
            }
            strokes[it.key().toInt()] = v;
        }
        for (auto it2 = strokes.begin(); it2 != strokes.end(); ++it2)
            for (Stroke &s : it2.value()) if (s.kind == 4 && s.id <= 0) s.id = nextId++;
    }
    int groupCounter = 0;

    // ---- sticky notes ----
    enum NoteDrag { ND_None, ND_Icon, ND_Move, ND_Resize, ND_Write, ND_Erase };
    NoteDrag nd = ND_None;
    int ndId = 0, ndPage = -1, activeId = -1, nextId = 1;
    QPointF ndStart, ndOrig;
    qint64 ndTime = 0;
    Ink liveInk;
    QTimer animTimer;

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
        qreal rad = 10.0 / r.width();
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
    void createNote(int pg, QPointF vp) {
        QRectF r = pageRect(pg);
        Stroke n; n.kind = 4; n.id = nextId++; n.layer = curLayer; n.color = color;
        n.pts << norm(vp, pg);
        qreal ph = r.height() / r.width();
        qreal x = n.pts[0].x() + 0.035;
        if (x + n.bw > 0.99) x = n.pts[0].x() - 0.035 - n.bw;
        x = qBound(0.01, x, qMax(0.01, 0.99 - n.bw));
        qreal y = qBound(0.01, n.pts[0].y() - 0.02, qMax(0.01, ph - n.bh - 0.01));
        n.pts << QPointF(x, y);
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
                if (s.kind != 4 || !layerVis[s.layer] || (s.id == activeId) != (pass == 1)) continue;
                if ((s.open || s.pinned) && noteGeom(s, r).card.contains(vp)) { hit = i; break; }
            }
        if (hit >= 0) {
            Stroke &s = v[hit];
            NoteGeom g = noteGeom(s, r);
            activeId = s.id; ndId = s.id; ndPage = pg; ndStart = vp;
            if (g.closeB.adjusted(-4, -4, 4, 4).contains(vp)) { s.pinned = false; s.open = false; markDirty(); startAnim(); return true; }
            if (g.pinB.adjusted(-4, -4, 4, 4).contains(vp)) { s.pinned = !s.pinned; markDirty(); viewport()->update(); return true; }
            if (g.typeB.adjusted(-4, -4, 4, 4).contains(vp)) {
                int id = s.id;
                QTimer::singleShot(0, this, [this, id] { editNoteText(id); });
                return true;
            }
            if (g.grip.contains(vp)) { nd = ND_Resize; ndOrig = QPointF(s.bw, s.bh); return true; }
            if (g.header.contains(vp)) { nd = ND_Move; ndOrig = s.pts[1]; return true; }
            if (t == Eraser) { nd = ND_Erase; eraseInk(s, g, vp); return true; }
            nd = ND_Write;
            liveInk = Ink(); liveInk.color = color; liveInk.width = 0.0030 * penScale;
            liveInk.pts << inkPt(g, r, vp); liveInk.pr << pr;
            return true;
        }
        if (t != Eraser && t != MaskT) {
            for (int i = v.size() - 1; i >= 0; --i) {
                const Stroke &s = v[i];
                if (s.kind == 4 && layerVis[s.layer] && noteGeom(s, r).icon.adjusted(-4, -4, 4, 4).contains(vp)) {
                    nd = ND_Icon; ndId = s.id; ndPage = pg; ndStart = vp; ndTime = laserClock.elapsed();
                    return true;
                }
            }
        }
        return false;
    }
    void noteMove(QPointF vp, float pr) {
        Stroke *s = findNote(ndId);
        if (!s) { nd = ND_None; return; }
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
            NoteGeom g = noteGeom(*s, r);
            QPointF q = inkPt(g, r, vp);
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
            if (laserClock.elapsed() - ndTime < 450) {  // quick tap toggles the card
                if (s->open || s->pinned) { if (s->pinned) { s->pinned = false; markDirty(); } s->open = false; }
                else s->open = true;
                activeId = s->id; startAnim();
            }
            break;
        case ND_Write:
            if (!liveInk.pts.isEmpty()) { s->ink.append(liveInk); s->redoInk.clear(); markDirty(); }
            liveInk = Ink();
            break;
        case ND_Move: case ND_Resize: case ND_Erase: markDirty(); break;
        default: break;
        }
        viewport()->update();
    }
};

class Win : public QMainWindow {
public:
    Canvas *cv;
    QToolBar *tb;
    QDockWidget *dock;
    QLineEdit *search;
    QCheckBox *hlBox;
    QListWidget *list;

    Win() {
        cv = new Canvas;
        setCentralWidget(cv);
        setAcceptDrops(true);
        setWindowTitle("InkPDF");
        resize(1280, 820);
        tb = addToolBar("tools");
        tb->setMovable(true);

        auto act = [&](const QString &t, const QKeySequence &k, std::function<void()> fn) {
            QAction *a = tb->addAction(t);
            a->setShortcut(k);
            connect(a, &QAction::triggered, this, [fn] { fn(); });
            return a;
        };
        act(FA("باز کردن"), QKeySequence::Open, [this] { openDialog(); });
        act(FA("ذخیره"), QKeySequence::Save, [this] { cv->saveInk(); });
        act(FA("خروجی PDF"), QKeySequence("Ctrl+E"), [this] { exportDialog(); });
        tb->addSeparator();

        auto grp = new QActionGroup(this);
        auto toolAct = [&](const QString &t, const QString &key, Tool tl) {
            QAction *a = tb->addAction(t);
            a->setCheckable(true); a->setShortcut(QKeySequence(key));
            grp->addAction(a);
            connect(a, &QAction::triggered, this, [this, tl] { cv->tool = tl; });
            return a;
        };
        toolAct(FA("قلم"), "P", Pen)->setChecked(true);
        toolAct(FA("هایلایت"), "H", Highlighter);
        toolAct(FA("خط صاف"), "L", Line);
        toolAct(FA("پاک‌کن"), "E", Eraser);
        toolAct(FA("لیزر"), "T", Laser);
        toolAct(FA("ماسک"), "M", MaskT);
        toolAct(FA("نوت"), "C", Note);
        tb->addSeparator();

        const char *cols[] = {"#d32f2f", "#1565c0", "#2e7d32", "#111111", "#ef6c00", "#7b1fa2"};
        int n = 1;
        for (const char *c : cols) {
            QPixmap pm(22, 22); pm.fill(Qt::transparent);
            QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(QColor(c)); p.setPen(Qt::NoPen); p.drawEllipse(2, 2, 18, 18); p.end();
            QAction *a = tb->addAction(QIcon(pm), QString());
            a->setShortcut(QKeySequence(QString::number(n++)));
            QString cc = c;
            connect(a, &QAction::triggered, this, [this, cc] { cv->color = QColor(cc); });
        }
        tb->addSeparator();
        act(FA("واگرد"), QKeySequence::Undo, [this] { cv->undo(); });
        act(FA("ازنو"), QKeySequence("Ctrl+Y"), [this] { cv->redo(); });
        act(FA("ضخیم‌تر"), QKeySequence("]"), [this] { cv->penScale = qMin(4.0, cv->penScale * 1.25); });
        act(FA("نازک‌تر"), QKeySequence("["), [this] { cv->penScale = qMax(0.3, cv->penScale / 1.25); });
        tb->addSeparator();
        act(FA("بزرگ‌نمایی"), QKeySequence("Ctrl+="), [this] { cv->setZoom(cv->zoomLevel() * 1.2); });
        act(FA("کوچک‌نمایی"), QKeySequence("Ctrl+-"), [this] { cv->setZoom(cv->zoomLevel() / 1.2); });
        act(FA("عرض صفحه"), QKeySequence("Ctrl+0"), [this] { cv->setZoom(1.0); });
        act(FA("حالت شب"), QKeySequence("N"), [this] { cv->night = !cv->night; cv->viewport()->update(); });
        act(FA("ارائه"), QKeySequence("F11"), [this] {
            bool full = !isFullScreen();
            tb->setVisible(!full);
            full ? showFullScreen() : showNormal();
        });
        // ---- layers + masks toolbar ----
        addToolBarBreak();
        QToolBar *tb2 = addToolBar("layers");
        tb2->addWidget(new QLabel(FA(" لایه فعال: ")));
        auto lg = new QActionGroup(this);
        for (int i = 0; i < 3; ++i) {
            QAction *a = tb2->addAction(layerName(i));
            a->setCheckable(true); a->setShortcut(QKeySequence(QString("Ctrl+%1").arg(i + 1)));
            lg->addAction(a);
            connect(a, &QAction::triggered, this, [this, i] { cv->curLayer = i; });
            if (i == 0) a->setChecked(true);
        }
        tb2->addSeparator();
        tb2->addWidget(new QLabel(FA(" نمایش: ")));
        for (int i = 0; i < 3; ++i) {
            QAction *a = tb2->addAction(layerName(i));
            a->setCheckable(true); a->setChecked(true); a->setShortcut(QKeySequence(QString("Alt+%1").arg(i + 1)));
            connect(a, &QAction::toggled, this, [this, i](bool on) { cv->layerVis[i] = on; cv->viewport()->update(); if (cv->changed) cv->changed(); });
        }
        tb2->addSeparator();
        connect(tb2->addAction(FA("پوشاندن همه ماسک‌ها")), &QAction::triggered, this, [this] { cv->setAllMasks(false); });
        connect(tb2->addAction(FA("نمایش همه ماسک‌ها")), &QAction::triggered, this, [this] { cv->setAllMasks(true); });
        tb2->addSeparator();
        connect(tb2->addAction(FA("بستن همه نوت‌ها")), &QAction::triggered, this, [this] { cv->closeAllNotes(); });
        QAction *nv = tb2->addAction(FA("نمایش نوت‌ها"));
        nv->setCheckable(true); nv->setChecked(true); nv->setShortcut(QKeySequence("Alt+N"));
        connect(nv, &QAction::toggled, this, [this](bool on) { cv->notesVisible = on; cv->viewport()->update(); });

        // ---- notes index dock ----
        dock = new QDockWidget(FA("فهرست نکات"), this);
        auto *w = new QWidget; auto *lay = new QVBoxLayout(w);
        search = new QLineEdit; search->setPlaceholderText(FA("جستجو در نکته‌ها..."));
        hlBox = new QCheckBox(FA("نمایش هایلایت‌ها"));
        list = new QListWidget; list->setWordWrap(true);
        auto *saveBtn = new QPushButton(FA("ذخیره فهرست در فایل متنی"));
        lay->addWidget(search); lay->addWidget(hlBox); lay->addWidget(list, 1); lay->addWidget(saveBtn);
        dock->setWidget(w);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->hide();
        connect(search, &QLineEdit::textChanged, this, [this] { refreshNotes(); });
        connect(hlBox, &QCheckBox::toggled, this, [this] { refreshNotes(); });
        connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
            cv->gotoPoint(it->data(Qt::UserRole).toInt(), QPointF(0, it->data(Qt::UserRole + 1).toDouble()));
            cv->openNoteById(it->data(Qt::UserRole + 2).toInt());
        });
        connect(saveBtn, &QPushButton::clicked, this, [this] { saveNotes(); });
        connect(dock, &QDockWidget::visibilityChanged, this, [this](bool v) { if (v) refreshNotes(); });
        cv->changed = [this] { if (dock->isVisible()) refreshNotes(); };
        act(FA("فهرست نکات"), QKeySequence("F2"), [this] { dock->setVisible(!dock->isVisible()); });
        auto *pageLbl = new QLabel;
        statusBar()->addPermanentWidget(pageLbl);
        auto *st = new QTimer(this);
        connect(st, &QTimer::timeout, this, [this, pageLbl] { pageLbl->setText(cv->statusText()); });
        st->start(250);
        addAction(tb->toggleViewAction());
    }

    void open(const QString &path) {
        if (!cv->load(path)) { QMessageBox::warning(this, "InkPDF", FA("فایل PDF باز نشد.")); return; }
        setWindowTitle("InkPDF - " + QFileInfo(path).fileName());
    }

protected:
    void closeEvent(QCloseEvent *e) override { cv->saveIfDirty(); QMainWindow::closeEvent(e); }
    void dragEnterEvent(QDragEnterEvent *e) override { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
    void dropEvent(QDropEvent *e) override {
        for (const QUrl &u : e->mimeData()->urls())
            if (u.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive)) { open(u.toLocalFile()); break; }
    }

private:
    static QString layerName(int i) {
        static const char *n[3] = {"پایه", "تکمیلی", "امتحانی"};
        return FA(n[i]);
    }
    void refreshNotes() {
        list->clear();
        for (const NoteRef &n : cv->collectNotes(hlBox->isChecked())) {
            if (!cv->layerVis[n.layer]) continue;
            QString body = n.kind == 4 ? (n.text.isEmpty() ? FA("(دست‌نویس)") : n.text.simplified()) : FA("(هایلایت)");
            if (!search->text().isEmpty() && !body.contains(search->text(), Qt::CaseInsensitive)) continue;
            auto *it = new QListWidgetItem(FA("ص ") + QString::number(n.page + 1) + " [" + layerName(n.layer) + "]  " + body);
            it->setData(Qt::UserRole, n.page);
            it->setData(Qt::UserRole + 1, n.pt.y());
            it->setData(Qt::UserRole + 2, n.id);
            list->addItem(it);
        }
    }
    void saveNotes() {
        QString f = QFileDialog::getSaveFileName(this, FA("ذخیره فهرست نکات"), QString(), "Text (*.txt)");
        if (f.isEmpty()) return;
        QFile file(f);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QStringList lines;
        for (int i = 0; i < list->count(); ++i) lines << list->item(i)->text();
        file.write(lines.join("\n").toUtf8());
    }
    void openDialog() {
        QString f = QFileDialog::getOpenFileName(this, FA("باز کردن PDF"), QString(), "PDF (*.pdf)");
        if (!f.isEmpty()) open(f);
    }
    void exportDialog() {
        QStringList opts{FA("بدون نوت"), FA("فقط آیکون نوت‌ها"), FA("نوت‌های باز شده")};
        bool okc = false;
        QString choice = QInputDialog::getItem(this, FA("خروجی PDF"), FA("نوت‌ها در خروجی:"), opts, 1, false, &okc);
        if (!okc) return;
        int mode = opts.indexOf(choice);
        QString f = QFileDialog::getSaveFileName(this, FA("خروجی PDF با نوشته‌ها"), QString(), "PDF (*.pdf)");
        if (f.isEmpty()) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool ok = cv->exportPdf(f, mode);
        QApplication::restoreOverrideCursor();
        if (!ok) QMessageBox::warning(this, "InkPDF", FA("خروجی گرفته نشد."));
    }
};

static void applyTheme(QApplication &app) {
    app.setStyle("Fusion");
    QFont f; f.setFamilies({"Vazirmatn", "Segoe UI", "Tahoma"}); f.setPointSize(10); app.setFont(f);
    app.setStyleSheet(R"(
QMainWindow, QDockWidget, QDialog { background:#1b1e27; color:#e8ebf3; }
QDialog QLabel { color:#e8ebf3; }
QToolBar { background:#232734; border:0; padding:5px 8px; spacing:4px; }
QToolBar QToolButton { color:#e8ebf3; background:transparent; border:0; border-radius:8px; padding:6px 8px; }
QToolBar QToolButton:hover { background:#31374a; }
QToolBar QToolButton:checked { background:#4f8cff; color:white; }
QToolBar QToolButton:pressed { background:#3b6fd4; }
QToolBar QLabel { color:#8d96ad; padding:0 4px; }
QToolBar::separator { background:#3a4056; width:1px; margin:6px 6px; }
QDockWidget::title { background:#232734; padding:8px; }
QListWidget, QLineEdit, QPlainTextEdit, QTextEdit, QComboBox { background:#1f2330; color:#e8ebf3; border:1px solid #343a4f; border-radius:8px; padding:5px; }
QListWidget::item { padding:7px; border-radius:6px; }
QListWidget::item:selected, QListWidget::item:hover { background:#31374a; color:#ffffff; }
QCheckBox { color:#c9cfdf; }
QPushButton { background:#4f8cff; color:white; border:0; border-radius:8px; padding:8px 14px; }
QPushButton:hover { background:#6a9eff; }
QScrollBar:vertical { background:transparent; width:10px; margin:2px; }
QScrollBar::handle:vertical { background:#454c63; border-radius:4px; min-height:30px; }
QScrollBar:horizontal { background:transparent; height:10px; margin:2px; }
QScrollBar::handle:horizontal { background:#454c63; border-radius:4px; min-width:30px; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { width:0; height:0; background:transparent; }
QStatusBar { background:#232734; color:#8d96ad; }
)");
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    applyTheme(app);
    app.setLayoutDirection(Qt::RightToLeft);
    Win w;
    w.show();
    if (argc > 1) w.open(QString::fromLocal8Bit(argv[1]));
#ifdef INKPDF_SNAPSHOT
    QTimer::singleShot(900, [&] { w.grab().save(qEnvironmentVariable("INKPDF_SNAPSHOT")); app.quit(); });
#endif
    return app.exec();
}
