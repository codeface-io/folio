#pragma once

#include <QList>
#include <QObject>
#include <QQmlComponent>

class Backend;
class FileOps;
class Plugins;
class QQmlContext;
class QQmlEngine;

// Folio's windows, all in one process. Each has its own folder, history,
// preview and file work (a Backend and a FileOps); they share the theme,
// the sidebar, the thumbnail cache and the compiled interface, so Ctrl+N
// opens a window in the time it takes to draw one rather than to start an
// app.
class Windows : public QObject {
    Q_OBJECT

public:
    explicit Windows(QQmlEngine *engine, Plugins *plugins, QObject *parent = nullptr);

    // Opens a window on a folder, or on a file's folder with it selected.
    // Null if the interface failed to load (the errors are on stderr).
    Backend *open(const QString &folder, const QString &select = {});

    // The window used most recently, for "Show in folder" from other apps.
    Backend *latest() const;

    // Splits a window: a second folder pane, with its own folder, history
    // and selection, starting at `folder`. Returns the pane, for Main.qml to
    // put on the right.
    Q_INVOKABLE QObject *openSplit(QObject *window, const QString &folder);
    Q_INVOKABLE void closeSplit(QObject *window);

private:
    struct Pane {
        QObject *item = nullptr;
        Backend *backend = nullptr;
        FileOps *fileOps = nullptr;
        QQmlContext *context = nullptr;
    };
    // The window's own pane is the window itself (its item is the
    // QQuickWindow); the split is empty until Ctrl+Alt+N.
    struct Window {
        Pane main;
        Pane split;
    };
    Pane makePane(QQmlComponent &component, QQmlContext *parent);
    Window *find(QObject *window);
    static void release(Pane &pane);
    void close(QObject *window);

    QQmlEngine *m_engine;
    Plugins *m_plugins;
    QQmlComponent m_component;
    QQmlComponent m_pane;
    // Most recently used first.
    QList<Window> m_windows;
};
