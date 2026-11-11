#include "fileops.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QSet>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QTimer>
#include <QtConcurrent>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

// GIO's headers use `signals` as a struct member, which Qt defines as a
// keyword.
#undef signals
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#define signals Q_SIGNALS

FileOps::FileOps(QObject *parent) : QObject(parent) {
    if (auto *clipboard = QGuiApplication::clipboard())
        connect(clipboard, &QClipboard::dataChanged, this, &FileOps::clipboardChanged);
}

QMimeData *FileOps::mimeDataFor(const QStringList &paths, bool cut) {
    auto *data = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QString &path : paths) {
        const QUrl url = QUrl::fromLocalFile(path);
        urls << url;
        gnome += '\n' + url.toEncoded();
    }
    data->setUrls(urls);
    data->setData("x-special/gnome-copied-files", gnome);
    data->setText(paths.join('\n'));
    return data;
}

QStringList FileOps::pathsFrom(const QMimeData *data, bool *cut) {
    if (cut)
        *cut = false;
    QStringList paths;
    if (!data)
        return paths;
    if (data->hasFormat("x-special/gnome-copied-files")) {
        const QList<QByteArray> lines = data->data("x-special/gnome-copied-files").split('\n');
        if (cut && !lines.isEmpty())
            *cut = lines.first().trimmed() == "cut";
        for (int i = 1; i < lines.size(); ++i) {
            const QUrl url = QUrl::fromEncoded(lines[i].trimmed());
            if (url.isLocalFile())
                paths << url.toLocalFile();
        }
        return paths;
    }
    for (const QUrl &url : data->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    return paths;
}

bool FileOps::canPaste() const {
    const auto *clipboard = QGuiApplication::clipboard();
    return clipboard && !pathsFrom(clipboard->mimeData(), nullptr).isEmpty();
}

void FileOps::copy(const QStringList &paths) {
    if (!paths.isEmpty())
        QGuiApplication::clipboard()->setMimeData(mimeDataFor(paths, false));
}

void FileOps::cut(const QStringList &paths) {
    if (!paths.isEmpty())
        QGuiApplication::clipboard()->setMimeData(mimeDataFor(paths, true));
}

void FileOps::yank(const QStringList &paths) {
    if (!paths.isEmpty())
        QGuiApplication::clipboard()->setText(paths.join('\n'));
}

int FileOps::extensionStart(const QString &name, bool isDir) {
    int dot = isDir ? -1 : name.lastIndexOf('.');
    if (dot <= 0)
        return -1;
    if (dot > 4 && name.mid(dot - 4, 4).compare(".tar", Qt::CaseInsensitive) == 0)
        dot -= 4;
    return dot;
}

QString FileOps::freeName(const QString &folder, const QString &name) {
    const QDir dir(folder);
    if (!QFileInfo::exists(dir.filePath(name)))
        return name;
    const QFileInfo info(dir.filePath(name));
    const int dot = extensionStart(name, info.isDir());
    const QString base = dot > 0 ? name.left(dot) : name;
    const QString ext = dot > 0 ? name.mid(dot) : QString();
    for (int n = 1;; ++n) {
        const QString candidate = n == 1 ? QObject::tr("%1 (copy)%2").arg(base, ext)
                                         : QObject::tr("%1 (copy %2)%3").arg(base).arg(n).arg(ext);
        if (!QFileInfo::exists(dir.filePath(candidate)))
            return candidate;
    }
}

// Whether something is at `path`, a link that leads nowhere included.
static bool taken(const QString &path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

static bool isFolder(const QString &path) {
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink();
}

static bool writeAll(int fd, const char *data, qint64 size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        size -= n;
    }
    return true;
}

// Copies a file a piece at a time, so a big one can be stopped partway (and
// is then removed), counting what's copied. copy_file_range lets the kernel
// do the copying, which on btrfs shares the blocks rather than copying them;
// what it can't take (a pipe, some drives) is read and written instead. Like
// QFile::copy, it never overwrites, and the copy gets the file's permissions.
static bool copyFile(const QString &from, const QString &to, FileOps::Job *job) {
    QFile in(from), out(to);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    bool kernel = true, ok = true;
    QByteArray buffer;
    for (;;) {
        if (job && job->cancelled) {
            ok = false;
            break;
        }
        ssize_t n;
        if (kernel) {
            n = copy_file_range(in.handle(), nullptr, out.handle(), nullptr, 8 << 20, 0);
            if (n < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) {
                kernel = false;
                continue;
            }
        } else {
            buffer.resize(1 << 20);
            n = ::read(in.handle(), buffer.data(), buffer.size());
            if (n > 0 && !writeAll(out.handle(), buffer.constData(), n))
                n = -1;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        if (job)
            job->bytes += n;
    }
    // Some drives (FAT) keep no permissions; the copy is still a copy.
    if (ok)
        out.setPermissions(in.permissions());
    out.close();
    if (!ok)
        out.remove();
    return ok;
}

bool FileOps::copyRecursively(const QString &from, const QString &to, QString *error, bool move, Job *job) {
    const QFileInfo info(from);
    if (info.isSymLink()) {
        // Links are copied as links, not followed, and as written: one
        // relative to its folder points into the copy, not at the original.
        if (!QFile::link(info.readSymLink(), to)) {
            *error = QObject::tr("Couldn't copy the link %1.").arg(info.fileName());
            return false;
        }
        if (move)
            QFile::remove(from);
        return true;
    }
    if (!info.isDir()) {
        if (!copyFile(from, to, job)) {
            // Stopped when asked: that's not a problem to report.
            if (!job || !job->cancelled)
                *error = QObject::tr("Couldn't copy %1.").arg(info.fileName());
            return false;
        }
        if (move)
            QFile::remove(from);
        return true;
    }
    // A folder that can't be read would list as empty and be copied empty.
    if (!info.isReadable()) {
        *error = QObject::tr("Couldn't read %1.").arg(info.fileName());
        return false;
    }
    // A folder keeps its permissions, as QFile::copy keeps a file's. It's
    // made with them, plus its owner's, so a private one is never open to
    // others even when the copy stops partway, and a read-only one can be
    // filled before it gets them exactly.
    if (!QDir().mkdir(to, info.permissions() | QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        *error = QObject::tr("Couldn't create %1.").arg(QFileInfo(to).fileName());
        return false;
    }
    const QDir source(from);
    const auto entries = source.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    const bool counting = job && !job->countIn.isEmpty() && job->countIn == QDir::cleanPath(from);
    if (counting)
        job->total = entries.size();
    for (const QFileInfo &entry : entries) {
        if (job && job->cancelled)
            return false;
        if (!copyRecursively(entry.filePath(), QDir(to).filePath(entry.fileName()), error, move, job))
            return false;
        if (counting)
            ++job->done;
    }
    QFile::setPermissions(to, info.permissions());
    // Only once it's empty: what turned up in it while the rest moved was
    // never copied, so it stays where it is, and the folder with it.
    if (move)
        QDir().rmdir(from);
    return true;
}

// The paths that aren't inside another of them. A folder takes what's in it
// along, so anything also chosen inside it (search results list both) would
// be copied or archived twice, be gone by its turn once the folder moved, or
// be deleted twice. Each path looks for the folders above it among the
// chosen, rather than at every other path, which for a Select All of
// thousands took minutes. A link on the way up is copied as a link, not
// gone into, so what was chosen beyond it stays chosen, and goes first:
// once the link had moved or gone to the trash, it would be out of reach.
static QStringList outermost(const QStringList &paths) {
    QSet<QString> chosen;
    for (const QString &path : paths)
        chosen.insert(QDir::cleanPath(path));
    QStringList beyondLinks, outer;
    for (const QString &path : paths) {
        const QString clean = QDir::cleanPath(path);
        bool inside = false, beyondLink = false;
        for (qsizetype slash = clean.lastIndexOf('/'); slash > 0 && !inside && !beyondLink;
             slash = clean.lastIndexOf('/', slash - 1)) {
            if (!chosen.contains(clean.left(slash)))
                continue;
            // Only now, for the few that have a chosen folder above them,
            // is each step between looked at on disk.
            for (qsizetype step = clean.lastIndexOf('/'); step >= slash && !beyondLink; step = clean.lastIndexOf('/', step - 1))
                beyondLink = QFileInfo(clean.left(step)).isSymLink();
            inside = !beyondLink;
        }
        if (beyondLink)
            beyondLinks << path;
        else if (!inside)
            outer << path;
    }
    return beyondLinks + outer;
}

// Whether `folder` is the folder at `path` or inside it, wherever links lead,
// so a link to a folder inside it counts. A link at `path` itself isn't a
// folder here, since it's copied as a link.
static bool inside(const QString &folder, const QString &path) {
    const QFileInfo item(path);
    const QString self = item.canonicalFilePath(), target = QFileInfo(folder).canonicalFilePath();
    return item.isDir() && !item.isSymLink() && (target == self || target.startsWith(self + '/'));
}

// A rename that never replaces what's there. A drive that can't promise
// that (some network ones) gets the check first, as QFile::rename does.
static bool renameNoReplace(const QString &from, const QString &to) {
    const QByteArray source = QFile::encodeName(from), target = QFile::encodeName(to);
    if (renameat2(AT_FDCWD, source.constData(), AT_FDCWD, target.constData(), RENAME_NOREPLACE) == 0)
        return true;
    if (errno != EINVAL)
        return false;
    return !taken(to) && ::rename(source.constData(), target.constData()) == 0;
}

// Puts one item at `to`. A rename is instant on the same drive; across
// drives a move is a copy that removes each original once it's copied. A
// link is made again rather than renamed, which across drives would copy
// what it leads to.
static bool put(const QString &from, const QString &to, bool move, QString *error, FileOps::Job *job) {
    const QFileInfo info(from);
    if (move && !info.isSymLink() && renameNoReplace(from, to))
        return true;
    if (!FileOps::copyRecursively(from, to, error, move, job)) {
        // Each original went as it was copied, so what arrived is now the
        // only copy of it: it's said to be only part.
        if (move && taken(to) && !error->isEmpty())
            *error = QObject::tr("Moved only part of %1: %2").arg(info.fileName(), *error);
        return false;
    }
    if (move && taken(from)) {
        *error = QObject::tr("Copied %1, but couldn't remove the original.").arg(info.fileName());
        return false;
    }
    return true;
}

QStringList FileOps::transfer(const QStringList &paths, const QString &folder, Mode mode, QString *error, Job *job) {
    QStringList made;
    const QString target = QDir::cleanPath(folder);
    const QStringList outer = outermost(paths);
    if (job)
        job->total = outer.size();
    for (const QString &path : outer) {
        if (job && job->cancelled)
            break;
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) {
            *error = QObject::tr("%1 is no longer there.").arg(info.fileName());
            break;
        }
        const bool move = mode == Move || (mode == Drop && sameDrive(path, target));
        // Moving something into the folder it's already in does nothing.
        if (move && QDir::cleanPath(info.absolutePath()) == target) {
            if (job)
                ++job->done;
            continue;
        }
        if (inside(target, path)) {
            *error = QObject::tr("Can't put %1 inside itself.").arg(info.fileName());
            break;
        }
        const QString name = freeName(target, info.fileName());
        const QString destination = QDir(target).filePath(name);
        // A single folder counts what's in it, so there's more to count.
        if (job && outer.size() == 1 && isFolder(path))
            job->countIn = QDir::cleanPath(path);
        if (!put(path, destination, move, error, job)) {
            // What arrived of a move is now the only copy of it, and what was
            // copied before a cancel stays: either is shown.
            if (taken(destination) && (move || error->isEmpty()))
                made << name;
            break;
        }
        made << name;
        if (job && job->countIn.isEmpty())
            ++job->done;
    }
    return made;
}

void FileOps::paste(const QString &folder) {
    bool cut = false;
    const QStringList paths = pathsFrom(QGuiApplication::clipboard()->mimeData(), &cut);
    if (paths.isEmpty() || !idle())
        return;
    // Only Delete puts things in the trash, with the note Restore needs.
    if (!QFileInfo(folder).isDir() || inTrash(folder)) {
        emit failed(tr("Files can't be pasted here."));
        return;
    }
    // Cut files are gone from where they were once moved; pasting them
    // again would find nothing, so the clipboard lets go of them.
    transferInBackground(paths, folder, cut ? Move : Copy, cut);
}

bool FileOps::sameDrive(const QString &path, const QString &folder) {
    // QStorageInfo follows a link, but a link is on the drive of the folder
    // it's in, wherever it leads.
    const QFileInfo item(path);
    return QStorageInfo(item.isSymLink() ? item.absolutePath() : path).device() == QStorageInfo(folder).device();
}

bool FileOps::canDrop(const QList<QUrl> &urls, const QString &folder) const {
    if (!QFileInfo(folder).isDir() || inTrash(folder))
        return false;
    const QString target = QDir::cleanPath(folder);
    bool any = false;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        // Not onto itself or inside itself.
        if (path == target || inside(target, path))
            return false;
        // Already there: nothing would happen.
        if (QFileInfo(path).absolutePath() != target)
            any = true;
    }
    return any;
}

void FileOps::dropInto(const QList<QUrl> &urls, const QString &folder) {
    if (!canDrop(urls, folder) || !idle())
        return;
    transferInBackground(localPaths(urls), folder, Drop, false);
}

QString FileOps::uriList(const QStringList &paths) const {
    QStringList uris;
    for (const QString &path : paths)
        uris << QString::fromLatin1(QUrl::fromLocalFile(path).toEncoded());
    return uris.join("\r\n");
}

QStringList FileOps::localPaths(const QList<QUrl> &urls) const {
    QStringList paths;
    for (const QUrl &url : urls)
        if (url.isLocalFile())
            paths << url.toLocalFile();
    return paths;
}

template <typename Work, typename Done>
void FileOps::inBackground(const std::function<QString(const Job &)> &label, Work work, Done done) {
    auto job = std::make_shared<Job>();
    m_job = job;
    setBusy(label(*job));
    auto *watcher = new QFutureWatcher<std::invoke_result_t<Work, Job &>>(this);
    // How far it's got, a few times a second.
    auto *timer = new QTimer(watcher);
    connect(timer, &QTimer::timeout, this, [this, job, label] { setBusy(label(*job)); });
    timer->start(200);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, timer, job, done = std::move(done)] {
        timer->stop();
        watcher->deleteLater();
        m_job.reset();
        setBusy({});
        done(watcher->result(), *job);
    });
    watcher->setFuture(QtConcurrent::run([job, work = std::move(work)]() mutable { return work(*job); }));
}

