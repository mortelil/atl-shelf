#include <QApplication>
#include <QScreen>
#include <QIntValidator>
#include <QPointer>
#include <QLockFile>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QProgressBar>
#include <QThread>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QSysInfo>
#include <QTabWidget>
#include <QTabBar>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>
#include <QProcess>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QDesktopServices>
#include <QTextDocumentFragment>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QIcon>
#include <QSet>
#include <QMap>
#include <algorithm>
#include <functional>
#include <memory>
#include <unistd.h>
#include <signal.h>
#include <QLocalServer>
#include <QLocalSocket>
#include <QCloseEvent>
#include "mobile_ui.h"
#include "display_profile.h"
#include "apk_icon.h"
#ifdef HAVE_KWINDOWSYSTEM
#include <KWindowSystem>
#endif

static QString rootDir() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}
static QString dbPath() { return rootDir() + "/apps.json"; }
static QString settingsFilePath() { return rootDir() + "/settings.json"; }
static QString runtimeRoot() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/atl-shelf/runtime"; }
static bool saveDisplayPatch(const QString &root) {
    QFile source(":/atl-native-density.patch"); QSaveFile target(root + "/atl-native-density.patch");
    return source.open(QIODevice::ReadOnly) && target.open(QIODevice::WriteOnly) && target.write(source.readAll()) > 0 && target.commit();
}
static std::unique_ptr<QLockFile> lockLibrary(QWidget *parent = nullptr) {
    QDir().mkpath(rootDir());
    auto lock = std::make_unique<QLockFile>(rootDir() + "/operations.lock");
    if (!lock->tryLock()) {
        if (parent) mobileNotice(parent, "An update is running", "Shelf is already installing or updating an app. Please wait for it to finish, then try again.");
        return {};
    }
    return lock;
}
static QJsonObject readSettings() {
    QFile f(settingsFilePath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    QJsonObject settings = QJsonDocument::fromJson(f.readAll()).object();
    if (settings.value("runtimeType").toString() == "github") {
        const QString root = runtimeRoot() + "/github";
        QJsonObject env = settings.value("runtimeEnv").toObject();
        env["ATL_WORKSPACE"] = root + "/workspace";
        env["ATL_PREFIX"] = root + "/prefix";
        env["ATL_BUILD_DIR"] = root + "/workspace/android_translation_layer/build-mobile";
        settings["runtimeEnv"] = env;
    }
    QJsonObject env = settings.value("runtimeEnv").toObject(); env["ATL_RENDER_SCALE"] = QString::number(currentDisplay().scale); settings["runtimeEnv"] = env;
    return settings;
}
static QSize appWindowSize(const QJsonObject &app) {
    if (!app.value("fitScreen").toBool(true)) return QSize(app.value("width").toInt(408), app.value("height").toInt(861));
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) && QGuiApplication::primaryScreen()) return currentDisplay().pixels;
    const auto settings = readSettings();
    return QSize(settings.value("screenWidth").toInt(408), settings.value("screenHeight").toInt(861));
}

static QJsonArray readApps() {
    QFile f(dbPath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).array();
}
static bool writeApps(const QJsonArray &apps) {
    QDir().mkpath(rootDir());
    QSaveFile f(dbPath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (f.write(QJsonDocument(apps).toJson(QJsonDocument::Indented)) < 0) return false;
    return f.commit();
}
static QString appDir(const QJsonObject &o) { return rootDir() + "/apps/" + o.value("id").toString(); }
static QString apkPath(const QJsonObject &o) { return appDir(o) + "/app.apk"; }
static QString desktopQuote(QString s) {
    s.replace("\\", "\\\\"); s.replace("\"", "\\\""); s.replace("`", "\\`"); s.replace("$", "\\$");
    return "\"" + s + "\"";
}
static QString desktopPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
}
static void updateDesktop(const QJsonObject &o, const QString &atl) {
    QDir().mkpath(desktopPath()); QDir().mkpath(appDir(o));
    Q_UNUSED(atl);
    const QString exec = desktopQuote(QCoreApplication::applicationFilePath()) + " --launch " + desktopQuote(o.value("id").toString());
    QFile f(desktopPath() + "/atl-shelf-" + o.value("id").toString() + ".desktop");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        const QString contents = "[Desktop Entry]\nType=Application\nName=" + o.value("name").toString().replace("\n", " ") +
          "\nComment=Android app via ATL\nExec=" + exec + "\n" +
          "Icon=" + (o.value("icon").toString().isEmpty() ? "application-x-apk" : o.value("icon").toString()) +
          "\nTerminal=false\nCategories=Utility;\n";
        f.write(contents.toUtf8()); f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
}
static QString safeId(QString s) {
    s = s.toLower(); s.replace(QRegularExpression("[^a-z0-9_-]+"), "-");
    return s.trimmed().replace(QRegularExpression("^-+|-+$"), "");
}
static void fitDialog(QWidget &widget, int width = 560, int height = 600) {
    QSize available = QGuiApplication::primaryScreen()->availableGeometry().size();
    if (widget.parentWidget()) available.setWidth(qMin(available.width(), widget.parentWidget()->window()->width()));
    widget.resize(qMin(width, available.width() - 16), qMin(height, available.height() - 32));
}
class TransferProgress;
static QPointer<TransferProgress> transferDialog;
class TransferProgress : public MobileDialog {
    QPointer<TransferProgress> previous;
    QLabel *label; QProgressBar *bar; bool canceled = false;
public:
    TransferProgress(const QString &text, QWidget *parent) : MobileDialog(parent), previous(transferDialog) {
        setWindowTitle("Please wait"); setBackText("Cancel");
        auto *layout = new QVBoxLayout(this); label = new QLabel(text); label->setWordWrap(true); layout->addWidget(label);
        bar = new QProgressBar; bar->setRange(0, 0); layout->addWidget(bar); layout->addStretch();
        transferDialog = this; show(); QApplication::processEvents();
    }
    ~TransferProgress() override { transferDialog = previous; }
    void reject() override { canceled = true; MobileDialog::reject(); }
    bool wasCanceled() const { return canceled; }
    void setLabelText(const QString &text) { label->setText(text); }
    void setRange(int min, int max) { bar->setRange(min, max); }
    void setValue(int value) { bar->setValue(value); }
};
static bool (*requestCanceled)() = nullptr;
static QByteArray httpGet(const QUrl &url, int *status = nullptr, bool html = false) {
    if (transferDialog && transferDialog->wasCanceled()) { if (status) *status = 0; return {}; }
    QNetworkAccessManager manager;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, html
        ? "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/131.0.0.0 Safari/537.36"
        : "ATL-Shelf/0.2");
    req.setRawHeader("Accept", html ? "text/html,application/xhtml+xml;q=0.9,*/*;q=0.8" : "application/vnd.github+json,application/json;q=0.9,*/*;q=0.8");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QEventLoop loop;
    QNetworkReply *reply = manager.get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64 received, qint64 total) {
        timer.start(60000); // Inactivity timeout; large APKs may take longer overall.
        if (transferDialog) {
            transferDialog->setLabelText("Downloading from " + url.host() + "\n" + QString::number(received / 1048576.0, 'f', 1) + " MB" + (total > 0 ? " of " + QString::number(total / 1048576.0, 'f', 1) + " MB" : ""));
            transferDialog->setRange(0, total > 0 ? 1000 : 0);
            if (total > 0) transferDialog->setValue(int(1000.0 * received / total));
        }
    });
    if (transferDialog) QObject::connect(transferDialog, &QDialog::rejected, reply, &QNetworkReply::abort);
    QTimer cancelPoll;
    if (requestCanceled) { QObject::connect(&cancelPoll, &QTimer::timeout, reply, [reply] { if (requestCanceled()) reply->abort(); }); cancelPoll.start(100); }
    timer.start(60000); loop.exec();
    if (status) *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray bytes = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
    reply->deleteLater(); return bytes;
}
struct ApkSourceResult { QByteArray bytes; QByteArray iconBytes; QString version; QString versionCode; QString latestCode; QString message; };

static bool apkMatchesHostArchitecture(const QByteArray &apk) {
    const QByteArray eocdSignature = QByteArray::fromHex("504b0506");
    const int eocd = apk.lastIndexOf(eocdSignature);
    if (eocd < 0 || eocd + 22 > apk.size()) return false;
    const auto byteAt = [&apk](int offset) -> quint32 { return static_cast<unsigned char>(apk.at(offset)); };
    const auto read16 = [&apk, &byteAt](int offset) -> quint16 { return offset >= 0 && offset <= apk.size() - 2 ? static_cast<quint16>(byteAt(offset) | (byteAt(offset + 1) << 8)) : 0; };
    const auto read32 = [&apk, &byteAt](int offset) -> quint32 { return offset >= 0 && offset <= apk.size() - 4 ? byteAt(offset) | (byteAt(offset + 1) << 8) | (byteAt(offset + 2) << 16) | (byteAt(offset + 3) << 24) : 0; };
    int position = static_cast<int>(read32(eocd + 16));
    const int entries = read16(eocd + 10);
    if (position < 0 || position >= apk.size()) return false;
    bool hasNativeLibs = false, compatible = false, hasManifest = false;
    const QString arch = QSysInfo::currentCpuArchitecture().toLower();
    const QStringList wanted = (arch.contains("aarch64") || arch.contains("arm64")) ? QStringList{"arm64-v8a"} :
        (arch.contains("arm") ? QStringList{"armeabi-v7a", "armeabi"} :
        ((arch.contains("x86_64") || arch.contains("amd64")) ? QStringList{"x86_64"} : QStringList{"x86"}));
    for (int i = 0; i < entries && position + 46 <= apk.size(); ++i) {
        if (read32(position) != 0x02014b50) break;
        const int nameLength = read16(position + 28), extraLength = read16(position + 30), commentLength = read16(position + 32);
        if (position + 46 + nameLength + extraLength + commentLength > apk.size()) break;
        const QString path = QString::fromUtf8(apk.constData() + position + 46, nameLength);
        if (path == "AndroidManifest.xml") hasManifest = true;
        if (path.startsWith("lib/")) {
            hasNativeLibs = true;
            for (const QString &abi : wanted) if (path.startsWith("lib/" + abi + "/")) compatible = true;
        }
        position += 46 + nameLength + extraLength + commentLength;
    }
    return hasManifest && (!hasNativeLibs || compatible);
}

static int apkArchitectureScore(const QString &fileName) {
    const QString n = fileName.toLower();
    if (!n.endsWith(".apk")) return -1;
    const QString arch = QSysInfo::currentCpuArchitecture().toLower();
    const bool wantsArm64 = arch.contains("aarch64") || arch.contains("arm64");
    const bool wantsArm32 = arch.contains("arm") && !wantsArm64;
    const bool wantsX64 = arch.contains("x86_64") || arch.contains("amd64");
    const bool wantsX86 = arch == "i386" || arch == "i686" || arch == "x86";
    const bool hasArm64 = n.contains("arm64") || n.contains("aarch64") || n.contains("armv8");
    const bool hasArm32 = n.contains("armeabi") || n.contains("armv7") || n.contains("arm32");
    const bool hasX64 = n.contains("x86_64") || n.contains("x64") || n.contains("amd64");
    const bool hasX86 = n.contains("x86") && !hasX64;
    if ((hasArm64 && !wantsArm64) || (hasArm32 && !wantsArm32) || (hasX64 && !wantsX64) || (hasX86 && !wantsX86)) return -1;
    if ((wantsArm64 && hasArm64) || (wantsArm32 && hasArm32) || (wantsX64 && hasX64) || (wantsX86 && hasX86)) return 100;
    if (n.contains("universal") || n.contains("fat") || n.contains("_all") || n.contains("-all")) return 60;
    const bool hasAbi = hasArm64 || hasArm32 || hasX64 || hasX86;
    return hasAbi ? -1 : 50;
}

