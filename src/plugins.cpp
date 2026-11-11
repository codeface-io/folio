#include "plugins.h"
#include "mounter.h"
#include "translator.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>
#include <algorithm>
#include <cstdio>
#include <signal.h>
#include <unistd.h>

namespace {

// What's wrong with a plugin goes to standard error (the journal), where
// whoever writes one looks; the window says nothing about it.
void complain(const QString &plugin, const QString &what) {
    fprintf(stderr, "folio: plugin %s: %s\n", qPrintable(plugin), qPrintable(what));
}

// A list of strings, where a missing one is an empty list.
std::optional<QStringList> stringList(const QJsonValue &value) {
    if (value.isUndefined())
        return QStringList();
    if (!value.isArray())
        return {};
    QStringList list;
    for (const QJsonValue &v : value.toArray()) {
        if (!v.isString() || v.toString().isEmpty())
            return {};
        list << v.toString();
    }
    return list;
}

// A command is the program and its arguments, so at least the program.
std::optional<QStringList> commandOf(const QJsonValue &value) {
    const auto list = value.isUndefined() ? std::nullopt : stringList(value);
    if (!list || list->isEmpty())
        return {};
    return list;
}

QString pathOf(const QString &plugin, const QString &item) {
    return "plugin:" + plugin + "/" + item;
}

// Why a command didn't give anything usable, for standard error.
QString whyNot(const Plugins::Run &run) {
    if (!run.started)
        return "couldn't be started";
    if (run.timedOut)
        return "took too long and was ended";
    if (run.tooLong)
        return QString("printed more than %1 bytes").arg(Plugins::maxOutput);
    if (run.code < 0)
        return "crashed";
    if (run.code > 0)
        return QString("exited with %1").arg(run.code);
    return "printed something that isn't a JSON object";
}

} // namespace

Plugins::Plugins(const QString &language, const QStringList &folders, const QString &settingsFile, QObject *parent)
    : QObject(parent), m_language(language), m_folders(folders.isEmpty() ? defaultFolders() : folders),
      m_settingsFile(settingsFile.isEmpty() ? QSettings().fileName() : settingsFile) {
    // Actions wait for their command in here for as long as it takes
    // (sending a big file), so there are threads enough that polls never
    // wait behind them.
    m_pool.setMaxThreadCount(32);
    migrate();
    reload(true);
}

Plugins::~Plugins() {
    // Closing Folio ends every command still running, with all it started.
    *m_stop = true;
    m_pool.waitForDone();
}

