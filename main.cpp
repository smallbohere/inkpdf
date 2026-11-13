// InkPDF - lightweight PDF reader with stylus ink for teaching (Windows, C++17 / Qt 6)
#include "canvas.h"
#include "logo.h"
#include <QSettings>

static QIcon colorIcon(const QColor &c, int s = 22) {
    QPixmap pm(s, s); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(c); p.setPen(QPen(QColor(255, 255, 255, 90), 1)); p.drawEllipse(2, 2, s - 4, s - 4);
    return QIcon(pm);
}
static QIcon paletteIcon(int s = 22) {
    QPixmap pm(s, s); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#c9cfdf")); p.drawEllipse(1, 2, s - 2, s - 4);
    const char *cs[4] = {"#e4524c", "#f2b632", "#3fa66b", "#3d9be0"};
    QPointF pts[4] = {{7, 8}, {12, 6}, {16.5, 9}, {16, 14.5}};
    for (int i = 0; i < 4; ++i) { p.setBrush(QColor(cs[i])); p.drawEllipse(pts[i], 2.2, 2.2); }
    p.setBrush(QColor("#232734")); p.drawEllipse(QPointF(9, 15), 2.6, 2.6);
    return QIcon(pm);
}

// shows the current brush size and colour
class SizePreview : public QWidget {
public:
    Canvas *cv;
    explicit SizePreview(Canvas *c) : cv(c) { setFixedSize(46, 30); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen); p.setBrush(QColor("#1f2330")); p.drawRoundedRect(rect(), 8, 8);
        qreal d = qBound(2.0, 2 + cv->sizes[cv->tool] * 0.4, 24.0);
        if (cv->tool == Eraser) { p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor("#c9cfdf"), 1.4)); }
        else p.setBrush(cv->color);
        p.drawEllipse(QRectF(rect()).center(), d / 2, d / 2);
    }
};

