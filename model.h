// InkPDF - data model + drawing helpers
#pragma once
#include <QtWidgets>
#include <cmath>
#include <functional>

#define FA(s) QString::fromUtf8(s)

enum Tool { Pen, Highlighter, Marker, Eraser, Line, Arrow, DArrow, Ellipse, Rect, Laser, MaskT, Note, ToolCount };

// kinds: 0 pen, 1 marker, 2 line, 3 mask, 4 note, 5 text highlight, 6 arrow, 7 double arrow, 8 ellipse, 9 rectangle
struct Ink {
    int kind = 0;
    QColor color;
    qreal width = 0.0035;           // fraction of page width
    QVector<QPointF> pts;           // normalized to page width
    QVector<float> pr;              // pressure per point (may be shorter than pts: use prAt)
};
static inline float prAt(const Ink &k, int i) { return (i >= 0 && i < k.pr.size()) ? k.pr[i] : 0.5f; }

struct Stroke : Ink {
    int layer = 0;                  // 0 base, 1 extra, 2 exam tips
    QString text;                   // typed note text
    qreal bw = 0.34, bh = 0.24;     // note card size (fraction of page width)
    bool revealed = false;          // mask state (runtime only)
    int id = 0;                     // note id
    bool pinned = false;            // note stays open
    bool open = false;              // runtime
    float anim = 0;                 // runtime open animation 0..1
    QColor ncolor;                  // note color (invalid = layer color)
    int theme = 0;                  // note paper theme
    QVector<Ink> ink, redoInk;      // handwriting inside the note (coords relative to card top-left)
};
struct Act { bool added; int page; int idx; Stroke s; int group; };
struct NoteRef { int page; QPointF pt; int kind; int layer; QString text; int id; };

static qreal distSeg(QPointF p, QPointF a, QPointF b) {
    QPointF ab = b - a;
    qreal l2 = ab.x() * ab.x() + ab.y() * ab.y();
    qreal t = l2 <= 0 ? 0 : qBound(0.0, ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / l2, 1.0);
    QPointF c = a + ab * t;
    return std::hypot(p.x() - c.x(), p.y() - c.y());
}
static bool isRtl(const QString &t) {
    for (QChar c : t) if (c.unicode() >= 0x0590 && c.unicode() <= 0x08FF) return true;
    return false;
}

// ---------------------------------------------------------------- drawing
static void drawArrowHead(QPainter &p, QPointF tip, QPointF from, qreal len) {
    QPointF d = tip - from;
    qreal l = std::hypot(d.x(), d.y());
    if (l < 0.5) return;
    d /= l;
    QPointF n(-d.y(), d.x());
    QPointF b = tip - d * len;
    const qreal w = len * 0.55;
    p.drawLine(tip, b + n * w);
    p.drawLine(tip, b - n * w);
}

