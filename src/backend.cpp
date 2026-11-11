#include "backend.h"
#include "fileops.h"
#include "previewer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QMimeDatabase>
#include <QStorageInfo>
#include <QUrl>
#include <QtConcurrent>
#include <unistd.h>

Backend::Backend(const QString &home, QObject *parent)
    : QObject(parent), m_home(home.isEmpty() ? QDir::homePath() : home) {
    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(4000);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this] {
        m_notice.clear();
        emit noticeChanged();
    });
    connect(&m_mounter, &Mounter::mounted, this, [this](const QString &local) {
        setBusy({});
        // A machine lands in the matching home folder when it has one,
        // which on a tailnet of one person's machines it usually does.
        const QString remoteHome = local + QDir::homePath();
        const QFileInfo target(m_mountPeer && QFileInfo(remoteHome).isDir() ? remoteHome : local);
        // An address can name a file, which opens its folder with it selected.
        if (target.isDir() || !target.exists())
            navigate(target.filePath());
        else
            showItem(target.filePath());
    });
    connect(&m_mounter, &Mounter::failed, this, [this](const QString &error) {
        setBusy({});
        // An empty error is the user cancelling the sign-in: nothing to report.
        if (!error.isEmpty())
            setMessage(tr("Couldn't connect to %1: %2").arg(m_mountName, error));
    });
    // The preview reports each file it steps to, and the grid follows.
    connect(&m_preview, &QProcess::readyReadStandardOutput, this, [this] {
        while (m_preview.canReadLine()) {
            const QString shown = QString::fromUtf8(m_preview.readLine()).trimmed();
            m_previewed = shown;
            const QFileInfo info(shown);
            if (info.absolutePath() == path())
                emit selectRequested(info.fileName());
        }
    });
    connect(&m_preview, &QProcess::stateChanged, this, &Backend::previewingChanged);
    connect(&m_preview, &QProcess::finished, this, [this](int, QProcess::ExitStatus status) {
        // A decoder that crashed took down the preview, not Folio; say so
        // rather than letting the window just vanish. Closing it from here
        // ends it with SIGTERM, which QProcess also reports as a crash.
        if (status == QProcess::CrashExit && !m_closingPreview)
            setMessage(tr("The preview closed unexpectedly on %1.").arg(QFileInfo(m_previewed).fileName()));
    });
}

QString Backend::gvfsRoot() {
    return QStringLiteral("/run/user/%1/gvfs").arg(getuid());
}

// What a gvfs mount is called on screen: the host or server from its
// folder's name ("sftp:host=nas,user=me", "smb-share:server=nas,..."), and only the machine's own name for a tailnet
// address, since every machine on it shares the "tailXXXX.ts.net" part.
QString Backend::mountLabel(const QString &mountFolder) {
    QString host = mountFolder;
    for (const QString &field : mountFolder.section(':', 1).split(','))
        if (field.startsWith("host=") || field.startsWith("server="))
            host = field.section('=', 1);
    return host.endsWith(".ts.net") ? host.section('.', 0, 0) : host;
}

QString Backend::title() const {
    const QString p = path();
    if (p == FolderModel::recentPath())
        return tr("Recents");
    if (p == FolderModel::tailnetPath())
        return tr("Tailnet");
    if (p == trashPath())
        return tr("Trash");
    if (p == m_home)
        return tr("Home");
    if (p == "/")
        return tr("Computer");
    if (QFileInfo(p).absolutePath() == gvfsRoot())
        return mountLabel(QFileInfo(p).fileName());
    return QFileInfo(p).fileName();
}

// Where the removable or extra drive holding `path` is mounted, when it's one
// (under /run/media, /media or /mnt), so its path starts at its name rather
// than at /run/media/<user>/.
QString Backend::driveRoot(const QString &path) {
    static const QStringList places{"/run/media/", "/media/", "/mnt/"};
    for (const QString &place : places) {
        if (!path.startsWith(place))
            continue;
        const QString root = QStorageInfo(path).rootPath();
        if (root.startsWith(place) && (path == root || path.startsWith(root + "/")))
            return root;
    }
    return {};
}