QStringList Plugins::defaultFolders() {
    // Run from a checkout's build folder, the checkout's own plugins are the
    // packaged ones, so what bin/build makes works without installing.
    QString packaged = qEnvironmentVariable("FOLIO_PLUGINS_PATH");
    const QString checkout = QCoreApplication::applicationDirPath() + "/..";
    if (packaged.isEmpty())
        packaged = QFileInfo::exists(checkout + "/folio.pro") ? QDir::cleanPath(checkout + "/plugins")
                                                              : QStringLiteral("/usr/share/folio/plugins");
    return {packaged, QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/folio/plugins"};
}

std::optional<Plugins::Manifest> Plugins::parseManifest(const QByteArray &json, QString *error) {
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return std::optional<Manifest>();
    };
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse);
    if (!document.isObject())
        return fail(parse.error != QJsonParseError::NoError ? "manifest.json isn't JSON: " + parse.errorString()
                                                            : "manifest.json isn't a JSON object");
    const QJsonObject root = document.object();
    if (!root.contains("folio"))
        return fail("manifest.json has no \"folio\" version");
    if (root.value("folio").toInt(-1) != 1)
        return fail("written for version " + root.value("folio").toVariant().toString()
                    + " of the plugin interface, and Folio reads version 1");

    Manifest m;
    const auto required = stringList(root.value("requires"));
    if (!required)
        return fail("\"requires\" isn't a list of programs");
    m.required = *required;

    if (root.contains("poll")) {
        const QJsonObject poll = root.value("poll").toObject();
        const auto command = commandOf(poll.value("command"));
        if (!command)
            return fail("\"poll\" has no command");
        m.poll = *command;
        m.every = std::max(5, poll.value("every").toInt(15));
    }

    if (root.contains("section")) {
        const QJsonObject section = root.value("section").toObject();
        m.section = true;
        m.sectionTitle = section.value("title").toString();
        if (m.sectionTitle.isEmpty())
            return fail("the section has no title");
        const QString rows = section.value("rows").toString("all");
        if (rows != "all" && rows != "opened")
            return fail("a section's rows are \"all\" or \"opened\"");
        m.opened = rows == "opened";
        m.limit = std::max(1, section.value("limit").toInt(6));
    }

    if (root.contains("view")) {
        const QJsonObject view = root.value("view").toObject();
        m.view = true;
        m.viewTitle = view.value("title").toString();
        if (m.viewTitle.isEmpty())
            return fail("the view has no title");
        m.viewLabel = view.value("label").toString(m.viewTitle);
        m.viewIcon = view.value("icon").toString("file");
        m.viewEmpty = view.value("empty").toString();
    }
    // The rest of the items are found in the view.
    if (m.opened && !m.view)
        return fail("a section of opened rows needs a view");

    if (root.contains("share") && !root.value("share").isArray())
        return fail("\"share\" isn't a list of entries");
    for (const QJsonValue &value : root.value("share").toArray()) {
        const QJsonObject entry = value.toObject();
        Share share;
        share.id = entry.value("id").toString();
        share.label = entry.value("label").toString();
        const auto command = commandOf(entry.value("command"));
        const auto under = stringList(entry.value("under"));
        if (share.id.isEmpty() || share.label.isEmpty() || !command || !under)
            return fail("a share entry needs an id, a label and a command");
        if (std::any_of(m.share.begin(), m.share.end(), [&](const Share &s) { return s.id == share.id; }))
            return fail("two share entries are called " + share.id);
        share.command = *command;
        share.under = *under;
        share.toItems = entry.value("to").toString() == "items";
        share.files = entry.value("files").toBool();
        share.single = entry.value("single").toBool();
        share.detach = entry.value("detach").toBool();
        share.busy = entry.value("busy").toString();
        m.share << share;
    }
    return m;
}

QList<Plugins::Manifest> Plugins::read(const QStringList &folders, const QString &language) {
    // Each id's folder, a later one replacing an earlier one, and in id
    // order, which is the order sections, marks and entries go in.
    static const QRegularExpression validId("^[a-z0-9][a-z0-9-]*$");
    QMap<QString, QString> dirs;
    for (const QString &folder : folders) {
        for (const QString &id : QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (validId.match(id).hasMatch())
                dirs.insert(id, QDir(folder).filePath(id));
            else
                complain(id, "isn't read: an id is lowercase letters, digits and dashes");
        }
    }

    QList<Manifest> manifests;
    for (auto it = dirs.cbegin(); it != dirs.cend(); ++it) {
        QFile file(it.value() + "/manifest.json");
        if (!file.open(QIODevice::ReadOnly)) {
            complain(it.key(), "has no manifest.json in " + it.value());
            continue;
        }
        QString error;
        auto manifest = parseManifest(file.readAll(), &error);
        if (!manifest) {
            complain(it.key(), error);
            continue;
        }
        const auto missing = std::find_if(manifest->required.cbegin(), manifest->required.cend(),
                                          [](const QString &p) { return QStandardPaths::findExecutable(p).isEmpty(); });
        if (missing != manifest->required.cend()) {
            complain(it.key(), "is off: it needs " + *missing + ", which isn't on PATH");
            continue;
        }
        manifest->id = it.key();
        manifest->dir = it.value();
        manifest->strings = Translator::catalog(it.value() + "/i18n", language);
        manifests << *manifest;
    }
    return manifests;
}