static void drawPen(QPainter &p, const Ink &s, const QRectF &r) {
    const qreal W = r.width();
    const int n = s.pts.size();
    auto P = [&](int i) { return QPointF(r.x() + s.pts[i].x() * W, r.y() + s.pts[i].y() * W); };
    auto wid = [&](float pr) { return qMax(0.6, s.width * W * (0.4 + 1.2 * pr)); };
    if (n == 1) {
        p.setPen(Qt::NoPen); p.setBrush(s.color);
        p.drawEllipse(P(0), wid(prAt(s, 0)) / 2, wid(prAt(s, 0)) / 2);
        return;
    }
    QVector<QPointF> pos; QVector<float> pw;   // smoothed samples (quadratic curve through midpoints)
    pos << P(0); pw << prAt(s, 0);
    if (n == 2) { pos << P(1); pw << prAt(s, 1); }
    else {
        QPointF prevMid = P(0);
        for (int i = 1; i < n - 1; ++i) {
            QPointF c = P(i), m = (P(i) + P(i + 1)) * 0.5;
            float p0 = (prAt(s, i - 1) + prAt(s, i)) * 0.5f, p1 = (prAt(s, i) + prAt(s, i + 1)) * 0.5f;
            int k = qBound(1, int(std::hypot(m.x() - prevMid.x(), m.y() - prevMid.y()) / 4.0), 8);
            for (int j = 1; j <= k; ++j) {
                qreal t = qreal(j) / k, u = 1 - t;
                pos << prevMid * (u * u) + c * (2 * u * t) + m * (t * t);
                pw << p0 + (p1 - p0) * float(t);
            }
            prevMid = m;
        }
        pos << P(n - 1); pw << prAt(s, n - 1);
    }
    bool constant = true;
    for (float v : pw) if (std::abs(v - pw[0]) > 0.02f) { constant = false; break; }
    if (constant) {
        QPainterPath path; path.moveTo(pos[0]);
        for (int i = 1; i < pos.size(); ++i) path.lineTo(pos[i]);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.color, wid(pw[0]), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    } else {
        for (int i = 1; i < pos.size(); ++i) {
            p.setPen(QPen(s.color, wid((pw[i - 1] + pw[i]) * 0.5f), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(pos[i - 1], pos[i]);
        }
    }
}

static void drawStroke(QPainter &p, const Stroke &s, const QRectF &r) {
    const qreal W = r.width();
    const int n = s.pts.size();
    if (n == 0) return;
    auto P = [&](int i) { return QPointF(r.x() + s.pts[i].x() * W, r.y() + s.pts[i].y() * W); };
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    switch (s.kind) {
    case 0: drawPen(p, s, r); break;
    case 1: {  // marker (free highlighter)
        QPainterPath path; path.moveTo(P(0));
        if (n < 3) { for (int i = 1; i < n; ++i) path.lineTo(P(i)); if (n == 1) path.lineTo(P(0) + QPointF(0.1, 0)); }
        else {
            for (int i = 1; i < n - 1; ++i) path.quadTo(P(i), (P(i) + P(i + 1)) * 0.5);
            path.lineTo(P(n - 1));
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.color, s.width * W, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin));
        p.drawPath(path);
        break;
    }
    case 2: case 6: case 7:
        if (n >= 2) {
            p.setPen(QPen(s.color, s.width * W, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPointF a = P(0), b = P(1);
            p.drawLine(a, b);
            if (s.kind >= 6) {
                qreal L = qMax(10.0, s.width * W * 4.5);
                drawArrowHead(p, b, a, L);
                if (s.kind == 7) drawArrowHead(p, a, b, L);
            }
        }
        break;
    case 8: case 9:
        if (n >= 2) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(s.color, s.width * W, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QRectF rc = QRectF(P(0), P(1)).normalized();
            if (s.kind == 8) p.drawEllipse(rc); else p.drawRect(rc);
        }
        break;
    case 3:  // mask
        if (n >= 2) {
            QRectF m = QRectF(P(0), P(1)).normalized();
            if (s.revealed) {
                p.setPen(QPen(QColor(40, 90, 160, 160), 1.5, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRect(m);
            } else {
                p.setPen(Qt::NoPen); p.setBrush(QColor(35, 70, 120)); p.drawRoundedRect(m, 4, 4);
                p.setPen(Qt::white);
                QFont f = p.font(); f.setPixelSize(qMax(10, int(qMin(m.height(), m.width()) * 0.6))); p.setFont(f);
                p.drawText(m, Qt::AlignCenter, QString::fromUtf8("؟"));
            }
        }
        break;
    case 5: {  // text highlight: union of line boxes, filled once (no double blending)
        QPainterPath path; path.setFillRule(Qt::WindingFill);
        for (int i = 0; i + 3 < n; i += 4) {
            QPolygonF poly; poly << P(i) << P(i + 1) << P(i + 2) << P(i + 3);
            path.addPolygon(poly); path.closeSubpath();
        }
        p.setPen(Qt::NoPen); p.setBrush(s.color); p.drawPath(path);
        break;
    }
    default: break;
    }
    p.restore();
}

// normalized bounding box (used to skip strokes outside the repaint area)
static QRectF strokeBounds(const Stroke &s) {
    if (s.pts.isEmpty()) return QRectF();
    qreal x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const QPointF &p : s.pts) { x0 = qMin(x0, p.x()); y0 = qMin(y0, p.y()); x1 = qMax(x1, p.x()); y1 = qMax(y1, p.y()); }
    qreal m = s.width * 1.6 + 0.03;
    return QRectF(QPointF(x0 - m, y0 - m), QPointF(x1 + m, y1 + m));
}

// eraser hit test in normalized coordinates
static bool hitStroke(const Stroke &s, QPointF q, qreal rad) {
    const int n = s.pts.size();
    if (n == 0) return false;
    switch (s.kind) {
    case 0: case 1: {
        if (n == 1) return distSeg(q, s.pts[0], s.pts[0]) < rad + s.width / 2;
        for (int k = 1; k < n; ++k) if (distSeg(q, s.pts[k - 1], s.pts[k]) < rad + s.width / 2) return true;
        return false;
    }
    case 2: case 6: case 7:
        return n >= 2 && distSeg(q, s.pts[0], s.pts[1]) < rad + s.width / 2;
    case 3:
        return n >= 2 && QRectF(s.pts[0], s.pts[1]).normalized().adjusted(-rad, -rad, rad, rad).contains(q);
    case 4:
        return std::hypot(q.x() - s.pts[0].x(), q.y() - s.pts[0].y()) < rad * 1.3;
    case 5:
        for (int i = 0; i + 3 < n; i += 4) {
            QPolygonF poly; poly << s.pts[i] << s.pts[i + 1] << s.pts[i + 2] << s.pts[i + 3];
            if (poly.containsPoint(q, Qt::WindingFill)) return true;
        }
        return false;
    case 8: case 9: {
        if (n < 2) return false;
        QRectF rc = QRectF(s.pts[0], s.pts[1]).normalized();
        QVector<QPointF> poly;
        if (s.kind == 9) poly << rc.topLeft() << rc.topRight() << rc.bottomRight() << rc.bottomLeft();
        else for (int i = 0; i < 40; ++i) {
            qreal t = i * 2 * 3.14159265358979323846 / 40;
            poly << QPointF(rc.center().x() + rc.width() / 2 * std::cos(t), rc.center().y() + rc.height() / 2 * std::sin(t));
        }
        for (int i = 0; i < poly.size(); ++i)
            if (distSeg(q, poly[i], poly[(i + 1) % poly.size()]) < rad + s.width / 2) return true;
        return false;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------- notes
static QColor layerColor(int l) {
    static const char *c[3] = {"#f2b632", "#3d9be0", "#e4524c"};
    return QColor(c[qBound(0, l, 2)]);
}
static QColor noteColor(const Stroke &s) { return s.ncolor.isValid() ? s.ncolor : layerColor(s.layer); }

struct NoteTheme { const char *name; const char *paper; const char *text; bool lines; };
static const NoteTheme kThemes[] = {
    {"کرم", "#fffcf0", "#282828", false},
    {"سفید", "#ffffff", "#222222", false},
    {"شب", "#262a36", "#e8ebf3", false},
    {"آبی ملایم", "#e8f3ff", "#1f2a3a", false},
    {"سبز ملایم", "#eaf8ec", "#1f3324", false},
    {"صورتی", "#fff0f4", "#3a1f2a", false},
    {"خط‌دار", "#fffcf0", "#282828", true},
};
static const int kThemeCount = int(sizeof(kThemes) / sizeof(kThemes[0]));

struct NoteGeom { QRectF icon, card, header, body, closeB, pinB, typeB, styleB, grip; };
static NoteGeom noteGeom(const Stroke &s, const QRectF &r) {
    NoteGeom g;
    const qreal W = r.width();
    qreal iw = qBound(20.0, W * 0.032, 34.0);
    QPointF a(r.x() + s.pts[0].x() * W, r.y() + s.pts[0].y() * W);
    QPointF c = s.pts.size() > 1 ? s.pts[1] : s.pts[0];
    g.icon = QRectF(a.x() - iw / 2, a.y() - iw * 0.55, iw, iw * 1.1);
    g.card = QRectF(r.x() + c.x() * W, r.y() + c.y() * W, s.bw * W, s.bh * W);
    qreal hh = qMax(24.0, W * 0.036);
    g.header = QRectF(g.card.x(), g.card.y(), g.card.width(), hh);
    g.body = QRectF(g.card.x(), g.card.y() + hh, g.card.width(), g.card.height() - hh);
    qreal bs = hh * 0.66, gap = hh * 0.14, y = g.card.y() + (hh - bs) / 2, x = g.card.x() + hh * 0.2;
    g.closeB = QRectF(x, y, bs, bs); x += bs + gap;
    g.pinB = QRectF(x, y, bs, bs); x += bs + gap;
    g.typeB = QRectF(x, y, bs, bs); x += bs + gap;
    g.styleB = QRectF(x, y, bs, bs);
    g.grip = QRectF(g.card.right() - 18, g.card.bottom() - 18, 18, 18);
    return g;
}

static void drawNote(QPainter &p, const Stroke &s, const QRectF &r, const Ink *live = nullptr) {
    if (s.pts.size() < 1) return;
    const qreal W = r.width();
    NoteGeom g = noteGeom(s, r);
    const QColor lc = noteColor(s);
    const NoteTheme &th = kThemes[qBound(0, s.theme, kThemeCount - 1)];
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
        p.setBrush(QColor(th.paper)); p.setPen(QPen(lc, 1.2));
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
            auto circ = [&](const QRectF &b, int a) { p.setPen(Qt::NoPen); p.setBrush(QColor(255, 255, 255, a)); p.drawEllipse(b); };
            // close
            circ(g.closeB, 70);
            p.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap));
            QPointF cc = g.closeB.center(); qreal d = g.closeB.width() * 0.22;
            p.drawLine(cc + QPointF(-d, -d), cc + QPointF(d, d)); p.drawLine(cc + QPointF(-d, d), cc + QPointF(d, -d));
            // pin
            circ(g.pinB, s.pinned ? 235 : 70);
            p.setPen(QPen(s.pinned ? lc.darker(140) : QColor(Qt::white), 1.8, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            QPointF pc = g.pinB.center(); qreal pd = g.pinB.width();
            p.drawEllipse(pc + QPointF(0, -pd * 0.13), pd * 0.14, pd * 0.14);
            p.drawLine(pc + QPointF(0, pd * 0.02), pc + QPointF(0, pd * 0.28));
            // type text
            circ(g.typeB, 70);
            p.setPen(Qt::white);
            QFont tf = p.font(); tf.setBold(true); tf.setPixelSize(qMax(8, int(g.typeB.height() * 0.72))); p.setFont(tf);
            p.drawText(g.typeB, Qt::AlignCenter, QStringLiteral("T"));
            // style (palette glyph)
            circ(g.styleB, 70);
            QPointF sc = g.styleB.center(); qreal sd = g.styleB.width() * 0.2, dot = g.styleB.width() * 0.09;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor("#ffd45c")); p.drawEllipse(sc + QPointF(-sd, -sd * 0.6), dot, dot);
            p.setBrush(QColor("#7fe08a")); p.drawEllipse(sc + QPointF(sd, -sd * 0.6), dot, dot);
            p.setBrush(QColor("#ffffff")); p.drawEllipse(sc + QPointF(0, sd * 0.9), dot, dot);
            // title
            QFont hf = p.font(); hf.setBold(true); hf.setPixelSize(qMax(9, int(g.header.height() * 0.46))); p.setFont(hf);
            p.setPen(Qt::white);
            p.drawText(g.header.adjusted(8, 0, -12, 0), Qt::AlignVCenter | Qt::AlignRight | Qt::AlignAbsolute, FA("یادداشت"));
            // body
            QFont bf = p.font(); bf.setBold(false); bf.setPixelSize(qMax(9, int(W * 0.017))); p.setFont(bf);
            const bool darkPaper = th.paper[1] == '2';  // "#26..." = night theme
            if (th.lines) {
                p.setPen(QPen(QColor(120, 150, 200, 90), 1));
                qreal lh = bf.pixelSize() * 1.7;
                for (qreal y = g.body.top() + lh; y < g.body.bottom() - 2; y += lh)
                    p.drawLine(QPointF(g.body.left() + 8, y), QPointF(g.body.right() - 8, y));
            }
            const bool empty = s.text.isEmpty() && s.ink.isEmpty() && !live;
            if (!s.text.isEmpty()) {
                p.setPen(QColor(th.text));
                p.drawText(g.body.adjusted(10, 8, -10, -8), Qt::TextWordWrap | Qt::AlignTop | Qt::AlignAbsolute | (isRtl(s.text) ? Qt::AlignRight : Qt::AlignLeft), s.text);
            } else if (empty) {
                p.setPen(darkPaper ? QColor(150, 155, 170) : QColor(150, 150, 150));
                p.drawText(g.body.adjusted(12, 8, -12, -8), Qt::TextWordWrap | Qt::AlignCenter, FA("با قلم بنویس یا از دکمه T متن تایپ کن"));
            }
            p.save();
            p.setClipRect(g.body);
            auto drawInk = [&](const Ink &k) {
                Stroke t; static_cast<Ink &>(t) = k; t.kind = 0;
                if (darkPaper && t.color.lightness() < 80) t.color = QColor(235, 238, 245);
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