void FileOps::cancel() {
    if (m_job)
        m_job->cancelled = true;
    if (m_process) {
        m_killed = true;
        m_process->kill();
    }
}

// "Restoring 3 of 120 items…" once there are several, `one` before that.
static QString counting(const FileOps::Job &job, const QString &several, const QString &one) {
    return job.total > 1 ? several.arg(qMin(job.done + 1, int(job.total))) : one;
}

void FileOps::sayStopped(const Job &job) {
    emit notice(tr("Stopped after %1 of %n items.", "", job.total).arg(job.done.load()));
}

namespace {
// What work hands back from the worker: what it made, and what went wrong.
struct Outcome {
    QStringList made;
    QString error;
    QStringList failures;
};
}

void FileOps::transferInBackground(const QStringList &paths, const QString &folder, Mode mode, bool releaseClipboard,
                                   bool duplicating) {
    // A drop says what its first item does; each is moved or copied by its
    // own drive.
    const bool moving = mode == Move || (mode == Drop && sameDrive(paths.first(), folder));
    const QFileInfo first(paths.first());
    // A single file shows how much of it is copied, its size known now.
    const qint64 size = paths.size() == 1 && first.isFile() && !first.isSymLink() ? first.size() : 0;
    const QString name = first.fileName();
    const int count = paths.size();
    inBackground(
        [=](const Job &job) {
            const auto bytes = [](qint64 n) { return QLocale().formattedDataSize(n, 1, QLocale::DataSizeSIFormat); };
            const QString one = size > 0 && job.bytes > 0
                                    ? (duplicating ? tr("Duplicating %1: %2 of %3…").arg(name, bytes(job.bytes), bytes(size))
                                       : moving    ? tr("Moving %1: %2 of %3…").arg(name, bytes(job.bytes), bytes(size))
                                                   : tr("Copying %1: %2 of %3…").arg(name, bytes(job.bytes), bytes(size)))
                                : duplicating ? tr("Duplicating %1…").arg(name)
                                : moving      ? tr("Moving %n items…", "", count)
                                              : tr("Copying %n items…", "", count);
            return moving ? counting(job, tr("Moving %1 of %n items…", "", job.total), one)
                          : counting(job, tr("Copying %1 of %n items…", "", job.total), one);
        },
        [paths, folder, mode](Job &job) {
            Outcome result;
            result.made = transfer(paths, folder, mode, &result.error, &job);
            return result;
        },
        [this, paths, folder, releaseClipboard](const Outcome &result, const Job &job) {
            // Only the cut that was pasted is let go of; something copied
            // while it moved stays on the clipboard.
            bool cut = false;
            if (releaseClipboard && result.error.isEmpty() && !job.cancelled
                && pathsFrom(QGuiApplication::clipboard()->mimeData(), &cut) == paths && cut)
                QGuiApplication::clipboard()->clear();
            if (!result.error.isEmpty())
                emit failed(result.error);
            else if (job.cancelled)
                sayStopped(job);
            if (!result.made.isEmpty())
                emit done(folder, result.made.first());
        });
}