static QString normalizeRepo(QString s) { s = s.trimmed(); s.remove(QRegularExpression("^https?://(www\\.)?github\\.com/", QRegularExpression::CaseInsensitiveOption)); while (s.endsWith('/')) s.chop(1); s.remove(QRegularExpression("\\.git$", QRegularExpression::CaseInsensitiveOption)); return s; }
static QJsonObject latestGithubRelease(const QString &repo, int *status = nullptr) {
    const QUrl url("https://api.github.com/repos/" + repo.trimmed() + "/releases/latest");
    return QJsonDocument::fromJson(httpGet(url, status)).object();
}
static QJsonObject bestGithubAsset(const QJsonObject &release, QString *why = nullptr) {
    QJsonObject best; int bestScore = -1;
    for (const QJsonValue &v : release.value("assets").toArray()) {
        const QJsonObject candidate = v.toObject();
        const QString fileName = candidate.value("name").toString().toLower();
        int score = apkArchitectureScore(fileName);
        if (score >= 0) {
            if (fileName.contains("release")) score += 5;
            if (fileName.contains("debug") || fileName.contains("test") || fileName.contains("source") || fileName.contains("symbols")) score -= 30;
        }
        if (score > bestScore) { best = candidate; bestScore = score; }
    }
    if (best.isEmpty()) { if (why) *why = "The latest GitHub release has no APK for " + QSysInfo::currentCpuArchitecture() + "."; return {}; }
    return best;
}
static bool downloadApk(const QUrl &url, const QString &expectedDigest, ApkSourceResult *out) {
    int status = 0; const QByteArray bytes = httpGet(url, &status);
    if (status != 200 || bytes.size() < 100 || !bytes.startsWith("PK")) { out->message = "Download failed, or the file is not a valid APK."; return false; }
    const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    if (!expectedDigest.isEmpty() && expectedDigest.toLatin1().toLower() != hash) { out->message = "SHA-256 verification failed. The APK was not installed."; return false; }
    if (!apkMatchesHostArchitecture(bytes)) { out->message = "This APK is invalid or does not support " + QSysInfo::currentCpuArchitecture() + "."; return false; }
    out->bytes = bytes; return true;
}
static QJsonDocument verifiedFdroidIndex(QString *message) {
    static QByteArray verifiedIndexDigest;
    static QJsonDocument verifiedIndexDocument;
    const QString expectedSigner = "802A9799016112346E1FEFF47A029E54DD5DCE7A";
    const QString expectedPrimary = "37D2C98789D8311948394E3E41E7044E1DBA2E89";
    int status = 0;
    const QByteArray entryBytes = httpGet(QUrl("https://f-droid.org/repo/entry.json"), &status);
    if (status != 200 || entryBytes.isEmpty()) { *message = "Could not fetch F-Droid's signed entry.json (HTTP " + QString::number(status) + ")."; return {}; }
    const QByteArray signatureBytes = httpGet(QUrl("https://f-droid.org/repo/entry.json.asc"), &status);
    if (status != 200 || signatureBytes.isEmpty()) { *message = "Could not fetch the F-Droid signature for entry.json."; return {}; }

    QTemporaryDir temp;
    if (!temp.isValid()) { *message = "Could not create a temporary directory for signature verification."; return {}; }
    const QString home = temp.path() + "/gnupg";
    if (!QDir().mkpath(home)) { *message = "Could not prepare GnuPG for signature verification."; return {}; }
    QFile::setPermissions(home, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile embeddedKey(":/fdroid-signing-key.asc");
    if (!embeddedKey.open(QIODevice::ReadOnly)) { *message = "ATL Shelf is missing its embedded F-Droid verification key."; return {}; }
    const QString keyPath = temp.path() + "/fdroid-signing-key.asc";
    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::WriteOnly) || keyFile.write(embeddedKey.readAll()) < 1000) { *message = "Could not prepare the F-Droid verification key."; return {}; }
    keyFile.close();

    QProcess gpg;
    gpg.start("gpg", {"--homedir", home, "--batch", "--quiet", "--import", keyPath});
    if (!gpg.waitForStarted(5000) || !gpg.waitForFinished(30000) || gpg.exitCode() != 0) { *message = "GnuPG is missing or could not load ATL Shelf's embedded F-Droid key."; return {}; }
    const QString entryPath = temp.path() + "/entry.json";
    const QString signaturePath = temp.path() + "/entry.json.asc";
    QFile entryFile(entryPath), signatureFile(signaturePath);
    if (!entryFile.open(QIODevice::WriteOnly) || entryFile.write(entryBytes) != entryBytes.size()) { *message = "Could not prepare the signed F-Droid index entry."; return {}; }
    entryFile.close();
    if (!signatureFile.open(QIODevice::WriteOnly) || signatureFile.write(signatureBytes) != signatureBytes.size()) { *message = "Could not prepare the F-Droid signature file."; return {}; }
    signatureFile.close();
    gpg.start("gpg", {"--homedir", home, "--batch", "--status-fd=1", "--verify", signaturePath, entryPath});
    if (!gpg.waitForStarted(5000) || !gpg.waitForFinished(30000)) { *message = "GnuPG could not verify the F-Droid signature."; return {}; }
    const QByteArray gpgStatus = gpg.readAllStandardOutput();
    bool trustedSignature = false;
    for (const QByteArray &line : gpgStatus.split('\n')) {
        const QList<QByteArray> fields = line.trimmed().split(' ');
        if (fields.size() > 3 && fields[0] == "[GNUPG:]" && fields[1] == "VALIDSIG" && fields[2].toUpper() == expectedSigner.toLatin1() && fields.last().toUpper() == expectedPrimary.toLatin1()) trustedSignature = true;
    }
    if (gpg.exitCode() != 0 || !trustedSignature) { *message = "F-Droid entry.json has an invalid signature or was signed with an unknown key."; return {}; }

    QJsonParseError parseError;
    const QJsonDocument entry = QJsonDocument::fromJson(entryBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !entry.isObject()) { *message = "The F-Droid signature is valid, but entry.json could not be parsed."; return {}; }
    const QJsonObject indexInfo = entry.object().value("index").toObject();
    const QString indexName = indexInfo.value("name").toString();
    const QString expectedHash = indexInfo.value("sha256").toString().toLower();
    if (!indexName.startsWith("/") || indexName.contains("..") || !QRegularExpression("^[a-f0-9]{64}$").match(expectedHash).hasMatch()) { *message = "The signed F-Droid entry.json contains invalid index metadata."; return {}; }
    if (verifiedIndexDigest == expectedHash.toLatin1() && !verifiedIndexDocument.isNull()) return verifiedIndexDocument;

    const QString cachePath = rootDir() + "/fdroid/index-v2.json";
    QByteArray indexBytes;
    QFile cache(cachePath);
    if (cache.open(QIODevice::ReadOnly)) indexBytes = cache.readAll();
    auto hashMatches = [&] { return !indexBytes.isEmpty() && QCryptographicHash::hash(indexBytes, QCryptographicHash::Sha256).toHex() == expectedHash.toLatin1(); };
    if (!hashMatches()) {
        const QUrl indexUrl("https://f-droid.org/repo" + indexName);
        indexBytes = httpGet(indexUrl, &status);
        if (status != 200 || !hashMatches()) { *message = status != 200 ? "Could not fetch the F-Droid index (HTTP " + QString::number(status) + ")." : "The F-Droid index SHA-256 does not match the signed entry.json; the file was rejected."; return {}; }
        QDir().mkpath(QFileInfo(cachePath).absolutePath());
        QSaveFile saved(cachePath);
        if (saved.open(QIODevice::WriteOnly) && saved.write(indexBytes) == indexBytes.size()) saved.commit();
    }
    const QJsonDocument index = QJsonDocument::fromJson(indexBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !index.isObject()) { *message = "The verified F-Droid index could not be parsed."; return {}; }
    verifiedIndexDigest = expectedHash.toLatin1();
    verifiedIndexDocument = index;
    return index;
}
static QByteArray fdroidIcon(const QJsonObject &package, QString *message) {
    const QJsonObject iconMap = package.value("metadata").toObject().value("icon").toObject();
    QJsonObject icon = iconMap.value("en-US").toObject();
    if (icon.isEmpty() && !iconMap.isEmpty()) icon = iconMap.begin().value().toObject();
    const QString name = icon.value("name").toString();
    const QString digest = icon.value("sha256").toString().toLower();
    if (!name.startsWith('/') || name.contains("..") || !QRegularExpression("^[a-f0-9]{64}$").match(digest).hasMatch()) return {};
    int status = 0;
    const QByteArray bytes = httpGet(QUrl("https://f-droid.org/repo" + name), &status);
    if (status != 200 || bytes.size() < 8 || !bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != digest.toLatin1()) {
        if (message) *message = "The signed F-Droid app icon could not be downloaded or verified.";
        return {};
    }
    return bytes;
}
static void saveAppIcon(QJsonObject &o, const QByteArray &bytes) {
    if (bytes.isEmpty() || o.value("iconSource").toString() == "apk") return;
    const QString path = appDir(o) + "/icon.png";
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) o["icon"] = path;
}
static void extractAppIcon(QJsonObject &o) {
    const QImage icon = ApkIcon().extract(apkPath(o));
    if (icon.isNull()) return;
    const QString path = appDir(o) + "/launcher-icon.png";
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && icon.save(&file, "PNG") && file.commit()) { o["icon"] = path; o["iconSource"] = "apk"; }
}
static bool latestFdroidVersion(const QString &packageId, QString *version, QString *versionCode, QString *message) {
    if (!QRegularExpression("^[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)+$").match(packageId).hasMatch()) { *message = "Invalid F-Droid package ID."; return false; }
    const QJsonDocument index = verifiedFdroidIndex(message);
    if (index.isNull()) return false;
    const QJsonObject versions = index.object().value("packages").toObject().value(packageId).toObject().value("versions").toObject();
    int latestCode = -1; QString latestName;
    for (auto it = versions.begin(); it != versions.end(); ++it) {
        const QJsonObject manifest = it.value().toObject().value("manifest").toObject();
        const int code = manifest.value("versionCode").toInt(-1);
        if (code > latestCode) { latestCode = code; latestName = manifest.value("versionName").toString(); }
    }
    if (latestCode < 0) { *message = "No published versions were found in the F-Droid index."; return false; }
    *version = latestName; *versionCode = QString::number(latestCode); return true;
}
static bool getFdroidApk(const QString &packageId, ApkSourceResult *out) {
    if (!QRegularExpression("^[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)+$").match(packageId).hasMatch()) { out->message = "Invalid F-Droid package ID."; return false; }
    const QJsonDocument index = verifiedFdroidIndex(&out->message);
    if (index.isNull()) return false;
    const QJsonObject package = index.object().value("packages").toObject().value(packageId).toObject();
    const QByteArray appIcon = fdroidIcon(package, nullptr);
    const QJsonObject versions = package.value("versions").toObject();
    QString latestName; int latestCode = -1;
    for (auto it = versions.begin(); it != versions.end(); ++it) { const QJsonObject manifest = it.value().toObject().value("manifest").toObject(); const int code = manifest.value("versionCode").toInt(-1); if (code > latestCode) { latestCode = code; latestName = manifest.value("versionName").toString(); } }
    if (latestCode < 0) { out->message = "No published versions were found in F-Droid."; return false; }
    out->latestCode = QString::number(latestCode);
    QList<QJsonObject> candidates;
    for (auto it = versions.begin(); it != versions.end(); ++it) { const QJsonObject item = it.value().toObject(); if (item.value("manifest").toObject().value("versionName").toString() == latestName) candidates.append(item); }
    std::sort(candidates.begin(), candidates.end(), [](const QJsonObject &a, const QJsonObject &b) { return a.value("manifest").toObject().value("versionCode").toInt() > b.value("manifest").toObject().value("versionCode").toInt(); });
    QString lastError;
    for (const QJsonObject &item : candidates) {
        const QJsonObject file = item.value("file").toObject();
        const QString filename = file.value("name").toString();
        if (!filename.startsWith("/") || filename.contains("..")) continue;
        ApkSourceResult candidate; candidate.version = latestName; candidate.versionCode = QString::number(item.value("manifest").toObject().value("versionCode").toInt());
        if (downloadApk(QUrl("https://f-droid.org/repo" + filename), file.value("sha256").toString(), &candidate)) { *out = candidate; out->iconBytes = appIcon; out->latestCode = QString::number(latestCode); return true; }
        lastError = candidate.message;
    }
    out->message = lastError.isEmpty() ? "No APK was found for the latest F-Droid version." : lastError;
    return false;
}
static void collectFdroidApps(const QJsonValue &value, QMap<QString, QJsonObject> *found) {
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) collectFdroidApps(item, found);
        return;
    }
    if (!value.isObject()) return;
    const QJsonObject object = value.toObject();
    QString packageId;
    for (const QString key : {"packageName", "package_name", "package", "applicationId", "appid"}) {
        if (object.value(key).isString()) { packageId = object.value(key).toString(); break; }
    }
    if (packageId.isEmpty() && object.value("id").isString()) packageId = object.value("id").toString();
    if (packageId.isEmpty() && object.value("url").isString()) {
        const QRegularExpressionMatch match = QRegularExpression("/packages/([A-Za-z0-9_.]+)(?:[/?#]|$)").match(object.value("url").toString());
        if (match.hasMatch()) packageId = match.captured(1);
    }
    const QString name = object.value("name").toString(object.value("title").toString());
    if (QRegularExpression("^[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)+$").match(packageId).hasMatch() && !name.isEmpty()) {
        QJsonObject app = object; app["packageName"] = packageId; app["name"] = name; found->insert(packageId, app);
    }
    for (auto it = object.begin(); it != object.end(); ++it) collectFdroidApps(it.value(), found);
}
static QJsonArray searchFdroid(const QString &query, QString *message) {
    QUrl url("https://search.f-droid.org/api/search_apps"); QUrlQuery q; q.addQueryItem("q", query); url.setQuery(q);
    int status = 0; const QByteArray bytes = httpGet(url, &status);
    if (status != 200) { *message = "F-Droid search returned HTTP " + QString::number(status) + "."; return {}; }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (document.isNull()) { *message = "F-Droid search returned an unknown response format."; return {}; }
    QMap<QString, QJsonObject> found; collectFdroidApps(document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object()), &found);
    QJsonArray results; for (const QJsonObject &item : found) results.append(item);
    if (results.isEmpty()) *message = "No apps matched your search.";
    return results;
}
static QJsonArray searchApkMirror(const QString &query, QString *message) {
    QUrl url("https://www.apkmirror.com/");
    QUrlQuery params;
    params.addQueryItem("post_type", "app_release");
    params.addQueryItem("searchtype", "apk");
    params.addQueryItem("s", query.trimmed());
    url.setQuery(params);
    int status = 0;
    const QByteArray html = httpGet(url, &status, true);
    if (status != 200 || html.isEmpty()) {
        *message = "APKMirror returned HTTP " + QString::number(status) + ".";
        return {};
    }
    QJsonArray results;
    QSet<QString> seen;
    const QRegularExpression titleAnchor(
        "<h5\\b[^>]*appRowTitle[^>]*>[\\s\\S]*?<a\\b[^>]*href=[\\\"']([^\\\"']+)[\\\"'][^>]*>([\\s\\S]*?)</a>[\\s\\S]*?</h5>",
        QRegularExpression::CaseInsensitiveOption);
    auto matches = titleAnchor.globalMatch(QString::fromUtf8(html));
    while (matches.hasNext()) {
        const auto match = matches.next();
        QString href = match.captured(1);
        const QString title = QTextDocumentFragment::fromHtml(match.captured(2)).toPlainText().simplified();
        if (href.startsWith("/")) href = "https://www.apkmirror.com" + href;
        const QUrl resultUrl(href);
        if (resultUrl.host() != "www.apkmirror.com" || !resultUrl.path().startsWith("/apk/") || title.isEmpty() || seen.contains(href)) continue;
        seen.insert(href);
        results.append(QJsonObject{{"name", title}, {"url", href}});
    }
    if (results.isEmpty()) *message = "No APKMirror results found. Try an app name or package name.";
    return results;
}
static bool resolveApkSource(const QJsonObject &o, ApkSourceResult *result) {
    const QString source = o.value("source").toString("local");
    if (source == "fdroid") {
        if (!latestFdroidVersion(o.value("packageId").toString(), &result->version, &result->versionCode, &result->message)) return false;
        const QString previousLatestCode = o.value("fdroidLatestCode").toString(o.value("versionCode").toString());
        if (result->versionCode == previousLatestCode) { result->message = "The app is already up to date (" + result->version + ")."; return false; }
        ApkSourceResult current;
        if (!getFdroidApk(o.value("packageId").toString(), &current)) { result->message = current.message; return false; }
        result->version = current.version; result->versionCode = current.versionCode; result->latestCode = current.latestCode;
        result->bytes = current.bytes; result->iconBytes = current.iconBytes; return true;
    }
    if (source != "github") { result->message = "This app does not have an automatic update source."; return false; }
    int status = 0;
    const QJsonObject release = latestGithubRelease(o.value("github").toString(), &status);
    if (status != 200 || release.isEmpty()) { result->message = "GitHub returned HTTP " + QString::number(status) + "."; return false; }
    const QJsonObject asset = bestGithubAsset(release, &result->message);
    if (asset.isEmpty()) return false;
    const QString releaseVersion = release.value("tag_name").toString();
    if (!releaseVersion.isEmpty() && releaseVersion == o.value("version").toString()) { result->message = "The app is already up to date (" + releaseVersion + ")."; return false; }
    const QString expected = asset.value("digest").toString().remove("sha256:");
    if (!downloadApk(QUrl(asset.value("browser_download_url").toString()), expected, result)) return false;
    result->version = releaseVersion; return true;
}
static bool stageAndActivate(QJsonObject &o, const QByteArray &bytes, QString *message) {
    if (requestCanceled && requestCanceled()) { *message = "Interrupted before activating the APK."; return false; }
    const QString dir = appDir(o); QDir().mkpath(dir);
    QFile staged(dir + "/app.apk.new");
    if (!staged.open(QIODevice::WriteOnly) || staged.write(bytes) != bytes.size()) { *message = "Could not write the APK file."; return false; }
    staged.close();
    const QString oldPath = apkPath(o) + ".old";
    QFile::remove(oldPath);
    const bool hadOld = QFile::exists(apkPath(o));
    if (hadOld && !QFile::rename(apkPath(o), oldPath)) { *message = "Could not prepare the existing APK for replacement."; return false; }
    if (!QFile::rename(dir + "/app.apk.new", apkPath(o))) {
        if (hadOld) QFile::rename(oldPath, apkPath(o));
        *message = "Could not activate the new APK."; return false;
    }
    QFile::remove(oldPath);
    extractAppIcon(o);
    return true;
}
static bool updateOne(QJsonObject &o, QString *message) {
    ApkSourceResult result;
    if (!resolveApkSource(o, &result)) { *message = result.message; return false; }
    if (!stageAndActivate(o, result.bytes, message)) return false;
    saveAppIcon(o, result.iconBytes);
    if (!result.version.isEmpty()) o["version"] = result.version;
    if (!result.versionCode.isEmpty()) o["versionCode"] = result.versionCode;
    if (!result.latestCode.isEmpty()) o["fdroidLatestCode"] = result.latestCode;
    o["updatedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    *message = "Updated to " + o.value("version").toString(); return true;
}
static bool runUserSystemctl(const QStringList &arguments, QString *error) {
    QStringList command{"--user"}; command += arguments;
    QProcess process;
    process.start("systemctl", command);
    if (!process.waitForStarted(5000)) { if (error) *error = "Could not start systemctl --user."; return false; }
    if (!process.waitForFinished(15000)) {
        process.kill(); process.waitForFinished(3000);
        if (error) *error = "systemctl --user timed out.";
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QString detail = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (detail.isEmpty()) detail = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        if (error) *error = detail.isEmpty() ? "systemctl --user exited with code " + QString::number(process.exitCode()) + "." : detail;
        return false;
    }
    return true;
}
static bool writeTimer(const QJsonObject &o, bool enabled, QString *why = nullptr) {
    const QString id = o.value("id").toString();
    const QString unitDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.config/systemd/user";
    const QString base = "atl-shelf-" + id;
    if (!enabled) {
        if (!QFile::exists(unitDir + "/" + base + ".timer") && !QFile::exists(unitDir + "/" + base + ".service")) return true;
        if (!runUserSystemctl({"disable", "--now", base + ".timer"}, why) || !runUserSystemctl({"stop", base + ".service"}, why)) return false;
        for (const auto &suffix : {".timer", ".service"}) {
            const QString path = unitDir + "/" + base + suffix;
            if (QFile::exists(path) && !QFile::remove(path)) { if (why) *why = "Could not remove " + path; return false; }
        }
        return runUserSystemctl({"daemon-reload"}, why);
    }
    QDir().mkpath(unitDir);
    QFile service(unitDir + "/" + base + ".service");
    if (!service.open(QIODevice::WriteOnly | QIODevice::Text)) { if (why) *why = "Could not create the systemd user service."; return false; }
    const QString exe = QCoreApplication::applicationFilePath();
    service.write(("[Unit]\nDescription=Update " + o.value("name").toString() + " via ATL Shelf\n\n[Service]\nType=oneshot\nExecStart=\"" + exe + "\" --update " + id + "\n").toUtf8());
    QFile timer(unitDir + "/" + base + ".timer");
    if (!timer.open(QIODevice::WriteOnly | QIODevice::Text)) { if (why) *why = "Could not create the systemd timer."; return false; }
    timer.write(("[Unit]\nDescription=Daily update check for " + o.value("name").toString() + "\n\n[Timer]\nOnCalendar=daily\nPersistent=true\nRandomizedDelaySec=1h\n\n[Install]\nWantedBy=timers.target\n").toUtf8());
    service.close(); timer.close(); // Flush units before systemd reads them.
    QString error;
    if (runUserSystemctl({"daemon-reload"}, &error) && runUserSystemctl({"enable", "--now", base + ".timer"}, &error)) return true;
    // A desktop session can briefly race user-manager startup; reload and retry once.
    QString retryError;
    if (runUserSystemctl({"daemon-reload"}, &retryError) && runUserSystemctl({"enable", "--now", base + ".timer"}, &retryError)) return true;
    if (!retryError.isEmpty()) error = retryError;
    if (why) *why = "Could not enable the daily systemd user timer: " + error;
    return false;
}

struct LaunchPlan {
    QString program, workingDirectory;
    QStringList arguments;
    QProcessEnvironment environment;
    QJsonObject overrides;
};
static bool prepareLaunch(const QJsonObject &o, LaunchPlan *plan, QString *error) {
        const QJsonObject settings = readSettings();
        const QString launcher = settings.value("launcher").toString(settings.value("atl").toString());
        if (settings.value("atl").toString().isEmpty() || !QFileInfo::exists(settings.value("atl").toString()) || !QFileInfo(launcher).isExecutable()) { *error = "Set up Android support in Shelf before launching apps."; return false; }
        plan->program = launcher; QStringList args{apkPath(o)};
        if (!o.value("activity").toString().isEmpty()) args << "-l" << o.value("activity").toString();
        // GTK window arguments are logical coordinates; Android renders at output density.
        const auto display = currentDisplay();
        const QSize windowSize = o.value("fitScreen").toBool(true) ? display.logical : QSize(qRound(appWindowSize(o).width() / display.scale), qRound(appWindowSize(o).height() / display.scale));
        args << "-w" << QString::number(windowSize.width()) << "-h" << QString::number(windowSize.height()); plan->arguments = args;
        auto env = QProcessEnvironment::systemEnvironment();
        const QJsonObject launchEnv = o.value("launchEnv").toObject();
        QJsonObject mergedEnv = settings.value("runtimeEnv").toObject();
        for (auto it = launchEnv.begin(); it != launchEnv.end(); ++it) mergedEnv[it.key()] = it.value();
        for (auto it = mergedEnv.begin(); it != mergedEnv.end(); ++it)
            if (QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(it.key()).hasMatch() && it.value().isString()) env.insert(it.key(), it.value().toString());
        if (!mergedEnv.contains("ANDROID_APP_DATA_DIR")) env.insert("ANDROID_APP_DATA_DIR", appDir(o));
        env.insert("ATL_RENDER_SCALE", QString::number(display.scale)); plan->environment = env;
        const QString workdir = o.value("workingDirectory").toString(settings.value("workingDirectory").toString());
        plan->workingDirectory = workdir;
        plan->overrides = mergedEnv;
        plan->overrides["ATL_RENDER_SCALE"] = QString::number(display.scale);
        plan->overrides["ANDROID_APP_DATA_DIR"] = env.value("ANDROID_APP_DATA_DIR");
        return true;
}
static void applyLaunch(QProcess &process, const LaunchPlan &plan) {
    process.setProgram(plan.program); process.setArguments(plan.arguments);
    process.setProcessEnvironment(plan.environment);
    if (!plan.workingDirectory.isEmpty()) process.setWorkingDirectory(plan.workingDirectory);
    process.setProcessChannelMode(QProcess::MergedChannels);
}
static bool launchAndroidApp(const QJsonObject &o, QString *error) {
    LaunchPlan plan; if (!prepareLaunch(o, &plan, error)) return false;
    QProcess process; applyLaunch(process, plan);
    process.setStandardOutputFile(appDir(o) + "/launch.log", QIODevice::Truncate);
    if (!process.startDetached()) { *error = process.errorString(); return false; }
    return true;
}
#include "cli.h"
#include <QFileSystemWatcher>

class Shelf : public QWidget {
    Q_OBJECT
public:
    Shelf() {
        setWindowTitle("ATL Shelf"); fitDialog(*this, 520, 780); setMinimumSize(320, 400);
        setStyleSheet("QPushButton { padding: 10px 14px; min-height: 24px; border: 1px solid palette(midlight); background: palette(button); border-radius: 8px; } QPushButton[primary=\"true\"] { background: #3859cf; color: white; border: none; font-weight: 600; } QLineEdit, QComboBox { padding: 7px; } QListWidget::item { padding: 11px; } QGroupBox { margin-top: 12px; font-weight: 600; } QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }");
        auto *rootLayout = new QVBoxLayout(this); rootLayout->setContentsMargins(0, 0, 0, 0);
        shell = new QStackedWidget(this); shell->setObjectName("mobileHost"); mobileHost = shell; rootLayout->addWidget(shell);
        mainPage = new QWidget; shell->addWidget(mainPage);
        auto *outer = new QVBoxLayout(mainPage); outer->setContentsMargins(18, 16, 18, 16); outer->setSpacing(12);
        auto *heading = new QLabel("ATL Shelf"); heading->setStyleSheet("font-size: 20px; font-weight: 700;"); auto *top = new QHBoxLayout(); navBack = new QPushButton("‹ Back"); navBack->setObjectName("mainBack"); navBack->setFixedWidth(100); top->addWidget(navBack); top->addWidget(heading); top->addStretch();
        auto *settingsButton = new QPushButton("Settings"); top->addWidget(settingsButton); outer->addLayout(top);
        auto *subtitle = new QLabel("Android apps on your Linux phone."); subtitle->setStyleSheet("color: palette(text); margin-top: -8px;"); outer->addWidget(subtitle);

        auto *settings = new QGroupBox("Android runtime"); auto *settingsLayout = new QVBoxLayout(settings);
        auto *runtimeRow = new QHBoxLayout(); atlPath = new QLineEdit(); atlPath->setReadOnly(true); atlPath->setPlaceholderText("Runtime not set up yet");
        runtimeStatus = new QLabel("Choose how ATL should be installed."); runtimeStatus->setWordWrap(true);
        auto *setupRuntimeButton = new QPushButton("Set up runtime…"); atlPath->hide(); runtimeRow->addWidget(setupRuntimeButton); settingsLayout->addLayout(runtimeRow); settingsLayout->addWidget(runtimeStatus);
        auto *updateRuntimeButton = new QPushButton("Look for ATL updates");
        settingsLayout->addWidget(updateRuntimeButton);
        connect(updateRuntimeButton, &QPushButton::clicked, this, [this]{ updateRuntime(); });
        connect(setupRuntimeButton, &QPushButton::clicked, this, [this]{ configureRuntime(); });

        pages = new QStackedWidget(); pages->setObjectName("pages"); outer->addWidget(pages, 1);
        auto *libraryPage = new QWidget(); auto *libraryLayout = new QVBoxLayout(libraryPage); libraryLayout->setContentsMargins(0, 4, 0, 0);
        auto *libraryHeader = new QHBoxLayout(); auto *libraryTitle = new QLabel("App library"); libraryTitle->setStyleSheet("font-size: 20px; font-weight: 650;");
        auto *add = new QPushButton("＋  Add app"); libraryHeader->addWidget(libraryTitle); libraryHeader->addStretch(); libraryHeader->addWidget(add); libraryLayout->addLayout(libraryHeader);
        welcome = new QLabel(); welcome->setWordWrap(true); libraryLayout->addWidget(welcome);
        prepareButton = new QPushButton("Set up Android support"); auto *prepare = prepareButton; libraryLayout->addWidget(prepare);
        prepare->setVisible(!QFileInfo::exists(readSettings().value("atl").toString()));
        connect(prepare, &QPushButton::clicked, this, [this, prepare]{ configureRuntime(); prepare->setVisible(!QFileInfo::exists(atlPath->text())); refreshWelcome(); });
        librarySearch = new QLineEdit(); librarySearch->setPlaceholderText("Search your apps"); librarySearch->setClearButtonEnabled(true); libraryLayout->addWidget(librarySearch);
        auto *updateAll = new QPushButton("Update apps"); libraryLayout->addWidget(updateAll);
        connect(updateAll, &QPushButton::clicked, this, [this]{ updateApps(); });
        connect(librarySearch, &QLineEdit::textChanged, this, [this](const QString &query){
            for (int i = 0; i < appsList->count(); ++i) appsList->item(i)->setHidden(!appsList->item(i)->text().contains(query, Qt::CaseInsensitive));
        });
        add->setProperty("primary", true);
        appsList = new QListWidget(); appsList->setFrameShape(QFrame::NoFrame); appsList->setSpacing(4); appsList->setObjectName("library"); appsList->setIconSize(QSize(48, 48)); appsList->setWordWrap(true); appsList->setSelectionMode(QAbstractItemView::SingleSelection); libraryLayout->addWidget(appsList, 1); pages->addWidget(libraryPage);
        connect(add, &QPushButton::clicked, this, [this]{ addApp(); });
        connect(appsList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item){ if (allowListActivation(appsList)) openDetails(appsList->row(item)); });
        connect(appsList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item){ if (allowListActivation(appsList)) openDetails(appsList->row(item)); });

        auto *detailScroll = new QScrollArea(); detailScroll->setWidgetResizable(true); detailScroll->setFrameShape(QFrame::NoFrame);
        auto *detailPage = new QWidget(); auto *detailLayout = new QVBoxLayout(detailPage); detailLayout->setContentsMargins(0, 4, 2, 4); detailLayout->setSpacing(12);
        auto *back = new QPushButton("‹  Back to apps"); back->setFlat(true); back->setStyleSheet("text-align: left; padding-left: 0;"); back->hide();
        selectedTitle = new QLabel("App"); selectedTitle->setStyleSheet("font-size: 21px; font-weight: 650;"); detailLayout->addWidget(selectedTitle);
        sourceLabel = new QLabel(); sourceLabel->setWordWrap(true); sourceLabel->setStyleSheet("color: palette(text);"); detailLayout->addWidget(sourceLabel);
        auto *open = new QPushButton("Open app"); open->setProperty("primary", true); detailLayout->addWidget(open);
        connect(open, &QPushButton::clicked, this, [this]{ launchSelected(); });
        auto *advancedToggle = new QPushButton("App settings ▾"); advancedToggle->setCheckable(true);
        auto *advanced = new QWidget(); auto *advancedLayout = new QVBoxLayout(advanced); advancedLayout->setContentsMargins(0, 0, 0, 0); advanced->hide();
        connect(advancedToggle, &QPushButton::toggled, advanced, &QWidget::setVisible);
        auto *runGroup = new QGroupBox("App settings"); auto *runForm = new QFormLayout(runGroup);
        name = new QLineEdit(); activity = new QLineEdit(); activity->setPlaceholderText("Optional, e.g. com/example/MainActivity");
        width = new QLineEdit("720"); height = new QLineEdit("1280"); width->setValidator(new QIntValidator(200, 4096, width)); height->setValidator(new QIntValidator(200, 4096, height));
        auto *resolution = new QHBoxLayout(); resolution->addWidget(width); resolution->addWidget(new QLabel("×")); resolution->addWidget(height); resolution->addStretch();
        auto *resolutionWidget = new QWidget(); resolutionWidget->setLayout(resolution);
        fitScreen = new QCheckBox("Use native screen resolution"); fitScreen->setChecked(true); runForm->addRow(fitScreen);
        connect(fitScreen, &QCheckBox::toggled, this, [this](bool automatic){ width->setEnabled(!automatic); height->setEnabled(!automatic); });
        runForm->addRow("Name", name); runForm->addRow("Launch activity", activity); runForm->addRow("Resolution", resolutionWidget); advancedLayout->addWidget(runGroup);
        auto *updateGroup = new QGroupBox("Updates"); auto *updateLayout = new QVBoxLayout(updateGroup);
        dailyCheck = new QCheckBox("Daily automatic updates"); updateLayout->addWidget(dailyCheck);
        status = new QLabel("Ready"); status->setWordWrap(true); updateLayout->addWidget(status); detailLayout->addWidget(updateGroup);
        auto *launchGroup = new QGroupBox("Advanced launcher settings"); auto *launchForm = new QFormLayout(launchGroup);
        workingDirectory = new QLineEdit(); workingDirectory->setPlaceholderText("Optional working directory");
        launchEnvironment = new QPlainTextEdit(); launchEnvironment->setPlaceholderText("KEY=value (one per line)"); launchEnvironment->setMaximumHeight(112);
        launchForm->addRow("Working directory", workingDirectory); launchForm->addRow("Environment", launchEnvironment); advancedLayout->addWidget(launchGroup);
        auto *buttons = new QHBoxLayout(); auto *launch = new QPushButton("Launch app"); checkButton = new QPushButton("Update app"); auto *save = new QPushButton("Save settings");
        launch->hide(); buttons->addWidget(checkButton); detailLayout->addLayout(buttons);
        advancedLayout->addWidget(save); detailLayout->addWidget(advancedToggle); detailLayout->addWidget(advanced);
        auto *diagnostics = new QPushButton("View launch log"); detailLayout->addWidget(diagnostics);
        connect(diagnostics, &QPushButton::clicked, this, [this]{ showLaunchLog(); });
        detailLayout->addStretch();
        auto *remove = new QPushButton("Remove app…"); remove->setStyleSheet("color: #b3261e;"); detailLayout->addWidget(remove);
        detailScroll->setWidget(detailPage); pages->addWidget(detailScroll);
        connect(back, &QPushButton::clicked, this, [this]{ pages->setCurrentIndex(0); });
        connect(launch, &QPushButton::clicked, this, [this]{ launchSelected(); });
        connect(checkButton, &QPushButton::clicked, this, [this]{ checkSelected(); });
        connect(save, &QPushButton::clicked, this, [this]{ saveDetails(); });
        connect(remove, &QPushButton::clicked, this, [this]{ removeSelected(); });
        connect(dailyCheck, &QCheckBox::clicked, this, [this]{ saveDetails(); });
        auto *runtimePage = new QWidget(); auto *runtimeLayout = new QVBoxLayout(runtimePage);
        auto *runtimeBack = new QPushButton("‹ Back to apps"); runtimeBack->hide();
        connect(runtimeBack, &QPushButton::clicked, this, [this]{ pages->setCurrentIndex(0); refreshWelcome(); });
        runtimeLayout->addWidget(settings);
        const QSize displaySize = currentDisplay().pixels;
        auto *screenInfo = new QLabel(QString("Native display: %1 × %2 pixels. Rendering density follows your screen automatically.").arg(displaySize.width()).arg(displaySize.height())); screenInfo->setWordWrap(true); runtimeLayout->addWidget(screenInfo);
        auto *explain = new QLabel("Android support is set up once and shared by all your apps. App updates and Android support updates are separate.\n\nApps run through Android Translation Layer. Compatibility varies by app."); explain->setWordWrap(true); runtimeLayout->addWidget(explain); runtimeLayout->addStretch(); pages->addWidget(runtimePage);
        connect(settingsButton, &QPushButton::clicked, this, [this]{ previousMainPage = pages->currentIndex(); pages->setCurrentIndex(2); });
        connect(navBack, &QPushButton::clicked, this, [this]{ navigateBack(); });
        connect(pages, &QStackedWidget::currentChanged, this, [this, settingsButton](int index){ navBack->setEnabled(index != 0); settingsButton->setEnabled(index != 2); }); navBack->setEnabled(false);
        for (auto *form : findChildren<QFormLayout *>()) form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        const QJsonObject settingsObject = readSettings();
        atlPath->setText(settingsObject.value("atl").toString());
        if (!atlPath->text().isEmpty() && QFileInfo::exists(atlPath->text())) runtimeStatus->setText("ATL is ready · " + settingsObject.value("runtimeLabel").toString("Existing runtime"));
        else runtimeStatus->setText("ATL is not configured. Set up a runtime to install and launch apps.");
        migrateDisplayDefaults(); enableTouchScrolling(this);
        reload();
        // Refresh existing menu entries after runtime profile migrations.
        for (const auto &app : apps) updateDesktop(app.toObject(), atlPath->text());
        auto *libraryWatcher = new QFileSystemWatcher(this); libraryWatcher->addPath(rootDir());
        connect(libraryWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
            if (shell->currentWidget() == mainPage && pages->currentIndex() == 0) { atlPath->setText(readSettings().value("atl").toString()); reload(); }
        });
        connect(pages, &QStackedWidget::currentChanged, this, [this](int index) { if (index == 0) reload(); });