std::optional<Plugins::Output> Plugins::parseOutput(const QByteArray &json) {
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject())
        return {};
    const QJsonObject root = document.object();
    Output output;
    QSet<QString> seen;
    for (const QJsonValue &value : root.value("items").toArray()) {
        const QJsonObject o = value.toObject();
        Item item{o.value("id").toString(), o.value("name").toString(), o.value("icon").toString("file"),
                  o.value("subtitle"), o.value("details").toArray(), o.value("open").toObject()};
        // The id is what Folio remembers an item by, so it has to be one.
        if (item.id.isEmpty() || item.name.isEmpty() || seen.contains(item.id))
            continue;
        seen.insert(item.id);
        output.items << item;
    }
    for (const QJsonValue &value : root.value("places").toArray()) {
        const QJsonObject o = value.toObject();
        Mark mark{o.value("path").toString(), o.value("icon").toString(), o.value("tone").toString("quiet"),
                  o.value("status")};
        if (!mark.path.isEmpty())
            output.places << mark;
    }
    if (root.contains("share")) {
        QStringList ids;
        for (const QJsonValue &value : root.value("share").toArray())
            ids << value.toString();
        output.share = ids;
    }
    return output;
}

Plugins::Run Plugins::execute(const Manifest &plugin, const QStringList &command, const QProcessEnvironment &environment,
                              int timeout, const std::atomic<bool> *stop) {
    Run run;
    // ./x and bin/x are the plugin's own; a bare name comes from PATH.
    QString program = command.value(0);
    if (program.contains('/') && !QDir::isAbsolutePath(program))
        program = QDir(plugin.dir).filePath(program);
    QProcess process;
    process.setProgram(program);
    process.setArguments(command.mid(1));
    process.setWorkingDirectory(plugin.dir);
    process.setProcessEnvironment(environment);
    process.setStandardInputFile(QProcess::nullDevice());
    // A group of its own, so ending it ends everything it started too.
    process.setChildProcessModifier([] { ::setpgid(0, 0); });
    process.start();
    if (!process.waitForStarted(5000)) {
        complain(plugin.id, program + " couldn't be started: " + process.errorString());
        return run;
    }
    run.started = true;
    const qint64 group = process.processId();

    // Standard error goes to Folio's, a line at a time, marked with the
    // plugin's id; its last line can say why a share entry failed.
    QByteArray errors;
    auto readErrors = [&](bool all) {
        errors += process.readAllStandardError();
        qsizetype end;
        while ((end = errors.indexOf('\n')) >= 0 || (all && !errors.isEmpty())) {
            if (end < 0)
                end = errors.size();
            const QString line = QString::fromUtf8(errors.left(end)).trimmed();
            errors.remove(0, end + 1);
            if (!line.isEmpty()) {
                complain(plugin.id, line);
                run.lastError = line;
            }
        }
    };

    QElapsedTimer clock;
    clock.start();
    while (!process.waitForFinished(50) && process.state() != QProcess::NotRunning) {
        run.out += process.readAllStandardOutput();
        readErrors(false);
        run.timedOut = timeout >= 0 && clock.elapsed() > timeout;
        run.tooLong = run.out.size() > maxOutput;
        if (run.timedOut || run.tooLong || (stop && *stop)) {
            ::kill(-pid_t(group), SIGKILL);
            process.waitForFinished(1000);
            return run;
        }
    }
    run.out += process.readAllStandardOutput();
    readErrors(true);
    run.tooLong = run.out.size() > maxOutput;
    run.code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return run;
}

QString Plugins::text(const Manifest &plugin, const QJsonValue &value) {
    auto translated = [&](const QString &english) {
        const QJsonValue t = plugin.strings.value(english);
        return t.isString() ? t.toString() : english;
    };
    if (value.isString())
        return translated(value.toString());
    const QJsonArray parts = value.toArray();
    if (parts.isEmpty() || !parts.first().isString())
        return {};
    // Filled in one pass, so a value holding "%1" stays as it is.
    const QString pattern = translated(parts.first().toString());
    static const QRegularExpression place("%(\\d+)");
    QString filled;
    qsizetype at = 0;
    for (auto it = place.globalMatch(pattern); it.hasNext();) {
        const auto match = it.next();
        const int n = match.captured(1).toInt();
        filled += pattern.mid(at, match.capturedStart() - at);
        filled += n >= 1 && n < parts.size() ? parts.at(n).toVariant().toString() : match.captured();
        at = match.capturedEnd();
    }
    return filled + pattern.mid(at);
}