// small pill at the top-center that shows/hides both toolbars
class BarHandle : public QToolButton {
public:
    bool expanded = true;
    explicit BarHandle(QWidget *parent) : QToolButton(parent) {
        setFixedSize(54, 18); setCursor(Qt::PointingHandCursor);
        setToolTip(FA("نمایش / مخفی کردن نوار ابزار (F10)"));
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen); p.setBrush(QColor(35, 39, 52, 225));
        p.drawRoundedRect(rect().adjusted(0, -10, 0, 0), 9, 9);
        p.setPen(QPen(QColor("#e8ebf3"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPointF c(width() / 2.0, height() / 2.0 + 1);
        qreal dy = expanded ? 2.5 : -2.5;  // chevron up = "hide"
        p.drawLine(c + QPointF(-6, dy), c + QPointF(0, -dy));
        p.drawLine(c + QPointF(0, -dy), c + QPointF(6, dy));
    }
};

class Win : public QMainWindow {
public:
    Canvas *cv;
    QToolBar *tb, *tb2;
    BarHandle *handle;
    QDockWidget *dock;
    QLineEdit *search;
    QCheckBox *hlBox;
    QListWidget *list;
    QSlider *sizeSlider, *stabSlider;
    SizePreview *preview;
    QActionGroup *toolGrp, *colorGrp;
    QAction *toolActs[ToolCount] = {};
    QAction *colorActs[8] = {};
    QAction *shapeAct = nullptr;
    QColor quick[8];
    int quickSel = 0;
    Tool lastShape = Line;
    bool barsBeforeFull = true;

    Win() {
        cv = new Canvas;
        setCentralWidget(cv);
        setAcceptDrops(true);
        setWindowTitle("InkPDF");
        resize(1280, 820);
        loadPrefs();

        // ------------------------------------------------ row 1: files, tools, view
        tb = addToolBar("tools");
        tb->setMovable(false);
        auto act = [&](const QString &t, const QKeySequence &k, std::function<void()> fn) {
            QAction *a = tb->addAction(t);
            if (!k.isEmpty()) a->setShortcut(k);
            connect(a, &QAction::triggered, this, [fn] { fn(); });
            return a;
        };
        act(FA("باز کردن"), QKeySequence::Open, [this] { openDialog(); });
        act(FA("ذخیره"), QKeySequence::Save, [this] { cv->saveInk(); });
        act(FA("خروجی"), QKeySequence("Ctrl+E"), [this] { exportDialog(); });
        tb->addSeparator();

        toolGrp = new QActionGroup(this);
        auto toolAct = [&](const QString &t, const QString &key, Tool tl) {
            QAction *a = tb->addAction(t);
            a->setCheckable(true);
            if (!key.isEmpty()) a->setShortcut(QKeySequence(key));
            toolGrp->addAction(a);
            toolActs[tl] = a;
            connect(a, &QAction::triggered, this, [this, tl] { setTool(tl); });
            return a;
        };
        toolAct(FA("قلم"), "P", Pen)->setChecked(true);
        toolAct(FA("هایلایت متن"), "H", Highlighter);
        toolAct(FA("ماژیک"), "K", Marker);

        auto *shapeMenu = new QMenu(this);
        struct Sh { const char *name; const char *key; Tool t; };
        const Sh shapes[] = {{"خط صاف", "L", Line}, {"پیکان", "A", Arrow}, {"پیکان دوطرفه", "D", DArrow},
                             {"بیضی / دایره", "O", Ellipse}, {"مستطیل / مربع", "R", Rect}};
        shapeAct = tb->addAction(FA("اشکال"));
        shapeAct->setCheckable(true);
        toolGrp->addAction(shapeAct);
        for (const Sh &s : shapes) {
            QAction *a = shapeMenu->addAction(FA(s.name));
            a->setShortcut(QKeySequence(s.key));
            addAction(a);  // keep the shortcut active
            Tool tl = s.t; QString nm = FA(s.name);
            connect(a, &QAction::triggered, this, [this, tl, nm] { lastShape = tl; shapeAct->setText(nm); shapeAct->setChecked(true); setTool(tl); });
        }
        shapeAct->setMenu(shapeMenu);
        connect(shapeAct, &QAction::triggered, this, [this] { setTool(lastShape); });
        if (auto *b = qobject_cast<QToolButton *>(tb->widgetForAction(shapeAct))) b->setPopupMode(QToolButton::MenuButtonPopup);

        toolAct(FA("پاک‌کن"), "E", Eraser);
        toolAct(FA("لیزر"), "T", Laser);
        toolAct(FA("ماسک"), "M", MaskT);
        toolAct(FA("نوت"), "C", Note);
        tb->addSeparator();
        act(FA("واگرد"), QKeySequence::Undo, [this] { cv->undo(); });
        act(FA("ازنو"), QKeySequence("Ctrl+Y"), [this] { cv->redo(); });
        tb->addSeparator();
        act(FA("+"), QKeySequence("Ctrl+="), [this] { cv->setZoom(cv->zoomLevel() * 1.2); });
        act(FA("−"), QKeySequence("Ctrl+-"), [this] { cv->setZoom(cv->zoomLevel() / 1.2); });
        act(FA("عرض"), QKeySequence("Ctrl+0"), [this] { cv->setZoom(1.0); });
        act(FA("شب"), QKeySequence("N"), [this] { cv->night = !cv->night; cv->viewport()->update(); });
        act(FA("ارائه"), QKeySequence("F11"), [this] { togglePresentation(); });

        // ------------------------------------------------ row 2: colours, size, stabilizer, layers
        addToolBarBreak();
        tb2 = addToolBar("style");
        tb2->setMovable(false);
        colorGrp = new QActionGroup(this);
        for (int i = 0; i < 8; ++i) {
            QAction *a = tb2->addAction(colorIcon(quick[i]), QString());
            a->setCheckable(true);
            a->setShortcut(QKeySequence(QString::number(i + 1)));
            a->setToolTip(FA("انتخاب رنگ — راست‌کلیک: تغییر این رنگ"));
            colorGrp->addAction(a);
            colorActs[i] = a;
            connect(a, &QAction::triggered, this, [this, i] { selectQuick(i); });
            if (QWidget *w = tb2->widgetForAction(a)) {
                w->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(w, &QWidget::customContextMenuRequested, this, [this, i] { openPalette(i); });
            }
        }
        colorActs[0]->setChecked(true);
        cv->color = quick[0];
        QAction *pal = tb2->addAction(paletteIcon(), FA("پالت"));
        pal->setToolTip(FA("پالت کامل رنگ و شخصی‌سازی رنگ‌های سریع"));
        connect(pal, &QAction::triggered, this, [this] { openPalette(quickSel); });
        tb2->addSeparator();

        tb2->addWidget(new QLabel(FA("ضخامت")));
        sizeSlider = new QSlider(Qt::Horizontal);
        sizeSlider->setLayoutDirection(Qt::LeftToRight);
        sizeSlider->setRange(1, 60); sizeSlider->setFixedWidth(130);
        sizeSlider->setToolTip(FA("ضخامت ابزار فعال ( [ و ] )"));
        tb2->addWidget(sizeSlider);
        preview = new SizePreview(cv);
        tb2->addWidget(preview);
        connect(sizeSlider, &QSlider::valueChanged, this, [this](int v) { cv->sizes[cv->tool] = v; preview->update(); });
        tb2->addSeparator();

        tb2->addWidget(new QLabel(FA("پایداری قلم")));
        stabSlider = new QSlider(Qt::Horizontal);
        stabSlider->setLayoutDirection(Qt::LeftToRight);
        stabSlider->setRange(0, 10); stabSlider->setFixedWidth(90); stabSlider->setValue(cv->stab);
        stabSlider->setToolTip(FA("هرچه بیشتر، لرزش دست کمتر می‌شود (۰ = بدون صاف‌سازی)"));
        tb2->addWidget(stabSlider);
        connect(stabSlider, &QSlider::valueChanged, this, [this](int v) { cv->stab = v; });
        tb2->addSeparator();

        tb2->addWidget(new QLabel(FA("لایه:")));
        auto lg = new QActionGroup(this);
        const char *lnames[3] = {"پایه", "تکمیلی", "امتحانی"};
        for (int i = 0; i < 3; ++i) {
            QAction *a = tb2->addAction(FA(lnames[i]));
            a->setCheckable(true); a->setShortcut(QKeySequence(QString("Ctrl+%1").arg(i + 1)));
            lg->addAction(a);
            connect(a, &QAction::triggered, this, [this, i] { cv->curLayer = i; });
            if (i == 0) a->setChecked(true);
        }
        auto *viewMenu = new QMenu(this);
        for (int i = 0; i < 3; ++i) {
            QAction *a = viewMenu->addAction(FA("نمایش لایه ") + FA(lnames[i]));
            a->setCheckable(true); a->setChecked(true); a->setShortcut(QKeySequence(QString("Alt+%1").arg(i + 1)));
            addAction(a);
            connect(a, &QAction::toggled, this, [this, i](bool on) { cv->layerVis[i] = on; cv->viewport()->update(); if (cv->changed) cv->changed(); });
        }
        viewMenu->addSeparator();
        QAction *nv = viewMenu->addAction(FA("نمایش نوت‌ها"));
        nv->setCheckable(true); nv->setChecked(true); nv->setShortcut(QKeySequence("Alt+N"));
        addAction(nv);
        connect(nv, &QAction::toggled, this, [this](bool on) { cv->notesVisible = on; cv->viewport()->update(); });
        connect(viewMenu->addAction(FA("بستن همه نوت‌ها")), &QAction::triggered, this, [this] { cv->closeAllNotes(); });
        viewMenu->addSeparator();
        connect(viewMenu->addAction(FA("پوشاندن همه ماسک‌ها")), &QAction::triggered, this, [this] { cv->setAllMasks(false); });
        connect(viewMenu->addAction(FA("نمایش همه ماسک‌ها")), &QAction::triggered, this, [this] { cv->setAllMasks(true); });
        QAction *viewAct = tb2->addAction(FA("نمایش و ماسک"));
        viewAct->setMenu(viewMenu);
        if (auto *b = qobject_cast<QToolButton *>(tb2->widgetForAction(viewAct))) b->setPopupMode(QToolButton::InstantPopup);

        // keys: [ and ] change the size of the current tool
        auto *bigger = new QAction(this); bigger->setShortcut(QKeySequence("]")); addAction(bigger);
        connect(bigger, &QAction::triggered, this, [this] { sizeSlider->setValue(sizeSlider->value() + 3); });
        auto *smaller = new QAction(this); smaller->setShortcut(QKeySequence("[")); addAction(smaller);
        connect(smaller, &QAction::triggered, this, [this] { sizeSlider->setValue(sizeSlider->value() - 3); });
        auto *barKey = new QAction(this); barKey->setShortcut(QKeySequence("F10")); addAction(barKey);
        connect(barKey, &QAction::triggered, this, [this] { setBars(!tb->isVisible()); });

        // handle that hides / shows both toolbars
        handle = new BarHandle(cv->viewport());
        cv->overlay = handle;
        connect(handle, &QToolButton::clicked, this, [this] { setBars(!tb->isVisible()); });
        handle->show(); handle->raise();

        // ------------------------------------------------ notes index dock
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

        setTool(Pen);
    }

    void open(const QString &path) {
        if (!cv->load(path)) { QMessageBox::warning(this, "InkPDF", FA("فایل PDF باز نشد.")); return; }
        setWindowTitle("InkPDF - " + QFileInfo(path).fileName());
    }

    void setTool(Tool t) {
        cv->tool = t;
        cv->hoverOn = false;
        QSignalBlocker b(sizeSlider);
        sizeSlider->setValue(int(cv->sizes[t]));
        sizeSlider->setEnabled(t != MaskT && t != Note);
        preview->update();
        cv->viewport()->update();
    }
    void selectQuick(int i) {
        quickSel = i;
        cv->color = quick[i];
        colorActs[i]->setChecked(true);
        preview->update();
    }
    void setQuick(int i, const QColor &c) {
        quick[i] = c;
        colorActs[i]->setIcon(colorIcon(c));
        if (quickSel == i) cv->color = c;
        preview->update();
    }
    void setBars(bool on) {
        tb->setVisible(on); tb2->setVisible(on);
        handle->expanded = on; handle->update();
    }
    void togglePresentation() {
        if (!isFullScreen()) { barsBeforeFull = tb->isVisible(); setBars(false); showFullScreen(); }
        else { showNormal(); setBars(barsBeforeFull); }
    }

    // full colour palette; edits the selected quick slot
    void openPalette(int slot) {
        QDialog dlg(this);
        dlg.setWindowTitle(FA("پالت رنگ"));
        auto *lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel(FA("یک خانه از رنگ‌های سریع را انتخاب کن، بعد روی رنگ دلخواه بزن:")));
        int target = slot;
        QToolButton *slotBtn[8];
        auto refresh = [&] { for (int i = 0; i < 8; ++i) { slotBtn[i]->setIcon(colorIcon(quick[i], 30)); slotBtn[i]->setChecked(i == target); } };
        auto *row = new QHBoxLayout;
        for (int i = 0; i < 8; ++i) {
            slotBtn[i] = new QToolButton; slotBtn[i]->setCheckable(true);
            slotBtn[i]->setIconSize(QSize(30, 30)); slotBtn[i]->setFixedSize(40, 40);
            slotBtn[i]->setStyleSheet("QToolButton{background:#1f2330;border:2px solid #343a4f;border-radius:10px;}QToolButton:checked{border-color:#4f8cff;background:#31374a;}");
            connect(slotBtn[i], &QToolButton::clicked, &dlg, [&, i] { target = i; refresh(); });
            row->addWidget(slotBtn[i]);
        }
        lay->addLayout(row);
        refresh();
        auto apply = [&](const QColor &c) { setQuick(target, c); refresh(); };
        auto *grid = new QGridLayout;
        grid->setSpacing(5);
        const qreal levels[5] = {0.86, 0.72, 0.56, 0.42, 0.28};
        QVector<QColor> cols;
        for (int l = 0; l < 5; ++l) for (int h = 0; h < 12; ++h) cols << QColor::fromHslF(h / 12.0, 0.78, levels[l]);
        for (int i = 0; i < 12; ++i) { int v = i * 255 / 11; cols << QColor(v, v, v); }
        for (int i = 0; i < cols.size(); ++i) {
            auto *b = new QToolButton; b->setFixedSize(30, 30);
            b->setStyleSheet(QString("QToolButton{background:%1;border:2px solid #343a4f;border-radius:7px;}QToolButton:hover{border-color:#ffffff;}").arg(cols[i].name()));
            QColor c = cols[i];
            connect(b, &QToolButton::clicked, &dlg, [&, c] { apply(c); });
            grid->addWidget(b, i / 12, i % 12);
        }
        lay->addLayout(grid);
        auto *btns = new QHBoxLayout;
        auto *custom = new QPushButton(FA("رنگ دلخواه…"));
        connect(custom, &QPushButton::clicked, &dlg, [&] {
            QColor c = QColorDialog::getColor(quick[target], &dlg, FA("رنگ دلخواه"));
            if (c.isValid()) apply(c);
        });
        auto *done = new QPushButton(FA("تأیید"));
        connect(done, &QPushButton::clicked, &dlg, &QDialog::accept);
        btns->addWidget(custom); btns->addStretch(); btns->addWidget(done);
        lay->addLayout(btns);
        dlg.exec();
        selectQuick(target);
        savePrefs();
    }

protected:
    void closeEvent(QCloseEvent *e) override { savePrefs(); cv->saveIfDirty(); QMainWindow::closeEvent(e); }
    void dragEnterEvent(QDragEnterEvent *e) override { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
    void dropEvent(QDropEvent *e) override {
        for (const QUrl &u : e->mimeData()->urls())
            if (u.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive)) { open(u.toLocalFile()); break; }
    }

private:
    static QString layerName(int i) {
        static const char *n[3] = {"پایه", "تکمیلی", "امتحانی"};
        return FA(n[qBound(0, i, 2)]);
    }
    void loadPrefs() {
        static const char *defq[8] = {"#d32f2f", "#1565c0", "#2e7d32", "#111111", "#ef6c00", "#7b1fa2", "#fbc02d", "#00838f"};
        QSettings st("InkPDF", "InkPDF");
        QStringList q = st.value("quick").toStringList();
        for (int i = 0; i < 8; ++i) {
            QColor c = i < q.size() ? QColor(q[i]) : QColor();
            quick[i] = c.isValid() ? c : QColor(defq[i]);
        }
        cv->stab = qBound(0, st.value("stab", 3).toInt(), 10);
        QStringList sz = st.value("sizes").toStringList();
        if (sz.size() == ToolCount) for (int i = 0; i < ToolCount; ++i) cv->sizes[i] = qBound(1, sz[i].toInt(), 60);
        cv->defTheme = qBound(0, st.value("noteTheme", 0).toInt(), kThemeCount - 1);
        QColor nc(st.value("noteColor").toString());
        if (nc.isValid()) cv->defNoteColor = nc;
    }
    void savePrefs() {
        QSettings st("InkPDF", "InkPDF");
        QStringList q; for (const QColor &c : quick) q << c.name();
        st.setValue("quick", q);
        st.setValue("stab", cv->stab);
        QStringList sz; for (int i = 0; i < ToolCount; ++i) sz << QString::number(int(cv->sizes[i]));
        st.setValue("sizes", sz);
        st.setValue("noteTheme", cv->defTheme);
        st.setValue("noteColor", cv->defNoteColor.isValid() ? cv->defNoteColor.name() : QString());
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
QToolBar QToolButton::menu-button { border:0; width:12px; }
QToolBar QLabel { color:#8d96ad; padding:0 4px; }
QToolBar::separator { background:#3a4056; width:1px; margin:6px 6px; }
QSlider::groove:horizontal { height:4px; background:#3a4056; border-radius:2px; }
QSlider::sub-page:horizontal { background:#4f8cff; border-radius:2px; }
QSlider::handle:horizontal { background:#e8ebf3; width:14px; height:14px; margin:-6px 0; border-radius:7px; }
QSlider::handle:horizontal:disabled { background:#59607a; }
QMenu { background:#232734; color:#e8ebf3; border:1px solid #343a4f; border-radius:8px; padding:6px; }
QMenu::item { padding:6px 22px; border-radius:6px; }
QMenu::item:selected { background:#31374a; }
QMenu::separator { height:1px; background:#343a4f; margin:5px 8px; }
QToolTip { background:#232734; color:#e8ebf3; border:1px solid #343a4f; }
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
    QIcon icon;
    for (int s : {16, 24, 32, 48, 64, 128, 256}) icon.addPixmap(makeLogo(s));
    app.setWindowIcon(icon);
    app.setLayoutDirection(Qt::RightToLeft);
    Win w;
    w.show();
    if (argc > 1) w.open(QString::fromLocal8Bit(argv[1]));
#ifdef INKPDF_SNAPSHOT
    QTimer::singleShot(900, [&] { w.grab().save(qEnvironmentVariable("INKPDF_SNAPSHOT")); app.quit(); });
#endif
    return app.exec();
}
