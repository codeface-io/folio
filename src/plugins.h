#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QProcessEnvironment>
#include <QQuickImageProvider>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QTimer;

// Plugins: folders holding a manifest.json and commands that print JSON
// (docs/plugins.md is the interface). Folio never loads a plugin's code; it
// runs its commands in a thread pool of its own, each with a time limit
// where it has one, and keeps what they print. A plugin that hangs or
// prints nonsense only goes missing, and the others carry on.
//
// What the sidebar, the views and the Share menu show comes from the
// latest output kept here, translated with the plugin's own catalog, so
// drawing them never runs a command.
class Plugins : public QObject {
    Q_OBJECT
    // Each plugin's sidebar section, in plugin id order, while it has
    // items: {id, title, rows: [row...], view: row or null}. A row is
    // {plugin, id, path, name, icon, root, folder, forgettable}; `path`
    // opens it (an item's plugin:<id>/<item> or the view's plugin:<id>),
    // `root` is where it's lit, and `folder` takes dropped files.
    Q_PROPERTY(QVariantList sections READ sections NOTIFY changed)
    // Statuses and icons for the sidebar's own rows, by their path:
    // {status, tone, icon}.
    Q_PROPERTY(QVariantMap marks READ marks NOTIFY changed)
    // Each plugin's view, by its path: {title, empty, items}. An item is
    // {path, name, icon, subtitle, details: [[label, value]...]}.
    Q_PROPERTY(QVariantMap views READ views NOTIFY changed)

public:
    struct Share {
        QString id, label, busy;
        QStringList command, under;
        bool toItems = false, files = false, single = false, detach = false;
    };
    struct Manifest {
        QString id, dir;
        QStringList required, poll;
        int every = 15;
        bool section = false, opened = false;
        QString sectionTitle;
        int limit = 6;
        bool view = false;
        QString viewTitle, viewLabel, viewIcon, viewEmpty;
        QList<Share> share;
        // The plugin's catalog for the language Folio shows.
        QHash<QString, QJsonValue> strings;
    };
    // Text a person reads is kept as the plugin printed it (a string, or a
    // template and its values) and translated when it's shown.
    struct Item {
        QString id, name, icon;
        QJsonValue subtitle;
        QJsonArray details;
        QJsonObject open;
    };
    struct Mark {
        QString path, icon, tone;
        QJsonValue status;
    };
    struct Output {
        QList<Item> items;
        QList<Mark> places;
        // Without a `share` key every entry is available.
        std::optional<QStringList> share;
    };
    // How a command went.
    struct Run {
        bool started = false, timedOut = false, tooLong = false;
        int code = -1;
        QByteArray out;
        // The last line it wrote to standard error.
        QString lastError;
        bool ok() const { return started && !timedOut && !tooLong && code == 0; }
    };
    // What a finished share entry or run command asks the window to do.
    struct Result {
        QString error, notice, copy, path;
    };
    using Done = std::function<void(const Result &)>;

    // `folders` are read in order, a plugin in a later one replacing one of
    // the same id in an earlier one; empty means the packaged plugins and
    // then the user's. `settingsFile` keeps the opened items; empty means
    // Folio's own settings.
    explicit Plugins(const QString &language, const QStringList &folders = {}, const QString &settingsFile = {},
                     QObject *parent = nullptr);
    ~Plugins() override;

    QVariantList sections() const { return m_sections; }
    QVariantMap marks() const { return m_marks; }
    QVariantMap views() const { return m_views; }

    // Reads the plugins again and polls every one, as F5 does.
    Q_INVOKABLE void refresh();
    // Folio's language changed: the plugins' text follows, without polling.
    void setLanguage(const QString &language);

    // The Share entries that apply to these paths: plain entries first,
    // then submenus, each in plugin id order. Each is {plugin, id, label},
    // and a submenu has `targets`: [{id, name}...].
    Q_INVOKABLE QVariantList shareFor(const QStringList &paths) const;
    // Runs a share entry on the paths (sent to the item `target` for a
    // submenu), or an item's run command, then hands `done` what it asks
    // for. Returns what the status bar says meanwhile.
    QString share(const QString &plugin, const QString &entry, const QStringList &paths, const QString &target,
                  const Done &done);
    QString run(const QString &itemPath, const Done &done);