QVariantList Backend::crumbs() const {
    // Breadcrumbs start at the nearest root that means something to a person:
    // ~ for home, a tailnet machine's name for its mount, a drive's name for
    // a drive, / otherwise.
    const QString p = path();
    if (p == FolderModel::recentPath())
        return {QVariantMap{{"name", tr("Recents")}, {"path", p}}};
    if (p == FolderModel::tailnetPath())
        return {QVariantMap{{"name", tr("Tailnet")}, {"path", p}}};
    // The trash's files folder is an implementation detail; it's "Trash".
    if (p == trashPath())
        return {QVariantMap{{"name", tr("Trash")}, {"path", p}}};
    QString base = "/";
    QString label = "/";
    if (p == m_home || p.startsWith(m_home + "/")) {
        base = m_home;
        label = "~";
    } else if (p.startsWith(gvfsRoot() + "/")) {
        const QString mount = p.section('/', 0, 5);
        base = mount;
        label = mountLabel(mount.section('/', -1));
    } else if (const QString drive = driveRoot(p); !drive.isEmpty()) {
        base = drive;
        label = QFileInfo(drive).fileName();
    }

    QVariantList crumbs{QVariantMap{{"name", label}, {"path", base}}};
    const QString rest = p.mid(base.length());
    QString walk = base;
    for (const QString &part : rest.split('/', Qt::SkipEmptyParts)) {
        walk = walk.endsWith('/') ? walk + part : walk + "/" + part;
        crumbs << QVariantMap{{"name", part}, {"path", walk}};
    }
    return crumbs;
}

void Backend::navigate(const QString &path) {
    go(path, true);
}

bool Backend::go(const QString &path, bool record) {
    const QString clean = FolderModel::isView(path) ? path : QDir::cleanPath(path);
    // The trash folder only exists once something has been trashed; the
    // spec lets anyone create it, so Trash always opens.
    if (clean == trashPath())
        QDir().mkpath(clean);
    if (!FolderModel::isView(clean) && !QFileInfo(clean).isDir()) {
        setMessage(tr("%1 isn't a folder.").arg(clean));
        return false;
    }
    if (record) {
        if (m_index >= 0 && m_history.value(m_index) == clean)
            return true;
        m_history = m_history.mid(0, m_index + 1);
        m_history << clean;
        m_index = m_history.size() - 1;
    }
    m_folder.setPath(clean);
    if (s_working == this)
        moveWorkingDirectory();
    emit pathChanged();
    return true;
}

void Backend::keepWorkingDirectory() {
    s_working = this;
    moveWorkingDirectory();
}

void Backend::moveWorkingDirectory() {
    const QString p = path();
    if (!FolderModel::isView(p) && !p.startsWith(gvfsRoot() + "/"))
        QDir::setCurrent(p);
}

void Backend::open(const QString &path) {
    // A machine in the tailnet view opens as itself, over SFTP.
    if (path.startsWith(FolderModel::tailnetPath()) && path != FolderModel::tailnetPath()) {
        openPeer(path.mid(FolderModel::tailnetPath().size()));
        return;
    }
    const QFileInfo info(path);
    if (info.isDir()) {
        navigate(path);
        return;
    }
    // The desktop's default app for the type, or for the nearest type it
    // belongs to.
    if (const QString problem = FileOps::openInDefaultApp(path); !problem.isEmpty())
        setMessage(problem);
}

void Backend::goUp() {
    // Recents, the tailnet and the trash are places of their own: nothing
    // above them.
    if (FolderModel::isView(path()) || path() == trashPath())
        return;
    const QString parent = QFileInfo(path()).absolutePath();
    if (parent == path())
        return;
    // Arriving back in the parent keeps the folder we came from selected, so
    // going up and down again is one keystroke each way.
    m_pendingSelection = QFileInfo(path()).fileName();
    navigate(parent);
}

// A folder in the history that's gone (deleted, or on a machine that went
// away) is said in the window, and the history stays on the folder shown.
void Backend::back() {
    if (!canBack())
        return;
    const QString pending = m_pendingSelection;
    m_pendingSelection = QFileInfo(path()).fileName();
    --m_index;
    if (!go(m_history.at(m_index), false)) {
        ++m_index;
        m_pendingSelection = pending;
    }
}

