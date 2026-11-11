#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>

// The sidebar: the user's places (with Dropbox first-class when it's set up),
// the drives, and the tailnet machines opened in Folio. Each list is small,
// so it is handed to QML as plain maps rather than a model.
class Places : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString home READ home CONSTANT)
    Q_PROPERTY(QVariantList places READ places NOTIFY changed)
    Q_PROPERTY(QVariantList pinned READ pinned NOTIFY changed)
    Q_PROPERTY(QVariantList views READ views NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    // Every online machine on the tailnet that can hold files.
    Q_PROPERTY(QVariantList peers READ peers NOTIFY changed)
    // The few of those opened in Folio, most recently opened first. A
    // tailnet can have hundreds of machines; these are the ones that matter.
    Q_PROPERTY(QVariantList machines READ machines NOTIFY changed)
    Q_PROPERTY(QString dropboxStatus READ dropboxStatus NOTIFY changed)

public:
    // `home` is overridable for tests; the real home also consults the XDG
    // user-dirs, so renamed or translated folders are found.
    explicit Places(const QString &home = {}, QObject *parent = nullptr);

    QString home() const { return m_home; }
    QVariantList places() const { return m_places; }
    QVariantList pinned() const { return m_pinned; }
    QVariantList views() const { return m_views; }
    QVariantList devices() const { return m_devices; }
    QVariantList peers() const { return m_peers; }
    QVariantList machines() const { return m_machines; }
    QString dropboxStatus() const { return m_dropboxStatus; }

    Q_INVOKABLE void refresh();

    // Pins are GTK's bookmarks (~/.config/gtk-3.0/bookmarks), so a folder
    // pinned here is also in every GTK file dialog, and the other way round.
    // Returns an empty string, or why it couldn't be pinned.
    Q_INVOKABLE QString pin(const QString &path);
    Q_INVOKABLE void unpin(const QString &path);
    Q_INVOKABLE bool isPinned(const QString &path) const;

    // Opening a machine records its host, and when, in Folio's settings;
    // forgetting it takes it off the sidebar again.
    Q_INVOKABLE void remember(const QString &host);
    Q_INVOKABLE void forget(const QString &host);

    // Lets go of the drive at `path`: ejects it when it can be (so a stick
    // or card is safe to pull out), otherwise unmounts it. Off the UI
    // thread, in GIO; `failed` says why when it can't.
    Q_INVOKABLE void eject(const QString &path);
    // The hosts recorded, most recently opened first.
    QStringList opened() const;
    // The peers among `opened`, in its order, at most `limit` of them.
    static QVariantList onlineOf(const QStringList &opened, const QVariantList &peers, int limit = 6);

    static QVariantList parseTailscale(const QByteArray &json);
    static QString parseDropboxStatus(const QByteArray &output);

    struct Mount {
        QString root;
        QString device;
        QString type;
        QString label;
        // What GIO can do with it, and the bytes left on it for the user;
        // -1 when that isn't known.
        bool canEject = false;
        bool canUnmount = false;
        qint64 free = -1;
    };
    // The physical drives among the mounts, one entry per drive: removable
    // and extra internal disks, never the system's own partitions. Each
    // says how it's let go of ("release": "eject", "unmount", or empty when
    // it can't be) and how much is free.
    static QVariantList physicalDrives(const QList<Mount> &mounts, const QString &systemDevice);

signals:
    void changed();
    // A drive is about to be let go of: whatever shows a folder on it
    // leaves, so Folio itself doesn't keep it busy.
    void releasing(const QString &root);
    void failed(const QString &message);

private:
    void refreshPlaces();
    void refreshDevices();
    void refreshPinned();
    QString bookmarksPath() const;
    QStringList readBookmarkLines() const;
    bool writeBookmarkLines(const QStringList &lines);
    // A machine opened, and when, as ISO 8601 text, which keeps the
    // settings file readable.
    struct Opened {
        QString host;
        QString when;
    };
    QString settingsPath() const;
    QList<Opened> readOpened() const;
    void writeOpened(const QList<Opened> &opened);
    void refreshMachines();
    void pollTailscale();
    void pollDropbox();
    QString userDirectory(int location, const QString &fallback) const;

    QString m_home;
    bool m_realHome;
    QVariantList m_places, m_pinned, m_views, m_devices, m_peers, m_machines;
    QString m_dropboxStatus;
    QProcess m_tailscale, m_dropbox;
    QTimer m_poll;
};
