// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <cmath>

namespace {

// Used when Omarchy isn't installed: a neutral dark surface with Omarchy's
// fallback yellow accent, so the app still reads well on its own.
const QHash<QString, QColor> &fallbackColors() {
    static const QHash<QString, QColor> colors{
        {"accent", QColor("#FFD60A")},
        {"background", QColor("#16161a")},
        {"dark_background", QColor("#111114")},
        {"lighter_background", QColor("#232329")},
        {"foreground", QColor("#c8c8d0")},
        {"bright_foreground", QColor("#ececf1")},
        {"red", QColor("#f7768e")},
        {"yellow", QColor("#e0af68")},
        {"blue", QColor("#7aa2f7")},
        {"magenta", QColor("#bb9af7")},
        {"cyan", QColor("#7dcfff")},
        {"orange", QColor("#ff9e64")},
    };
    return colors;
}

double luminance(const QColor &c) {
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}

QColor mix(const QColor &a, const QColor &b, double t) {
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

struct Palette {
    QHash<QString, QColor> colors;
    bool light = false;
};

Palette readPalette(const QString &path) {
    Palette palette;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return palette;
    const QString text = QString::fromUtf8(file.readAll());
    static const QRegularExpression colorLine(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
                                              QRegularExpression::MultilineOption);
    for (auto it = colorLine.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        palette.colors.insert(match.captured(1), QColor(match.captured(2)));
    }
    static const QRegularExpression modeLine(R"re(^\s*mode\s*=\s*["']light["'])re",
                                             QRegularExpression::MultilineOption);
    palette.light = modeLine.match(text).hasMatch();
    return palette;
}

} // namespace

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!m_hyprctl.isEmpty()) {
        m_roundingPoll.start();
        // The rounding only needs following while Folio is in use, and it's
        // read again the moment Folio is back.
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                refreshRounding();
                m_roundingPoll.start();
            } else {
                m_roundingPoll.stop();
            }
        });
    }
    reload();
}

Theme::~Theme() {
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

// Anything that isn't a whole number in a readable range is the default, so
// a typo in folio.conf can't make the window unusable.
int Theme::fontSizeFrom(const QVariant &value) {
    bool ok = false;
    const int size = value.toInt(&ok);
    return ok && size >= 8 && size <= 32 ? size : defaultFontSize;
}

int Theme::configuredFontSize() {
    return fontSizeFrom(QSettings().value("fontSize"));
}

QColor Theme::color(const QString &key) const {
    if (const auto it = m_colors.constFind(key); it != m_colors.constEnd())
        return *it;
    // Older themes may lack the derived backgrounds; fall back to the base.
    if (key.endsWith("background") && m_colors.contains("background"))
        return m_colors.value("background");
    if (key.endsWith("foreground") && m_colors.contains("foreground"))
        return m_colors.value("foreground");
    return fallbackColors().value(key, QColor("#888888"));
}

// Folder cards, tiles and panels are drawn in the lighter background. Some
// themes set it to the background itself (or leave it out), which would make
// them invisible, so then it's the background nudged toward the foreground.
QColor Theme::lighterBackground() const {
    const QColor lighter = color("lighter_background");
    const QColor bg = background();
    const double gap = qAbs(luminance(lighter) - luminance(bg));
    if (gap > 0.004)
        return lighter;
    return mix(bg, foreground(), 0.08);
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QColor Theme::accentForeground() const {
    return luminance(accent()) > .179 ? QColor("black") : QColor("white");
}

// Themes' own dim foregrounds are often too faint for small text, so derive a
// secondary color by mixing the foreground toward the background, keeping
// roughly a 4.5:1 contrast for metadata and labels.
QColor Theme::secondaryForeground() const {
    return mix(background(), foreground(), 0.68);
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const Palette palette = readPalette(m_directory + "/theme/colors.toml");
    if (palette.colors != m_colors || palette.light != m_light) {
        m_colors = palette.colors;
        m_light = palette.light;
        emit changed();
    }
}