#ifndef ATL_SHELF_TESTING
        bool missingIcons = false;
        for (const auto &app : apps) if (app.toObject().value("iconSource").toString() != "apk") missingIcons = true;
        if (missingIcons) QTimer::singleShot(0, this, [this] {
            auto *worker = new QProcess(this);
            connect(worker, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this, [this,worker](int, QProcess::ExitStatus) { reload(); worker->deleteLater(); });
            worker->start(QCoreApplication::applicationFilePath(), {"--refresh-icons"});
        });
#endif
    }
protected:
    void closeEvent(QCloseEvent *event) override {
        if (shell->currentWidget() != mainPage) { if (auto *page = qobject_cast<QDialog *>(shell->currentWidget())) page->reject(); event->ignore(); }
        else QWidget::closeEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override { if (event->key() == Qt::Key_Back || event->key() == Qt::Key_Escape) { navigateBack(); event->accept(); } else QWidget::keyPressEvent(event); }
private:
    QStackedWidget *shell; QWidget *mainPage; QPushButton *navBack; int previousMainPage = 0;
    void navigateBack() { if (shell->currentWidget() != mainPage) { if (auto *page = qobject_cast<QDialog *>(shell->currentWidget())) page->reject(); } else pages->setCurrentIndex(pages->currentIndex() == 2 ? previousMainPage : 0); }
    void migrateDisplayDefaults() {
        auto lock = lockLibrary(); if (!lock) return;
        auto settings = readSettings(); const QSize size = currentDisplay().pixels;
        settings["screenWidth"] = size.width(); settings["screenHeight"] = size.height();
        const bool migrate = settings.value("displayDefaultsVersion").toInt() < 1; settings["displayDefaultsVersion"] = 1;
        QSaveFile file(settingsFilePath()); if (file.open(QIODevice::WriteOnly)) { file.write(QJsonDocument(settings).toJson()); file.commit(); }
        auto installed = readApps();
        for (int i = 0; i < installed.size(); ++i) {
            auto app = installed[i].toObject(); auto env = app.value("launchEnv").toObject(); env.remove("ATL_RENDER_SCALE"); app["launchEnv"] = env;
            if (migrate) app["fitScreen"] = true;
            if (app.value("fitScreen").toBool(true)) { app["width"] = size.width(); app["height"] = size.height(); }
            installed[i] = app;
        }
        writeApps(installed);
    }
    QLabel *welcome; QPushButton *prepareButton; QLineEdit *librarySearch;
    QLineEdit *atlPath, *name, *activity, *width, *height, *workingDirectory; QPlainTextEdit *launchEnvironment; QListWidget *appsList; QLabel *status, *selectedTitle, *sourceLabel, *runtimeStatus; QCheckBox *dailyCheck, *fitScreen; QPushButton *checkButton; QStackedWidget *pages; QJsonArray apps;
    void loadLatestForAction() {
        const QString id = selectedRow >= 0 && selectedRow < apps.size() ? apps[selectedRow].toObject().value("id").toString() : QString();
        apps = readApps(); selectedRow = -1;
        for (int i = 0; i < apps.size(); ++i) if (apps[i].toObject().value("id").toString() == id) selectedRow = i;
    }
    void refreshWelcome() {
        const bool ready = !atlPath->text().isEmpty() && QFileInfo::exists(atlPath->text());
        prepareButton->setVisible(!ready);
        welcome->setText(!ready ? "Welcome! Set up Android support, then choose your first app." : apps.isEmpty() ? "Your shelf is ready. Add an app from F-Droid, GitHub or an APK file." : QString::number(apps.size()) + (apps.size() == 1 ? " app installed · tap to open or manage" : " apps installed · tap to open or manage"));
    }
    void showLaunchLog() {
        const int row = selected(); if (row < 0 || row >= apps.size()) return;
        MobileDialog dialog(this); dialog.setWindowTitle("Launch details"); dialog.setBackText("‹ Back"); fitDialog(dialog);
        auto *layout = new QVBoxLayout(&dialog); auto *text = new QPlainTextEdit(); text->setReadOnly(true);
        QFile log(appDir(apps[row].toObject()) + "/launch.log");
        if (log.open(QIODevice::ReadOnly)) { if (log.size() > 60000) log.seek(log.size() - 60000); text->setPlainText(QString::fromUtf8(log.readAll())); }
        else text->setPlainText("No launch log yet. Open the app from Shelf first.");
        layout->addWidget(text); auto *close = new QPushButton("Close"); layout->addWidget(close); connect(close, &QPushButton::clicked, &dialog, &QDialog::accept); dialog.exec();
    }
    void updateApps() {
        if (apps.isEmpty()) { addApp(); return; }
        auto lock = lockLibrary(this); if (!lock) return; loadLatestForAction();
        TransferProgress progress("Checking installed apps…", this);
        QStringList results;
        for (int i = 0; i < apps.size() && !progress.wasCanceled(); ++i) {
            QJsonObject app = apps[i].toObject(); const QString source = app.value("source").toString();
            if (source != "github" && source != "fdroid") continue;
            progress.setLabelText("Checking " + app.value("name").toString() + "…");
            QString message; const bool updated = updateOne(app, &message);
            if (updated) { apps[i] = app; if (!writeApps(apps)) message = "Updated APK, but could not save its version."; updateDesktop(app, atlPath->text()); }
            results << app.value("name").toString() + ": " + (progress.wasCanceled() ? "Canceled" : message);
        }
        progress.close(); reload();
        mobileNotice(this, "App updates", results.isEmpty() ? "These apps use APK files. Install a newer APK to update them." : results.join("\n\n"));
    }
    void saveAtl() { QDir().mkpath(rootDir()); QJsonObject settings = readSettings(); settings["atl"] = atlPath->text(); QSaveFile f(settingsFilePath()); if(f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(settings).toJson(QJsonDocument::Indented)); f.commit(); } }
    void updateRuntime() {
        const QString mode = readSettings().value("runtimeType").toString();
        if (mode != "github" && mode != "gitlab") {
            mobileNotice(this, "ATL updates", "Repository updates require a GitHub or GitLab runtime. Use Set up runtime to choose one. Package runtimes are updated by Alpine's package manager.");
            return;
        }
        const QString root = runtimeRoot();
        if (!saveDisplayPatch(root)) { mobileNotice(this, "ATL updates", "Could not save the runtime display patch."); return; }
        QFile resource(":/runtime-setup.sh"); QSaveFile script(root + "/runtime-setup.sh");
        if (!resource.open(QIODevice::ReadOnly) || !script.open(QIODevice::WriteOnly) || script.write(resource.readAll()) < 100 || !script.commit()) {
            mobileNotice(this, "ATL updates", "Could not save the runtime update script."); return;
        }
        struct UpdateDialog : MobileDialog {
            using MobileDialog::MobileDialog;
            bool busy = true; std::function<void()> cancelRequest;
            void reject() override { if (busy) { if (cancelRequest) cancelRequest(); } else MobileDialog::reject(); }
        };
        UpdateDialog dialog(this); dialog.setWindowTitle("ATL updates"); fitDialog(dialog, 650, 450);
        auto *layout = new QVBoxLayout(&dialog);
        auto *label = new QLabel("Checking repositories and rebuilding if needed. Please close Android apps before using the updated runtime."); label->setWordWrap(true); layout->addWidget(label);
        auto *bar = new QProgressBar(); bar->setRange(0, 0); layout->addWidget(bar);
        auto *log = new QPlainTextEdit(); log->setReadOnly(true); layout->addWidget(log);
        auto *stop = new QPushButton("Cancel update"); layout->addWidget(stop);
        auto *close = new QPushButton("Close"); close->setEnabled(false); layout->addWidget(close);
        bool canceled = false;
        // Keep the dialog alive if Escape is pressed while its child process runs.
        QProcess process; process.setProcessChannelMode(QProcess::MergedChannels);
        process.setChildProcessModifier([] { ::setpgid(0, 0); });
        connect(stop, &QPushButton::clicked, &dialog, [&] {
            canceled = true; stop->setEnabled(false); label->setText("Stopping the update…");
            const qint64 pid = process.processId(); if (pid > 0) ::kill(-pid_t(pid), SIGTERM);
            QTimer::singleShot(3000, &process, [&, pid]{ if (pid > 0 && process.state() != QProcess::NotRunning) ::kill(-pid_t(pid), SIGKILL); });
        });
        dialog.cancelRequest = [stop]{ stop->click(); };
        connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
        connect(&process, &QProcess::readyRead, &dialog, [&] {
            log->moveCursor(QTextCursor::End); log->insertPlainText(QString::fromLocal8Bit(process.readAll())); log->ensureCursorVisible();
        });
        auto finish = [&](bool ok) {
            dialog.busy = false; dialog.setBackText("‹ Back"); stop->setEnabled(false); bar->setRange(0, 1); bar->setValue(1); close->setEnabled(true);
            label->setText(canceled ? "Update canceled. Run the update again to finish building Android support." : ok ? "ATL is up to date. See the log below for build details." : "ATL update failed. See the log below; you can retry the update.");
            if (ok) for (const auto &app : apps) updateDesktop(app.toObject(), atlPath->text());
#ifndef ATL_SHELF_TESTING
        bool missingIcons = false;
        for (const auto &app : apps) if (app.toObject().value("iconSource").toString() != "apk") missingIcons = true;
        if (missingIcons) QTimer::singleShot(0, this, [this] {
            auto *worker = new QProcess(this);
            connect(worker, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this, [this,worker](int, QProcess::ExitStatus) { reload(); worker->deleteLater(); });
            worker->start(QCoreApplication::applicationFilePath(), {"--refresh-icons"});
        });
#endif
        };
        connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog, [&](int code, QProcess::ExitStatus state){ finish(code == 0 && state == QProcess::NormalExit); });
        connect(&process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error){ if (error == QProcess::FailedToStart) { log->appendPlainText(process.errorString()); finish(false); } });
        process.start("/bin/sh", {root + "/runtime-setup.sh", mode, root + "/" + mode, "2", "update"});
        dialog.exec();
    }
    void configureRuntime() {
        MobileDialog choose(this); choose.setWindowTitle("Set up Android runtime"); fitDialog(choose, 560, 650);
        auto *layout = new QVBoxLayout(&choose);
        auto *intro = new QLabel("Set up Android support once. Shelf will reuse it for every app. The first setup may take several minutes."); intro->setWordWrap(true); layout->addWidget(intro);
        auto *github = new QRadioButton("Mobile build · mortelil on GitHub");
        auto *gitlab = new QRadioButton("Upstream build · GitLab");
        auto *packages = new QRadioButton("System packages · Alpine APK");
        auto *existing = new QRadioButton("Use an existing installation");
        auto *group = new QButtonGroup(&choose); for (auto *radio : {github, gitlab, packages, existing}) { group->addButton(radio); layout->addWidget(radio); }
        github->setChecked(true);
        auto *notes = new QLabel("The GitHub mobile build is recommended for this phone. Shelf downloads and builds it for you. If system packages are needed, a password prompt will appear."); notes->setWordWrap(true); notes->setStyleSheet("color: palette(text);"); layout->addWidget(notes);
        auto *setupAdvanced = new QPushButton("Advanced setup options ▾"); setupAdvanced->setCheckable(true); layout->addWidget(setupAdvanced);
        auto *setupOptions = new QWidget(); auto *options = new QFormLayout(setupOptions); options->setRowWrapPolicy(QFormLayout::WrapLongRows); setupOptions->hide(); layout->addWidget(setupOptions);
        connect(setupAdvanced, &QPushButton::toggled, setupOptions, &QWidget::setVisible);
        auto *scale = new QDoubleSpinBox(); scale->setRange(0.5, 5.0); scale->setSingleStep(0.05); scale->setDecimals(2); scale->setValue(currentDisplay().scale); scale->setEnabled(false);
        auto *noFullscreen = new QCheckBox("Keep apps in a window"); noFullscreen->setChecked(true);
        auto *sharePictures = new QCheckBox("Let Android apps read files in Pictures");
        auto *applyRuntime = new QCheckBox("Use these runtime defaults for all installed apps"); applyRuntime->setChecked(true);
        auto *jobs = new QSpinBox(); jobs->setRange(1, qMax(1, QThread::idealThreadCount())); jobs->setValue(qMin(2, qMax(1, QThread::idealThreadCount())));
        options->addRow("Display scale", scale); options->addRow("Window mode", noFullscreen); options->addRow("Media access", sharePictures); options->addRow("Build jobs", jobs); options->addRow(applyRuntime);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok); buttons->button(QDialogButtonBox::Ok)->setText("Continue"); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &choose, &QDialog::reject); connect(buttons, &QDialogButtonBox::accepted, &choose, &QDialog::accept);
        if (choose.exec() != QDialog::Accepted) return;

        QString mode = github->isChecked() ? "github" : gitlab->isChecked() ? "gitlab" : packages->isChecked() ? "apk" : "existing";
        QString existingBinary;
        if (mode == "existing") {
            existingBinary = chooseMobileFile(this, "Choose ATL executable", atlPath->text(), "Android Translation Layer (android-translation-layer);;All files (*)");
            if (existingBinary.isEmpty()) return;
            if (!QFileInfo(existingBinary).isExecutable()) { mobileNotice(this, "Not an executable", "Choose an executable Android Translation Layer binary."); return; }
        }

        struct SetupDialog : MobileDialog {
            using MobileDialog::MobileDialog;
            std::function<void()> cancelRequest;
            void reject() override { if (cancelRequest) cancelRequest(); else QDialog::reject(); }
        };
        SetupDialog progress(this); progress.setWindowTitle("Setting up Android runtime"); fitDialog(progress, 650, 430);
        auto *progressLayout = new QVBoxLayout(&progress); auto *step = new QLabel("Preparing…"); progressLayout->addWidget(step);
        auto *bar = new QProgressBar(); bar->setRange(0, 0); progressLayout->addWidget(bar);
        auto *log = new QPlainTextEdit(); log->setReadOnly(true); log->setMaximumBlockCount(6000); progressLayout->addWidget(log, 1);
        auto *cancel = new QPushButton("Cancel setup"); progressLayout->addWidget(cancel);
        bool canceled = false; bool activeCanStop = true; QProcess *active = nullptr;
        auto requestCancel = [&] {
            canceled = true; cancel->setEnabled(false);
            step->setText(activeCanStop ? "Stopping the current step…" : "Finishing the system package step safely. Setup will stop afterward…");
            if (activeCanStop && active && active->processId() > 0) ::kill(-pid_t(active->processId()), SIGTERM);
        };
        connect(cancel, &QPushButton::clicked, &progress, requestCancel);
        progress.cancelRequest = requestCancel;
        progress.show(); QApplication::processEvents();
        auto runStep = [&](const QString &title, const QString &program, const QStringList &arguments) {
            if (canceled) return false;
            step->setText(title); log->appendPlainText("\n$ " + program + " " + arguments.join(' '));
            QProcess process; active = &process; activeCanStop = QFileInfo(program).fileName() != "pkexec" && QFileInfo(program).fileName() != "apk"; process.setChildProcessModifier([] { ::setpgid(0, 0); }); process.setProcessChannelMode(QProcess::MergedChannels); process.setWorkingDirectory(QDir::homePath());
            process.start(program, arguments);
            if (!process.waitForStarted(10000)) { log->appendPlainText("Could not start the setup command: " + process.errorString()); active = nullptr; return false; }
            while (process.state() != QProcess::NotRunning) {
                process.waitForReadyRead(100);
                const QByteArray output = process.readAll();
                if (!output.isEmpty()) { log->moveCursor(QTextCursor::End); log->insertPlainText(QString::fromLocal8Bit(output)); log->ensureCursorVisible(); }
                QApplication::processEvents(QEventLoop::AllEvents, 50);
                if (canceled && activeCanStop && process.state() != QProcess::NotRunning) { const qint64 pid = process.processId(); if (pid > 0) ::kill(-pid_t(pid), SIGTERM); if (!process.waitForFinished(2500) && pid > 0) ::kill(-pid_t(pid), SIGKILL); }
            }
            const QByteArray rest = process.readAll(); if (!rest.isEmpty()) { log->moveCursor(QTextCursor::End); log->insertPlainText(QString::fromLocal8Bit(rest)); log->ensureCursorVisible(); }
            active = nullptr;
            return !canceled && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        };

        bool success = false; QString atl, launcher, workdir; const QString root = runtimeRoot(); const QString setupRoot = root + "/" + mode; QDir().mkpath(root);
        if (mode == "existing") {
            atl = launcher = existingBinary;
            success = true;
        } else {
            QStringList apkPackages;
            if (mode == "apk") apkPackages = {"android-translation-layer", "bionic_translation", "art_standalone"};
            else {
                apkPackages = {"git", "build-base", "meson", "python3", "pkgconf", "java-common", "openjdk8-jdk", "android-build-tools", "elfutils-dev", "libunwind-dev", "libbsd-dev", "libcap-dev", "pc:alsa", "pc:glib-2.0", "pc:gtk4", "pc:gudev-1.0", "pc:libportal", "pc:openxr", "pc:vulkan", "pc:webkitgtk-6.0", "pc:libsecret-1", "ffmpeg-dev", "bionic_translation-dev", "art_standalone-dev", "libandroidfw-dev"};
                if (mode == "gitlab") apkPackages << "wolfssl-dev";
            }
            apkPackages << "android-build-tools" << "unzip" << "qt6-qtsvg";
            apkPackages.removeDuplicates();
            const QString apk = QStandardPaths::findExecutable("apk");
            const QString pkexec = QStandardPaths::findExecutable("pkexec");
            QString program = apk; QStringList args{"add", "--no-progress"}; args += apkPackages;
            if (apk.isEmpty()) { log->appendPlainText("Alpine's apk command is not available on this device."); }
            else if (geteuid() == 0) {
                success = runStep("Installing runtime packages", program, args);
            } else if (!pkexec.isEmpty()) {
                program = pkexec; args = {apk, "add", "--no-progress"}; args += apkPackages;
                success = runStep("Installing dependencies — authenticate in the system dialog", program, args);
            } else {
                log->appendPlainText("pkexec is missing. Install a Polkit authentication agent to use the graphical administrator prompt.");
            }
            if (success && mode == "apk") {
                atl = QStandardPaths::findExecutable("android-translation-layer");
                if (atl.isEmpty()) { log->appendPlainText("The packages were installed, but the ATL executable was not found in PATH."); success = false; }
                else launcher = atl;
            } else if (success) {
                const bool patchSaved = saveDisplayPatch(root);
                QFile resource(":/runtime-setup.sh");
                const QString scriptPath = root + "/runtime-setup.sh";
                if (!patchSaved || !resource.open(QIODevice::ReadOnly)) { log->appendPlainText("The runtime build script is missing from this ATL Shelf package."); success = false; }
                else {
                    QSaveFile script(scriptPath);
                    if (!script.open(QIODevice::WriteOnly) || script.write(resource.readAll()) < 100 || !script.commit()) { log->appendPlainText("Could not save the runtime build script."); success = false; }
                    else {
                        QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
                        success = runStep(mode == "github" ? "Downloading and building the mobile ATL runtime" : "Downloading and building the upstream ATL runtime", "/bin/sh", {scriptPath, mode, setupRoot, QString::number(jobs->value())});
                    }
                }
                const QString workspace = setupRoot + "/workspace";
                if (mode == "github") {
                    atl = workspace + "/android_translation_layer/build-mobile/android-translation-layer";
                    launcher = workspace + "/android_translation_layer/scripts/mobile/run.sh";
                    workdir = workspace + "/android_translation_layer/build-mobile";
                } else {
                    atl = workspace + "/android_translation_layer/build-mobile/android-translation-layer";
                    launcher = setupRoot + "/atl-launcher.sh";
                    workdir = workspace + "/android_translation_layer/build-mobile";
                }
            }
        }
        progress.cancelRequest = {}; progress.close();
        if (!success) {
            if (canceled) mobileNotice(this, "Setup canceled", "The current setup step was stopped. You can run setup again; completed downloads and packages will be reused.");
            else mobileNotice(this, "Runtime setup failed", log->toPlainText().right(5000));
            return;
        }
        auto libraryLock = lockLibrary(this); if (!libraryLock) return;
        QJsonObject settings = readSettings(); settings["atl"] = atl; settings["launcher"] = launcher; settings["workingDirectory"] = workdir; settings["runtimeType"] = mode;
        settings["runtimeLabel"] = mode == "github" ? "Built from mortelil GitHub" : mode == "gitlab" ? "Built from ATL GitLab" : mode == "apk" ? "Alpine APK packages" : "Existing binary";
        QJsonObject runtimeEnv; runtimeEnv["ATL_RENDER_SCALE"] = QString::number(currentDisplay().scale);
        if (noFullscreen->isChecked()) runtimeEnv["ATL_DISABLE_FULLSCREEN"] = "1";
        if (sharePictures->isChecked()) runtimeEnv["ATL_MEDIA_ROOT"] = QDir::homePath() + "/Pictures";
        settings["runtimeEnv"] = runtimeEnv;
        QSaveFile settingsFile(settingsFilePath());
        if (!settingsFile.open(QIODevice::WriteOnly) || settingsFile.write(QJsonDocument(settings).toJson(QJsonDocument::Indented)) < 0 || !settingsFile.commit()) { mobileNotice(this, "Could not save runtime", "ATL was set up, but its settings could not be saved."); return; }
        atlPath->setText(atl); runtimeStatus->setText("ATL is ready · " + settings.value("runtimeLabel").toString());
        apps = readApps();
        if (applyRuntime->isChecked() && mode != "existing") {
            static const QStringList managed = {"XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY", "GDK_BACKEND", "ATL_RENDER_SCALE", "ATL_DISABLE_FULLSCREEN", "RUN_FROM_BUILDDIR", "LD_LIBRARY_PATH", "ATL_MEDIA_ROOT"};
            for (int i = 0; i < apps.size(); ++i) {
                QJsonObject app = apps[i].toObject(); QJsonObject env = app.value("launchEnv").toObject();
                for (const QString &key : managed) env.remove(key);
                if (env.isEmpty()) app.remove("launchEnv"); else app["launchEnv"] = env;
                app.remove("workingDirectory"); apps[i] = app;
            }
            writeApps(apps);
        }
        for (const QJsonValue &value : apps) updateDesktop(value.toObject(), atl);
        QProcess::execute("kbuildsycoca6", {"--noincremental"}); reload();
        mobileNotice(this, "ATL is ready", "The Android runtime is installed and configured. New apps will use it automatically with the selected display settings.");
    }
    void reload() {
        apps = readApps(); appsList->clear(); refreshWelcome();
        for (const QJsonValue &v : apps) {
            const QJsonObject o = v.toObject(); QString text = o.value("name").toString();
            const QString source = o.value("source").toString();
            text += QString("\n") + (source == "github" ? "GitHub" : source == "fdroid" ? "F-Droid" : "APK file");
            const QString ver = o.value("version").toString(); if (!ver.isEmpty()) text += " · " + ver;
            auto *item = new QListWidgetItem(text); item->setData(Qt::UserRole, o.value("id").toString());
            const QString icon = o.value("icon").toString();
            if (!icon.isEmpty() && QFileInfo::exists(icon)) item->setIcon(QIcon(icon)); else item->setIcon(QIcon(":/org.atl-shelf.png"));
            appsList->addItem(item); item->setHidden(!text.contains(librarySearch->text(), Qt::CaseInsensitive));
        }
    }
    int selected() const { return selectedRow; }
    int selectedRow = -1;
    void openDetails(int row) {
        if (row < 0 || row >= apps.size()) return;
        selectedRow = row; appsList->setCurrentRow(row);
        const QJsonObject o = apps[row].toObject();
        selectedTitle->setText(o.value("name").toString()); name->setText(o.value("name").toString());
        const QString source = o.value("source").toString("local");
        sourceLabel->setText(source == "github" ? "GitHub · " + o.value("github").toString() : source == "fdroid" ? "F-Droid · " + o.value("packageId").toString() : source == "apkmirror" ? "APKMirror · manual · " + o.value("sourceUrl").toString() : "Local APK");
        fitScreen->setChecked(o.value("fitScreen").toBool(true)); width->setEnabled(!fitScreen->isChecked()); height->setEnabled(!fitScreen->isChecked());
        activity->setText(o.value("activity").toString()); width->setText(QString::number(appWindowSize(o).width())); height->setText(QString::number(appWindowSize(o).height()));
        QStringList envLines;
        const QJsonObject launchEnv = o.value("launchEnv").toObject();
        for (auto it = launchEnv.begin(); it != launchEnv.end(); ++it) envLines << it.key() + "=" + it.value().toString();
        launchEnvironment->setPlainText(envLines.join('\n'));
        workingDirectory->setText(o.value("workingDirectory").toString());
        const bool canAutoUpdate = source == "github" || source == "fdroid";
        dailyCheck->setChecked(o.value("daily").toBool()); dailyCheck->setEnabled(canAutoUpdate); checkButton->setEnabled(canAutoUpdate);
        QString version = o.value("version").toString(); if (version.isEmpty()) version = "Unknown version";
        status->setText("Installed · " + version + (o.value("updateError").toString().isEmpty() ? QString() : "\nAutomatic updates need attention. Save app settings to retry.")); pages->setCurrentIndex(1);
    }
    void saveDetails() {
        auto lock = lockLibrary(this); if (!lock) return; loadLatestForAction();
        int row = selected(); if (row < 0 || row >= apps.size()) return;
        QJsonObject o = apps[row].toObject(); o["name"] = name->text().trimmed(); o["activity"] = activity->text().trimmed();
        QJsonObject launchEnv;
        for (const QString &line : launchEnvironment->toPlainText().split('\n')) {
            const int equals = line.indexOf('=');
            if (equals > 0) {
                const QString key = line.left(equals).trimmed();
                if (QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(key).hasMatch()) launchEnv[key] = line.mid(equals + 1).trimmed();
            }
        }
        o["launchEnv"] = launchEnv;
        o["workingDirectory"] = workingDirectory->text().trimmed();
        if (!width->hasAcceptableInput() || !height->hasAcceptableInput()) { mobileNotice(this, "Invalid size", "Enter a width and height between 200 and 4096."); return; }
        o["fitScreen"] = fitScreen->isChecked();
        o["width"] = qMax(1, width->text().toInt()); o["height"] = qMax(1, height->text().toInt());
        o["daily"] = dailyCheck->isEnabled() && dailyCheck->isChecked();
        if (o.value("name").toString().isEmpty()) { mobileNotice(this, "Name required", "Enter a name for the app."); return; }
        apps[row] = o;
        if (!writeApps(apps)) { mobileNotice(this, "Save failed", "Could not save the app settings."); return; }
        updateDesktop(o, atlPath->text()); QString why;
        if (!writeTimer(o, o.value("daily").toBool(), &why) && o.value("daily").toBool()) {
            o["daily"] = false; o["updateError"] = why; dailyCheck->setChecked(false); status->setText("Settings saved. Automatic updates could not be enabled.\n" + why);
        } else { o.remove("updateError"); status->setText("Settings saved."); }
        apps[row] = o; writeApps(apps);
        selectedTitle->setText(o.value("name").toString()); reload();
    }
    void launchSelected() {
        int row = selected(); if (row < 0 || row >= apps.size()) return;
        QString error; const auto app = apps[row].toObject();
        if (!launchAndroidApp(app, &error)) mobileNotice(this, "Launch failed", error);
        else status->setText("Opening " + app.value("name").toString() + "… First launch can take a minute.");
    }
    void checkSelected() {
        auto lock = lockLibrary(this); if (!lock) return; loadLatestForAction();
        int row = selected(); if (row < 0 || row >= apps.size()) return; QJsonObject o = apps[row].toObject();
        TransferProgress progress("Checking for an app update…", this); QString message; const bool updated = updateOne(o, &message); progress.hide();
        if (updated) { apps[row] = o; writeApps(apps); updateDesktop(o, atlPath->text()); }
        status->setText(progress.wasCanceled() ? "Update canceled." : message); reload();
    }
    void removeSelected() {
        auto lock = lockLibrary(this); if (!lock) return; loadLatestForAction();
        const int row = selected();
        if (row < 0 || row >= apps.size()) return;
        const QJsonObject o = apps[row].toObject();
        MobileDialog confirm(this); confirm.setWindowTitle("Remove app");
        auto *confirmLayout = new QVBoxLayout(&confirm);
        auto *text = new QLabel("Remove " + o.value("name").toString() + "?\n\nThis deletes its APK, app data, menu entry and update timer."); text->setTextFormat(Qt::PlainText); text->setWordWrap(true); confirmLayout->addWidget(text);
        auto *remove = new QPushButton("Remove app"); confirmLayout->addWidget(remove); connect(remove, &QPushButton::clicked, &confirm, &QDialog::accept);
        if (confirm.exec() != QDialog::Accepted) return;

        QJsonArray remaining = apps;
        remaining.removeAt(row);
        if (!writeApps(remaining)) { mobileNotice(this, "Remove failed", "Could not update the app library. Nothing was removed."); return; }

        QString ignored;
        writeTimer(o, false, &ignored);
        QStringList leftovers;
        const QString desktopFile = desktopPath() + "/atl-shelf-" + o.value("id").toString() + ".desktop";
        if (QFile::exists(desktopFile) && !QFile::remove(desktopFile)) leftovers << "the Plasma menu entry";
        QProcess::execute("kbuildsycoca6", {"--noincremental"});
        QDir appData(appDir(o));
        if (appData.exists() && !appData.removeRecursively()) leftovers << "the app APK/data directory";

        apps = remaining;
        selectedRow = -1;
        reload();
        pages->setCurrentIndex(0);
        if (leftovers.isEmpty()) mobileNotice(this, "App removed", o.value("name").toString() + " was removed from ATL Shelf.");
        else mobileNotice(this, "App removed with leftovers", "The app was removed from the library, but ATL Shelf could not remove " + leftovers.join(" and ") + ". You can delete those manually.");
    }
    void addApp() {
        if (atlPath->text().isEmpty() || !QFileInfo::exists(atlPath->text())) { configureRuntime(); if (!QFileInfo::exists(atlPath->text())) return; refreshWelcome(); }
        MobileDialog dialog(this); dialog.setWindowTitle("Add app"); fitDialog(dialog, 540, 720); dialog.setMinimumSize(300, 400);
        auto *layout = new QVBoxLayout(&dialog); auto *sourcePicker = new QComboBox(); sourcePicker->setObjectName("sourcePicker"); sourcePicker->addItems({"GitHub repository", "F-Droid catalogue", "APKMirror · browser download", "APK file on this device"}); layout->addWidget(sourcePicker);
        auto *tabs = new QTabWidget(); tabs->tabBar()->hide(); layout->addWidget(tabs, 1);
        connect(sourcePicker, qOverload<int>(&QComboBox::currentIndexChanged), tabs, &QTabWidget::setCurrentIndex);
        auto *githubTab = new QWidget(); auto *ghLayout = new QVBoxLayout(githubTab); auto *ghForm = new QFormLayout();
        auto *ghRepo = new QLineEdit(); ghRepo->setPlaceholderText("Paste a GitHub link or owner/repository"); auto *fetchReleases = new QPushButton("Find latest release");
        auto *ghRow = new QVBoxLayout(); ghRow->addWidget(ghRepo, 1); ghRow->addWidget(fetchReleases); auto *ghWrap = new QWidget(); ghWrap->setLayout(ghRow); ghForm->addRow(ghWrap); ghLayout->addLayout(ghForm);
        auto *ghInfo = new QLabel("Fetches APKs from the latest GitHub release and suggests the right CPU architecture."); ghInfo->setWordWrap(true); ghLayout->addWidget(ghInfo);
        auto *ghAssets = new QComboBox(); ghAssets->setMinimumHeight(42); ghLayout->addWidget(ghAssets); ghLayout->addStretch(); tabs->addTab(githubTab, "GitHub");
        auto *fdTab = new QWidget(); auto *fdLayout = new QVBoxLayout(fdTab); auto *searchRow = new QHBoxLayout();
        auto *fdQuery = new QLineEdit(); fdQuery->setPlaceholderText("Search by app name or feature"); auto *fdSearchButton = new QPushButton("Search"); searchRow->addWidget(fdQuery, 1); searchRow->addWidget(fdSearchButton); fdLayout->addLayout(searchRow);
        auto *fdResults = new QListWidget(); fdLayout->addWidget(fdResults, 1); auto *fdInfo = new QLabel("Find an app, select it, then tap Install app."); fdInfo->setWordWrap(true); fdLayout->addWidget(fdInfo); tabs->addTab(fdTab, "F-Droid");
        auto *mirrorTab = new QWidget(); auto *mirrorLayout = new QVBoxLayout(mirrorTab); auto *mirrorSearchRow = new QHBoxLayout();
        auto *mirrorQuery = new QLineEdit(); mirrorQuery->setPlaceholderText("Search by app name or package name"); auto *mirrorSearchButton = new QPushButton("Search"); mirrorSearchRow->addWidget(mirrorQuery, 1); mirrorSearchRow->addWidget(mirrorSearchButton); mirrorLayout->addLayout(mirrorSearchRow);
        auto *mirrorResults = new QListWidget(); mirrorLayout->addWidget(mirrorResults, 1);
        auto *mirrorInfo = new QLabel("Search APKMirror, open a result, download an APK variant in your browser, then choose the file here."); mirrorInfo->setWordWrap(true); mirrorLayout->addWidget(mirrorInfo);
        auto *mirrorActions = new QHBoxLayout(); auto *mirrorOpen = new QPushButton("Open selected page"); auto *mirrorChoose = new QPushButton("Choose downloaded APK…"); mirrorActions->addWidget(mirrorOpen); mirrorActions->addWidget(mirrorChoose); mirrorLayout->addLayout(mirrorActions); tabs->addTab(mirrorTab, "APKMirror");
        auto *localTab = new QWidget(); auto *localLayout = new QVBoxLayout(localTab); auto *localInfo = new QLabel("Add an APK file from this device. This app will be updated manually."); localInfo->setWordWrap(true); localLayout->addWidget(localInfo);
        auto *localPath = new QLineEdit(); localPath->setReadOnly(true); auto *localChoose = new QPushButton("Choose APK…"); auto *localRow = new QHBoxLayout(); localRow->addWidget(localPath, 1); localRow->addWidget(localChoose); localLayout->addLayout(localRow); localLayout->addStretch(); tabs->addTab(localTab, "APK file");
        auto *common = new QGroupBox("App settings"); auto *commonForm = new QFormLayout(common); commonForm->setRowWrapPolicy(QFormLayout::WrapAllRows); auto *appName = new QLineEdit(); appName->setObjectName("installName"); auto *appActivity = new QLineEdit(); appActivity->setPlaceholderText("Optional");
        const QSize screenSize = QGuiApplication::primaryScreen()->availableGeometry().size(); auto *appWidth = new QLineEdit(QString::number(qBound(320, screenSize.width(), 4096))); auto *appHeight = new QLineEdit(QString::number(qBound(400, screenSize.height(), 4096))); appWidth->setValidator(new QIntValidator(200, 4096, appWidth)); appHeight->setValidator(new QIntValidator(200, 4096, appHeight)); auto *sizeRow = new QHBoxLayout(); sizeRow->addWidget(appWidth); sizeRow->addWidget(new QLabel("×")); sizeRow->addWidget(appHeight); auto *sizeWrap = new QWidget(); sizeWrap->setLayout(sizeRow);
        auto *autoUpdate = new QCheckBox("Daily automatic updates"); autoUpdate->setChecked(true); commonForm->addRow("App name", appName); commonForm->addRow(autoUpdate); layout->addWidget(common);
        auto *advancedInstall = new QPushButton("Advanced options ▾"); advancedInstall->setCheckable(true); layout->addWidget(advancedInstall);
        auto *installOptions = new QWidget(); auto *installForm = new QFormLayout(installOptions); installForm->setRowWrapPolicy(QFormLayout::WrapLongRows); auto *autoSize = new QCheckBox("Use native screen resolution"); autoSize->setChecked(true); installForm->addRow(autoSize);
        appWidth->setEnabled(false); appHeight->setEnabled(false); connect(autoSize, &QCheckBox::toggled, &dialog, [=](bool automatic){ appWidth->setEnabled(!automatic); appHeight->setEnabled(!automatic); });
        installForm->addRow("Launch activity", appActivity); installForm->addRow("Width × height", sizeWrap); installOptions->hide(); layout->addWidget(installOptions);
        connect(advancedInstall, &QPushButton::toggled, installOptions, &QWidget::setVisible);
        auto *buttons = new QDialogButtonBox(); buttons->addButton("Cancel", QDialogButtonBox::RejectRole); auto *addButton = buttons->addButton("Install app", QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
        addButton->setProperty("primary", true); addButton->setEnabled(false);
        connect(tabs, &QTabWidget::currentChanged, &dialog, [=](int tab) { autoUpdate->setEnabled(tab == 0 || tab == 1); addButton->setText("Install app"); appName->clear(); });

        QJsonObject release; QString localFile; QString fdName, fdPackage, fdVersion; QString fdCode; QJsonObject mirrorChoice;

        connect(fetchReleases, &QPushButton::clicked, &dialog, [&] {
            const QString normalized = normalizeRepo(ghRepo->text()); ghRepo->setText(normalized);
            if (!QRegularExpression("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$").match(normalized).hasMatch()) { ghInfo->setText("Enter the repository as owner/name."); return; }
            TransferProgress progress("Looking for the latest release…", &dialog); ghInfo->setText("Looking for the latest release…"); ghAssets->clear(); QApplication::setOverrideCursor(Qt::WaitCursor); int code = 0; release = latestGithubRelease(normalized, &code); QApplication::restoreOverrideCursor();
            if (code != 200 || release.isEmpty()) { ghInfo->setText("No public release found (HTTP " + QString::number(code) + "). Check the repository or try F-Droid."); return; }
            appName->setText(normalized.section('/', -1));
            const QString recommendedName = bestGithubAsset(release).value("name").toString(); QList<QPair<int, QJsonObject>> choices;
            for (const QJsonValue &v : release.value("assets").toArray()) { const QJsonObject a = v.toObject(); const int score = apkArchitectureScore(a.value("name").toString()); if (score >= 0) { choices.append({score, a}); } }
            if (choices.isEmpty()) { ghInfo->setText("Release ‘" + release.value("tag_name").toString() + "’ has no APK for " + QSysInfo::currentCpuArchitecture() + "."); return; }
            for (const auto &choice : choices) { const QJsonObject a = choice.second; const QString name = a.value("name").toString(); const QString label = name + (name == recommendedName ? "  ·  recommended" : ""); ghAssets->addItem(label, QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact))); }
            for (int i = 0; i < choices.size(); ++i) if (choices[i].second.value("name").toString() == recommendedName) { ghAssets->setCurrentIndex(i); break; }
            ghInfo->setText("Release ‘" + release.value("tag_name").toString() + "’ · " + QString::number(choices.size()) + " APK file(s). Suggested architecture: " + QSysInfo::currentCpuArchitecture() + ".");
        });
        connect(fdSearchButton, &QPushButton::clicked, &dialog, [&] {
            if (fdQuery->text().trimmed().isEmpty()) { fdInfo->setText("Enter a search term first."); return; }
            TransferProgress progress("Searching F-Droid…", &dialog); fdInfo->setText("Searching F-Droid…"); fdResults->clear(); QApplication::setOverrideCursor(Qt::WaitCursor); QString message; const QJsonArray results = searchFdroid(fdQuery->text().trimmed(), &message); QApplication::restoreOverrideCursor();
            for (const QJsonValue &v : results) { const QJsonObject item = v.toObject(); const QString title = item.value("name").toString(); const QString id = item.value("packageName").toString(); auto *result = new QListWidgetItem(title + "\n" + id); result->setData(Qt::UserRole, QJsonDocument(item).toJson(QJsonDocument::Compact)); result->setToolTip(item.value("summary").toString()); fdResults->addItem(result); }
            fdInfo->setText(results.isEmpty() ? message : QString::number(results.size()) + " results · select an app to add.");
        });
        connect(mirrorSearchButton, &QPushButton::clicked, &dialog, [&] {
            if (mirrorQuery->text().trimmed().isEmpty()) { mirrorInfo->setText("Enter a search term first."); return; }
            TransferProgress progress("Searching APKMirror…", &dialog); mirrorInfo->setText("Searching APKMirror…"); mirrorResults->clear(); mirrorChoice = {};
            QApplication::setOverrideCursor(Qt::WaitCursor); QString message; const QJsonArray results = searchApkMirror(mirrorQuery->text(), &message); QApplication::restoreOverrideCursor();
            for (const QJsonValue &value : results) {
                const QJsonObject item = value.toObject();
                auto *result = new QListWidgetItem(item.value("name").toString());
                result->setData(Qt::UserRole, QJsonDocument(item).toJson(QJsonDocument::Compact));
                mirrorResults->addItem(result);
            }
            mirrorInfo->setText(results.isEmpty() ? message : QString::number(results.size()) + " results · select an app, open its APKMirror page, and download an APK file.");
        });
        connect(mirrorResults, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *current, QListWidgetItem *) {
            mirrorChoice = current ? QJsonDocument::fromJson(current->data(Qt::UserRole).toByteArray()).object() : QJsonObject();
            if (!mirrorChoice.isEmpty() && appName->text().isEmpty()) appName->setText(mirrorChoice.value("name").toString().section(QRegularExpression("\\s+\\d+(?:\\.\\d+)*.*$"), 0, 0).trimmed());
        });
        connect(mirrorOpen, &QPushButton::clicked, &dialog, [&] {
            if (mirrorChoice.isEmpty()) { mirrorInfo->setText("Select a search result first."); return; }
            QDesktopServices::openUrl(QUrl(mirrorChoice.value("url").toString()));
            mirrorInfo->setText("Download a .apk file for the variant in your browser, then choose it with the button here.");
        });
        auto chooseApk = [&](QWidget *parent) {
            const QString path = chooseMobileFile(parent, "Choose Android APK", {}, "Android packages (*.apk)");
            if (!path.isEmpty()) { localFile = path; localPath->setText(path); if (appName->text().isEmpty()) appName->setText(QFileInfo(path).completeBaseName()); }
        };
        connect(mirrorChoose, &QPushButton::clicked, &dialog, [&] {
            if (mirrorChoice.isEmpty()) { mirrorInfo->setText("Search for and select an APKMirror listing first."); return; }
            chooseApk(&dialog);
            if (!localFile.isEmpty()) mirrorInfo->setText("Selected APK: " + QFileInfo(localFile).fileName() + " · this source is updated manually.");
        });
        connect(fdResults, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *current, QListWidgetItem *) {
            fdName.clear(); fdPackage.clear(); fdVersion.clear(); fdCode.clear(); if (!current) return;
            const QJsonObject item = QJsonDocument::fromJson(current->data(Qt::UserRole).toByteArray()).object(); fdName = item.value("name").toString(); fdPackage = item.value("packageName").toString();
            appName->setText(fdName);
            fdInfo->setText(fdName + "\nThe download will be checked against F-Droid's signed index when you install.");
        });
        connect(localChoose, &QPushButton::clicked, &dialog, [&] { const QString p = chooseMobileFile(&dialog, "Choose Android APK", {}, "Android packages (*.apk)"); if (!p.isEmpty()) { localFile = p; localPath->setText(p); if (appName->text().isEmpty()) appName->setText(QFileInfo(p).completeBaseName()); } });
        connect(ghRepo, &QLineEdit::textChanged, &dialog, [&]{ release = {}; ghAssets->clear(); });
        connect(ghRepo, &QLineEdit::returnPressed, fetchReleases, &QPushButton::click);
        connect(fdQuery, &QLineEdit::returnPressed, fdSearchButton, &QPushButton::click);
        connect(mirrorQuery, &QLineEdit::returnPressed, mirrorSearchButton, &QPushButton::click);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(addButton, &QPushButton::clicked, &dialog, [&] {
            auto lock = lockLibrary(&dialog); if (!lock) return; loadLatestForAction();
            const int tab = tabs->currentIndex(); const QString display = appName->text().trimmed();
            if (!appWidth->hasAcceptableInput() || !appHeight->hasAcceptableInput()) { mobileNotice(&dialog, "Invalid size", "Enter a width and height between 200 and 4096."); return; }
            if (display.isEmpty()) { mobileNotice(&dialog, "Name required", "Enter a display name for the app."); return; }
            if (tab == 0 && ghAssets->currentIndex() < 0) { mobileNotice(&dialog, "No APK selected", "Find a release and select an APK file first."); return; }
            if (tab == 1 && fdPackage.isEmpty()) { mobileNotice(&dialog, "Select an app", "Search for and select an F-Droid app first."); return; }
            if (tab == 2 && (localFile.isEmpty() || mirrorChoice.isEmpty())) { mobileNotice(&dialog, "APK required", "Choose an APKMirror listing and its downloaded APK file first."); return; }
            if (tab == 3 && localFile.isEmpty()) { mobileNotice(&dialog, "APK required", "Choose an APK file first."); return; }

            for (int i = 0; i < apps.size(); ++i) {
                const auto existing = apps[i].toObject();
                if ((tab == 0 && existing.value("source") == "github" && existing.value("github").toString().compare(normalizeRepo(ghRepo->text()), Qt::CaseInsensitive) == 0) ||
                    (tab == 1 && existing.value("source") == "fdroid" && existing.value("packageId").toString() == fdPackage)) {
                    dialog.accept(); openDetails(i); status->setText("Already installed. Use Update app to get the latest version."); return;
                }
            }
            TransferProgress progress("Preparing installation…", &dialog);
            QJsonObject o; const QString id = safeId(display); QString uniqueId = id.isEmpty() ? "app" : id; int suffix = 2;
            auto used = [&] { for (const QJsonValue &v : apps) if (v.toObject().value("id").toString() == uniqueId) return true; return false; };
            while (used()) uniqueId = (id.isEmpty() ? "app" : id) + "-" + QString::number(suffix++);
            o["id"] = uniqueId; o["name"] = display; o["fitScreen"] = autoSize->isChecked(); o["activity"] = appActivity->text().trimmed(); o["width"] = qMax(1, appWidth->text().toInt()); o["height"] = qMax(1, appHeight->text().toInt()); o["daily"] = autoUpdate->isChecked() && (tab == 0 || tab == 1);
            QString error;
            if (tab == 0) {
                o["source"] = "github"; o["github"] = normalizeRepo(ghRepo->text()); o["version"] = release.value("tag_name").toString();
                const QJsonObject asset = QJsonDocument::fromJson(ghAssets->currentData().toString().toUtf8()).object(); ApkSourceResult result; const QString digest = asset.value("digest").toString().remove("sha256:");
                if (!downloadApk(QUrl(asset.value("browser_download_url").toString()), digest, &result)) error = result.message;
                else if (!stageAndActivate(o, result.bytes, &error)) {}
            } else if (tab == 1) {
                o["source"] = "fdroid"; o["packageId"] = fdPackage;
                ApkSourceResult result; if (!getFdroidApk(fdPackage, &result)) error = result.message;
                else { o["version"] = result.version; o["versionCode"] = result.versionCode; o["fdroidLatestCode"] = result.latestCode; if (!stageAndActivate(o, result.bytes, &error)) {} else saveAppIcon(o, result.iconBytes); }
            } else {
                const bool fromMirror = tab == 2;
                o["source"] = fromMirror ? "apkmirror" : "local";
                if (fromMirror) o["sourceUrl"] = mirrorChoice.value("url").toString();
                QFile file(localFile);
                if (!file.open(QIODevice::ReadOnly)) error = "Could not read the APK file.";
                else { const QByteArray bytes = file.readAll();
                    if (!apkMatchesHostArchitecture(bytes)) error = "This APK is invalid or does not support this device. Choose a standalone APK for " + QSysInfo::currentCpuArchitecture() + ".";
                    else stageAndActivate(o, bytes, &error);
                }
            }
            progress.hide();
            if (!error.isEmpty()) { QDir(appDir(o)).removeRecursively(); if (!progress.wasCanceled()) mobileNotice(&dialog, "Could not install app", error); return; }
            QJsonArray previousApps = apps; apps.append(o); if (!writeApps(apps)) { apps = previousApps; QDir(appDir(o)).removeRecursively(); mobileNotice(&dialog, "Save failed", "Could not save the app library."); return; }
            updateDesktop(o, atlPath->text()); QString timerMessage; const bool timerEnabled = !o.value("daily").toBool() || writeTimer(o, true, &timerMessage);
            if (!timerEnabled) { o["daily"] = false; o["updateError"] = timerMessage; apps[apps.size() - 1] = o; writeApps(apps); }
            dialog.accept(); reload(); openDetails(apps.size() - 1); status->setText("Installed · ready to open" + (timerEnabled ? QString() : "\nAutomatic updates could not be enabled."));
            if (!timerEnabled) QTimer::singleShot(0, this, [this, display, timerMessage] { mobileNotice(this, "App installed", display + " was installed, but its daily update timer could not be enabled. You can still update it with ‘Update app’.\n\n" + timerMessage); });
        });
        auto refreshInstall = [&] {
            const int tab = tabs->currentIndex();
            const bool sourceReady = tab == 0 ? ghAssets->currentIndex() >= 0 : tab == 1 ? !fdPackage.isEmpty() : tab == 2 ? !mirrorChoice.isEmpty() && !localFile.isEmpty() : !localFile.isEmpty();
            addButton->setEnabled(sourceReady && !appName->text().trimmed().isEmpty());
        };
        connect(appName, &QLineEdit::textChanged, &dialog, refreshInstall);
        connect(ghAssets, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, refreshInstall);
        connect(fdResults, &QListWidget::currentItemChanged, &dialog, refreshInstall);
        connect(localPath, &QLineEdit::textChanged, &dialog, refreshInstall);
        connect(tabs, &QTabWidget::currentChanged, &dialog, [&] {
            const int tab = tabs->currentIndex();
            appName->setText(tab == 0 ? ghRepo->text().section('/', -1) : tab == 1 ? fdName : QFileInfo(localFile).completeBaseName()); refreshInstall();
        });
        sourcePicker->setCurrentIndex(1);
        dialog.exec();
    }
};

