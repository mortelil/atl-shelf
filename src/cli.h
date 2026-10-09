#pragma once
#include <QTextStream>
#include <QThread>
#include <QElapsedTimer>
#include <QSet>
#include <sys/stat.h>
#include <csignal>

// CLI uses QCoreApplication: no display connection, widgets, or nested GUI dialogs.
namespace ShelfCli {
static volatile sig_atomic_t interrupted = 0;
static void interrupt(int) { interrupted = 1; }
static int reply(bool ok, const QJsonValue &result = {}, const QString &error = {}, int code = 5) {
    if (!ok && interrupted) code = 130;
    QJsonObject object{{"schemaVersion",1},{"ok",ok}};
    if (!result.isUndefined()) object["result"] = result;
    if (!error.isEmpty()) object["error"] = error;
    QTextStream(stdout) << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n';
    return ok ? 0 : code;
}
static void sessionEnvironment() {
    const QString runtime = "/run/user/" + QString::number(getuid());
    const QFileInfo info(runtime);
    if (!info.isDir() || info.ownerId() != getuid()) return;
    if (qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR")) qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") && QFile::exists(runtime+"/bus")) qputenv("DBUS_SESSION_BUS_ADDRESS",("unix:path="+runtime+"/bus").toUtf8());
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        for (const auto &name : QDir(runtime).entryList({"wayland-*"},QDir::System)) {
            struct stat st;
            if (::stat(QFile::encodeName(runtime+"/"+name).constData(),&st)==0 && S_ISSOCK(st.st_mode)) { qputenv("WAYLAND_DISPLAY",name.toUtf8()); break; }
        }
    }
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        if (qEnvironmentVariableIsEmpty("GDK_BACKEND")) qputenv("GDK_BACKEND","wayland");
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM","wayland");
    }
}
static bool validId(const QString &id) { return QRegularExpression("^[a-z0-9][a-z0-9_-]*$").match(id).hasMatch(); }
static QJsonObject rawSettings() { QFile f(settingsFilePath()); if (!f.open(QIODevice::ReadOnly)) return {}; return QJsonDocument::fromJson(f.readAll()).object(); }
static bool saveSettings(const QJsonObject &value) {
    QDir().mkpath(rootDir()); QSaveFile file(settingsFilePath());
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(value).toJson()) > 0 && file.commit();
}
static QJsonObject display() {
    const auto d=currentDisplay();
    return {{"width",d.pixels.width()},{"height",d.pixels.height()},{"logicalWidth",d.logical.width()},{"logicalHeight",d.logical.height()},{"renderScale",d.scale},{"source",d.source}};
}
static QJsonArray processes(const QJsonObject &entry) {
    QJsonArray result; const auto settings=readSettings();
    const QString binary=settings.value("atl").toString(), launcher=settings.value("launcher").toString(binary);
    for (const auto &dir : QDir("/proc").entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        bool numeric=false; const int pid=dir.fileName().toInt(&numeric);
        if (!numeric || pid<=1 || dir.ownerId()!=getuid()) continue;
        QFile file(dir.filePath()+"/cmdline"); if (!file.open(QIODevice::ReadOnly)) continue;
        const auto args=file.readAll().split('\0');
        if (!args.contains(apkPath(entry).toUtf8())) continue;
        const auto exe=QFileInfo(dir.filePath()+"/exe").symLinkTarget();
        if (exe==binary || exe==launcher || args.contains(binary.toUtf8()) || args.contains(launcher.toUtf8())) result.append(pid);
    }
    return result;
}
// ATL stores the launched APK's private files in <data root>/<APK filename>_.
static bool clearPrivateData(const QJsonObject &entry, QString *error) {
    if (!processes(entry).isEmpty()) { *error = "Close the app before clearing its private data."; return false; }
    const QString base = QDir::cleanPath(appDir(entry));
    if (!validId(entry.value("id").toString()) || QFileInfo(base).isSymLink() ||
        QFileInfo(base).canonicalFilePath() != base) {
        *error = "The managed app directory is missing or redirected. Nothing was cleared."; return false;
    }
    auto env = readSettings().value("runtimeEnv").toObject();
    const auto local = entry.value("launchEnv").toObject();
    for (auto i=local.begin(); i!=local.end(); ++i) env[i.key()]=i.value();
    if (env.contains("ANDROID_APP_DATA_DIR") && QDir::cleanPath(env.value("ANDROID_APP_DATA_DIR").toString()) != base) {
        *error = "This app uses a custom data directory. Automatic clearing is unavailable to protect shared data."; return false;
    }
    for (const auto &name : QStringList{QFileInfo(apkPath(entry)).fileName()+"_", ".cache"}) {
        const QString path = base+"/"+name;
        const QFileInfo info(path);
        // Unlink a symlink itself; never traverse its target.
        const bool ok = info.isSymLink() ? QFile::remove(path) : !info.exists() || (info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path));
        if (!ok) { *error = "Could not clear " + path + ". Some private data may already have been removed."; return false; }
    }
    return true;
}
static QJsonObject inspect(QJsonObject entry) {
    entry["apkPath"]=apkPath(entry); entry["dataDirectory"]=appDir(entry);
    entry["logPath"]=appDir(entry)+"/launch.log"; entry["processes"]=processes(entry);
    return entry;
}
static bool environmentValid(const QJsonValue &value) {
    if (!value.isObject()) return false;
    const auto env=value.toObject();
    for (auto i=env.begin();i!=env.end();++i) if (!QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$").match(i.key()).hasMatch() || (!i.value().isString() && !i.value().isNull()) || i.value().toString().contains(QChar(0))) return false;
    return true;
}
static bool merge(QJsonObject &target,const QJsonObject &patch,bool settings,QString *error) {
    const QStringList text=settings ? QStringList{"atl","launcher","workingDirectory","runtimeType","runtimeLabel"} : QStringList{"name","activity","workingDirectory","source","github","packageId","sourceUrl"};
    const QStringList boolean=settings ? QStringList{} : QStringList{"daily","fitScreen"};
    for (auto i=patch.begin();i!=patch.end();++i) {
        const auto key=i.key(); const auto value=i.value();
        const bool env=key==(settings ? "runtimeEnv" : "launchEnv");
        if (!text.contains(key) && !boolean.contains(key) && !env && (settings || (key!="width" && key!="height"))) { *error="Unknown or read-only field: "+key; return false; }
        if (value.isNull()) { target.remove(key); continue; }
        if ((text.contains(key) && (!value.isString() || value.toString().contains(QChar(0)) || value.toString().contains('\n'))) || (boolean.contains(key) && !value.isBool()) || (env && !environmentValid(value)) || ((!settings && (key=="width" || key=="height")) && (!value.isDouble() || value.toDouble()!=value.toInt() || value.toInt()<1 || value.toInt()>16384))) { *error="Invalid value for "+key; return false; }
        if (env) {
            auto current=target.value(key).toObject(); const auto changes=value.toObject();
            for (auto e=changes.begin();e!=changes.end();++e) { if(e.value().isNull()) current.remove(e.key()); else current[e.key()]=e.value(); }
            target[key]=current;
        } else target[key]=value;
    }
    if (!settings && target.value("name").toString().trimmed().isEmpty()) { *error="App name cannot be empty."; return false; }
    if (!settings && !QStringList{"local","github","fdroid","apkmirror"}.contains(target.value("source").toString())) { *error="Unknown app source."; return false; }
    if (settings && target.contains("runtimeType") && !QStringList{"existing","github","gitlab","apk"}.contains(target.value("runtimeType").toString())) { *error="Unknown runtime type."; return false; }
    if (!settings && target.value("daily").toBool() && !QStringList{"github","fdroid"}.contains(target.value("source").toString())) { *error="Daily updates require a GitHub or F-Droid source."; return false; }
    return true;
}
// Child output goes to stderr and a file, preserving stdout for the JSON result.
static int run(QProcess &process,const QString &logPath,bool cancellable=true) {
    QDir().mkpath(QFileInfo(logPath).absolutePath()); QFile log(logPath); if(!log.open(QIODevice::WriteOnly|QIODevice::Truncate)) return -1;
    process.setProcessChannelMode(QProcess::MergedChannels); process.setInputChannelMode(QProcess::ForwardedInputChannel);
    process.setChildProcessModifier([] { ::setpgid(0,0); }); process.start();
    if(!process.waitForStarted(10000)) return -1;
    while(process.state()!=QProcess::NotRunning) {
        process.waitForReadyRead(100);
        const auto bytes=process.readAll(); log.write(bytes); log.flush(); fwrite(bytes.constData(),1,bytes.size(),stderr); fflush(stderr);
        if(interrupted && cancellable) { const auto pid=process.processId(); if(pid>1) ::kill(-pid_t(pid),SIGTERM); if(!process.waitForFinished(3000) && pid>1) { ::kill(-pid_t(pid),SIGKILL); process.waitForFinished(3000); } break; }
    }
    const auto tail=process.readAll(); log.write(tail); fwrite(tail.constData(),1,tail.size(),stderr);
    if(interrupted) return 130;
    return process.exitStatus()==QProcess::NormalExit ? process.exitCode() : 128+process.exitCode();
}
static QJsonObject help() {
    return {{"usage","atl-shelf cli COMMAND [ID] [OPTIONS]"},{"output","JSON on stdout; progress on stderr. No GUI required."},
        {"commands",QJsonArray{
            "status | doctor | list | show ID",
            "search --source fdroid|github|apkmirror --query TEXT",
            "install --source local|github|fdroid|apkmirror --value PATH_OR_REPO_OR_PACKAGE [--name NAME] [--id ID] [--daily] [--source-url URL]",
            "replace ID --apk PATH (replace APK, preserve app data)",
            "configure ID --set JSON_OBJECT (or --file JSON_FILE; '-' reads stdin)",
            "settings [--set JSON_OBJECT | --file JSON_FILE]",
            "launch ID [--dry-run] [--wait] [--env JSON_OBJECT] [--activity CLASS]",
            "stop ID [--force]",
            "logs ID [--lines N] [--follow]",
            "check-updates ID | check-updates --all",
            "update ID | update --all",
            "remove ID [--keep-data]",
            "clear-data ID",
            "icons ID | icons --all",
            "refresh (regenerate menu entries)",
            "runtime show | runtime update [--jobs N]",
            "runtime setup --source github|gitlab|apk|existing [--binary PATH] [--launcher PATH] [--working-directory PATH] [--jobs N] [--install-deps]"
        }},
        {"appWritableFields",QJsonArray{"name","activity","workingDirectory","source","github","packageId","sourceUrl","width","height","fitScreen","daily","launchEnv"}},
        {"settingsWritableFields",QJsonArray{"atl","launcher","workingDirectory","runtimeType","runtimeLabel","runtimeEnv"}},
        {"exitCodes",QJsonObject{{"0","Success"},{"2","Invalid command or configuration"},{"3","App not found"},{"4","Busy"},{"5","Operation failed; inspect result for partial changes"},{"6","Child process failed"},{"130","Interrupted"}}}};
}
}

