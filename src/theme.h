#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariant>

// The Omarchy palette, followed live. A file manager draws far more surface
// than a one-tool app, so it reads the whole colors.toml rather than only the
// accent, and the Material style flips with the theme's light or dark mode.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor darkBackground READ darkBackground NOTIFY changed)
    Q_PROPERTY(QColor lighterBackground READ lighterBackground NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor brightForeground READ brightForeground NOTIFY changed)
    Q_PROPERTY(QColor secondaryForeground READ secondaryForeground NOTIFY changed)
    Q_PROPERTY(QColor red READ red NOTIFY changed)
    Q_PROPERTY(QColor yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QColor blue READ blue NOTIFY changed)
    Q_PROPERTY(QColor magenta READ magenta NOTIFY changed)
    Q_PROPERTY(QColor cyan READ cyan NOTIFY changed)
    Q_PROPERTY(QColor orange READ orange NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(int fontSize READ fontSize CONSTANT)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QColor accent() const { return color("accent"); }
    QColor accentForeground() const;
    QColor background() const { return color("background"); }
    QColor darkBackground() const { return color("dark_background"); }
    QColor lighterBackground() const;
    QColor foreground() const { return color("foreground"); }
    QColor brightForeground() const { return color("bright_foreground"); }
    QColor secondaryForeground() const;
    QColor red() const { return color("red"); }
    QColor yellow() const { return color("yellow"); }
    QColor blue() const { return color("blue"); }
    QColor magenta() const { return color("magenta"); }
    QColor cyan() const { return color("cyan"); }
    QColor orange() const { return color("orange"); }
    bool light() const { return m_light; }
    int radius() const { return m_radius; }

    // The text size, in pixels, from `fontSize` in ~/.config/folio/folio.conf.
    // Read once at launch: the app font is set from it before any window
    // opens. File marks, toolbar glyphs and the rows that hold text follow;
    // the padding around them stays. Grid pictures stay on the zoom Ctrl+= sets.
    static constexpr int defaultFontSize = 13;
    static int fontSizeFrom(const QVariant &value);
    static int configuredFontSize();
    int fontSize() const { return m_fontSize; }
    // A size drawn for the default 13px text, grown or shrunk to match.
    Q_INVOKABLE int scaled(int size) const { return qRound(size * m_fontSize / double(defaultFontSize)); }

signals:
    void changed();

private:
    QColor color(const QString &key) const;
    void reload();
    void refreshRounding();

    QString m_directory;
    QHash<QString, QColor> m_colors;
    bool m_light = false;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    int m_fontSize = configuredFontSize();
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
