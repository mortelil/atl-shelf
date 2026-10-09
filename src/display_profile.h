#pragma once
#include <QGuiApplication>
#include <QScreen>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>

struct DisplayProfile {
    QSize pixels;
    QSize logical;
    double scale = 1;
};
inline DisplayProfile currentDisplay() {
    static DisplayProfile cached;
    static qint64 checked = 0;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (checked && now - checked < 2000) return cached;
    checked = now;
    auto *screen = qobject_cast<QGuiApplication *>(QCoreApplication::instance()) ? QGuiApplication::primaryScreen() : nullptr;
    cached.logical = screen ? screen->availableGeometry().size() : QSize(960, 540);
    cached.scale = screen ? screen->devicePixelRatio() : 1;
    cached.pixels = cached.logical * cached.scale;
    // Qt can round a fractional Wayland scale to an integer buffer scale.
    // KScreen supplies the actual output mode and compositor scale on Plasma.
    QProcess query;
    query.start("kscreen-doctor", {"-j"});
    if (!query.waitForFinished(1500)) { query.kill(); query.waitForFinished(); return cached; }
    const auto outputs = QJsonDocument::fromJson(query.readAllStandardOutput()).object().value("outputs").toArray();
    for (const auto &value : outputs) {
        const auto output = value.toObject();
        if (!output.value("enabled").toBool() || !output.value("connected").toBool()) continue;
        if (screen && output.value("name").toString() != screen->name()) continue;
        const double scale = output.value("scale").toDouble();
        for (const auto &modeValue : output.value("modes").toArray()) {
            const auto mode = modeValue.toObject();
            if (mode.value("id") != output.value("currentModeId")) continue;
            const auto size = mode.value("size").toObject();
            QSize pixels(size.value("width").toInt(), size.value("height").toInt());
            if (!pixels.isValid() || scale < 1 || scale > 4) continue;
            if (output.value("rotation").toInt() == 2 || output.value("rotation").toInt() == 8) pixels.transpose();
            cached.pixels = pixels; cached.scale = scale;
            return cached;
        }
    }
    cached.scale = qBound(1.0, cached.scale, 4.0);
    return cached;
}
