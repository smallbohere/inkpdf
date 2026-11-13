// InkPDF logo, drawn in code (used for the window icon and to generate inkpdf.ico)
#pragma once
#include <QtGui>

static QPixmap makeLogo(int s) {
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(s / 100.0, s / 100.0);
    // rounded tile with a blue->violet gradient
    QLinearGradient bg(8, 4, 92, 96);
    bg.setColorAt(0, QColor("#4f8cff"));
    bg.setColorAt(1, QColor("#7a3cff"));
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(4, 4, 92, 92), 23, 23);
    QLinearGradient gloss(0, 4, 0, 50);
    gloss.setColorAt(0, QColor(255, 255, 255, 70));
    gloss.setColorAt(1, QColor(255, 255, 255, 0));
    p.setBrush(gloss);
    p.drawRoundedRect(QRectF(4, 4, 92, 46), 23, 23);
    // page with a folded corner
    QPainterPath sheet;
    sheet.moveTo(22, 17); sheet.lineTo(52, 17); sheet.lineTo(68, 33);
    sheet.lineTo(68, 80); sheet.quadTo(68, 84, 64, 84); sheet.lineTo(26, 84);
    sheet.quadTo(22, 84, 22, 80); sheet.closeSubpath();
    p.setBrush(QColor(0, 0, 40, 55));
    p.drawPath(sheet.translated(1.5, 2.5));
    p.setBrush(QColor("#ffffff"));
    p.drawPath(sheet);
    QPainterPath fold;
    fold.moveTo(52, 17); fold.lineTo(52, 29); fold.quadTo(52, 33, 56, 33); fold.lineTo(68, 33); fold.closeSubpath();
    p.setBrush(QColor("#d9e2ff"));
    p.drawPath(fold);
    // text lines
    p.setPen(QPen(QColor("#b9c6ee"), 3.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(30, 43), QPointF(60, 43));
    p.drawLine(QPointF(30, 53), QPointF(57, 53));
    // ink stroke
    p.setPen(QPen(QColor("#4f6bff"), 4, Qt::SolidLine, Qt::RoundCap));
    QPainterPath ink;
    ink.moveTo(29, 70); ink.cubicTo(36, 60, 40, 78, 47, 69);
    p.drawPath(ink);
    // pen (gold body, dark nib), pointing to the bottom-left
    p.save();
    p.translate(47, 69);
    p.rotate(-45);
    QPainterPath nib;
    nib.moveTo(0, 0); nib.lineTo(15, -6); nib.lineTo(15, 6); nib.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#2b2f4a"));
    p.drawPath(nib);
    QLinearGradient body(0, -6, 0, 6);
    body.setColorAt(0, QColor("#ffd45c"));
    body.setColorAt(1, QColor("#ff9f1a"));
    p.setBrush(body);
    p.drawRoundedRect(QRectF(15, -6, 36, 12), 5, 5);
    p.setBrush(QColor("#fff2c4"));
    p.drawRect(QRectF(15, -6, 4, 12));
    p.setBrush(QColor(255, 255, 255, 120));
    p.drawRoundedRect(QRectF(22, -4.2, 24, 2.4), 1.2, 1.2);
    p.restore();
    return pm;
}