QString Plugins::expand(const QString &path) {
    if (path == "~")
        return QDir::homePath();
    if (path.startsWith("~/"))
        return QDir::homePath() + path.mid(1);
    return path;
}

std::pair<QString, QString> Plugins::split(const QString &itemPath) {
    if (!itemPath.startsWith("plugin:"))
        return {};
    const QString rest = itemPath.mid(7);
    const qsizetype slash = rest.indexOf('/');
    return slash < 0 ? std::pair{rest, QString()} : std::pair{rest.left(slash), rest.mid(slash + 1)};
}

void Plugins::refresh() {
    reload(true);
}

void Plugins::setLanguage(const QString &language) {
    if (language == m_language)
        return;
    m_language = language;
    // The catalogs are read again; the text kept from the last polls is
    // translated anew, with nothing run.
    reload(false);
}

void Plugins::reload(bool pollAll) {
    m_pollAll = m_pollAll || pollAll;
    const quint64 reading = ++m_reading;
    auto *watcher = new QFutureWatcher<QList<Manifest>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, reading] {
        watcher->deleteLater();
        // A newer reading (a second F5, or another language) wins.
        if (reading == m_reading)
            apply(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, [folders = m_folders, language = m_language] {
        return read(folders, language);
    }));
}

void Plugins::apply(const QList<Manifest> &manifests) {
    QSet<QString> before;
    for (const Manifest &m : std::as_const(m_manifests))
        before.insert(m.id);
    m_manifests = manifests;

    qDeleteAll(m_timers);
    m_timers.clear();
    QSet<QString> polled;
    for (const Manifest &m : std::as_const(m_manifests)) {
        if (m.poll.isEmpty())
            continue;
        polled.insert(m.id);
        auto *timer = new QTimer(this);
        timer->setInterval(m.every * 1000);
        connect(timer, &QTimer::timeout, this, [this, id = m.id] { poll(id); });
        timer->start();
        m_timers.insert(m.id, timer);
    }
    // What plugins that went away printed goes with them.
    m_outputs.removeIf([&](const auto &it) { return !polled.contains(it.key()); });
    for (const QString &id : polled)
        if (m_pollAll || !before.contains(id))
            poll(id);
    m_pollAll = false;
    rebuild();
}

const Plugins::Manifest *Plugins::manifest(const QString &plugin) const {
    for (const Manifest &m : m_manifests)
        if (m.id == plugin)
            return &m;
    return nullptr;
}

QProcessEnvironment Plugins::environment(const Manifest &plugin) const {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("FOLIO_PLUGIN_DIR", plugin.dir);
    environment.insert("FOLIO_LANGUAGE", m_language);
    return environment;
}

void Plugins::poll(const QString &plugin) {
    const Manifest *m = manifest(plugin);
    // One at a time: a poll that's due while the last still runs is skipped.
    if (!m || m->poll.isEmpty() || m_polling.contains(plugin))
        return;
    m_polling.insert(plugin);
    auto *watcher = new QFutureWatcher<Run>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, plugin] {
        watcher->deleteLater();
        m_polling.remove(plugin);
        if (!manifest(plugin))
            return;
        const Run run = watcher->result();
        const auto output = run.ok() ? parseOutput(run.out) : std::nullopt;
        // A failed poll counts as an empty answer, and says why once each
        // time the plugin goes from working to failing.
        if (output) {
            m_failing.remove(plugin);
            m_outputs.insert(plugin, *output);
        } else {
            if (!m_failing.contains(plugin))
                complain(plugin, "its poll " + whyNot(run));
            m_failing.insert(plugin);
            m_outputs.remove(plugin);
        }
        rebuild();
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, [plugin = *m, environment = environment(*m), stop = m_stop] {
        return execute(plugin, plugin.poll, environment, pollTimeout, stop.get());
    }));
}

QString Plugins::icon(const Manifest &plugin, const QString &name) const {
    if (!name.endsWith(".svg"))
        return name.isEmpty() ? QStringLiteral("file") : name;
    // Only a file inside the plugin's own folder.
    const QString path = QDir::cleanPath(QDir(plugin.dir).filePath(name));
    return path.startsWith(plugin.dir + "/") && QFileInfo::exists(path) ? path : QStringLiteral("file");
}