    // The item a plugin:<id>/<item> path names, if its plugin lists it.
    std::optional<Item> item(const QString &itemPath) const;
    // The name of the item whose mount is `folder`, for the path bar; empty
    // when no item mounts there.
    QString nameForMount(const QString &folder) const;
    bool hasView(const QString &path) const { return m_views.contains(path); }

    // Opening an item from a section of opened rows (or its view) puts it
    // at the top of the section; ✕ takes it off. Kept per plugin in
    // Folio's settings, the 20 most recent, most recent first.
    void remember(const QString &itemPath);
    Q_INVOKABLE void forget(const QString &plugin, const QString &item);
    QStringList opened(const QString &plugin) const;

    // Building blocks, kept static for tests.
    static QStringList defaultFolders();
    static std::optional<Manifest> parseManifest(const QByteArray &json, QString *error);
    // Every usable plugin in `folders`, in id order, with its catalog for
    // `language`. What's skipped is said on standard error.
    static QList<Manifest> read(const QStringList &folders, const QString &language);
    static std::optional<Output> parseOutput(const QByteArray &json);
    // Runs a command of the plugin's, with `timeout` milliseconds (or none
    // when negative), stopping early once `stop` is set.
    static Run execute(const Manifest &plugin, const QStringList &command, const QProcessEnvironment &environment,
                       int timeout, const std::atomic<bool> *stop = nullptr);
    // A plugin's text in Folio's language: a string, or [template, values...].
    static QString text(const Manifest &plugin, const QJsonValue &value);
    // ~ and ~/… as the home folder.
    static QString expand(const QString &path);
    // A plugin:<id>/<item> path, split into the plugin and the item.
    static std::pair<QString, QString> split(const QString &itemPath);

    // Longest a poll may take, and most a command may print.
    static constexpr int pollTimeout = 10000;
    static constexpr int maxOutput = 1 << 20;

signals:
    void changed();

private:
    void reload(bool pollAll);
    void apply(const QList<Manifest> &manifests);
    void poll(const QString &plugin);
    void rebuild();
    const Manifest *manifest(const QString &plugin) const;
    bool available(const Manifest &plugin, const QString &entry) const;
    QProcessEnvironment environment(const Manifest &plugin) const;
    QString icon(const Manifest &plugin, const QString &name) const;
    QVariantMap itemMap(const Manifest &plugin, const Item &item) const;
    // Runs an action, one at a time per `key`; later ones wait their turn.
    void act(const Manifest &plugin, const QString &key, const QString &label, const QStringList &command,
             const QProcessEnvironment &environment, bool detach, const Done &done);
    void next(const QString &key);
    // Hands `done` its result later, never inside the call that started it.
    void finish(const Done &done, const Result &result);
    static Result interpret(const Manifest &plugin, const QString &label, const Run &run);
    void writeOpened(const QString &plugin, const QStringList &ids);
    void migrate();

    QString m_language;
    QStringList m_folders;
    QString m_settingsFile;
    QList<Manifest> m_manifests;
    // The latest output of each plugin whose poll works.
    QHash<QString, Output> m_outputs;
    QSet<QString> m_polling;
    QSet<QString> m_failing;
    QHash<QString, QTimer *> m_timers;
    QHash<QString, QList<std::function<void()>>> m_queue;
    QVariantList m_sections;
    QVariantMap m_marks, m_views;
    // Commands wait for their process in here, never on the UI thread.
    QThreadPool m_pool;
    std::shared_ptr<std::atomic<bool>> m_stop = std::make_shared<std::atomic<bool>>(false);
    quint64 m_reading = 0;
    // The next reading polls every plugin, not just new ones (F5).
    bool m_pollAll = false;
};

// A plugin's SVG icon in one color, drawn by its shape whatever colors it
// was drawn in, so it matches the glyphs around it:
// image://plugin-icon/<rrggbb>/<encoded path>.
class PluginIconProvider : public QQuickImageProvider {
public:
    PluginIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