static int shelfCli(QStringList arguments) {
    using namespace ShelfCli;
    sessionEnvironment();
    if(arguments.contains("--version")) return reply(true,QJsonObject{{"version","0.4.0"},{"cliSchemaVersion",1}});
    if(!arguments.isEmpty() && arguments.first()=="cli") arguments.removeFirst();
    if(arguments.isEmpty() || arguments.contains("--help") || arguments.first()=="help") return reply(true,help());
    QMap<QString,QString> options; QStringList positional;
    const QStringList flags{"--all","--daily","--dry-run","--wait","--force","--keep-data","--follow","--install-deps","--json"};
    const QStringList values{"--env","--activity","--apk","--source","--value","--query","--name","--id","--source-url","--set","--file","--lines","--jobs","--binary","--launcher","--working-directory"};
    for(int i=0;i<arguments.size();++i) {
        const QString a=arguments[i];
        if(a.startsWith("--")) {
            if(options.contains(a)) return reply(false,{},"Duplicate option: "+a,2);
            if(flags.contains(a)) options[a]="true";
            else if(values.contains(a) && i+1<arguments.size()) options[a]=arguments[++i];
            else return reply(false,{},"Unknown option or missing value: "+a,2);
        } else positional<<a;
    }
    const QString command=positional.value(0), id=positional.value(1);
    const QStringList commands{"status","doctor","list","show","search","install","replace","configure","settings","launch","stop","logs","check-updates","update","remove","clear-data","icons","refresh","runtime"};
    if(!commands.contains(command) || positional.size()>2) return reply(false,{},"Unknown command or extra argument. Run atl-shelf cli help.",2);
    const QMap<QString,QStringList> allowed{
        {"search",{"--source","--query"}}, {"install",{"--source","--value","--name","--id","--daily","--source-url"}},
        {"replace",{"--apk"}}, {"configure",{"--set","--file"}}, {"settings",{"--set","--file"}}, {"launch",{"--dry-run","--wait","--env","--activity"}},
        {"stop",{"--force"}}, {"logs",{"--lines","--follow"}}, {"check-updates",{"--all"}}, {"update",{"--all"}},
        {"remove",{"--keep-data"}}, {"icons",{"--all"}},
        {"runtime",id=="setup" ? QStringList{"--source","--binary","--launcher","--working-directory","--jobs","--install-deps"} : id=="update" ? QStringList{"--jobs"} : QStringList{}}
    };
    for (auto i=options.begin();i!=options.end();++i) if (i.key()!="--json" && !allowed.value(command).contains(i.key())) return reply(false,{},"Option "+i.key()+" is not supported by "+command,2);
    if (!id.isEmpty() && QStringList{"status","doctor","list","search","install","settings","refresh"}.contains(command)) return reply(false,{},"Unexpected positional argument: "+id,2);
    if (options.contains("--all") && !id.isEmpty()) return reply(false,{},"Use an ID or --all, not both.",2);
    if (options.contains("--dry-run") && options.contains("--wait")) return reply(false,{},"Use --dry-run or --wait, not both.",2);
    for (const auto &path : {dbPath(),settingsFilePath()}) {
        if (!QFile::exists(path)) continue;
        QFile file(path); if(!file.open(QIODevice::ReadOnly)) return reply(false,{},"Cannot read "+path);
        QJsonParseError error; const auto doc=QJsonDocument::fromJson(file.readAll(),&error);
        if(error.error!=QJsonParseError::NoError || (path==dbPath() ? !doc.isArray() : !doc.isObject())) return reply(false,{},"Invalid JSON in "+path+"; repair it before modifying the library.");
    }
    std::signal(SIGINT,interrupt); std::signal(SIGTERM,interrupt);
    requestCanceled = [] { return interrupted != 0; };
    QJsonObject patch;
    if(options.contains("--set") || options.contains("--file")) {
        if(options.contains("--set") && options.contains("--file")) return reply(false,{},"Use either --set or --file.",2);
        QByteArray bytes=options.value("--set").toUtf8();
        if(options.contains("--file")) { QFile file; bool opened=false; if(options["--file"]=="-") opened=file.open(stdin,QIODevice::ReadOnly); else { file.setFileName(options["--file"]); opened=file.open(QIODevice::ReadOnly); } if(!opened) return reply(false,{},"Cannot read JSON file.",2); bytes=file.readAll(); }
        QJsonParseError error; auto doc=QJsonDocument::fromJson(bytes,&error);
        if(error.error!=QJsonParseError::NoError || !doc.isObject()) return reply(false,{},"Expected a JSON object: "+error.errorString(),2);
        patch=doc.object();
    }
    const bool modifies=QStringList{"install","replace","configure","update","remove","clear-data","icons","refresh","runtime"}.contains(command) || (command=="settings" && (options.contains("--set")||options.contains("--file")));
    auto lock=(modifies && !(command=="runtime" && id=="show")) ? lockLibrary() : std::unique_ptr<QLockFile>();
    if(modifies && !(command=="runtime" && id=="show") && !lock) return reply(false,{},"Another installation, update or settings change is running.",4);
    auto entries=readApps();
    QSet<QString> ids;
    for (const auto &value : entries) {
        const auto entry=value.toObject(); const auto appId=entry.value("id").toString();
        if (!value.isObject() || !validId(appId) || ids.contains(appId) || QFileInfo(appDir(entry)).isSymLink()) return reply(false,{},"Invalid, duplicate or symlinked app entry in library.");
        ids.insert(appId);
    }
    if(command=="status" || command=="doctor") {
        QJsonArray apps; for(const auto &v:entries) apps.append(inspect(v.toObject()));
        QJsonObject tools; for(const auto &tool:QStringList{"aapt","unzip","gpg","systemctl","kscreen-doctor","git","apk","doas","pkexec"}) tools[tool]=QStandardPaths::findExecutable(tool);
        const auto settings=readSettings();
        return reply(true,QJsonObject{{"version","0.4.0"},{"dataDirectory",rootDir()},{"settingsPath",settingsFilePath()},{"runtime",settings},{"display",display()},{"apps",apps},{"tools",tools},{"runtimeExecutable",QFileInfo(settings.value("atl").toString()).isExecutable()},{"waylandDisplay",qEnvironmentVariable("WAYLAND_DISPLAY")},{"sessionBus",qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS")}});
    }
    if(command=="list") { QJsonArray result; for(const auto &v:entries) result.append(inspect(v.toObject())); return reply(true,result); }
    if(command=="settings") {
        if(modifies) { auto settings=rawSettings(); QString error; if(!merge(settings,patch,true,&error)) return reply(false,{},error,2); if(!saveSettings(settings)) return reply(false,{},"Could not save settings."); }
        return reply(true,QJsonObject{{"stored",rawSettings()},{"effective",readSettings()}});
    }
    if(command=="search") {
        const QString source=options.value("--source"),query=options.value("--query"); QString error;
        if(query.isEmpty()) return reply(false,{},"--query is required.",2);
        if(source=="github") { int status=0; const auto release=latestGithubRelease(normalizeRepo(query),&status); return reply(status==200,release,status==200 ? QString() : "GitHub HTTP "+QString::number(status)); }
        QJsonArray result;
        if(source=="fdroid") result=searchFdroid(query,&error); else if(source=="apkmirror") result=searchApkMirror(query,&error); else return reply(false,{},"Unknown search source.",2);
        return reply(error.isEmpty(),result,error);
    }
    if(command=="runtime") {
        if(id=="show") return reply(true,QJsonObject{{"settings",readSettings()},{"display",display()},{"logPath",runtimeRoot()+"/cli-runtime.log"}});
        if(id!="setup" && id!="update") return reply(false,{},"Expected runtime show, setup or update.",2);
        for(const auto &v:entries) if(!processes(v.toObject()).isEmpty()) return reply(false,{},"Close Android apps before changing the runtime.",4);
        const QString mode=id=="update" ? readSettings().value("runtimeType").toString() : options.value("--source");
        if(!QStringList{"github","gitlab","apk","existing"}.contains(mode) || (id=="update" && mode!="github" && mode!="gitlab")) return reply(false,{},"Select github, gitlab, apk or existing. Repository updates require github/gitlab.",2);
        bool ok=false; const int jobs=options.value("--jobs","2").toInt(&ok); if(!ok || jobs<1 || jobs>64) return reply(false,{},"--jobs must be between 1 and 64.",2);
        const QString root=runtimeRoot(),base=root+"/"+mode; QDir().mkpath(root);
        QString binary,launcher,workdir;
        if(mode=="existing") {
            binary=QFileInfo(options.value("--binary")).absoluteFilePath(); launcher=options.contains("--launcher") ? QFileInfo(options["--launcher"]).absoluteFilePath() : binary; workdir=options.value("--working-directory",QFileInfo(binary).absolutePath());
            if(options.value("--binary").isEmpty() || !QFileInfo(binary).isExecutable() || !QFileInfo(launcher).isExecutable()) return reply(false,{},"--binary and launcher must be executable files.",2);
        } else {
            if(options.contains("--install-deps") || mode=="apk") {
                QStringList packages=mode=="apk" ? QStringList{"android-translation-layer","bionic_translation","art_standalone"} : QStringList{"git","build-base","meson","python3","pkgconf","java-common","openjdk8-jdk","android-build-tools","elfutils-dev","libunwind-dev","libbsd-dev","libcap-dev","pc:alsa","pc:glib-2.0","pc:gtk4","pc:gudev-1.0","pc:libportal","pc:openxr","pc:vulkan","pc:webkitgtk-6.0","pc:libsecret-1","ffmpeg-dev","bionic_translation-dev","art_standalone-dev","libandroidfw-dev"};
                if(mode=="gitlab") packages<<"wolfssl-dev";
                packages<<"android-build-tools"<<"unzip"<<"qt6-qtsvg"; packages.removeDuplicates();
                const auto apk=QStandardPaths::findExecutable("apk"); if(apk.isEmpty()) return reply(false,{},"Alpine apk was not found.");
                QProcess process; QStringList args{"add","--no-progress"}; args+=packages;
                if(geteuid()!=0) { const auto elevate=QStandardPaths::findExecutable("doas"); if(elevate.isEmpty()) return reply(false,{},"Install dependencies with apk as administrator, or install doas and use ssh -t."); args.prepend(apk); process.setProgram(elevate); } else process.setProgram(apk);
                process.setArguments(args); const int exit=run(process,root+"/cli-dependencies.log",false); if(exit!=0) return reply(false,QJsonObject{{"exitCode",exit},{"logPath",root+"/cli-dependencies.log"}},"Dependency installation failed.",exit==130 ? 130 : 6);
            }
            if(mode=="apk") { binary=launcher=QStandardPaths::findExecutable("android-translation-layer"); if(binary.isEmpty()) return reply(false,{},"ATL package executable was not found."); }
            else {
                QFile source(":/runtime-setup.sh"); QSaveFile script(root+"/runtime-setup.sh");
                if(!saveDisplayPatch(root) || !source.open(QIODevice::ReadOnly) || !script.open(QIODevice::WriteOnly) || script.write(source.readAll())<100 || !script.commit()) return reply(false,{},"Cannot prepare runtime build files.");
                QProcess process; process.setProgram("/bin/sh"); process.setArguments({root+"/runtime-setup.sh",mode,base,QString::number(jobs),id=="update" ? "update" : "setup"});
                const int exit=run(process,root+"/cli-runtime.log"); if(exit!=0) return reply(false,QJsonObject{{"exitCode",exit},{"logPath",root+"/cli-runtime.log"}},"Runtime build failed.",exit==130 ? 130 : 6);
                workdir=base+"/workspace/android_translation_layer/build-mobile"; binary=workdir+"/android-translation-layer";
                launcher=mode=="github" ? base+"/workspace/android_translation_layer/scripts/mobile/run.sh" : base+"/atl-launcher.sh";
            }
        }
        if(id=="update") return reply(true,readSettings());
        auto settings=rawSettings(); const QString previousMode=settings.value("runtimeType").toString(); settings["runtimeType"]=mode; settings["runtimeLabel"]="Configured via CLI ("+mode+")"; settings["atl"]=binary; settings["launcher"]=launcher; settings["workingDirectory"]=workdir;
        auto env=settings.value("runtimeEnv").toObject();
        if(previousMode!=mode) for(const auto &key:QStringList{"ATL_WORKSPACE","ATL_PREFIX","ATL_BUILD_DIR"}) env.remove(key);
        if(!env.contains("ATL_DISABLE_FULLSCREEN")) env["ATL_DISABLE_FULLSCREEN"]="1";
        settings["runtimeEnv"]=env;
        if(!saveSettings(settings)) return reply(false,{},"Runtime built but settings could not be saved.");
        return reply(true,readSettings());
    }
    if(command=="install") {
        const auto source=options.value("--source"),value=options.value("--value");
        if(value.isEmpty() || !QStringList{"local","github","fdroid","apkmirror"}.contains(source)) return reply(false,{},"Provide --source and --value. APKMirror imports a downloaded APK.",2);
        QString name=options.value("--name",source=="github" ? normalizeRepo(value).section('/',-1) : source=="fdroid" ? value : QFileInfo(value).completeBaseName());
        QString appId=options.value("--id",safeId(name)); if(!validId(appId) || name.trimmed().isEmpty() || name.contains('\n')) return reply(false,{},"Invalid app ID or name.",2);
        for(const auto &v:entries) if(v.toObject().value("id").toString()==appId) return reply(false,{},"App ID or data directory already exists. Choose another --id.",2);
        if(QFileInfo::exists(rootDir()+"/apps/"+appId)) return reply(false,{},"App data already exists; choose another --id.",2);
        QJsonObject entry{{"id",appId},{"name",name},{"source",source},{"fitScreen",true},{"daily",options.contains("--daily")}};
        if(source=="github") entry["github"]=normalizeRepo(value);
        if(source=="fdroid") entry["packageId"]=value;
        if(source=="apkmirror") entry["sourceUrl"]=options.value("--source-url");
        QString error; if(!merge(entry,{},false,&error)) return reply(false,{},error,2);
        ApkSourceResult result;
        if(source=="github" || source=="fdroid") { if(!resolveApkSource(entry,&result)) return reply(false,{},result.message); }
        else { QFile file(value); if(!file.open(QIODevice::ReadOnly)) return reply(false,{},"Could not read APK."); result.bytes=file.readAll(); if(!apkMatchesHostArchitecture(result.bytes)) return reply(false,{},"Invalid or incompatible APK."); }
        if(interrupted) return reply(false,{},"Interrupted.",130);
        if(!stageAndActivate(entry,result.bytes,&error)) return reply(false,{},error);
        saveAppIcon(entry,result.iconBytes); entry["version"]=result.version; entry["versionCode"]=result.versionCode; entry["fdroidLatestCode"]=result.latestCode;
        entries.append(entry); if(!writeApps(entries)) { QDir(appDir(entry)).removeRecursively(); return reply(false,{},"Cannot save app library."); }
        updateDesktop(entry,{});
        if(entry.value("daily").toBool() && !writeTimer(entry,true,&error)) { entry["daily"]=false; entry["updateError"]=error; entries[entries.size()-1]=entry; writeApps(entries); return reply(false,inspect(entry),"App installed, but daily updates could not be enabled: "+error); }
        return reply(true,inspect(entry));
    }
    if(command=="refresh") { for(const auto &v:entries) updateDesktop(v.toObject(),{}); return reply(true,QJsonObject{{"refreshed",entries.size()}}); }
    QList<int> selected;
    if(options.contains("--all") && QStringList{"update","check-updates","icons"}.contains(command) && id.isEmpty()) { for(int i=0;i<entries.size();++i) selected<<i; }
    else {
        if(!validId(id)) return reply(false,{},"Provide a valid app ID from cli list.",2);
        for(int i=0;i<entries.size();++i) if(entries[i].toObject().value("id").toString()==id) selected<<i;
        if(selected.isEmpty()) return reply(false,{},"App not found: "+id,3);
    }
    if(QStringList{"update","check-updates","icons"}.contains(command)) {
        QJsonArray results; bool success=true;
        for(int index:selected) {
            if(interrupted) break;
            auto entry=entries[index].toObject(); QString message; bool ok=true;
            if(command!="icons" && options.contains("--all") && !QStringList{"github","fdroid"}.contains(entry.value("source").toString())) { results.append(QJsonObject{{"id",entry.value("id")},{"ok",true},{"skipped",true},{"message","No automatic update source."}}); continue; }
            if(command=="icons") { extractAppIcon(entry); ok=entry.value("iconSource").toString()=="apk"; if(!ok) message="Could not extract a supported APK icon. Check aapt, unzip and Qt SVG support."; }
            else if(command=="update") { if(!processes(entry).isEmpty()) { ok=false; message="Close this app before updating."; } else { ok=updateOne(entry,&message); if(message.startsWith("The app is already up to date")) ok=true; } }
            else {
                QString latest,code; const auto source=entry.value("source").toString();
                if(source=="github") { int status=0; const auto release=latestGithubRelease(entry.value("github").toString(),&status); latest=release.value("tag_name").toString(); ok=status==200; if(!ok) message="GitHub HTTP "+QString::number(status); }
                else if(source=="fdroid") ok=latestFdroidVersion(entry.value("packageId").toString(),&latest,&code,&message);
                else { ok=false; message="This source has no automatic updates."; }
                results.append(QJsonObject{{"id",entry.value("id")},{"ok",ok},{"latestVersion",latest},{"updateAvailable",ok && (source=="fdroid" ? code!=entry.value("fdroidLatestCode").toString(entry.value("versionCode").toString()) : latest!=entry.value("version").toString())},{"message",message}}); success &= ok; continue;
            }
            entries[index]=entry; updateDesktop(entry,{}); results.append(QJsonObject{{"id",entry.value("id")},{"ok",ok},{"message",message}}); success &= ok;
        }
        if(interrupted) { if(command!="check-updates") writeApps(entries); return reply(false,results,"Interrupted.",130); }
        if(command!="check-updates" && !writeApps(entries)) return reply(false,results,"Cannot save app library.");
        return reply(success,results,success ? QString() : "Some operations failed.");
    }
    const int index=selected.first(); auto entry=entries[index].toObject();
    if(command=="show") return reply(true,inspect(entry));
    if(command=="clear-data") {
        QString error; const bool ok=clearPrivateData(entry,&error);
        return reply(ok,QJsonObject{{"id",id},{"cleared",ok}},error);
    }
    if(command=="replace") {
        if(!processes(entry).isEmpty()) return reply(false,{},"Stop the app before replacing its APK.",4);
        QFile file(options.value("--apk")); if(!file.open(QIODevice::ReadOnly)) return reply(false,{},"Provide a readable --apk file.",2);
        const auto bytes=file.readAll(); if(!apkMatchesHostArchitecture(bytes)) return reply(false,{},"Invalid or incompatible APK.");
        QString error; if(!stageAndActivate(entry,bytes,&error)) return reply(false,{},error);
        entry["updatedAt"]=QDateTime::currentDateTime().toString(Qt::ISODate); entry["version"]="manually replaced";
        entry.remove("versionCode"); entry.remove("fdroidLatestCode"); entries[index]=entry;
        if(!writeApps(entries)) return reply(false,{},"APK replaced, but metadata could not be saved.");
        updateDesktop(entry,{}); return reply(true,inspect(entry));
    }
    if(command=="configure") {
        if(!options.contains("--set") && !options.contains("--file")) return reply(false,{},"Provide --set or --file.",2);
        const bool oldDaily=entry.value("daily").toBool(); QString error;
        if(!merge(entry,patch,false,&error)) return reply(false,{},error,2);
        if(oldDaily!=entry.value("daily").toBool() && !writeTimer(entry,entry.value("daily").toBool(),&error)) return reply(false,{},error);
        entries[index]=entry; if(!writeApps(entries)) { if(oldDaily!=entry.value("daily").toBool()) writeTimer(entry,oldDaily); return reply(false,{},"Cannot save app library."); } updateDesktop(entry,{}); return reply(true,inspect(entry));
    }
    if(command=="launch") {
        if(options.contains("--activity")) entry["activity"]=options.value("--activity");
        LaunchPlan plan; QString error; if(!prepareLaunch(entry,&plan,&error)) return reply(false,{},error);
        if(options.contains("--env")) {
            QJsonParseError parse; const auto doc=QJsonDocument::fromJson(options.value("--env").toUtf8(),&parse);
            if(parse.error!=QJsonParseError::NoError || !environmentValid(doc.object()) || !doc.isObject()) return reply(false,{},"--env must be a JSON object of environment names and string/null values.",2);
            const auto changes=doc.object();
            for(auto i=changes.begin();i!=changes.end();++i) { if(i.value().isNull()) { plan.environment.remove(i.key()); plan.overrides[i.key()]=QJsonValue::Null; } else { plan.environment.insert(i.key(),i.value().toString()); plan.overrides[i.key()]=i.value(); } }
        }
        QJsonObject result{{"program",plan.program},{"arguments",QJsonArray::fromStringList(plan.arguments)},{"workingDirectory",plan.workingDirectory},{"environment",plan.overrides},{"display",display()},{"logPath",appDir(entry)+"/launch.log"}};
        if(options.contains("--dry-run")) return reply(true,result);
        if(!QFile::exists(apkPath(entry))) return reply(false,result,"APK file is missing.");
        const auto active=processes(entry); if(!active.isEmpty()) return reply(false,QJsonObject{{"processes",active}},"App is already running. Use stop before relaunching.",4);
        QProcess process; applyLaunch(process,plan);
        if(options.contains("--wait")) { const int exit=run(process,appDir(entry)+"/launch.log"); result["exitCode"]=exit; return reply(exit==0,result,exit==0 ? QString() : "App exited unsuccessfully. See launch log.",exit==130 ? 130 : 6); }
        process.setStandardOutputFile(appDir(entry)+"/launch.log",QIODevice::Truncate); qint64 pid=0;
        if(!process.startDetached(&pid)) return reply(false,result,process.errorString());
        result["pid"]=pid; result["state"]="started (not a compatibility check)"; return reply(true,result);
    }
    if(command=="stop") {
        const auto running=processes(entry); for(const auto &pid:running) ::kill(pid.toInt(),SIGTERM);
        QElapsedTimer timer; timer.start(); while(!processes(entry).isEmpty() && timer.elapsed()<3000) QThread::msleep(100);
        if(options.contains("--force")) { for(const auto &pid:processes(entry)) ::kill(pid.toInt(),SIGKILL); QThread::msleep(100); }
        const auto remaining=processes(entry); return reply(remaining.isEmpty(),QJsonObject{{"signaled",running},{"remaining",remaining}},remaining.isEmpty() ? QString() : "App did not stop. Use --force if necessary.");
    }
    if(command=="logs") {
        bool ok=false; int lines=options.value("--lines","100").toInt(&ok); if(!ok || lines<1 || lines>10000) return reply(false,{},"--lines must be 1–10000.",2);
        const QString path=appDir(entry)+"/launch.log"; QFile file(path); if(!file.open(QIODevice::ReadOnly)) return reply(false,{},"No launch log is available.",3);
        if(file.size()>2*1024*1024) file.seek(file.size()-2*1024*1024);
        auto content=file.readAll().split('\n'); while(content.size()>lines+1) content.removeFirst();
        reply(true,QJsonObject{{"path",path},{"text",QString::fromUtf8(content.join('\n'))}});
        while(options.contains("--follow") && !interrupted) { QThread::msleep(200); if(file.size()<file.pos()) file.seek(0); const auto bytes=file.readAll(); if(!bytes.isEmpty()) reply(true,QJsonObject{{"event","log"},{"text",QString::fromUtf8(bytes)}}); }
        return interrupted ? 130 : 0;
    }
    if(command=="remove") {
        if(!processes(entry).isEmpty()) return reply(false,{},"Stop the app before removing it.",4);
        QString error; if(!writeTimer(entry,false,&error)) return reply(false,{},"Could not disable app update timer: "+error);
        entries.removeAt(index); if(!writeApps(entries)) return reply(false,{},"Cannot save app library; app data was preserved.");
        QStringList leftovers;
        const QString desktop=desktopPath()+"/atl-shelf-"+id+".desktop";
        if(QFile::exists(desktop) && !QFile::remove(desktop)) leftovers<<desktop;
        if(!options.contains("--keep-data") && !QDir(appDir(entry)).removeRecursively()) leftovers<<appDir(entry);
        return reply(leftovers.isEmpty(),QJsonObject{{"removed",id},{"dataKept",options.contains("--keep-data")},{"dataDirectory",appDir(entry)},{"leftovers",QJsonArray::fromStringList(leftovers)}},leftovers.isEmpty() ? QString() : "App removed with leftovers.");
    }
    return reply(false,{},"Unsupported command.",2);
}
