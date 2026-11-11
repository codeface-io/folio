#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include "foldermodel.h"
#include "mounter.h"

// Where the window is and how it got there: the current folder, its
// breadcrumbs, back/forward history, and the actions that leave the folder
// (opening files, mounting a tailnet machine).
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(FolderModel *folder READ folder CONSTANT)
    Q_PROPERTY(Mounter *mounter READ mounter CONSTANT)
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
    Q_PROPERTY(QString title READ title NOTIFY pathChanged)
    Q_PROPERTY(QVariantList crumbs READ crumbs NOTIFY pathChanged)
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

    FolderModel *folder() { return &m_folder; }
    Mounter *mounter() { return &m_mounter; }
    QString path() const { return m_folder.path(); }
    QString title() const;
    QVariantList crumbs() const;
    bool canBack() const { return m_index > 0; }
    bool canForward() const { return m_index + 1 < m_history.size(); }
    QString message() const { return m_message; }
    QString notice() const { return m_notice; }
    QString busy() const { return m_busy; }
    bool previewing() const { return m_preview.state() != QProcess::NotRunning; }

    Q_INVOKABLE void navigate(const QString &path);
    Q_INVOKABLE void open(const QString &path);
    Q_INVOKABLE void goUp();
    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();
    Q_INVOKABLE void openPeer(const QString &host);
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
    // path (a folder, or a file to select in its folder; ~ works; or a
    // server's address, connecting to it), and completing the last part of
    // one to a folder name.
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
    // A server address (sftp://, smb://, ftp://, dav://, davs://) as GIO
    // mounts it, or empty when the text isn't one.
    static QString serverAddress(const QString &text);

signals:
    void pathChanged();
    void messageChanged();
    void noticeChanged();
    void busyChanged();
    void previewingChanged();
    void detailsChanged();
    void newWindowRequested(const QString &path);
    void selectRequested(const QString &name);
    void activateRequested();
    // A machine was asked for, for the sidebar to remember it by.
    void peerOpened(const QString &host);

private:
    static QString gvfsRoot();
    QString trashPath() const;
    // False when `path` isn't a folder (any more), which is said in the
    // window and leaves everything as it was.
    bool go(const QString &path, bool record);
    void setMessage(const QString &message);
    void setBusy(const QString &busy);
    // Mounts a server's address and goes where it points once it's mounted;
    // a tailnet machine's home, when it has one like ours.
    void connectTo(const QString &uri, const QString &name, bool peer);

    QString m_home;
    FolderModel m_folder;
    QStringList m_history;
    int m_index = -1;
    QString m_message;
    QString m_notice;
    QTimer m_noticeTimer;
    QString m_busy;
    QString m_pendingSelection;
    Mounter m_mounter;
    // What's being connected to, as the window names it, and whether it's
    // a tailnet machine, which lands in the matching home folder.
    QString m_mountName;
    bool m_mountPeer = false;
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