void FileOps::duplicate(const QString &path) {
    if (!idle())
        return;
    const QFileInfo info(path);
    transferInBackground({path}, info.absolutePath(), Copy, false, true);
}

QString FileOps::archiveName(const QStringList &paths, const QString &folder) {
    const QString base = paths.size() == 1 ? QFileInfo(paths.first()).fileName() : tr("Archive");
    return freeName(folder, base + ".zip");
}

void FileOps::compress(const QStringList &selected) {
    const QStringList paths = outermost(selected);
    if (paths.isEmpty() || !idle())
        return;
    // Search results and Recents can span folders, so the archive goes in
    // the deepest folder holding them all, with their paths from there.
    const auto holds = [](const QString &folder, const QString &path) {
        const QString relative = QDir(folder).relativeFilePath(QFileInfo(path).absolutePath());
        return relative != ".." && !relative.startsWith("../");
    };
    QString folder = QFileInfo(paths.first()).absolutePath();
    for (const QString &path : paths)
        while (!holds(folder, path))
            folder = QFileInfo(folder).absolutePath();
    // bsdtar writes into a hidden folder of its own beside the archive, so a
    // half-written archive stays out of sight, and another compression into
    // this folder (from another window, or another Folio) can't write into
    // the same file or remove it when that one fails. The folder goes away
    // with what's left in it once bsdtar is done.
    auto temporary = std::make_shared<QTemporaryDir>(QDir(folder).filePath(".compressing-XXXXXX"));
    if (!temporary->isValid()) {
        emit failed(tr("Couldn't compress: %1").arg(temporary->errorString()));
        return;
    }
    const QString archive = archiveName(paths, folder);
    // -v names each item as bsdtar starts on it, which counts them.
    QStringList args{"-a", "-v", "-cf", temporary->filePath(archive), "-C", folder, "--"};
    QStringList items;
    for (const QString &path : paths)
        items << QDir(folder).relativeFilePath(path);
    // What bsdtar says that isn't an item is a problem, kept for saying why
    // it failed.
    auto errors = std::make_shared<QString>();
    const auto read = [this, items, archive, errors](QProcess *process) {
        while (process->canReadLine()) {
            const QString line = QString::fromUtf8(process->readLine()).chopped(1);
            if (!line.startsWith("a "))
                *errors += line + '\n';
            else if (const qsizetype at = items.indexOf(line.mid(2)); at >= 0 && items.size() > 1)
                setBusy(tr("Compressing %1 of %n items into %2…", "", items.size()).arg(at + 1).arg(archive));
        }
    };
    QProcess *process = runTool(
        "bsdtar", args + items, tr("Compressing into %1…").arg(archive),
        tr("Compressing needs bsdtar, from the libarchive package."), tr("Stopped compressing; no archive was made."),
        [this, paths, folder, temporary, archive, errors, read](QProcess *process, int code) {
            if (code != 0) {
                read(process);
                emit failed(tr("Couldn't compress: %1").arg((*errors + QString::fromUtf8(process->readAll())).trimmed()));
                return;
            }
            // Only a finished archive is named, with a name that's free
            // now. A rename never overwrites, so if another took that
            // name first, it takes the next.
            QFile made(temporary->filePath(archive));
            QString name = archiveName(paths, folder);
            while (!made.rename(QDir(folder).filePath(name))) {
                if (!QFileInfo::exists(QDir(folder).filePath(name))) {
                    emit failed(tr("Couldn't compress: %1").arg(made.errorString()));
                    return;
                }
                name = archiveName(paths, folder);
            }
            emit done(folder, name);
        });
    if (!process)
        return;
    process->setReadChannel(QProcess::StandardError);
    connect(process, &QProcess::readyReadStandardError, this, [process, read] { read(process); });
}

