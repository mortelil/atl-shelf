#pragma once
#include <QAbstractScrollArea>
#include <QApplication>
#include <QInputMethod>
#include <QTabWidget>
#include <QTabBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QPointer>
#include <QStackedWidget>
#include <QDialog>
#include <QFormLayout>
#include <QKeyEvent>
#include <QTouchEvent>
#include <QDateTime>
#include <QTimer>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QListWidget>
#include <QLineEdit>
#include <QDir>
#include <QEventLoop>
#include <functional>

inline QPointer<QStackedWidget> mobileHost;
inline int mobileTransition = 0;
// The compositor may resize the window, or report an overlapping keyboard.
// Handle either route without subtracting the keyboard height twice.
class MobileInputController : public QObject {
    QWidget *host;
    bool pending = false;
    void schedule() {
        if (pending) return;
        pending = true;
        QTimer::singleShot(0, this, [this] {
            pending = false;
            auto *input = QGuiApplication::inputMethod();
            updateViewport(input->isVisible() ? input->keyboardRectangle() : QRectF());
        });
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == host && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) schedule();
        return false;
    }
public:
    explicit MobileInputController(QWidget *window) : QObject(window), host(window) {
        host->installEventFilter(this);
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
            if (now && host->isAncestorOf(now) && now->testAttribute(Qt::WA_InputMethodEnabled)) schedule();
        });
        auto *input = QGuiApplication::inputMethod();
        connect(input, &QInputMethod::keyboardRectangleChanged, this, [this]{ schedule(); });
        connect(input, &QInputMethod::visibleChanged, this, [this]{ schedule(); });
        connect(input, &QInputMethod::animatingChanged, this, [this]{ schedule(); });
    }
    void updateViewport(const QRectF &keyboard) {
        if (!host->layout()) return;
        // QInputMethod rectangles use window logical coordinates, not screen pixels.
        const QRectF overlap = QRectF(host->rect()).intersected(keyboard);
        const int bottom = overlap.isEmpty() ? 0 : qMax(0, host->height()-qFloor(overlap.top()));
        auto margins=host->layout()->contentsMargins();
        if (margins.bottom()!=bottom) {
            margins.setBottom(bottom); host->layout()->setContentsMargins(margins);
            host->layout()->activate();
        }
        QTimer::singleShot(0, this, [this]{ revealEditor(); });
    }
    void revealEditor() {
        auto *editor=QApplication::focusWidget();
        if (!editor || !host->isAncestorOf(editor) || !editor->isVisible() || !editor->testAttribute(Qt::WA_InputMethodEnabled)) return;
        // Only react to focus/keyboard/window changes, never to the user's scroll.
        for (auto *parent=editor->parentWidget(); parent && parent!=host; parent=parent->parentWidget()) {
            if (auto *scroll=qobject_cast<QScrollArea *>(parent)) {
                if (scroll->widget() && scroll->widget()->isAncestorOf(editor)) {
                    if (scroll->widget()->layout()) scroll->widget()->layout()->activate();
                    scroll->ensureWidgetVisible(editor,12,24);
                }
            }
        }
    }
};