QVariantMap Plugins::itemMap(const Manifest &plugin, const Item &item) const {
    QVariantList details;
    for (const QJsonValue &row : item.details) {
        const QJsonArray pair = row.toArray();
        if (pair.size() == 2)
            details << QVariant(QVariantList{text(plugin, pair.at(0)), pair.at(1).toVariant().toString()});
    }
    // Where it's lit in the sidebar: inside the folder it opens, or inside
    // the mount, wherever GVfs puts it.
    const QString folder = item.open.contains("path") ? QDir::cleanPath(expand(item.open.value("path").toString()))
                                                      : QString();
    const QString root = item.open.contains("mount") ? Mounter::folderFor(item.open.value("mount").toString()) : folder;
    return {{"plugin", plugin.id}, {"id", item.id}, {"path", pathOf(plugin.id, item.id)}, {"name", item.name},
            {"icon", icon(plugin, item.icon)}, {"subtitle", item.subtitle.isUndefined() ? QString() : text(plugin, item.subtitle)},
            {"details", details}, {"root", root}, {"folder", folder}, {"forgettable", plugin.opened}};
}

void Plugins::rebuild() {
    QVariantList sections;
    QVariantMap marks, views;
    for (const Manifest &m : std::as_const(m_manifests)) {
        const Output output = m_outputs.value(m.id);
        QVariantList items;
        for (const Item &item : output.items)
            items << itemMap(m, item);

        if (m.view)
            views.insert("plugin:" + m.id, QVariantMap{{"title", text(m, m.viewTitle)},
                                                       {"empty", m.viewEmpty.isEmpty() ? QString() : text(m, m.viewEmpty)},
                                                       {"items", items}});

        if (m.section && !items.isEmpty()) {
            QVariantList rows = items;
            if (m.opened) {
                rows.clear();
                for (const QString &id : opened(m.id)) {
                    for (const QVariant &item : std::as_const(items))
                        if (item.toMap().value("id") == id)
                            rows << item;
                    if (rows.size() == m.limit)
                        break;
                }
            }
            QVariant view;
            if (m.view)
                view = QVariantMap{{"path", "plugin:" + m.id}, {"root", "plugin:" + m.id}, {"name", text(m, m.viewLabel)},
                                   {"icon", icon(m, m.viewIcon)}, {"count", items.size()}};
            sections << QVariantMap{{"id", m.id}, {"title", text(m, m.sectionTitle)}, {"rows", rows}, {"view", view}};
        }

        // Where two plugins mark one row, the first by id is shown.
        for (const Mark &mark : output.places) {
            const QString path = QDir::cleanPath(expand(mark.path));
            if (!marks.contains(path))
                marks.insert(path, QVariantMap{{"status", mark.status.isUndefined() ? QString() : text(m, mark.status)},
                                               {"tone", mark.tone},
                                               {"icon", mark.icon.isEmpty() ? QString() : icon(m, mark.icon)}});
        }
    }
    if (sections == m_sections && marks == m_marks && views == m_views)
        return;
    m_sections = sections;
    m_marks = marks;
    m_views = views;
    emit changed();
}

std::optional<Plugins::Item> Plugins::item(const QString &itemPath) const {
    const auto [plugin, id] = split(itemPath);
    for (const Item &item : m_outputs.value(plugin).items)
        if (item.id == id)
            return item;
    return {};
}

QString Plugins::nameForMount(const QString &folder) const {
    for (const Output &output : m_outputs)
        for (const Item &item : output.items)
            if (item.open.contains("mount") && Mounter::folderFor(item.open.value("mount").toString()) == folder)
                return item.name;
    return {};
}

bool Plugins::available(const Manifest &plugin, const QString &entry) const {
    // A plugin with a poll offers its entries only while the poll works,
    // and then only those it names, if it names any.
    if (plugin.poll.isEmpty())
        return true;
    if (!m_outputs.contains(plugin.id))
        return false;
    const auto &share = m_outputs.value(plugin.id).share;
    return !share || share->contains(entry);
}