void FileOps::shareLocalSend(const QStringList &paths) {
    // The same command Omarchy's Nautilus extension runs.
    if (paths.isEmpty())
        return;
    if (!QProcess::startDetached("localsend", QStringList{"--headless", "send"} + paths))
        emit failed(tr("Sharing nearby needs LocalSend, which isn't installed."));
}

bool FileOps::hasLocalSend() const {
    return !QStandardPaths::findExecutable("localsend").isEmpty();
}

void FileOps::dropboxLink(const QString &path) {
    const QString name = QFileInfo(path).fileName();
    runTool("dropbox-cli", {"sharelink", path}, tr("Getting a Dropbox link for %1…").arg(name),
            tr("Dropbox links need dropbox-cli, which isn't installed."), tr("Stopped getting the Dropbox link."),
            [this, name](QProcess *process, int code) {
                const QString out = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
                // dropbox-cli prints the link, or a sentence saying why not.
                const QString link = out.section('\n', -1).trimmed();
                if (code != 0 || !link.startsWith("https://")) {
                    const QString why = out.isEmpty() ? QString::fromUtf8(process->readAllStandardError()).trimmed() : out;
                    emit failed(tr("Couldn't get a Dropbox link for %1: %2").arg(name, why));
                    return;
                }
                QGuiApplication::clipboard()->setText(link);
                emit notice(tr("Dropbox link for %1 copied.").arg(name));
            });
}