// Hidden source pages must not force empty space into the active install form.
class MobileSourceTabs : public QTabWidget {
public:
    explicit MobileSourceTabs(QWidget *parent=nullptr) : QTabWidget(parent) { setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum); }
    QSize minimumSizeHint() const override {
        return currentWidget() ? currentWidget()->minimumSizeHint()+QSize(8,8) : QSize(0,0);
    }
    QSize sizeHint() const override {
        return currentWidget() ? currentWidget()->sizeHint()+QSize(8,8) : QSize(0,0);
    }
};
inline bool allowListActivation(QListWidget *list) {
    auto *viewport = list->viewport();
    return QScroller::scroller(viewport)->state() == QScroller::Inactive && viewport->property("shelfScrollUntil").toLongLong() < QDateTime::currentMSecsSinceEpoch();
}
inline void enableTouchScrolling(QWidget *root) {
    for (auto *button : root->findChildren<QPushButton *>()) button->setFocusPolicy(Qt::TabFocus);
    const auto areas = root->findChildren<QAbstractScrollArea *>();
    for (auto *area : areas) {
        if (area->property("shelfTouchReady").toBool()) continue;
        area->setProperty("shelfTouchReady", true);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // Background drags should scroll, not transfer focus away from the editor.
        // Keep list/text editor focus policies intact for keyboard accessibility.
        if (qobject_cast<QScrollArea *>(area)) {
            area->setFocusPolicy(Qt::NoFocus);
            area->viewport()->setFocusPolicy(Qt::NoFocus);
        }
        if (auto *list = qobject_cast<QAbstractItemView *>(area)) {
            list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
            list->setFocusPolicy(Qt::TabFocus);
            list->viewport()->setFocusPolicy(Qt::NoFocus);
        }
        QScroller::grabGesture(area->viewport(), QScroller::TouchGesture);
        QObject::connect(QScroller::scroller(area->viewport()), &QScroller::stateChanged, area, [area](QScroller::State state) {
            if (state == QScroller::Dragging || state == QScroller::Scrolling) area->viewport()->setProperty("shelfScrollUntil", QDateTime::currentMSecsSinceEpoch() + 350);
        });
        auto properties = QScroller::scroller(area->viewport())->scrollerProperties();
        properties.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
        properties.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
        QScroller::scroller(area->viewport())->setScrollerProperties(properties);
    }
}