void Backend::forward() {
    if (!canForward())
        return;
    ++m_index;
    if (!go(m_history.at(m_index), false))
        --m_index;
}

void Backend::openPeer(const QString &host) {
    if (m_mounter.busy())
        return;
    // Remembered as it's asked for, not once it's mounted: a machine that
    // didn't open (asleep, or without Tailscale SSH yet) is still the one
    // to try again, and the sidebar only shows it while it's online.
    emit peerOpened(host);
    const QString root = gvfsRoot() + "/sftp:host=" + host;
    if (QFileInfo(root).isDir()) {
        const QString remoteHome = root + QDir::homePath();
        navigate(QFileInfo(remoteHome).isDir() ? remoteHome : root);
        return;
    }
    // SFTP over the tailnet: with Tailscale SSH on the other machine there
    // are no keys or passwords to manage, and GVfs makes the mount look like
    // any other folder.
    connectTo("sftp://" + host + "/", host, true);
}

void Backend::connectTo(const QString &uri, const QString &name, bool peer) {
    m_mountName = name;
    m_mountPeer = peer;
    setBusy(tr("Connecting to %1…").arg(name));
    m_mounter.mount(uri, peer);
}

QString Backend::serverAddress(const QString &text) {
    static const QStringList schemes{"sftp", "smb", "ftp", "dav", "davs"};
    // Tolerant, so a path typed with spaces in it is escaped rather than
    // refused; QUrl puts the scheme in lower case.
    QUrl url(text.trimmed(), QUrl::TolerantMode);
    if (!url.isValid() || !schemes.contains(url.scheme()) || url.host().isEmpty())
        return {};
    if (url.path().isEmpty())
        url.setPath("/");
    return url.toString(QUrl::FullyEncoded);
}

void Backend::showNotice(const QString &notice) {
    m_notice = notice;
    emit noticeChanged();
    m_noticeTimer.start();
}

void Backend::dismissMessage() {
    setMessage({});
}

QString Backend::takeSelection() {
    const QString name = m_pendingSelection;
    m_pendingSelection.clear();
    return name;
}

void Backend::describe(const QVariantMap &item, bool inTrash) {
    const QString path = item.value("path").toString();
    const QString kind = item.value("kind").toString();
    const QString mime = item.value("mime").toString();
    const quint64 generation = ++m_describing;
    // The system's own name for the type ("JPEG image"), already translated,
    // needs nothing from the file. The same item again (its folder re-read)
    // keeps what it shows until the new reading is in, rather than flickering.
    QVariantMap details{{"type", QMimeDatabase().mimeTypeForName(mime).comment()}};
    if (const QString described = path + '\n' + mime; described != m_described) {
        m_described = described;
        m_details = details;
        emit detailsChanged();
    }
    if (kind != "image" && kind != "text" && !inTrash)
        return;

    // Reading the file waits on its disk, which for a slow mount would hold
    // up moving through files, so it happens off the UI thread.
    auto *watcher = new QFutureWatcher<QVariantMap>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, details]() mutable {
        watcher->deleteLater();
        if (generation != m_describing)
            return; // The selection moved on while this was read.
        details.insert(watcher->result());
        m_details = details;
        emit detailsChanged();
    });
    watcher->setFuture(QtConcurrent::run([path, kind, inTrash] {
        QVariantMap read;
        // A trashed item's note: finding it looks over every mounted drive,
        // and one that's hung would hold up the window.
        if (inTrash) {
            read.insert("origin", FileOps::originalPath(path));
            read.insert("deleted", FileOps::deletedAt(path));
        }
        if (kind == "image") {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QSize size = reader.size();
            if (size.isValid())
                read.insert("dimensions", QString("%1 × %2").arg(size.width()).arg(size.height()));
        } else if (kind == "text") {
            // Enough of the top of the file to recognize it, never the whole
            // thing: a log can be gigabytes.
            read.insert("text", Previewer::readText(path, 6000).section('\n', 0, 40));
        }
        return read;
    }));
}

Backend::~Backend() {
    if (s_working == this)
        s_working = nullptr;
    // The preview follows this window; it closes with it.
    if (previewing()) {
        m_preview.terminate();
        m_preview.waitForFinished(1000);
    }
}

