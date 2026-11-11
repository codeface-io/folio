#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include "foldermodel.h"
#include "mounter.h"
#include "plugins.h"

// Where the window is and how it got there: the current folder, its
// breadcrumbs, back/forward history, and the actions that leave the folder
// (opening files and plugins' items, mounting other machines, running
// plugins' share entries).
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(FolderModel *folder READ folder CONSTANT)
    Q_PROPERTY(Mounter *mounter READ mounter CONSTANT)
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
    // What the place is called: changes with the path, and with a plugin's
    // labels (a poll, a language switch) while its view is shown.
    Q_PROPERTY(QString title READ title NOTIFY placeChanged)
    Q_PROPERTY(QVariantList crumbs READ crumbs NOTIFY placeChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(QString busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY previewingChanged)
    // What the details pane shows beyond the listing, for the item last
    // given to describe(): its type's name, and once they've been read in
    // the background, a picture's dimensions, a text's opening lines, or a
    // trashed item's origin and deletion time.
    Q_PROPERTY(QVariantMap details READ details NOTIFY detailsChanged)

public:
    explicit Backend(const QString &home = {}, QObject *parent = nullptr);
    ~Backend() override;

    // The plugins whose views, items and share entries this window offers.
    void setPlugins(Plugins *plugins);

    FolderModel *folder() { return &m_folder; }
    Mounter *mounter() { return &m_mounter; }
    QString path() const { return m_folder.path(); }
    QString title() const;
    QVariantList crumbs() const;
    bool canBack() const { return m_index > 0; }
    bool canForward() const { return m_index + 1 < m_history.size(); }
    QString message() const { return m_message; }
    QString notice() const { return m_notice; }
    // Mounting, or else the latest plugin action still running.
    QString busy() const { return m_busy.isEmpty() && !m_runs.isEmpty() ? m_runs.last().second : m_busy; }
    bool previewing() const { return m_preview.state() != QProcess::NotRunning; }

    Q_INVOKABLE void navigate(const QString &path);
    Q_INVOKABLE void open(const QString &path);
    Q_INVOKABLE void goUp();
    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();
    // Runs a plugin's share entry on the paths, sent to `target` (an item's
    // id) for a submenu, showing its busy text meanwhile and what it says
    // when it's done.
    Q_INVOKABLE void share(const QString &plugin, const QString &entry, const QStringList &paths,
                           const QString &target = {});
    Q_INVOKABLE void dismissMessage();
    Q_INVOKABLE void showMessage(const QString &message) { setMessage(message); }
    // Good news that needs no answer ("Link copied"): shown for a few
    // seconds in the accent, unlike problems, which wait to be dismissed.
    Q_INVOKABLE void showNotice(const QString &notice);
    // Selects `name` the next time the folder loads (after file work).
    Q_INVOKABLE void expectSelection(const QString &name) { m_pendingSelection = name; }
    QVariantMap details() const { return m_details; }
    Q_INVOKABLE void describe(const QVariantMap &item, bool inTrash = false);
    // The item to select once the folder has loaded (where we came from, or
    // what "Show in folder" asked for); empty means the first item.
    Q_INVOKABLE QString takeSelection();
    // The path as it's shown for editing (~ for home), going to a typed
    // path (a folder, or a file to select in its folder; ~ works), and
    // completing the last part of one to a folder name.
    Q_INVOKABLE QString typedPath() const;
    Q_INVOKABLE bool goTyped(const QString &text);
    Q_INVOKABLE QString completePath(const QString &text) const;
    // Space: opens the large preview of a file in its own process, or closes
    // it when it's already open.
    Q_INVOKABLE void togglePreview(const QString &path);
    // Ctrl+N: another window, on the folder this one shows.
    Q_INVOKABLE void newWindow();
    // Keeps an open preview on the item selected in the grid.
    Q_INVOKABLE void followSelection(const QString &path);

    // Called for org.freedesktop.FileManager1: open a folder, or open the
    // folder holding a file and select it.
    void showFolder(const QString &path);
    void showItem(const QString &path);

    static QString expandTyped(const QString &text, const QString &home, const QString &current);
    static QString complete(const QString &text, const QString &home, const QString &current);
    static QString mountLabel(const QString &mountFolder);

signals:
    void pathChanged();
    void placeChanged();
    void messageChanged();
    void noticeChanged();
    void busyChanged();
    void previewingChanged();
    void detailsChanged();
    void newWindowRequested(const QString &path);
    void selectRequested(const QString &name);
    void activateRequested();

private:
    static QString gvfsRoot();
    QString trashPath() const;
    // False when `path` isn't a folder (any more), which is said in the
    // window and leaves everything as it was.
    bool go(const QString &path, bool record);
    void setMessage(const QString &message);
    void setBusy(const QString &busy);
    // Opening a plugin's item: a folder, a mount or a command.
    void openItem(const QString &path);
    // Mounts `url` through GVfs and goes to its folder; a bare server lands
    // in the folder at home's path on it when there is one.
    void mount(const QString &url, const QString &name, bool verified);
    // A plugin action finished: its busy text goes, and what it asks for
    // happens.
    void finish(int run, const Plugins::Result &result);
    // A GVfs mount's name on screen: its plugin item's name, or its host.
    QString mountName(const QString &mount) const;

    QString m_home;
    FolderModel m_folder;
    QStringList m_history;
    int m_index = -1;
    QString m_message;
    QString m_notice;
    QTimer m_noticeTimer;
    QString m_busy;
    QString m_pendingSelection;
    Plugins *m_plugins = nullptr;
    Mounter m_mounter;
    QString m_mountName;
    bool m_mountBare = false;
    // The plugin actions running, each with its busy text, oldest first.
    QList<std::pair<int, QString>> m_runs;
    int m_lastRun = 0;
    QProcess m_preview;
    // Set while Folio closes the preview itself, which isn't a crash.
    bool m_closingPreview = false;
    QString m_previewed;
    QVariantMap m_details;
    // Whose details they are (the path and its type, as a file can be
    // replaced by a folder of the same name), and the newest describe(),
    // which a reading has to match to be shown.
    QString m_described;
    quint64 m_describing = 0;
};