QVariantList Plugins::shareFor(const QStringList &paths) const {
    if (paths.isEmpty())
        return {};
    const bool folders = std::any_of(paths.begin(), paths.end(), [](const QString &p) { return QFileInfo(p).isDir(); });
    auto inside = [&](const QStringList &under) {
        return under.isEmpty() || std::all_of(paths.begin(), paths.end(), [&](const QString &path) {
            return std::any_of(under.begin(), under.end(), [&](const QString &folder) {
                return path.startsWith(QDir::cleanPath(expand(folder)) + "/");
            });
        });
    };
    QVariantList plain, menus;
    for (const Manifest &m : m_manifests) {
        for (const Share &entry : m.share) {
            if (!available(m, entry.id) || (entry.single && paths.size() > 1) || (entry.files && folders)
                || !inside(entry.under))
                continue;
            QVariantMap shown{{"plugin", m.id}, {"id", entry.id}, {"label", text(m, entry.label)}};
            if (!entry.toItems) {
                plain << shown;
                continue;
            }
            QVariantList targets;
            for (const Item &item : m_outputs.value(m.id).items)
                targets << QVariantMap{{"id", item.id}, {"name", item.name}};
            if (targets.isEmpty())
                continue;
            shown.insert("targets", targets);
            menus << shown;
        }
    }
    return plain + menus;
}

QString Plugins::share(const QString &plugin, const QString &entry, const QStringList &paths, const QString &target,
                       const Done &done) {
    const Manifest *m = manifest(plugin);
    const Share *share = nullptr;
    for (const Share &s : m ? m->share : QList<Share>())
        if (s.id == entry)
            share = &s;
    const auto chosen = item(pathOf(plugin, target));
    if (!share || paths.isEmpty() || (share->toItems && !chosen)) {
        finish(done, {});
        return {};
    }
    QProcessEnvironment environment = this->environment(*m);
    QString name = QFileInfo(paths.first()).fileName();
    if (share->toItems) {
        environment.insert("FOLIO_TARGET", chosen->id);
        environment.insert("FOLIO_TARGET_NAME", chosen->name);
        name = chosen->name;
    }
    act(*m, plugin + "/" + entry, text(*m, share->label), share->command + paths, environment, share->detach, done);
    return share->busy.isEmpty() ? QString() : text(*m, QJsonArray{share->busy, name});
}

QString Plugins::run(const QString &itemPath, const Done &done) {
    const Manifest *m = manifest(split(itemPath).first);
    const auto found = item(itemPath);
    const auto command = found ? commandOf(found->open.value("run")) : std::nullopt;
    if (!m || !command) {
        finish(done, {});
        return {};
    }
    act(*m, itemPath, found->name, *command, environment(*m), false, done);
    return {};
}

void Plugins::finish(const Done &done, const Result &result) {
    // Always later, never inside the call that started it.
    QTimer::singleShot(0, this, [done, result] { done(result); });
}

void Plugins::act(const Manifest &plugin, const QString &key, const QString &label, const QStringList &command,
                  const QProcessEnvironment &environment, bool detach, const Done &done) {
    if (detach) {
        // Started and forgotten: a program with a window of its own.
        QProcess process;
        QString program = command.first();
        if (program.contains('/') && !QDir::isAbsolutePath(program))
            program = QDir(plugin.dir).filePath(program);
        process.setProgram(program);
        process.setArguments(command.mid(1));
        process.setWorkingDirectory(plugin.dir);
        process.setProcessEnvironment(environment);
        process.setStandardInputFile(QProcess::nullDevice());
        Result result;
        if (!process.startDetached())
            result.error = tr("%1: %2").arg(label, process.errorString());
        finish(done, result);
        return;
    }
    // A second click on the same entry waits until the first is done.
    QList<std::function<void()>> &queue = m_queue[key];
    queue << [this, plugin, key, label, command, environment, done] {
        auto *watcher = new QFutureWatcher<Run>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, plugin, key, label, done] {
            watcher->deleteLater();
            done(interpret(plugin, label, watcher->result()));
            // Whatever it changed shows at once.
            poll(plugin.id);
            next(key);
        });
        // Work that was asked for takes as long as it takes.
        watcher->setFuture(QtConcurrent::run(&m_pool, [plugin, command, environment, stop = m_stop] {
            return execute(plugin, command, environment, -1, stop.get());
        }));
    };
    if (queue.size() == 1)
        queue.first()();
}

