#pragma once

#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <memory>

class QMimeData;
class QProcess;

// What can be done to files: the clipboard (copy, cut, paste, and yanking
// paths as text), trash, rename, new folder, and opening with another app.
// Long work (copying a folder of videos) runs off the UI thread, reports how
// far it's got through `busy`, and can be cancelled. Nothing is ever
// overwritten, so a name that's taken gets a "(copy)" name beside it.
class FileOps : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged)

public:
    explicit FileOps(QObject *parent = nullptr);

    QString busy() const { return m_busy; }
    bool canPaste() const;

    // How far a piece of work has got, and whether it's been asked to stop:
    // the worker counts and checks, the UI thread shows and asks.
    struct Job {
        std::atomic<bool> cancelled = false;
        std::atomic<int> done = 0, total = 0;
        // Bytes copied so far, for showing progress through one big file.
        std::atomic<qint64> bytes = 0;
        // A single folder counts what's in it as its items, set by the
        // worker once it has listed it.
        QString countIn;
    };

    // The clipboard holds files the way GNOME and KDE apps expect
    // (x-special/gnome-copied-files plus text/uri-list), and their paths as
    // plain text, so pasting into a terminal or editor gives the path.
    Q_INVOKABLE void copy(const QStringList &paths);
    Q_INVOKABLE void cut(const QStringList &paths);
    Q_INVOKABLE void yank(const QStringList &paths);
    Q_INVOKABLE void paste(const QString &folder);
    // Files dropped on a folder: moved when they're on the same drive,
    // copied from another drive. Dropping something where it already is
    // does nothing.
    Q_INVOKABLE void dropInto(const QList<QUrl> &urls, const QString &folder);
    // Whether these can be dropped on `folder`: not onto themselves, and
    // not into the trash, which only Delete fills.
    Q_INVOKABLE bool canDrop(const QList<QUrl> &urls, const QString &folder) const;
    // Files as a text/uri-list, for dragging them somewhere, and back from
    // dropped URLs to the paths of the local ones. QUrl escapes what a
    // path may hold but a URI can't (a "#" or "?" in a name).
    Q_INVOKABLE QString uriList(const QStringList &paths) const;
    Q_INVOKABLE QStringList localPaths(const QList<QUrl> &urls) const;

    // A copy beside the original, named "(copy)".
    Q_INVOKABLE void duplicate(const QString &path);
    // Packs the items into a .zip beside them (bsdtar, from libarchive).
    Q_INVOKABLE void compress(const QStringList &paths);
    // Sends with LocalSend to a nearby device, or with Taildrop to a
    // machine on the tailnet.
    Q_INVOKABLE void shareLocalSend(const QStringList &paths);
    Q_INVOKABLE bool hasLocalSend() const;
    // Asks Dropbox for a share link to a file in the Dropbox folder and
    // puts it on the clipboard.
    Q_INVOKABLE void dropboxLink(const QString &path);
    Q_INVOKABLE void shareTaildrop(const QStringList &paths, const QString &host);

    Q_INVOKABLE void trash(const QStringList &paths);

    // The trash, per the freedesktop Trash spec: each item in Trash/files
    // has a note in Trash/info saying where it came from.
    Q_INVOKABLE bool inTrash(const QString &path) const;
    // Where a trashed item came from, or empty if its note is missing.
    Q_INVOKABLE static QString originalPath(const QString &trashedPath);
    // When it was trashed, from its note.
    Q_INVOKABLE static QDateTime deletedAt(const QString &trashedPath);
    // Puts items back where they came from (recreating the folder if it's
    // gone, and never overwriting what's there now).
    Q_INVOKABLE void restore(const QStringList &paths);
    // Deletes trashed items for good, and empties the whole trash.
    Q_INVOKABLE void deleteForever(const QStringList &paths);
    Q_INVOKABLE void emptyTrash();
    Q_INVOKABLE int trashCount() const;
    // Empty string on success, or why not.
    Q_INVOKABLE QString rename(const QString &path, const QString &name);
    // Where a name's extension begins, for renaming and "(copy)" names:
    // ".pdf" in "Report v1.2.pdf", ".tar.gz" kept whole. -1 for folders and
    // dotfiles, which have none.
    Q_INVOKABLE static int extensionStart(const QString &name, bool isDir);
    // Makes "New Folder" (or "New Folder 2"...); done() names it.
    Q_INVOKABLE void newFolder(const QString &folder);

    // The apps that can open a file, from the desktop's MIME database:
    // {id, name, isDefault}, the default first.
    Q_INVOKABLE QVariantList appsFor(const QString &path) const;
    Q_INVOKABLE void openWith(const QString &path, const QString &appId);
    Q_INVOKABLE void openTerminal(const QString &folder);

    // Stops the work under way at the next file; what's done stays done.
    Q_INVOKABLE void cancel();

    // Building blocks, kept static for tests.
    static QMimeData *mimeDataFor(const QStringList &paths, bool cut);
    // The paths on the clipboard and whether they were cut.
    static QStringList pathsFrom(const QMimeData *data, bool *cut);
    // A name in `folder` that isn't taken: the name itself, or
    // "name (copy).ext", "name (copy 2).ext"...
    static QString freeName(const QString &folder, const QString &name);
    // Copies `from` to `to`, a folder with all it holds. Moving, each file
    // is removed once it's copied and each folder once it's empty; what
    // can't be removed is left where it is.
    // With a job, it counts the bytes it copies and stops when asked; a file
    // stopped partway is removed.
    static bool copyRecursively(const QString &from, const QString &to, QString *error, bool move = false,
                                Job *job = nullptr);
    // The archive name for compressing these into `folder`: "name.zip" for
    // one item, "Archive.zip" for several; never one that exists.
    static QString archiveName(const QStringList &paths, const QString &folder);
    // The app a file opens with by default; GIO also looks at the type's
    // parents, so a Ruby file finds the plain-text app. Empty for none.
    static QString defaultAppFor(const QString &path);
    // Opens the file in that app; a sentence saying why not when it can't.
    static QString openInDefaultApp(const QString &path);
    // Whether the item at `path` is on the drive `folder` is on.
    static bool sameDrive(const QString &path, const QString &folder);
    // The home trash, where g_file_trash puts things: $XDG_DATA_HOME/Trash.
    static QString homeTrash();
    // The trash folders: the home one, then each mounted drive's own.
    static QStringList trashDirs();
    // For an item directly in a trash's files folder: its note. Empty for
    // anything else, such as what's inside a trashed folder, which has none.
    static QString infoFileFor(const QString &trashedPath);
    // Restores one item; returns where it went, or empty with `error` set.
    static QString restoreOne(const QString &trashedPath, QString *error, Job *job = nullptr);
    static bool deleteTrashed(const QString &trashedPath);
    static int emptyTrashDir(const QString &trashDir, Job *job = nullptr);
    // How transfer() takes the paths: copied, moved, or as a drop takes
    // them, each moved from the folder's drive and copied from any other.
    enum Mode { Copy, Move, Drop };
    // Copies or moves each path into `folder`; returns the new names.
    static QStringList transfer(const QStringList &paths, const QString &folder, Mode mode, QString *error,
                                Job *job = nullptr);

