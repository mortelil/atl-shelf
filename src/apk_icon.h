#pragma once
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QBuffer>
#include <QProcess>
#include <QRegularExpression>
#include <QMap>
#include <memory>
#include <cstring>
#include <cmath>

// Decode Android resources with aapt, then render the APK's own launcher layers.
// No filenames, commands or external SVG references from the APK are executed.
class ApkIcon {
    struct Node { QString tag; QMap<QString, QString> attrs; QList<std::shared_ptr<Node>> children; };
    QString apk;
    QMap<QString, QString> resources;
    QString defs;
    int serial = 0;
    QByteArray run(const QString &program, const QStringList &args) {
        QProcess p; p.start(program, args);
        if (!p.waitForFinished(15000)) { p.kill(); p.waitForFinished(); return {}; }
        return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 ? p.readAllStandardOutput() : QByteArray();
    }
    QString resolve(QString value) const {
        for (int i = 0; i < 16 && value.startsWith('@'); ++i) value = resources.value(value.mid(1).toLower());
        return value;
    }
    static QString escaped(const QString &s) { return s.toHtmlEscaped(); }
    static double number(const QString &s, double fallback = 0) {
        if (s.isEmpty()) return fallback;
        const auto match = QRegularExpression("^\\(type 0x([0-9a-f]+)\\)0x([0-9a-f]+)$").match(s);
        if (match.hasMatch()) {
            quint32 bits = match.captured(2).toUInt(nullptr, 16);
            if (match.captured(1) == "4") { float f; std::memcpy(&f, &bits, sizeof(f)); return std::isfinite(f) ? f : fallback; }
            return bits;
        }
        bool ok; double n = s.toDouble(&ok); return ok ? n : fallback;
    }
    std::shared_ptr<Node> xml(const QString &path) {
        if (!path.startsWith("res/") || !path.endsWith(".xml")) return {};
        const QString dump = QString::fromUtf8(run("aapt", {"dump", "xmltree", apk, path}));
        QList<QPair<int, std::shared_ptr<Node>>> stack;
        std::shared_ptr<Node> root;
        for (const QString &line : dump.split('\n')) {
            auto element = QRegularExpression("^( *)E: ([^ ]+)").match(line);
            if (element.hasMatch()) {
                int indent = element.captured(1).size();
                while (!stack.isEmpty() && stack.last().first >= indent) stack.removeLast();
                auto node = std::make_shared<Node>(); node->tag = element.captured(2);
                if (stack.isEmpty()) root = node; else stack.last().second->children.append(node);
                stack.append({indent, node});
            } else if (!stack.isEmpty()) {
                auto attr = QRegularExpression("^ *A: (?:android:)?([^ (]+)(?:\\([^)]*\\))?=(.*)$").match(line);
                if (!attr.hasMatch()) continue;
                QString value = attr.captured(2);
                if (value.startsWith('"')) value = value.mid(1, value.indexOf('"', 1) - 1);
                stack.last().second->attrs[attr.captured(1)] = value;
            }
        }
        return root;
    }
    QString color(QString value, int depth) {
        value = resolve(value);
        if (value.startsWith("res/")) {
            auto gradient = xml(value);
            if (!gradient || gradient->tag != "gradient" || depth > 12) return {};
            const QString id = "gradient" + QString::number(++serial);
            auto n = [&](const char *key, double d = 0) { return QString::number(number(gradient->attrs.value(key), d)); };
            const bool radial = number(gradient->attrs.value("type")) == 1;
            QString g = radial ? "<radialGradient id=\"" + id + "\" gradientUnits=\"userSpaceOnUse\" cx=\""+n("centerX")+"\" cy=\""+n("centerY")+"\" r=\""+n("gradientRadius", 1)+"\">" : "<linearGradient id=\"" + id + "\" gradientUnits=\"userSpaceOnUse\" x1=\""+n("startX")+"\" y1=\""+n("startY")+"\" x2=\""+n("endX")+"\" y2=\""+n("endY")+"\">";
            for (const auto &stop : gradient->children) g += "<stop offset=\"" + QString::number(number(stop->attrs.value("offset"))) + "\" stop-color=\"" + color(stop->attrs.value("color"), depth + 1) + "\"/>";
            if (gradient->children.isEmpty()) g += "<stop offset=\"0\" stop-color=\""+color(gradient->attrs.value("startColor"),depth+1)+"\"/><stop offset=\"1\" stop-color=\""+color(gradient->attrs.value("endColor"),depth+1)+"\"/>";
            defs += g + (radial ? "</radialGradient>" : "</linearGradient>");
            return "url(#" + id + ")";
        }
        auto m = QRegularExpression("(?:#|\\(type 0x1[c-f]\\)0x)([0-9a-fA-F]{8})$").match(value);
        if (!m.hasMatch()) return "none";
        return "#" + m.captured(1).right(6);
    }
    QString vectorNode(const std::shared_ptr<Node> &node, int depth) {
        if (!node || depth > 16) return {};
        auto n = [&](const char *key, double d = 0) { return QString::number(number(node->attrs.value(key), d)); };
        QString result;
        if (node->tag == "path") {
            result = "<path d=\"" + escaped(node->attrs.value("pathData")) + "\" fill=\"" + color(node->attrs.value("fillColor"),depth) + "\" fill-opacity=\""+n("fillAlpha",1)+"\" fill-rule=\""+(number(node->attrs.value("fillType")) == 1 ? "evenodd" : "nonzero")+"\"";
            if (node->attrs.contains("strokeColor")) result += " stroke=\""+color(node->attrs.value("strokeColor"),depth)+"\" stroke-width=\""+n("strokeWidth")+"\" stroke-opacity=\""+n("strokeAlpha",1)+"\"";
            return result + "/>";
        }
        for (const auto &child : node->children) result += vectorNode(child, depth+1);
        if (node->tag == "group") result = "<g transform=\"translate("+n("translateX")+","+n("translateY")+") translate("+n("pivotX")+","+n("pivotY")+") rotate("+n("rotation")+") scale("+n("scaleX",1)+","+n("scaleY",1)+") translate("+QString::number(-number(node->attrs.value("pivotX")))+","+QString::number(-number(node->attrs.value("pivotY")))+")\">"+result+"</g>";
        return result;
    }
    QImage drawable(QString value, int depth = 0) {
        if (depth > 16) return {};
        value = resolve(value);
        if (value.isEmpty()) return {};
        if (!value.startsWith("res/")) {
            const auto c = color(value, depth); if (c == "none" || c.isEmpty()) return {};
            QImage image(512,512,QImage::Format_ARGB32_Premultiplied); image.fill(QColor(c)); return image;
        }
        if (!value.endsWith(".xml")) return QImage::fromData(run("unzip", {"-p", apk, value}));
        const auto node = xml(value); if (!node) return {};
        if (node->tag == "vector") {
            defs.clear(); QString body = vectorNode(node, depth+1);
            const double w = number(node->attrs.value("viewportWidth"),108), h = number(node->attrs.value("viewportHeight"),108);
            QByteArray svg = ("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"512\" height=\"512\" viewBox=\"0 0 "+QString::number(w)+" "+QString::number(h)+"\"><defs>"+defs+"</defs>"+body+"</svg>").toUtf8();
            return QImage::fromData(svg,"SVG");
        }
        if (node->tag == "adaptive-icon") {
            QImage bg, fg;
            for (const auto &child : node->children) {
                if (child->tag == "background") bg = drawable(child->attrs.value("drawable"), depth+1);
                if (child->tag == "foreground") fg = drawable(child->attrs.value("drawable"), depth+1);
            }
            if (bg.isNull() || fg.isNull()) return {};
            QImage image(512,512,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QPainter p(&image); p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
            QPainterPath mask; mask.addEllipse(QRectF(0,0,512,512)); p.setClipPath(mask);
            // Android's 108dp layers extend 18dp past the 72dp launcher mask.
            const QRectF layer(-128,-128,768,768); p.drawImage(layer,bg); p.drawImage(layer,fg); p.end(); return image;
        }
        if (node->tag == "bitmap") return drawable(node->attrs.value("src"),depth+1);
        return {};
    }
public:
    QImage extract(const QString &path) {
        apk = path;
        const QString dump = QString::fromUtf8(run("aapt", {"dump", "--values", "resources", apk}));
        QString id;
        const QRegularExpression resourcePattern("^ *resource (0x[0-9a-f]+) .* t=0x([0-9a-f]+) d=0x([0-9a-f]+)");
        const QRegularExpression stringPattern("^ *\\(string8?\\) \"([^\"]+)\"");
        for (const auto &line : dump.split('\n')) {
            if (line.contains("resource 0x") && !line.contains("spec resource")) {
                id.clear();
                if (!line.contains(":drawable/") && !line.contains(":mipmap/") && !line.contains(":color/")) continue;
            } else if (id.isEmpty()) continue;
            const auto r = resourcePattern.match(line);
            if (r.hasMatch()) {
                id=r.captured(1);
                const int type=r.captured(2).toInt(nullptr,16);
                if (type>=28 && type<=31) resources[id]="#"+r.captured(3).rightJustified(8,'0');
                else if (type==1) resources[id]="@0x"+r.captured(3);
            } else if (!id.isEmpty()) {
                auto str=stringPattern.match(line);
                if (str.hasMatch()) resources[id]=str.captured(1);
            }
        }
        const QString badging=QString::fromUtf8(run("aapt", {"dump","badging",apk}));
        QStringList candidates;
        auto matches=QRegularExpression("application-icon-[0-9]+:'([^']+)'").globalMatch(badging);
        while(matches.hasNext()) candidates.prepend(matches.next().captured(1));
        const auto fallback=QRegularExpression("application:.* icon='([^']+)'").match(badging);
        if(fallback.hasMatch()) candidates.prepend(fallback.captured(1));
        candidates.removeDuplicates();
        for(const auto &candidate:candidates) { auto result=drawable(candidate); if(!result.isNull()) return result; }
        return {};
    }
};