void Plugins::next(const QString &key) {
    QList<std::function<void()>> &queue = m_queue[key];
    queue.removeFirst();
    if (queue.isEmpty())
        m_queue.remove(key);
    else
        queue.first()();
}

Plugins::Result Plugins::interpret(const Manifest &plugin, const QString &label, const Run &run) {
    // Printing nothing is fine; what it prints says what to do next.
    const QJsonObject said = run.tooLong ? QJsonObject() : QJsonDocument::fromJson(run.out).object();
    Result result;
    if (said.contains("error"))
        result.error = text(plugin, said.value("error"));
    if (said.contains("notice"))
        result.notice = text(plugin, said.value("notice"));
    result.copy = said.value("copy").toString();
    result.path = said.value("path").toString();
    if (!run.ok() && result.error.isEmpty()) {
        const QString why = !run.lastError.isEmpty() ? run.lastError
                          : !run.started             ? tr("it couldn't be started")
                          : run.tooLong              ? tr("it printed too much")
                          : run.code < 0             ? tr("it crashed")
                                                     : tr("it stopped with code %1").arg(run.code);
        result.error = tr("%1: %2").arg(label, why);
    }
    if (!result.error.isEmpty())
        result.notice.clear();
    return result;
}

QStringList Plugins::opened(const QString &plugin) const {
    QSettings settings(m_settingsFile, QSettings::IniFormat);
    QStringList ids;
    const int count = settings.beginReadArray("opened/" + plugin);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        ids << settings.value("id").toString();
    }
    settings.endArray();
    return ids;
}

void Plugins::writeOpened(const QString &plugin, const QStringList &ids) {
    QSettings settings(m_settingsFile, QSettings::IniFormat);
    // Rewritten whole, so a shorter list leaves none of the old entries.
    settings.remove("opened/" + plugin);
    settings.beginWriteArray("opened/" + plugin, ids.size());
    for (int i = 0; i < ids.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("id", ids[i]);
    }
    settings.endArray();
}

void Plugins::remember(const QString &itemPath) {
    const auto [plugin, id] = split(itemPath);
    const Manifest *m = manifest(plugin);
    if (!m || !m->opened || id.isEmpty())
        return;
    QStringList ids = opened(plugin);
    ids.removeAll(id);
    ids.prepend(id);
    // Enough to bring back what went away for a while (a machine asleep),
    // without the settings growing with everything ever opened.
    writeOpened(plugin, ids.mid(0, 20));
    rebuild();
}

void Plugins::forget(const QString &plugin, const QString &item) {
    QStringList ids = opened(plugin);
    if (ids.removeAll(item) == 0)
        return;
    writeOpened(plugin, ids);
    rebuild();
}

void Plugins::migrate() {
    // Before plugins, Folio itself remembered the tailnet machines opened,
    // by the same DNS names the Tailscale plugin's items go by.
    QSettings settings(m_settingsFile, QSettings::IniFormat);
    if (!settings.contains("opened-machines/size"))
        return;
    QStringList hosts;
    const int count = settings.beginReadArray("opened-machines");
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        hosts << settings.value("host").toString();
    }
    settings.endArray();
    settings.remove("opened-machines");
    settings.sync();
    if (opened("tailscale").isEmpty())
        writeOpened("tailscale", hosts);
}

QImage PluginIconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    const QColor color("#" + id.section('/', 0, 0));
    QImageReader reader(QUrl::fromPercentEncoding(id.section('/', 1).toUtf8()));
    const QSize want = requestedSize.isEmpty() ? QSize(24, 24) : requestedSize;
    reader.setScaledSize(want);
    QImage image = reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        image = QImage(want, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
    }
    // Only the shape counts: every pixel takes the color, keeping how
    // opaque it was.
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int alpha = qAlpha(line[x]);
            line[x] = qRgba(color.red() * alpha / 255, color.green() * alpha / 255, color.blue() * alpha / 255, alpha);
        }
    }
    if (size)
        *size = image.size();
    return image;
}