void FileOps::shareTaildrop(const QStringList &paths, const QString &host) {
    if (paths.isEmpty())
        return;
    const QString machine = host.section('.', 0, 0);
    runTool("tailscale", QStringList{"file", "cp"} + paths + QStringList{host + ":"}, tr("Sending to %1…").arg(machine),
            tr("Sending with Taildrop needs tailscale, which isn't installed."), tr("Stopped sending to %1.").arg(machine),
            [this, machine](QProcess *process, int code) {
                if (code != 0)
                    emit failed(tr("Couldn't send to %1: %2").arg(machine, QString::fromUtf8(process->readAllStandardError()).trimmed()));
            });
}

void FileOps::trash(const QStringList &paths) {
    if (paths.isEmpty() || !idle())
        return;
    const int count = paths.size();
    inBackground(
        [count](const Job &job) {
            return counting(job, tr("Moving %1 of %n items to the trash…", "", job.total),
                            tr("Moving %n items to the trash…", "", count));
        },
        [paths](Job &job) {
            Outcome result;
            const QStringList outer = outermost(paths);
            job.total = outer.size();
            for (const QString &path : outer) {
                if (job.cancelled)
                    break;
                GFile *file = g_file_new_for_path(path.toUtf8().constData());
                GError *error = nullptr;
                if (!g_file_trash(file, nullptr, &error)) {
                    result.failures << QFileInfo(path).fileName() + (error ? QStringLiteral(" (") + QString::fromUtf8(error->message) + ")" : QString());
                    if (error)
                        g_error_free(error);
                }
                g_object_unref(file);
                ++job.done;
            }
            return result;
        },
        [this](const Outcome &result, const Job &job) {
            if (!result.failures.isEmpty())
                emit failed(tr("Couldn't move to the trash: %1").arg(result.failures.join(", ")));
            else if (job.cancelled)
                sayStopped(job);
        });
}

