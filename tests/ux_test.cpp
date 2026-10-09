#include <QApplication>
#include <QTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QStackedWidget>
#include <QDialog>
#include <QFileDialog>
#include <QScreen>
#include <QScroller>
#include <QProcess>
#include <QTimer>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTcpServer>
#include <QElapsedTimer>
#include <memory>
#include <QLocalServer>
#include <QLocalSocket>
#include "../src/apk_icon.h"

QWidget *createShelfForTests();
QWidget *currentPageForTests();
QByteArray fetchForTests(const QUrl &, QWidget *);
static QString dataRoot;
static void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); if (!f.open(QIODevice::WriteOnly)) qFatal("Cannot write fixture"); f.write(data);
}
static int visibleWindows() { int count = 0; for (auto *w : QApplication::topLevelWidgets()) if (w->isVisible() && w->isWindow()) ++count; return count; }
static QPushButton *button(QWidget *root, const QString &text) {
    for (auto *b : root->findChildren<QPushButton *>()) if (b->text() == text) return b;
    return nullptr;
}
class UXTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        QDir(dataRoot + "/ATL Shelf").removeRecursively();
        const QString root = dataRoot + "/ATL Shelf/atl-shelf";
        put(root + "/settings.json", QJsonDocument(QJsonObject{{"runtimeType", "existing"}, {"atl", dataRoot + "/fake-atl"}, {"launcher", dataRoot + "/fake-atl"}}).toJson());
        put(root + "/apps.json", QJsonDocument(QJsonArray{QJsonObject{{"id", "immich"}, {"name", "Immich"}, {"source", "local"}, {"version", "3.2.4"}, {"width",720}, {"height",1280}}}).toJson());
        put(root + "/apps/immich/app.apk", "fixture");
        put(dataRoot + "/fake-atl", "#!/bin/sh\nprintf '%s\\n' \"$ATL_RENDER_SCALE\" \"$@\" > '" + dataRoot.toUtf8() + "/launched'\n");
        QFile::setPermissions(dataRoot + "/fake-atl", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::remove(dataRoot + "/launched");
    }
    void navigationAndLaunchAfterSave() {
        std::unique_ptr<QWidget> w(createShelfForTests()); w->resize(408, 794); w->show(); QTest::qWait(100);
        QVERIFY(w->width() <= 408);
        QVERIFY(!button(w.get(), "Set up runtime…")->isVisible());
        w->grab().save("/tmp/shelf-library.png");
        auto *library = w->findChild<QListWidget *>("library"); QVERIFY(library); QCOMPARE(library->count(), 1);
        QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier, library->visualItemRect(library->item(0)).center());
        QCOMPARE(w->findChild<QStackedWidget *>("pages")->currentIndex(), 1);
        QVERIFY(button(w.get(), "Open app")->isVisible());
        QVERIFY(!button(w.get(), "Save settings")->isVisible());
        QTest::qWait(100); QCOMPARE(w->findChild<QScrollArea *>()->horizontalScrollBar()->maximum(), 0);
        w->grab().save("/tmp/shelf-detail.png");
        QCOMPARE(w->findChild<QPushButton *>("mainBack")->mapTo(w.get(), QPoint(0,0)), QPoint(18,16));
        button(w.get(), "App settings ▾")->click();
        button(w.get(), "Save settings")->click();
        button(w.get(), "Open app")->click();
        QTRY_VERIFY(QFile::exists(dataRoot + "/launched"));
        QFile launch(dataRoot + "/launched"); QVERIFY(launch.open(QIODevice::ReadOnly)); QVERIFY(launch.readAll().contains("apps/immich/app.apk"));
        button(w.get(), "Settings")->click(); QCOMPARE(w->findChild<QStackedWidget *>("pages")->currentIndex(), 2);
        QVERIFY(button(w.get(), "Look for ATL updates")->isVisible());
    }
    void cancelNetworkTransfer() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        std::unique_ptr<QWidget> parent(createShelfForTests()); parent->show(); bool canceled = false;
        QTimer::singleShot(100, [&] {
            auto *dialog = qobject_cast<QDialog *>(currentPageForTests()); QVERIFY(dialog); QVERIFY(!dialog->isWindow()); QCOMPARE(visibleWindows(), 1);
            auto *cancel = dialog->findChild<QPushButton *>("pageBack"); QVERIFY(cancel); canceled = true; cancel->click();
        });
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(fetchForTests(QUrl("http://127.0.0.1:" + QString::number(server.serverPort()) + "/slow.apk"), parent.get()).isEmpty());
        QVERIFY(canceled); QVERIFY(elapsed.elapsed() < 3000);
    }
    void installLocalApkAndOpen() {
        put(dataRoot + "/Example.apk", QByteArray::fromBase64("UEsDBBQAAAAAAOoBSV34+BLbIgAAACIAAAATAAAAQW5kcm9pZE1hbmlmZXN0LnhtbDxtYW5pZmVzdCBwYWNrYWdlPSJleGFtcGxlLnRlc3QiLz5QSwECFAMUAAAAAADqAUld+PgS2yIAAAAiAAAAEwAAAAAAAAAAAAAAgAEAAAAAQW5kcm9pZE1hbmlmZXN0LnhtbFBLBQYAAAAAAQABAEEAAABTAAAAAAA="));
        std::unique_ptr<QWidget> w(createShelfForTests()); w->resize(408, 794); w->show();
        bool installed = false;
        QTimer::singleShot(50, [&] {
            auto *dialog = qobject_cast<QDialog *>(currentPageForTests()); QVERIFY(dialog);
            dialog->findChild<QComboBox *>("sourcePicker")->setCurrentIndex(3);
            QTimer::singleShot(50, [&] {
                auto *picker = qobject_cast<QDialog *>(currentPageForTests()); QVERIFY(picker); QVERIFY(!picker->isWindow()); QCOMPARE(visibleWindows(), 1);
                auto *fileName = picker->findChild<QLineEdit *>("fileNameEdit"); QVERIFY(fileName); fileName->setText(dataRoot + "/Example.apk"); button(picker, "Choose file")->click();
            });
            button(dialog, "Choose APK…")->click();
            QCOMPARE(dialog->findChild<QLineEdit *>("installName")->text(), "Example");
            QVERIFY(button(dialog, "Install app")->isEnabled());
            button(dialog, "Install app")->click(); installed = true;
        });
        button(w.get(), "＋  Add app")->click(); QVERIFY(installed);
        QCOMPARE(w->findChild<QListWidget *>("library")->count(), 2);
        QVERIFY(QFile::exists(dataRoot + "/applications/atl-shelf-example.desktop"));
        button(w.get(), "Open app")->click();
        QTRY_VERIFY(QFile::exists(dataRoot + "/launched"));
        QFile launch(dataRoot + "/launched"); QVERIFY(launch.open(QIODevice::ReadOnly)); QVERIFY(launch.readAll().contains("apps/example/app.apk"));
    }
    void touchScrollDoesNotOpenAnApp() {
        QJsonArray entries; for (int i=0; i<40; ++i) entries.append(QJsonObject{{"id",QString::number(i)}, {"name",QString("App %1").arg(i)}, {"source","local"}});
        put(dataRoot + "/ATL Shelf/atl-shelf/apps.json", QJsonDocument(entries).toJson());
        std::unique_ptr<QWidget> w(createShelfForTests()); w->resize(408,794); w->show(); QTest::qWait(100);
        auto *list = w->findChild<QListWidget *>("library"); auto *viewport = list->viewport();
        QVERIFY(QScroller::hasScroller(viewport));
        static auto *device = QTest::createTouchDevice();
        QTest::touchEvent(viewport, device).press(0, QPoint(150,300), viewport); QTest::qWait(50);
        QTest::touchEvent(viewport, device).move(0, QPoint(150,220), viewport); QTest::qWait(50);
        QTest::touchEvent(viewport, device).move(0, QPoint(150,80), viewport); QTest::qWait(50);
        QTest::touchEvent(viewport, device).release(0, QPoint(150,80), viewport);
        QTRY_VERIFY(list->verticalScrollBar()->value() > 0);
        QCOMPARE(w->findChild<QStackedWidget *>("pages")->currentIndex(), 0);
        QScroller::scroller(viewport)->stop(); QTest::qWait(400);
        QTest::touchEvent(viewport, device).press(0, QPoint(150,40), viewport); QTest::qWait(30);
        QTest::touchEvent(viewport, device).release(0, QPoint(150,40), viewport);
        QTRY_COMPARE(w->findChild<QStackedWidget *>("pages")->currentIndex(), 1);
    }
    void displayMigrationAndMenuLaunch() {
        const QString root = dataRoot + "/ATL Shelf/atl-shelf";
        put(root + "/settings.json", QJsonDocument(QJsonObject{{"atl",dataRoot + "/fake-atl"}, {"launcher",dataRoot + "/fake-atl"}, {"runtimeEnv",QJsonObject{{"ATL_RENDER_SCALE","2.65"}}}}).toJson());
        std::unique_ptr<QWidget> w(createShelfForTests());
        QFile settings(root + "/settings.json"); QVERIFY(settings.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(settings.readAll()).object()["runtimeEnv"].toObject()["ATL_RENDER_SCALE"].toString(), "1");
        QFile apps(root + "/apps.json"); QVERIFY(apps.open(QIODevice::ReadOnly)); const auto entry=QJsonDocument::fromJson(apps.readAll()).array()[0].toObject();
        QVERIFY(entry["fitScreen"].toBool()); QCOMPARE(entry["width"].toInt(), QGuiApplication::primaryScreen()->availableGeometry().width());
        QFile desktop(dataRoot + "/applications/atl-shelf-immich.desktop"); QVERIFY(desktop.open(QIODevice::ReadOnly)); QVERIFY(desktop.readAll().contains("--launch"));
        const QString binary=qEnvironmentVariable("SHELF_BINARY",QCoreApplication::applicationDirPath()+"/atl-shelf");
        QVERIFY(QFile::exists(binary)); QProcess launch; launch.start(binary,{"--launch","immich"}); QVERIFY(launch.waitForFinished(5000)); QCOMPARE(launch.exitCode(),0);
        QTRY_VERIFY(QFile::exists(dataRoot+"/launched")); QFile result(dataRoot+"/launched"); QVERIFY(result.open(QIODevice::ReadOnly)); const auto output=result.readAll();
        QVERIFY2(output.startsWith("1\n"), output.constData());
        QVERIFY(output.contains("-w\n"+QByteArray::number(QGuiApplication::primaryScreen()->availableGeometry().width())+"\n"));
    }
    void fractionalScreenLaunch() {
        const QString binary=qEnvironmentVariable("SHELF_BINARY",QCoreApplication::applicationDirPath()+"/atl-shelf");
        const QString tools=dataRoot+"/screen-tools";
        const QJsonObject mode{{"id","1"},{"size",QJsonObject{{"width",1080},{"height",2280}}}};
        const QJsonObject output{{"name",QGuiApplication::primaryScreen()->name()},{"enabled",true},{"connected",true},{"scale",2.65},{"currentModeId","1"},{"rotation",1},{"modes",QJsonArray{mode}}};
        const QByteArray fixture=QJsonDocument(QJsonObject{{"outputs",QJsonArray{output}}}).toJson(QJsonDocument::Compact);
        put(tools+"/kscreen-doctor","#!/bin/sh\ncat <<'SCREEN'\n"+fixture+"\nSCREEN\n");
        QFile::setPermissions(tools+"/kscreen-doctor",QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
        auto env=QProcessEnvironment::systemEnvironment(); env.insert("PATH",tools+":"+env.value("PATH"));
        QProcess launch; launch.setProcessEnvironment(env); launch.start(binary,{"--launch","immich"});
        QVERIFY(launch.waitForFinished(5000)); QCOMPARE(launch.exitCode(),0);
        QTRY_VERIFY(QFile::exists(dataRoot+"/launched")); QFile result(dataRoot+"/launched"); QVERIFY(result.open(QIODevice::ReadOnly));
        const auto args=result.readAll(); QVERIFY2(args.startsWith("2.65\n"),args.constData());
        QVERIFY(args.contains("-w\n"+QByteArray::number(QGuiApplication::primaryScreen()->availableGeometry().width())+"\n"));
        QProcess gui; gui.setProcessEnvironment(env); gui.start(binary); QVERIFY(gui.waitForStarted());
        const QString settings=dataRoot+"/ATL Shelf/atl-shelf/settings.json";
        auto nativeWidth=[&]{ QFile f(settings); f.open(QIODevice::ReadOnly); return QJsonDocument::fromJson(f.readAll()).object()["screenWidth"].toInt(); };
        QTRY_COMPARE(nativeWidth(),1080);
        gui.terminate(); QVERIFY(gui.waitForFinished(5000));
    }
    void forwardsDesktopActivationToken() {
        QLocalServer server;
        QVERIFY(server.listen(dataRoot+"/ATL Shelf/atl-shelf/ui.sock"));
        const QString binary=qEnvironmentVariable("SHELF_BINARY",QCoreApplication::applicationDirPath()+"/atl-shelf");
        auto env=QProcessEnvironment::systemEnvironment(); env.insert("XDG_ACTIVATION_TOKEN","shelf-test-token");
        QProcess second; second.setProcessEnvironment(env); second.start(binary); QVERIFY(second.waitForFinished(5000)); QCOMPARE(second.exitCode(),0);
        QTRY_VERIFY(server.hasPendingConnections());
        auto *socket=server.nextPendingConnection(); QTRY_VERIFY(socket->canReadLine());
        QCOMPARE(QByteArray::fromBase64(socket->readLine().trimmed()),QByteArray("shelf-test-token"));
        delete socket;
    }
    void adaptiveApkIcon() {
        QTemporaryDir fixture;
        const auto fake=fixture.path()+"/aapt";
        put(fake, R"SH(#!/bin/sh
case "$2" in
--values) cat <<'RES'
        resource 0x7f010001 app:color/bg: t=0x1c d=0xff1122cc
          (color) #ff1122cc
        resource 0x7f020001 app:drawable/fg: t=0x03 d=0x00000001
          (string8) "res/fg.xml"
RES
;;
badging) echo "application-icon-640:'res/icon.xml'" ;;
xmltree) case "$4" in
res/icon.xml) cat <<'XML'
  E: adaptive-icon (line=1)
    E: background (line=2)
      A: android:drawable(0x01010199)=@0x7f010001
    E: foreground (line=3)
      A: android:drawable(0x01010199)=@0x7f020001