signals:
    void busyChanged();
    void clipboardChanged();
    void failed(const QString &message);
    void notice(const QString &notice);
    // Work finished in `folder`, which now holds `name` (to select it).
    void done(const QString &folder, const QString &name);

private:
    void setBusy(const QString &busy);
    // One piece of work runs at a time: whether nothing else is running,
    // saying why when something is, so what's refused doesn't just do
    // nothing.
    bool idle();
    // Runs `work` off the UI thread with a job it counts in and checks,
    // showing what `label` makes of the job meanwhile, then hands what it
    // returns to `done`, back on this one.
    template <typename Work, typename Done>
    void inBackground(const std::function<QString(const Job &)> &label, Work work, Done done);
    // Copies or moves off the UI thread, showing how far it's got meanwhile.
    void transferInBackground(const QStringList &paths, const QString &folder, Mode mode, bool releaseClipboard,
                              bool duplicating = false);
    // Says the work was stopped, and how far it got.
    void sayStopped(const Job &job);
    // Runs a helper program (bsdtar, dropbox-cli, tailscale) while showing
    // `busy`, one piece of work at a time like the rest. `missing` says
    // what to install when it isn't there, and `stopped` what a cancel
    // left; `finished` gets its exit code, or -1 if it crashed. Returns the
    // program, or null when other work is under way.
    QProcess *runTool(const QString &program, const QStringList &args, const QString &busy, const QString &missing,
                      const QString &stopped, const std::function<void(QProcess *, int)> &finished);

    QString m_busy;
    std::shared_ptr<Job> m_job;
    QProcess *m_process = nullptr;
    bool m_killed = false;
};