QString FileOps::homeTrash() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash";
}

QStringList FileOps::trashDirs() {
    QStringList dirs{homeTrash()};
    // Drives keep their own trash at the top, so deleting never copies
    // across disks: .Trash-<uid> (or .Trash/<uid>).
    const QString uid = QString::number(getuid());
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        for (const QString &dir : {volume.rootPath() + "/.Trash-" + uid, volume.rootPath() + "/.Trash/" + uid})
            if (QFileInfo(dir + "/files").isDir() && !dirs.contains(dir))
                dirs << dir;
    }
    return dirs;
}

bool FileOps::inTrash(const QString &path) const {
    const QString clean = QDir::cleanPath(path);
    for (const QString &dir : trashDirs())
        if (clean == dir + "/files" || clean.startsWith(dir + "/files/"))
            return true;
    return false;
}

QString FileOps::infoFileFor(const QString &trashedPath) {
    // Only what's directly in a trash's files folder was trashed on its own,
    // with a note. What's inside a trashed folder came along with it and has
    // none; looking one level up would find a file in a trashed folder that
    // happens to be called "info".
    const QFileInfo item(trashedPath);
    const QString folder = QDir::cleanPath(item.absolutePath());
    for (const QString &dir : trashDirs())
        if (folder == QDir::cleanPath(dir + "/files"))
            return dir + "/info/" + item.fileName() + ".trashinfo";
    return {};
}

QString FileOps::originalPath(const QString &trashedPath) {
    QFile info(infoFileFor(trashedPath));
    if (info.fileName().isEmpty() || !info.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : info.readAll().split('\n')) {
        if (!line.startsWith("Path="))
            continue;
        const QString path = QUrl::fromPercentEncoding(line.mid(5).trimmed());
        if (path.startsWith('/'))
            return path;
        // A drive's own trash records paths from the top of that drive.
        const QString trashDir = QFileInfo(QFileInfo(trashedPath).absolutePath()).absolutePath();
        const QString top = trashDir.contains("/.Trash/") ? trashDir.section("/.Trash/", 0, 0)
                                                          : QFileInfo(trashDir).absolutePath();
        return QDir(top).filePath(path);
    }
    return {};
}

QDateTime FileOps::deletedAt(const QString &trashedPath) {
    QFile info(infoFileFor(trashedPath));
    if (info.fileName().isEmpty() || !info.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : info.readAll().split('\n'))
        if (line.startsWith("DeletionDate="))
            // Local time, without a zone, as the spec writes it.
            return QDateTime::fromString(QString::fromUtf8(line.mid(13).trimmed()), Qt::ISODate);
    return {};
}

QString FileOps::restoreOne(const QString &trashedPath, QString *error, Job *job) {
    const QString original = originalPath(trashedPath);
    if (original.isEmpty()) {
        *error = tr("%1 has no record of where it came from.").arg(QFileInfo(trashedPath).fileName());
        return {};
    }
    const QString folder = QFileInfo(original).absolutePath();
    if (!QDir().mkpath(folder)) {
        *error = tr("Couldn't recreate %1.").arg(folder);
        return {};
    }
    const QString destination = QDir(folder).filePath(freeName(folder, QFileInfo(original).fileName()));
    // A link is made again rather than renamed, which across drives would
    // copy what it leads to.
    if (!QFileInfo(trashedPath).isSymLink() && QFile::rename(trashedPath, destination)) {
        QFile::remove(infoFileFor(trashedPath));
        return destination;
    }
    // Across drives (rare: trash lives on the item's own drive), it moves
    // the way a move across drives does, each original removed once it's
    // copied. The note stays while anything of it is left in the trash.
    if (!copyRecursively(trashedPath, destination, error, true, job)) {
        // What arrived is now the only copy of it, so where it went is said,
        // stopped when asked or not.
        const QString name = QFileInfo(trashedPath).fileName();
        if (QFileInfo::exists(destination))
            *error = error->isEmpty() ? tr("Stopped partway: part of %1 is restored, to %2.").arg(name, destination)
                                      : tr("Restored only part of %1, to %2: %3").arg(name, destination, *error);
        return {};
    }
    if (QFileInfo::exists(trashedPath) || QFileInfo(trashedPath).isSymLink()) {
        *error = tr("Restored %1, but couldn't remove it from the trash.").arg(QFileInfo(trashedPath).fileName());
        return {};
    }
    QFile::remove(infoFileFor(trashedPath));
    return destination;
}

bool FileOps::deleteTrashed(const QString &trashedPath) {
    const QFileInfo item(trashedPath);
    const bool removed = item.isDir() && !item.isSymLink() ? QDir(trashedPath).removeRecursively()
                                                           : QFile::remove(trashedPath);
    // The note goes once its item is gone, so what's left can be restored.
    if (const QString note = infoFileFor(trashedPath); removed && !note.isEmpty())
        QFile::remove(note);
    return removed;
}