// Dialog-style operations are pages inside Shelf's sole native window.
class MobileDialog : public QDialog {
    QPointer<QWidget> previous;
    bool mounted = false, changing = false, prepared = false;
    QPushButton *back = nullptr;
    QString backText = "Cancel";
    void prepare() {
        if (prepared) return;
        prepared = true;
        auto *body = new QWidget;
        if (layout()) body->setLayout(layout());
        for (auto *box : body->findChildren<QDialogButtonBox *>()) {
            for (auto *button : box->buttons()) if (box->buttonRole(button) == QDialogButtonBox::RejectRole) { box->removeButton(button); button->hide(); }
        }
        for (auto *button : body->findChildren<QPushButton *>()) {
            if (QStringList{"Cancel", "Close", "Cancel setup", "Cancel update"}.contains(button->text())) button->hide();
        }
        for (auto *form : body->findChildren<QFormLayout *>()) if (form->rowWrapPolicy() != QFormLayout::WrapAllRows) form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        auto *outer = new QVBoxLayout(this); outer->setContentsMargins(18, 16, 18, 16); outer->setSpacing(12);
        auto *row = new QHBoxLayout; back = new QPushButton(backText); back->setObjectName("pageBack"); back->setFixedWidth(80); row->addWidget(back);
        auto *title = new QLabel(windowTitle()); title->setWordWrap(true); title->setStyleSheet("font-size: 20px; font-weight: 600;"); row->addWidget(title, 1); outer->addLayout(row);
        QObject::connect(back, &QPushButton::clicked, this, [this]{ reject(); });
        auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(body); outer->addWidget(scroll, 1);
        enableTouchScrolling(this);
    }
    void detach() {
        if (!mounted || changing || !mobileHost) return;
        changing = true; ++mobileTransition;
        const bool current = mobileHost->currentWidget() == this;
        mobileHost->removeWidget(this); mounted = false;
        if (current && previous) mobileHost->setCurrentWidget(previous);
        --mobileTransition; changing = false;
    }
protected:
    void keyPressEvent(QKeyEvent *event) override { if (event->key() == Qt::Key_Back || event->key() == Qt::Key_Escape) { reject(); event->accept(); } else QDialog::keyPressEvent(event); }
public:
    explicit MobileDialog(QWidget *parent = nullptr) : QDialog(parent) { setWindowFlags(Qt::Widget); }
    ~MobileDialog() override { detach(); }
    void setBackText(const QString &text) { backText = text; if (back) back->setText(text); }
    void setVisible(bool visible) override {
        if (changing || mobileTransition) { QDialog::setVisible(visible); return; }
        if (visible && !mounted && mobileHost) {
            prepare(); setMinimumHeight(0); changing = true; ++mobileTransition; previous = mobileHost->currentWidget();
            mobileHost->addWidget(this); mounted = true; mobileHost->setCurrentWidget(this); --mobileTransition; changing = false;
        } else if (!visible) detach();
        QDialog::setVisible(visible);
    }
    int exec() override {
        QEventLoop loop; QObject::connect(this, &QDialog::finished, &loop, &QEventLoop::quit);
        setResult(QDialog::Rejected); show(); loop.exec(); hide(); return result();
    }
};
inline void mobileNotice(QWidget *parent, const QString &title, const QString &message) {
    MobileDialog page(parent); page.setWindowTitle(title); page.setBackText("‹ Back");
    auto *layout = new QVBoxLayout(&page); auto *label = new QLabel(message); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); layout->addWidget(label); layout->addStretch(); page.exec();
}
inline QString chooseMobileFile(QWidget *parent, const QString &title, const QString &initial, const QString &filter) {
    MobileDialog page(parent); page.setWindowTitle(title);
    auto *layout = new QVBoxLayout(&page);
    auto *places = new QHBoxLayout; auto *home = new QPushButton("Home"); auto *downloads = new QPushButton("Downloads"); auto *up = new QPushButton("Parent folder");
    places->addWidget(home); places->addWidget(downloads); layout->addLayout(places); layout->addWidget(up);
    auto *path = new QLineEdit; path->setObjectName("fileNameEdit"); path->setPlaceholderText("Folder or file path"); layout->addWidget(path);
    auto *list = new QListWidget; list->setObjectName("fileList"); layout->addWidget(list, 1);
    auto *hint = new QLabel; hint->setWordWrap(true); layout->addWidget(hint);
    auto *choose = new QPushButton("Choose file"); choose->setProperty("primary", true); layout->addWidget(choose);
    QString directory, selected;
    const bool apkOnly = filter.contains("*.apk");
    auto load = [&](const QString &folder) {
        directory = QDir(folder).absolutePath(); path->setText(directory); list->clear();
        for (const auto &file : QDir(directory).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable, QDir::DirsFirst | QDir::Name)) {
            if (file.isFile() && apkOnly && file.suffix().toLower() != "apk") continue;
            auto *item = new QListWidgetItem((file.isDir() ? "▸  " : "") + file.fileName(), list); item->setData(Qt::UserRole, file.absoluteFilePath());
        }
        hint->setText(apkOnly ? "Choose an APK file. Tap a folder to open it." : "Choose the executable file.");
    };
    QObject::connect(home, &QPushButton::clicked, &page, [&]{ load(QDir::homePath()); });
    QObject::connect(downloads, &QPushButton::clicked, &page, [&]{ const QString dir = QDir::homePath() + "/Downloads"; load(QDir(dir).exists() ? dir : QDir::homePath()); });
    QObject::connect(up, &QPushButton::clicked, &page, [&]{ QDir dir(directory); dir.cdUp(); load(dir.absolutePath()); });
    QObject::connect(list, &QListWidget::itemClicked, &page, [&](QListWidgetItem *item){ if (!allowListActivation(list)) return; QFileInfo file(item->data(Qt::UserRole).toString()); if (file.isDir()) load(file.absoluteFilePath()); else path->setText(file.absoluteFilePath()); });
    auto acceptFile = [&]{ QFileInfo file(path->text()); if (file.isDir()) { load(file.absoluteFilePath()); return; } if (!file.isFile() || (apkOnly && file.suffix().toLower() != "apk")) { hint->setText("Choose an existing " + QString(apkOnly ? "APK file." : "file.")); return; } selected = file.absoluteFilePath(); page.accept(); };
    QObject::connect(choose, &QPushButton::clicked, &page, acceptFile); QObject::connect(path, &QLineEdit::returnPressed, &page, acceptFile);
    load(initial.isEmpty() ? QDir::homePath() : QFileInfo(initial).absolutePath());
    return page.exec() == QDialog::Accepted ? selected : QString();
}