QString Backend::trashPath() const {
    return QDir::cleanPath(FileOps::homeTrash() + "/files");
}

QString Backend::typedPath() const {
    const QString p = path();
    if (p == m_home)
        return "~/";
    if (p.startsWith(m_home + "/"))
        return "~" + p.mid(m_home.size()) + "/";
    return FolderModel::isView(p) || p == "/" ? p : p + "/";
}

QString Backend::expandTyped(const QString &text, const QString &home, const QString &current) {
    QString t = text.trimmed();
    if (t.startsWith("file://"))
        t = QUrl(t).toLocalFile();
    if (t == "~")
        return home;
    if (t.startsWith("~/"))
        return home + t.mid(1);
    // A relative path is taken from the folder being shown, or from home in
    // Recents, which isn't a folder.
    if (!t.startsWith('/'))
        return QDir(QFileInfo(current).isDir() ? current : home).filePath(t);
    return t;
}

bool Backend::goTyped(const QString &text) {
    // A server is mounted through GVfs, which also says where on the mount
    // the address's path is (a WebDAV address's path can be part of the
    // mount itself), and connecting goes there.
    if (const QString address = serverAddress(text); !address.isEmpty()) {
        if (!m_mounter.busy())
            connectTo(address, QUrl(address).host(), false);
        return true;
    }
    // A view is typed as the path bar shows it, by its own name.
    if (const QString view = text.trimmed(); FolderModel::isView(view)) {
        navigate(view);
        return true;
    }
    const QString target = QDir::cleanPath(expandTyped(text, m_home, path()));
    const QFileInfo info(target);
    if (info.isDir()) {
        navigate(target);
        return true;
    }
    if (info.exists()) {
        showItem(target);
        return true;
    }
    setMessage(tr("%1 doesn't exist.").arg(text.trimmed()));
    return false;
}

QString Backend::complete(const QString &text, const QString &home, const QString &current) {
    // Completes the last part to the folder names it starts, as far as
    // they agree, keeping how the user typed the front (~, relative).
    const int slash = text.lastIndexOf('/');
    const QString head = slash >= 0 ? text.left(slash + 1) : QString();
    const QString part = text.mid(slash + 1);
    const QString folder = expandTyped(head.isEmpty() ? "." : head, home, current);
    QStringList matches;
    const auto names = QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QString &name : names)
        if (name.startsWith(part) && (part.startsWith('.') || !name.startsWith('.')))
            matches << name;
    if (matches.isEmpty())
        return text;
    if (matches.size() == 1)
        return head + matches.first() + "/";
    QString common = matches.first();
    for (const QString &m : matches)
        while (!m.startsWith(common))
            common.chop(1);
    return head + common;
}

QString Backend::completePath(const QString &text) const {
    return complete(text, m_home, path());
}

void Backend::newWindow() {
    emit newWindowRequested(FolderModel::isView(path()) ? m_home : path());
}

void Backend::togglePreview(const QString &path) {
    if (previewing()) {
        m_closingPreview = true;
        m_preview.terminate();
        return;
    }
    if (path.isEmpty())
        return;
    QStringList args{"preview", "--follow", "--sort", m_folder.sortMode()};
    if (m_folder.showHidden())
        args << "--hidden";
    args << path;
    m_previewed = path;
    m_preview.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m_closingPreview = false;
    m_preview.start(QCoreApplication::applicationFilePath(), args);
}

void Backend::followSelection(const QString &path) {
    if (!previewing() || path.isEmpty() || path == m_previewed)
        return;
    m_previewed = path;
    m_preview.write(path.toUtf8() + '\n');
}

void Backend::showFolder(const QString &path) {
    navigate(path);
    emit activateRequested();
}

void Backend::showItem(const QString &path) {
    const QFileInfo info(path);
    m_pendingSelection = info.fileName();
    const QString parent = info.absolutePath();
    if (parent == this->path()) {
        emit selectRequested(m_pendingSelection);
        m_pendingSelection.clear();
    } else {
        navigate(parent);
    }
    emit activateRequested();
}

void Backend::setMessage(const QString &message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

void Backend::setBusy(const QString &busy) {
    if (busy == m_busy)
        return;
    m_busy = busy;
    emit busyChanged();
}