#ifdef ATL_SHELF_TESTING
QWidget *createShelfForTests() { return new Shelf; }
QWidget *currentPageForTests() { return mobileHost ? mobileHost->currentWidget() : nullptr; }
QByteArray fetchForTests(const QUrl &url, QWidget *parent) { TransferProgress progress("Downloading…", parent); return httpGet(url); }
#else
int main(int argc,char **argv) {
    const QByteArray activationToken = qgetenv("XDG_ACTIVATION_TOKEN");
    const QStringList args = [&] { QStringList a; for (int i=0; i<argc; ++i) a << QString::fromLocal8Bit(argv[i]); return a; }();
    if ((args.size() > 1 && args[1] == "cli") || args.contains("--help") || args.contains("--version")) {
        QCoreApplication app(argc,argv); QCoreApplication::setApplicationName("atl-shelf"); QCoreApplication::setOrganizationName("ATL Shelf");
        return shelfCli(args.mid(1));
    }
    if (args.size() == 2 && args[1] == "--refresh-icons") {
        QCoreApplication app(argc,argv); QCoreApplication::setApplicationName("atl-shelf"); QCoreApplication::setOrganizationName("ATL Shelf");
        auto lock = lockLibrary(); if (!lock) return 3;
        auto entries = readApps();
        for (int i=0; i<entries.size(); ++i) { auto entry=entries[i].toObject(); if(entry.value("iconSource").toString() != "apk") extractAppIcon(entry); entries[i]=entry; updateDesktop(entry,{}); }
        return writeApps(entries) ? 0 : 1;
    }
    if(args.size()==3 && (args[1]=="--update" || args[1]=="--check-updates")) {
        QCoreApplication app(argc,argv); QCoreApplication::setApplicationName("atl-shelf"); QCoreApplication::setOrganizationName("ATL Shelf");
        auto lock = lockLibrary(); if (!lock) return 3;
        QJsonArray apps=readApps(); for(int i=0;i<apps.size();++i) { auto o=apps[i].toObject(); if(o.value("id").toString()==args[2]) { QString msg; const bool ok=updateOne(o,&msg); apps[i]=o; writeApps(apps); QFile sf(rootDir()+"/settings.json"); QString atl; if(sf.open(QIODevice::ReadOnly)) atl=QJsonDocument::fromJson(sf.readAll()).object().value("atl").toString(); updateDesktop(o,atl); return (ok || msg.startsWith("The app is already up to date")) ? 0 : 1; } } return 2;
    }
    QApplication app(argc,argv); QCoreApplication::setApplicationName("atl-shelf"); QCoreApplication::setOrganizationName("ATL Shelf"); app.setWindowIcon(QIcon(":/org.atl-shelf.png")); app.setDesktopFileName("org.atl-shelf");
    if (args.size() == 3 && args[1] == "--launch") {
        for (const auto &value : readApps()) { const auto entry = value.toObject(); if (entry.value("id").toString() == args[2]) { QString error; if (launchAndroidApp(entry, &error)) return 0; qWarning().noquote() << error; return 1; } } return 2;
    }
    QDir().mkpath(rootDir()); const QString socketName = rootDir() + "/ui.sock";
    auto activateExisting = [&] { QLocalSocket socket; socket.connectToServer(socketName); if (!socket.waitForConnected(400)) return false; socket.write(activationToken.toBase64() + "\n"); socket.waitForBytesWritten(1000); return true; };
    if (activateExisting()) return 0;
    QLockFile instance(rootDir() + "/ui.lock");
    if (!instance.tryLock(1000)) { for (int attempt = 0; attempt < 5; ++attempt) if (activateExisting()) return 0; return 3; }
    QLocalServer::removeServer(socketName); QLocalServer server; server.setSocketOptions(QLocalServer::UserAccessOption); if (!server.listen(socketName)) return 3;
    Shelf w;
    QObject::connect(&server, &QLocalServer::newConnection, &w, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            auto activate = [&w, socket] {
                if (!socket->canReadLine()) return;
                const auto token = QByteArray::fromBase64(socket->readLine().trimmed());
                if (w.isMinimized()) w.showMaximized(); else w.show();
#ifdef HAVE_KWINDOWSYSTEM
                KWindowSystem::setCurrentXdgActivationToken(QString::fromUtf8(token));
                KWindowSystem::activateWindow(w.windowHandle());
#else
                w.raise(); w.activateWindow();
#endif
                socket->disconnectFromServer(); socket->deleteLater();
            };
            QObject::connect(socket, &QLocalSocket::readyRead, &w, activate);
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            activate();
        }
    });
    if (QGuiApplication::primaryScreen()->availableGeometry().width() < 600) w.showMaximized(); else w.show(); return app.exec();
}

#endif
#include "main.moc"