int FileOps::emptyTrashDir(const QString &trashDir, Job *job) {
    int count = 0;
    const QDir files(trashDir + "/files");
    for (const QFileInfo &item : files.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (job && job->cancelled)
            break;
        if (deleteTrashed(item.absoluteFilePath()))
            ++count;
        if (job)
            ++job->done;
    }
    // The size cache goes too. A note without its item stays: something
    // being trashed gets its note first, so it may be on its way.
    QFile::remove(trashDir + "/directorysizes");
    return count;
}

int FileOps::trashCount() const {
    int count = 0;
    for (const QString &dir : trashDirs())
        count += QDir(dir + "/files").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size();
    return count;
}

void FileOps::restore(const QStringList &paths) {
    if (paths.isEmpty() || !idle())
        return;
    // Restoring across drives copies, which for a folder can take a while.
    const int count = paths.size();
    inBackground(
        [count](const Job &job) {
            return counting(job, tr("Restoring %1 of %n items…", "", job.total), tr("Restoring %n items…", "", count));
        },
        [paths](Job &job) {
            Outcome result;
            const QStringList outer = outermost(paths);
            job.total = outer.size();
            for (const QString &path : outer) {
                if (job.cancelled)
                    break;
                QString error;
                const QString to = restoreOne(path, &error, &job);
                if (!to.isEmpty())
                    result.made = {to};
                else if (!error.isEmpty())
                    result.failures << error;
                ++job.done;
            }
            return result;
        },
        [this, count](const Outcome &result, const Job &job) {
            if (!result.failures.isEmpty())
                emit failed(result.failures.join(' '));
            else if (job.cancelled)
                sayStopped(job);
            else if (!result.made.isEmpty()) {
                const QFileInfo last(result.made.first());
                emit notice(count == 1 ? tr("Restored %1 to %2.").arg(last.fileName(), last.absolutePath())
                                       : tr("Restored %n items.", "", count));
            }
        });
}

void FileOps::deleteForever(const QStringList &paths) {
    if (paths.isEmpty() || !idle())
        return;
    const int count = paths.size();
    inBackground(
        [count](const Job &job) {
            return counting(job, tr("Deleting %1 of %n items…", "", job.total), tr("Deleting %n items…", "", count));
        },
        [paths](Job &job) {
            Outcome result;
            const QStringList outer = outermost(paths);
            job.total = outer.size();
            for (const QString &path : outer) {
                if (job.cancelled)
                    break;
                if (!deleteTrashed(path))
                    result.failures << path;
                ++job.done;
            }
            return result;
        },
        [this](const Outcome &result, const Job &job) {
            if (!result.failures.isEmpty())
                emit failed(tr("Couldn't delete %n items.", "", result.failures.size()));
            else if (job.cancelled)
                sayStopped(job);
        });
}

void FileOps::emptyTrash() {
    if (!idle())
        return;
    inBackground(
        [](const Job &job) {
            return counting(job, tr("Emptying the trash: %1 of %n items…", "", job.total), tr("Emptying the trash…"));
        },
        [](Job &job) {
            const QStringList dirs = trashDirs();
            int total = 0;
            for (const QString &dir : dirs)
                total += QDir(dir + "/files").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size();
            job.total = total;
            int count = 0;
            for (const QString &dir : dirs)
                count += emptyTrashDir(dir, &job);
            return count;
        },
        [this](int count, const Job &job) {
            if (job.cancelled)
                sayStopped(job);
            else
                emit notice(tr("Emptied the trash: %n items deleted.", "", count));
        });
}

QString FileOps::rename(const QString &path, const QString &name) {
    const QString clean = name.trimmed();
    const QFileInfo info(path);
    if (clean.isEmpty() || clean == info.fileName())
        return {};
    if (clean.contains('/'))
        return tr("A name can't contain \"/\".");
    const QString destination = QDir(info.absolutePath()).filePath(clean);
    // QFile::rename never overwrites, and lets a name change only its case
    // on drives that ignore case, where the new name already "exists".
    if (!QFile::rename(path, destination))
        return QFileInfo::exists(destination) ? tr("%1 already exists here.").arg(clean)
                                              : tr("Couldn't rename %1.").arg(info.fileName());
    emit done(info.absolutePath(), clean);
    return {};
}

void FileOps::newFolder(const QString &folder) {
    if (!QFileInfo(folder).isDir()) {
        emit failed(tr("Folders can't be made here."));
        return;
    }
    QString name = tr("New Folder");
    for (int n = 2; QFileInfo::exists(QDir(folder).filePath(name)); ++n)
        name = tr("New Folder %1").arg(n);
    if (!QDir(folder).mkdir(name)) {
        emit failed(tr("Couldn't create a folder here."));
        return;
    }
    emit done(folder, name);
}