XML
;;
res/fg.xml) cat <<'XML'
  E: vector (line=1)
    A: android:viewportWidth(0x01010402)=(type 0x4)0x42d80000
    A: android:viewportHeight(0x01010403)=(type 0x4)0x42d80000
    E: path (line=2)
      A: android:fillColor(0x01010404)=(type 0x1c)0xffff0000
      A: android:pathData(0x01010405)="M44,44h20v20h-20z"
XML
;;
esac ;;
esac
)SH");
        QFile::setPermissions(fake,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
        const auto oldPath=qgetenv("PATH"); qputenv("PATH",fixture.path().toUtf8()+":"+oldPath);
        const auto image=ApkIcon().extract("fixture.apk"); qputenv("PATH",oldPath);
        QVERIFY(!image.isNull()); QCOMPARE(image.size(),QSize(512,512));
        QCOMPARE(image.pixelColor(0,0).alpha(),0);
        QCOMPARE(image.pixelColor(256,256),QColor("#ff0000"));
        QCOMPARE(image.pixelColor(256,30),QColor("#1122cc"));
    }
    void cliRefreshesVisibleLibrary() {
        std::unique_ptr<QWidget> w(createShelfForTests()); w->resize(408,794); w->show();
        auto *list=w->findChild<QListWidget *>("appsList");
        if(!list) list=w->findChild<QListWidget *>();
        QVERIFY(list); QVERIFY(list->count()>0);
        const QString binary=qEnvironmentVariable("SHELF_BINARY",QCoreApplication::applicationDirPath()+"/atl-shelf");
        QProcess cli; cli.start(binary,{"cli","configure","immich","--set",R"({"name":"Changed over SSH"})"});
        QVERIFY(cli.waitForFinished(5000)); QCOMPARE(cli.exitCode(),0);
        QTRY_VERIFY(list->item(0)->text().contains("Changed over SSH"));
    }
    void singleInstance() {
        const QString binary=qEnvironmentVariable("SHELF_BINARY",QCoreApplication::applicationDirPath()+"/atl-shelf");
        QProcess first; first.start(binary); QVERIFY(first.waitForStarted());
        QTRY_VERIFY(QFile::exists(dataRoot+"/ATL Shelf/atl-shelf/ui.sock"));
        QProcess second; second.start(binary); QVERIFY(second.waitForFinished(5000)); QCOMPARE(second.exitCode(),0); QCOMPARE(first.state(),QProcess::Running);
        first.terminate(); QVERIFY(first.waitForFinished(5000));
    }
    void firstRunAndInstallDialog() {
        QFile::remove(dataRoot + "/ATL Shelf/atl-shelf/settings.json");
        {
            std::unique_ptr<QWidget> w(createShelfForTests()); w->resize(408, 794); w->show(); QTest::qWait(50);
            QVERIFY(button(w.get(), "Set up Android support")->isVisible());
            w->grab().save("/tmp/shelf-welcome.png");
        }
        init();
        std::unique_ptr<QWidget> w(createShelfForTests()); w->resize(408, 794); w->show();
        bool visited = false;
        QTimer::singleShot(50, [&] {
            auto *dialog = qobject_cast<QDialog *>(currentPageForTests());
            QVERIFY(dialog); QVERIFY(!dialog->isWindow()); QCOMPARE(visibleWindows(), 1); visited = true; QVERIFY(dialog->width() <= w->width());
            auto *source = dialog->findChild<QComboBox *>("sourcePicker"); QVERIFY(source); QCOMPARE(source->currentIndex(), 1);
            QVERIFY(!button(dialog, "Install app")->isEnabled());
            QCOMPARE(dialog->findChild<QPushButton *>("pageBack")->mapTo(w.get(), QPoint(0,0)), QPoint(18,16));
            w->grab().save("/tmp/shelf-add.png");
            source->setCurrentIndex(3); QVERIFY(!button(dialog, "Install app")->isEnabled());
            dialog->reject();
        });
        button(w.get(), "＋  Add app")->click(); QVERIFY(visited);
    }
};
int main(int argc, char **argv) {
    QTemporaryDir data; dataRoot = data.path();
    qputenv("HOME", dataRoot.toUtf8());
    qputenv("XDG_DATA_HOME", dataRoot.toUtf8());
    qputenv("XDG_CONFIG_HOME", (dataRoot + "/config").toUtf8());
    // No calls to the real session's menu index or systemd during UI tests.
    for (const QString &command : {"systemctl", "kbuildsycoca6"}) {
        const QString path = dataRoot + "/bin/" + command; put(path, "#!/bin/sh\nexit 0\n");
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    qputenv("PATH", (dataRoot + "/bin:").toUtf8() + qgetenv("PATH"));
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv); app.setApplicationName("atl-shelf"); app.setOrganizationName("ATL Shelf");
    UXTest test; return QTest::qExec(&test,argc,argv);
}
#include "ux_test.moc"
