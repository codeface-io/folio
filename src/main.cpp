// folio — Omarchy's file manager: airy, keyboard-first, and following the theme.

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>
#include <cstdio>

#include "filemanager1.h"
#include "foldermodel.h"
#include "places.h"
#include "previewer.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"
#include "windows.h"

// Accepts a path, a file:// URI (how launchers and xdg-open pass folders),
// or nothing for home. A file opens its folder with the file selected.
static QString startPath(const QStringList &args, QString *select) {
    if (args.size() < 2)
        return QDir::homePath();
    const QString arg = args.at(1);
    const QUrl url(arg);
    const QString path = url.isLocalFile() ? url.toLocalFile() : QFileInfo(arg).absoluteFilePath();
    const QFileInfo info(path);
    if (info.isDir())
        return info.absoluteFilePath();
    if (info.exists()) {
        *select = info.fileName();
        return info.absolutePath();
    }
    return QDir::homePath();
}

// Omarchy points fontconfig's monospace at the font picked in its menu, so
// asking for "monospace" follows the user's choice. The size is Folio's own
// (`fontSize` in folio.conf).
static void setUp(QGuiApplication &app) {
    app.setApplicationName("folio");
    // Settings (the chosen view, the text size) live in ~/.config/folio/folio.conf.
    app.setOrganizationName("folio");
    app.setWindowIcon(QIcon::fromTheme("folio"));
    QFont font("monospace");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(Theme::configuredFontSize());
    app.setFont(font);
    QQuickStyle::setStyle("Material");
}

// `folio preview [--hidden] [--sort name|kind|modified|oldest|size] [--follow] FILE`:
// the large look at one file, stepping through its folder. Its own process,
// so a file that crashes a decoder only closes this window.
static int preview(QGuiApplication &app) {
    QStringList args = app.arguments().mid(2);
    bool showHidden = false;
    bool follow = false;
    QString sort = "name";
    QString path;
    while (!args.isEmpty()) {
        const QString arg = args.takeFirst();
        if (arg == "--hidden")
            showHidden = true;
        else if (arg == "--follow")
            follow = true;
        else if (arg == "--sort" && !args.isEmpty())
            sort = args.takeFirst();
        else
            path = QUrl(arg).isLocalFile() ? QUrl(arg).toLocalFile() : arg;
    }
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        fprintf(stderr, "usage: folio preview [--hidden] [--sort name|kind|modified|oldest|size] [--follow] FILE\n");
        return 1;
    }

    // Its own app id, so window rules can float it the way Omarchy floats
    // Sushi's previewer.
    app.setDesktopFileName("folio-preview");

    Translator translator;
    app.installTranslator(&translator);
    Theme theme;
    Thumbnails thumbnails;
    Previewer previewer;
    previewer.start(path, showHidden, sort);
    if (follow)
        previewer.connectToFolio();

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.addImageProvider("thumb", new ThumbnailProvider(&thumbnails));
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("previewer", &previewer);
    engine.load(QUrl("qrc:/PreviewWindow.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    setUp(app);
    if (app.arguments().value(1) == "preview")
        return preview(app);

    // Matches the window to folio.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("folio");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Places places;
    Thumbnails thumbnails;

    QString select;
    const QString start = startPath(app.arguments(), &select);

    QQmlEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.addImageProvider("thumb", new ThumbnailProvider(&thumbnails));
    // Creatable too: the column view lists each folder on the way here.
    qmlRegisterType<FolderModel>("Folio", 1, 0, "FolderModel");
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("places", &places);

    Windows windows(&engine);
    if (!windows.open(start, select))
        return 1;
    FileManager1::registerOn(&windows);

    return app.exec();
}