QString FileOps::defaultAppFor(const QString &path) {
    const QString mime = QFileInfo(path).isDir() ? QStringLiteral("inode/directory")
                                                  : QMimeDatabase().mimeTypeForFile(path).name();
    char *type = g_content_type_from_mime_type(mime.toUtf8().constData());
    if (!type)
        return {};
    GAppInfo *app = g_app_info_get_default_for_type(type, FALSE);
    g_free(type);
    if (!app)
        return {};
    const QString id = QString::fromUtf8(g_app_info_get_id(app));
    g_object_unref(app);
    return id;
}

QString FileOps::openInDefaultApp(const QString &path) {
    // xdg-open asks only for the file's exact type, so a type with no
    // default of its own (Ruby, C++, shell scripts) opened nothing at all.
    const QString id = defaultAppFor(path);
    if (id.isEmpty())
        return tr("No app is set up to open %1.").arg(QFileInfo(path).fileName());
    GDesktopAppInfo *app = g_desktop_app_info_new(id.toUtf8().constData());
    if (!app)
        return tr("No app is set up to open %1.").arg(QFileInfo(path).fileName());
    GList *files = g_list_append(nullptr, g_file_new_for_path(path.toUtf8().constData()));
    GError *error = nullptr;
    QString problem;
    if (!g_app_info_launch(G_APP_INFO(app), files, nullptr, &error))
        problem = tr("Couldn't open %1 with %2: %3")
                      .arg(QFileInfo(path).fileName(), QString::fromUtf8(g_app_info_get_name(G_APP_INFO(app))),
                           QString::fromUtf8(error ? error->message : ""));
    if (error)
        g_error_free(error);
    g_list_free_full(files, g_object_unref);
    g_object_unref(app);
    return problem;
}

QVariantList FileOps::appsFor(const QString &path) const {
    QVariantList apps;
    const QString mime = QFileInfo(path).isDir() ? QStringLiteral("inode/directory")
                                                  : QMimeDatabase().mimeTypeForFile(path).name();
    char *type = g_content_type_from_mime_type(mime.toUtf8().constData());
    if (!type)
        return apps;
    GAppInfo *fallback = g_app_info_get_default_for_type(type, FALSE);
    const QString defaultId = fallback ? QString::fromUtf8(g_app_info_get_id(fallback)) : QString();
    if (fallback)
        g_object_unref(fallback);
    GList *all = g_app_info_get_all_for_type(type);
    for (GList *l = all; l; l = l->next) {
        auto *info = G_APP_INFO(l->data);
        if (!g_app_info_should_show(info))
            continue;
        const QString id = QString::fromUtf8(g_app_info_get_id(info));
        const QVariantMap app{{"id", id}, {"name", QString::fromUtf8(g_app_info_get_name(info))},
                              {"isDefault", id == defaultId}};
        if (id == defaultId)
            apps.prepend(app);
        else
            apps << app;
    }
    g_list_free_full(all, g_object_unref);
    g_free(type);
    return apps;
}

void FileOps::openWith(const QString &path, const QString &appId) {
    GDesktopAppInfo *app = g_desktop_app_info_new(appId.toUtf8().constData());
    if (!app)
        return;
    GList *files = g_list_append(nullptr, g_file_new_for_path(path.toUtf8().constData()));
    GError *error = nullptr;
    if (!g_app_info_launch(G_APP_INFO(app), files, nullptr, &error))
        emit failed(tr("Couldn't open %1 with %2: %3")
                        .arg(QFileInfo(path).fileName(), QString::fromUtf8(g_app_info_get_name(G_APP_INFO(app))),
                             QString::fromUtf8(error ? error->message : "")));
    if (error)
        g_error_free(error);
    g_list_free_full(files, g_object_unref);
    g_object_unref(app);
}

void FileOps::openTerminal(const QString &folder) {
    // xdg-terminal-exec starts the terminal Omarchy is set up with.
    if (!QProcess::startDetached("xdg-terminal-exec", {"--dir=" + folder}, folder))
        emit failed(tr("Couldn't start a terminal: xdg-terminal-exec isn't installed."));
}

QProcess *FileOps::runTool(const QString &program, const QStringList &args, const QString &busy, const QString &missing,
                          const QString &stopped, const std::function<void(QProcess *, int)> &finished) {
    if (!idle())
        return nullptr;
    auto *process = new QProcess(this);
    m_process = process;
    m_killed = false;
    setBusy(busy);
    connect(process, &QProcess::finished, this, [this, process, finished, stopped](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        m_process = nullptr;
        setBusy({});
        if (m_killed)
            emit notice(stopped);
        else
            finished(process, status == QProcess::NormalExit ? code : -1);
    });
    // A program that isn't installed never finishes, so this is the only
    // word from it.
    connect(process, &QProcess::errorOccurred, this, [this, process, missing](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        m_process = nullptr;
        setBusy({});
        emit failed(missing);
    });
    process->start(program, args);
    return process;
}

bool FileOps::idle() {
    if (m_busy.isEmpty())
        return true;
    emit failed(tr("Try again once the work under way is done."));
    return false;
}

void FileOps::setBusy(const QString &busy) {
    if (busy == m_busy)
        return;
    m_busy = busy;
    emit busyChanged();
}
