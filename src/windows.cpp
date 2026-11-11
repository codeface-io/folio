#include "windows.h"

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>

#include "backend.h"
#include "fileops.h"

Windows::Windows(QQmlEngine *engine, Plugins *plugins, QObject *parent)
    : QObject(parent), m_engine(engine), m_plugins(plugins), m_component(engine, QUrl("qrc:/Main.qml")),
      m_pane(engine, QUrl("qrc:/FolderPane.qml")) {
    // Main.qml calls windows.openSplit() and windows.closeSplit().
    engine->rootContext()->setContextProperty("windows", this);
}

Windows::Pane Windows::makePane(QQmlComponent &component, QQmlContext *parent) {
    // A context per pane overrides `backend` and `fileOps`; everything else
    // comes from the contexts above it, shared.
    Pane pane;
    pane.context = new QQmlContext(parent, this);
    pane.backend = new Backend({}, this);
    pane.backend->setPlugins(m_plugins);
    pane.fileOps = new FileOps(this);
    pane.context->setContextProperty("backend", pane.backend);
    pane.context->setContextProperty("fileOps", pane.fileOps);
    pane.item = component.create(pane.context);
    if (!pane.item) {
        qWarning().noquote() << component.errorString();
        release(pane);
        return {};
    }
    connect(pane.backend, &Backend::newWindowRequested, this, [this](const QString &path) { open(path); });
    return pane;
}

Backend *Windows::open(const QString &folder, const QString &select) {
    Pane main = makePane(m_component, m_engine->rootContext());
    auto *window = qobject_cast<QQuickWindow *>(main.item);
    if (!window) {
        release(main);
        return nullptr;
    }
    m_windows.prepend({main, {}});

    // Whichever window was used last answers "Show in folder".
    connect(window, &QQuickWindow::activeChanged, this, [this, window] {
        if (!window->isActive())
            return;
        for (int i = 0; i < m_windows.size(); ++i)
            if (m_windows[i].main.item == window)
                m_windows.move(i, 0);
    });
    connect(window, &QQuickWindow::closing, this, [this, window] { close(window); });

    if (select.isEmpty())
        main.backend->navigate(folder);
    else
        main.backend->showItem(folder + "/" + select);
    return main.backend;
}

Backend *Windows::latest() const {
    return m_windows.isEmpty() ? nullptr : m_windows.first().main.backend;
}

void Windows::close(QObject *window) {
    for (int i = 0; i < m_windows.size(); ++i) {
        if (m_windows[i].main.item != window)
            continue;
        Window w = m_windows.takeAt(i);
        // The right pane goes before the window holding it. Releasing a
        // backend also closes its preview, if one is open.
        release(w.split);
        release(w.main);
        return;
    }
}

Windows::Window *Windows::find(QObject *window) {
    for (Window &w : m_windows)
        if (w.main.item == window)
            return &w;
    return nullptr;
}

QObject *Windows::openSplit(QObject *window, const QString &folder) {
    Window *w = find(window);
    if (!w)
        return nullptr;
    if (!w->split.item) {
        // Inside the window's context, with its own backend and file work.
        w->split = makePane(m_pane, w->main.context);
        if (!w->split.item)
            return nullptr;
        // Owned here, not by QML's garbage collector, though QML holds it.
        QQmlEngine::setObjectOwnership(w->split.item, QQmlEngine::CppOwnership);
        w->split.backend->navigate(folder);
    }
    return w->split.item;
}

void Windows::closeSplit(QObject *window) {
    if (Window *w = find(window))
        release(w->split);
}

void Windows::release(Pane &pane) {
    // The pane goes first, then what its QML was bound to.
    if (pane.item)
        pane.item->deleteLater();
    if (pane.backend)
        pane.backend->deleteLater();
    if (pane.fileOps)
        pane.fileOps->deleteLater();
    if (pane.context)
        pane.context->deleteLater();
    pane = {};
}
