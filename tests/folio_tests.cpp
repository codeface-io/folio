#include <QClipboard>
#include <QDir>
#include <QMimeData>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <memory>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtConcurrent>
#include <QtTest>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "fileops.h"
#include "foldermodel.h"
#include "places.h"
#include "previewer.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

// Puts XDG_DATA_HOME, where the trash and recent files live, in `path` for
// as long as the returned guard does.
[[nodiscard]] static auto dataHomeAt(const QString &path)
{
    const QByteArray old = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", QFile::encodeName(path));
    return qScopeGuard([old] {
        if (old.isNull())
            qunsetenv("XDG_DATA_HOME");
        else
            qputenv("XDG_DATA_HOME", old);
    });
}

// A temporary folder on another drive than QTemporaryDir's, for what has to
// cross drives: beside the tests, when they're on another drive. Null when
// they aren't.
static std::unique_ptr<QTemporaryDir> elsewhere()
{
    auto dir = std::make_unique<QTemporaryDir>(QDir::current().filePath("drive-XXXXXX"));
    if (!dir->isValid() || FileOps::sameDrive(dir->path(), QDir::tempPath()))
        return {};
    return dir;
}

// `tailscale status --json`, trimmed to what Folio reads, for a made-up
// tailnet: three desktops online, one offline, and a phone.
static QByteArray tailnet()
{
    return R"({"Peer":{
        "a":{"HostName":"studio","DNSName":"studio.example.ts.net.","OS":"linux","Online":true},
        "b":{"HostName":"nas","DNSName":"nas.example.ts.net.","OS":"linux","Online":false},
        "c":{"HostName":"Conference-Room","DNSName":"conference-room.example.ts.net.","OS":"macOS","Online":true},
        "d":{"HostName":"phone","DNSName":"phone.example.ts.net.","OS":"android","Online":true},
        "e":{"HostName":"build","DNSName":"build.example.ts.net.","OS":"windows","Online":true}}})";
}

class FolioTests : public QObject {
    Q_OBJECT

private slots:
    void folderListsDirectoriesFirstWithoutHidden()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("zeta");
        write(dir.filePath("alpha.md"), "# hi");
        write(dir.filePath("photo10.jpg"), "x");
        write(dir.filePath("photo9.jpg"), "x");
        write(dir.filePath(".secret"), "x");

        QString error;
        const auto entries = FolderModel::list(dir.path(), false, "name", &error);
        QVERIFY(error.isEmpty());
        QStringList names;
        for (const auto &e : entries) names << e.name;
        // Folders first, then natural order, so photo9 comes before photo10.
        QCOMPARE(names, QStringList({"zeta", "alpha.md", "photo9.jpg", "photo10.jpg"}));
        QCOMPARE(entries.at(0).kind, QString("folder"));
        QCOMPARE(entries.at(1).kind, QString("text"));
        QCOMPARE(entries.at(2).kind, QString("image"));
        QCOMPARE(entries.at(2).extension, QString("JPG"));

        QCOMPARE(FolderModel::list(dir.path(), true, "name", &error).size(), 5);
    }

    void folderReportsMissingFolders()
    {
        QString error;
        QVERIFY(FolderModel::list("/definitely/not/here", false, "name", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void folderSummaryCountsAndPeeks()
    {
        QTemporaryDir dir;
        write(dir.filePath("a.jpg"), "x");
        write(dir.filePath("b.png"), "x");
        write(dir.filePath("c.png"), "x");
        write(dir.filePath("notes.txt"), "x");
        write(dir.filePath(".hidden"), "x");
        FolderModel::Entry entry;
        entry.path = dir.path();
        FolderModel::summarize(entry, false);
        QCOMPARE(entry.itemCount, 4);
        QCOMPARE(entry.peek.size(), 2);
        // The folder counts just count, the hidden only when they show.
        QCOMPARE(FolderModel::itemCount(dir.path(), false), 4);
        QCOMPARE(FolderModel::itemCount(dir.path(), true), 5);
    }

    void folderModelLoadsInTheBackground()
    {
        QTemporaryDir dir;
        write(dir.filePath("one.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.get(0).value("name").toString(), QString("one.txt"));
        QCOMPARE(model.indexOf("one.txt"), 0);
    }

    void folderSearchesEveryFolderBelow()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Photos/2024");
        QDir(dir.path()).mkpath(".cache");
        write(dir.filePath("beach notes.txt"), "x");
        write(dir.filePath("Photos/2024/Beach.jpg"), "x");
        write(dir.filePath("Photos/city.jpg"), "x");
        write(dir.filePath(".cache/beach.tmp"), "x");

        auto names = [&](bool hidden) {
            QStringList out;
            for (const auto &e : FolderModel::search(dir.path(), "beach", hidden, "name"))
                out << e.name;
            return out.join(", ");
        };
        // Named by where they are, ignoring case, hidden ones only when shown.
        QCOMPARE(names(false), QString("beach notes.txt, Photos/2024/Beach.jpg"));
        QCOMPARE(names(true), QString(".cache/beach.tmp, beach notes.txt, Photos/2024/Beach.jpg"));
        // Stopped, it gives nothing rather than half an answer.
        QVERIFY(FolderModel::search(dir.path(), "beach", false, "name", [] { return true; }).isEmpty());

        // In the model, a query swaps the folder for its matches; clearing
        // it, or going elsewhere, brings the folder back.
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        model.setQuery("BEACH");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.get(1).value("path").toString(), dir.filePath("Photos/2024/Beach.jpg"));
        model.setQuery("");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.indexOf("Photos"), 0);
        model.setQuery("city");
        QVERIFY(loaded.wait(3000));
        model.setPath(dir.filePath("Photos"));
        QVERIFY(model.query().isEmpty());
    }

    void folderSearchSkipsSymlinkedFolders()
    {
        // A link back up the tree would otherwise be walked round and round.
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("real");
        write(dir.filePath("real/match.txt"), "x");
        QVERIFY(QFile::link(dir.path(), dir.filePath("loop")));
        const auto found = FolderModel::search(dir.path(), "match", false, "name");
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().name, QString("real/match.txt"));
    }

    void folderModelSwapsFoldersWithoutAnEmptyFrame()
    {
        QTemporaryDir a, b;
        write(a.filePath("one.txt"), "x");
        write(b.filePath("two.txt"), "x");
        write(b.filePath("three.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(a.path());
        QVERIFY(loaded.wait(3000));

        // Moving on keeps the old entries until the new ones are in.
        QSignalSpy counts(&model, &FolderModel::countChanged);
        model.setPath(b.path());
        QCOMPARE(model.count(), 1);
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        QCOMPARE(counts.count(), 1); // straight from one to two, never zero

        // A column with no folder clears at once.
        model.setPath(QString());
        QCOMPARE(model.count(), 0);

        // Going back to a folder just seen shows it in the same step, and
        // the background re-read leaves it alone when nothing changed.
        loaded.clear();
        model.setPath(a.path());
        QCOMPARE(model.count(), 1);
        QCOMPARE(loaded.count(), 1);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QVERIFY(loaded.wait(3000));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(model.count(), 1);
    }

    void folderModelCountsFoldersAfterAQuickReread()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Photos");
        QDir(dir.path()).mkpath("Music");
        write(dir.filePath("Photos/a.jpg"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        // A re-read before the counts came in still ends with them.
        model.reload();
        QVERIFY(loaded.wait(3000));
        QTRY_COMPARE(model.get(0).value("itemCount").toInt(), 0);
        QTRY_COMPARE(model.get(1).value("itemCount").toInt(), 1);
    }

    void folderModelForgetsAFolderLeftWhileLoading()
    {
        QTemporaryDir dir;
        write(dir.filePath("one.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(model.loading());
        // Emptied before the listing lands, the column stays empty.
        model.setPath(QString());
        QVERIFY(!model.loading());
        QVERIFY(!loaded.wait(200));
        QCOMPARE(model.count(), 0);
    }

    void folderModelStopsReadingWhatsBeenOvertaken()
    {
        // A big folder left before it's been read, or a search of it, stops
        // being read instead of holding on to a thread of the pool that the
        // next listing, the folder counts and file transfers all wait for.
        // With a pool of one thread, the next folder waits for the overtaken
        // read to stop, so it shows in a fraction of the time the read takes.
        QTemporaryDir big, small, other;
        for (int i = 0; i < 20000; ++i)
            write(big.filePath(QString("%1.txt").arg(i)), "x");
        write(small.filePath("a.txt"), "x");
        write(other.filePath("b.txt"), "x");
        QElapsedTimer timer;
        timer.start();
        FolderModel::list(big.path(), false, "name", nullptr);
        const qint64 listing = timer.restart();
        FolderModel::search(big.path(), "none", false, "name");
        const qint64 searching = timer.elapsed();

        QThreadPool *pool = QThreadPool::globalInstance();
        pool->waitForDone();
        const int threads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        const auto restore = qScopeGuard([&] { pool->setMaxThreadCount(threads); });
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);

        model.setPath(big.path());
        timer.restart();
        model.setPath(small.path());
        QVERIFY(loaded.wait(5000));
        QVERIFY(timer.elapsed() < listing / 2);

        model.setPath(big.path());
        QVERIFY(loaded.wait(5000));
        model.setQuery("none");
        timer.restart();
        model.setPath(other.path());
        QVERIFY(loaded.wait(5000));
        QVERIFY(timer.elapsed() < searching / 2);
    }

    void folderModelTellsCopiedFoldersApart()
    {
        // A copy (cp -a, rsync -a) lists exactly like its original, but its
        // items are its own.
        QTemporaryDir a, b;
        const QDateTime when = QDateTime::fromSecsSinceEpoch(1700000000);
        for (const QTemporaryDir *dir : {&a, &b}) {
            QFile file(dir->filePath("report.txt"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("same");
            file.flush();
            QVERIFY(file.setFileTime(when, QFileDevice::FileModificationTime));
        }
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(a.path());
        QVERIFY(loaded.wait(3000));
        model.setPath(b.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.get(0).value("path").toString(), b.filePath("report.txt"));
    }

    void fileOpsNeverOverwrite()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.tar.gz"), "x");
        QDir(dir.path()).mkdir("Photos");
        QCOMPARE(FileOps::freeName(dir.path(), "fresh.txt"), QString("fresh.txt"));
        QCOMPARE(FileOps::freeName(dir.path(), "notes.tar.gz"), QString("notes (copy).tar.gz"));
        write(dir.filePath("notes (copy).tar.gz"), "x");
        QCOMPARE(FileOps::freeName(dir.path(), "notes.tar.gz"), QString("notes (copy 2).tar.gz"));
        QCOMPARE(FileOps::freeName(dir.path(), "Photos"), QString("Photos (copy)"));
        // The extension starts at the last dot, so dots in the name stay.
        QCOMPARE(FileOps::extensionStart("Report v1.2.pdf", false), 11);
        write(dir.filePath("Report v1.2.pdf"), "x");
        QCOMPARE(FileOps::freeName(dir.path(), "Report v1.2.pdf"), QString("Report v1.2 (copy).pdf"));
    }

    void fileOpsRenameNeverOverwrites()
    {
        // The rename itself refuses to overwrite. Nothing checks for the new
        // name first, since on drives that ignore case it "exists" when only
        // its case changes; the tests' drive heeds case, though, so the case
        // change at the end is an ordinary rename here.
        QTemporaryDir dir;
        write(dir.filePath("IMG.JPG"), "photo");
        write(dir.filePath("notes.txt"), "notes");
        QDir(dir.path()).mkdir("Album");
        QDir(dir.path()).mkdir("Empty");
        FileOps ops;
        QCOMPARE(ops.rename(dir.filePath("IMG.JPG"), "notes.txt"), QString("notes.txt already exists here."));
        QCOMPARE(ops.rename(dir.filePath("Album"), "Empty"), QString("Empty already exists here."));
        QFile kept(dir.filePath("notes.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("notes"));
        QVERIFY(QFileInfo(dir.filePath("Album")).isDir());
        QCOMPARE(ops.rename(dir.filePath("IMG.JPG"), "img.jpg"), QString());
        QVERIFY(QFile::exists(dir.filePath("img.jpg")));
    }

    void fileOpsCopyAndMove()
    {
        QTemporaryDir from, to;
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        write(from.filePath("todo.txt"), "list");
        write(to.filePath("todo.txt"), "already here");

        QString error;
        QStringList made = FileOps::transfer({from.filePath("album"), from.filePath("todo.txt")}, to.path(), FileOps::Copy, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(made, QStringList({"album", "todo (copy).txt"}));
        QVERIFY(QFile::exists(to.filePath("album/inner/a.jpg")));
        QFile kept(to.filePath("todo.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("already here"));
        QVERIFY(QFile::exists(from.filePath("todo.txt"))); // copied, not moved

        made = FileOps::transfer({from.filePath("todo.txt")}, to.path(), FileOps::Move, &error);
        QCOMPARE(made, QStringList({"todo (copy 2).txt"}));
        QVERIFY(!QFile::exists(from.filePath("todo.txt")));

        // A folder can't go inside itself.
        error.clear();
        FileOps::transfer({to.filePath("album")}, to.filePath("album/inner"), FileOps::Move, &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(QFile::exists(to.filePath("album/inner/a.jpg")));
        // Nor through a link to a folder inside it, which would copy it into
        // its own copy until the drive was full.
        QVERIFY(QFile::link(to.filePath("album/inner"), from.filePath("inner")));
        error.clear();
        FileOps::transfer({to.filePath("album")}, from.filePath("inner"), FileOps::Copy, &error);
        QCOMPARE(error, QString("Can't put album inside itself."));
        QVERIFY(!QFile::exists(to.filePath("album/inner/album")));
    }

    void fileOpsMoveAcrossDrivesLeavesWhatArrivesMeanwhile()
    {
        // Across drives, a move copies, then removes the original. What
        // turns up in a folder while the rest of it copies was never copied,
        // so it stays where it is.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir from;
        QDir(from.path()).mkdir("box");
        // Copying a pipe waits until whoever writes to it is done, which
        // holds the move after it has looked in the folder.
        const QByteArray pipe = QFile::encodeName(from.filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        QString error;
        QFuture<QStringList> moved = QtConcurrent::run([&] {
            return FileOps::transfer({from.filePath("box")}, other->path(), FileOps::Move, &error);
        });
        int writer = -1;
        // Even when a check fails, the move is let go of and finishes
        // before what it uses is gone.
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            moved.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        write(from.filePath("box/new.txt"), "new");
        close(std::exchange(writer, -1));
        QCOMPARE(moved.result(), QStringList({"box"}));
        QVERIFY(QFile::exists(from.filePath("box/new.txt")));
        QCOMPARE(error, QString("Copied box, but couldn't remove the original."));
        QVERIFY(!QFileInfo::exists(from.filePath("box/pipe")));
        QVERIFY(QFileInfo::exists(other->filePath("box/pipe")));

        // With nothing new, all of it goes.
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        error.clear();
        QCOMPARE(FileOps::transfer({from.filePath("album")}, other->path(), FileOps::Move, &error), QStringList({"album"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(from.filePath("album")));
        QVERIFY(QFile::exists(other->filePath("album/inner/a.jpg")));
    }

    void fileOpsSayWhenAMoveAcrossDrivesStopsPartway()
    {
        // Each original goes as it's copied, so a move that stops partway
        // (a full USB stick) has already taken some of the folder. What
        // arrived is shown and said to be only part, not left looking like
        // a failed copy that's safe to delete.
        if (geteuid() == 0)
            QSKIP("Root can read every file.");
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir from;
        QDir(from.path()).mkpath("box/a");
        QDir(from.path()).mkpath("box/z");
        write(from.filePath("box/a/1.txt"), "1");
        write(from.filePath("box/m.txt"), "m");
        write(from.filePath("box/z/2.txt"), "2");
        QVERIFY(QFile::setPermissions(from.filePath("box/m.txt"), QFile::WriteOwner));
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("box")}, other->path(), FileOps::Move, &error), QStringList({"box"}));
        QCOMPARE(error, QString("Moved only part of box: Couldn't copy m.txt."));
        QVERIFY(QFile::exists(other->filePath("box/a/1.txt")));
        QVERIFY(!QFile::exists(from.filePath("box/a/1.txt")));
        QVERIFY(QFile::exists(from.filePath("box/z/2.txt")));
    }

    void fileOpsCopyLinksAsWritten()
    {
        // A link relative to its folder points into the copy, not back at
        // the original.
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("project");
        write(from.filePath("project/data.txt"), "data");
        QVERIFY(QFile::link("data.txt", from.filePath("project/link")));
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("project")}, to.path(), FileOps::Copy, &error), QStringList({"project"}));
        QCOMPARE(QFileInfo(to.filePath("project/link")).readSymLink(), QString("data.txt"));
    }

    void fileOpsMoveLinksAcrossDrivesAsLinks()
    {
        // A link moved or restored to another drive arrives as the link it
        // was, not as a copy of what it leads to. One dropped from another
        // drive is copied, wherever it leads.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir here, data, folder;
        write(here.filePath("data.txt"), "data");
        QVERIFY(QFile::link("data.txt", here.filePath("relative")));
        QVERIFY(QFile::link(here.filePath("data.txt"), here.filePath("absolute")));
        QString error;
        QCOMPARE(FileOps::transfer({here.filePath("relative"), here.filePath("absolute")}, other->path(), FileOps::Move, &error),
                 QStringList({"relative", "absolute"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(other->filePath("relative")).readSymLink(), QString("data.txt"));
        QCOMPARE(QFileInfo(other->filePath("absolute")).readSymLink(), here.filePath("data.txt"));
        QVERIFY(!QFileInfo(here.filePath("relative")).isSymLink());

        const auto restore = dataHomeAt(data.path());
        QDir().mkpath(data.filePath("Trash/files"));
        QDir().mkpath(data.filePath("Trash/info"));
        QVERIFY(QFile::link(here.filePath("data.txt"), data.filePath("Trash/files/trashed")));
        write(data.filePath("Trash/info/trashed.trashinfo"), "[Trash Info]\nPath=" + other->filePath("trashed").toUtf8() + "\n");
        QCOMPARE(FileOps::restoreOne(data.filePath("Trash/files/trashed"), &error), other->filePath("trashed"));
        QCOMPARE(QFileInfo(other->filePath("trashed")).readSymLink(), here.filePath("data.txt"));

        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.dropInto({QUrl::fromLocalFile(other->filePath("absolute"))}, folder.path());
        QVERIFY(done.wait(3000));
        QCOMPARE(QFileInfo(folder.filePath("absolute")).readSymLink(), here.filePath("data.txt"));
        QVERIFY(QFileInfo(other->filePath("absolute")).isSymLink());
    }

    void fileOpsCopyFolderPermissions()
    {
        // A private folder stays private, and a read-only one is filled
        // before it's made read-only.
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("private");
        QDir(from.path()).mkdir("locked");
        write(from.filePath("locked/a.txt"), "a");
        const auto all = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        QVERIFY(QFile::setPermissions(from.filePath("private"), all));
        QVERIFY(QFile::setPermissions(from.filePath("locked"), QFile::ReadOwner | QFile::ExeOwner));
        const auto unlock = qScopeGuard([&] {
            QFile::setPermissions(from.filePath("locked"), all);
            QFile::setPermissions(to.filePath("locked"), all);
        });
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("private"), from.filePath("locked")}, to.path(), FileOps::Copy, &error),
                 QStringList({"private", "locked"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(to.filePath("locked/a.txt")));
        QCOMPARE(QFileInfo(to.filePath("private")).permissions(), QFileInfo(from.filePath("private")).permissions());
        QCOMPARE(QFileInfo(to.filePath("locked")).permissions(), QFileInfo(from.filePath("locked")).permissions());
    }

    void fileOpsCopyOnlyWhatCanBeRead()
    {
        // A folder that can't be read would look empty, so copying it fails
        // rather than making an empty copy.
        if (geteuid() == 0)
            QSKIP("Root can read every folder.");
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("secret");
        write(from.filePath("secret/a.txt"), "a");
        QVERIFY(QFile::setPermissions(from.filePath("secret"), QFile::WriteOwner | QFile::ExeOwner));
        const auto unlock = qScopeGuard([&] {
            QFile::setPermissions(from.filePath("secret"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QString error;
        QVERIFY(FileOps::transfer({from.filePath("secret")}, to.path(), FileOps::Copy, &error).isEmpty());
        QCOMPARE(error, QString("Couldn't read secret."));
        QVERIFY(!QFile::exists(to.filePath("secret")));

        // A copy that stops at a file that can't be read is left as private
        // as the folder it came from.
        QDir(from.path()).mkpath("private/keys");
        write(from.filePath("private/z.txt"), "z");
        QVERIFY(QFile::setPermissions(from.filePath("private"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::setPermissions(from.filePath("private/z.txt"), QFile::WriteOwner));
        QVERIFY(FileOps::transfer({from.filePath("private")}, to.path(), FileOps::Copy, &error).isEmpty());
        QCOMPARE(error, QString("Couldn't copy z.txt."));
        QCOMPARE(QFileInfo(to.filePath("private")).permissions(), QFileInfo(from.filePath("private")).permissions());
    }

    void fileOpsClipboardFormats()
    {
        std::unique_ptr<QMimeData> data(FileOps::mimeDataFor({"/tmp/a b.txt", "/tmp/c.txt"}, true));
        QCOMPARE(data->text(), QString("/tmp/a b.txt\n/tmp/c.txt"));
        QVERIFY(data->data("x-special/gnome-copied-files").startsWith("cut\nfile:///tmp/a%20b.txt"));
        bool cut = false;
        QCOMPARE(FileOps::pathsFrom(data.get(), &cut), QStringList({"/tmp/a b.txt", "/tmp/c.txt"}));
        QVERIFY(cut);

        // From another app that only offers a URI list: a copy.
        QMimeData plain;
        plain.setUrls({QUrl::fromLocalFile("/tmp/x.png")});
        QCOMPARE(FileOps::pathsFrom(&plain, &cut), QStringList({"/tmp/x.png"}));
        QVERIFY(!cut);
    }

    void fileOpsCompressAcrossFolders()
    {
        // Search results and Recents can hold items from different folders:
        // the archive goes in the folder holding them all, with their paths.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Compressing needs bsdtar.");
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("a/inner");
        QDir(dir.path()).mkpath("b");
        write(dir.filePath("a/inner/x.txt"), "x");
        write(dir.filePath("b/y.txt"), "y");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        const auto compress = [&](const QStringList &names) {
            QStringList paths;
            for (const QString &name : names)
                paths << dir.filePath(name);
            ops.compress(paths);
            if (!done.wait(5000))
                return QStringList();
            const QList<QVariant> made = done.takeFirst();
            QProcess list;
            list.start("bsdtar", {"-tf", QDir(made[0].toString()).filePath(made[1].toString())});
            list.waitForFinished();
            return QStringList({made[0].toString(), made[1].toString()})
                 + QString::fromUtf8(list.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        };
        QCOMPARE(compress({"a/inner/x.txt", "b/y.txt"}), QStringList({dir.path(), "Archive.zip", "a/inner/x.txt", "b/y.txt"}));
        // A folder beside a file inside it (as search results list them):
        // the archive holds the folder, and the file once, within it.
        QCOMPARE(compress({"a/inner/x.txt", "a/inner"}), QStringList({dir.filePath("a"), "inner.zip", "inner/", "inner/x.txt"}));
    }

    void fileOpsCompressNeverOverwritesAnArchive()
    {
        // Two windows compressing into one folder at once each get an
        // archive of their own, and one that fails takes only its own with
        // it, leaving nothing half-written behind.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Compressing needs bsdtar.");
        QTemporaryDir dir;
        for (const char *name : {"a.txt", "b.txt", "c.txt"})
            write(dir.filePath(name), name);
        FileOps one, two, three;
        QSignalSpy doneOne(&one, &FileOps::done), doneTwo(&two, &FileOps::done), failed(&three, &FileOps::failed);
        one.compress({dir.filePath("a.txt"), dir.filePath("b.txt")});
        two.compress({dir.filePath("b.txt"), dir.filePath("c.txt")});
        three.compress({dir.filePath("a.txt"), dir.filePath("gone.txt")});
        QTRY_VERIFY(doneOne.count() == 1 && doneTwo.count() == 1 && failed.count() == 1);
        const auto members = [&](const QSignalSpy &done) {
            QProcess list;
            list.start("bsdtar", {"-tf", dir.filePath(done.first().at(1).toString())});
            list.waitForFinished();
            return QString::fromUtf8(list.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        };
        QCOMPARE(members(doneOne), QStringList({"a.txt", "b.txt"}));
        QCOMPARE(members(doneTwo), QStringList({"b.txt", "c.txt"}));
        QStringList names{doneOne.first().at(1).toString(), doneTwo.first().at(1).toString()};
        names.sort();
        QCOMPARE(names, QStringList({"Archive (copy).zip", "Archive.zip"}));
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name),
                 QStringList({"Archive (copy).zip", "Archive.zip", "a.txt", "b.txt", "c.txt"}));
    }

    void fileOpsTakeAFolderOnceWithWhatsInIt()
    {
        // Search results can list a folder and what's in it. The folder
        // takes those along, so they aren't copied twice, found gone once
        // it has moved, deleted twice, or restored without a note of their
        // own.
        QTemporaryDir from, copies, moves, trash;
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        const QStringList both{from.filePath("album"), from.filePath("album/inner/a.jpg")};
        QString error;
        QCOMPARE(FileOps::transfer(both, copies.path(), FileOps::Copy, &error), QStringList({"album"}));
        QVERIFY(!QFile::exists(copies.filePath("a.jpg")));
        QCOMPARE(FileOps::transfer(both, moves.path(), FileOps::Move, &error), QStringList({"album"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(moves.filePath("album/inner/a.jpg")));

        QDir(trash.path()).mkpath("files/album");
        write(trash.filePath("files/album/a.jpg"), "picture");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.deleteForever({trash.filePath("files/album"), trash.filePath("files/album/a.jpg")});
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QVERIFY(!QFileInfo::exists(trash.filePath("files/album")));

        const auto restore = dataHomeAt(trash.path());
        QDir(trash.path()).mkpath("Trash/files/album");
        QDir(trash.path()).mkpath("Trash/info");
        write(trash.filePath("Trash/files/album/a.jpg"), "picture");
        write(trash.filePath("Trash/info/album.trashinfo"), "[Trash Info]\nPath=" + copies.filePath("restored").toUtf8() + "\n");
        ops.restore({trash.filePath("Trash/files/album"), trash.filePath("Trash/files/album/a.jpg")});
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QVERIFY(QFile::exists(copies.filePath("restored/a.jpg")));
    }

    void fileOpsKeepWhatsChosenBeyondALink()
    {
        // A link is copied as a link, not gone into, so something chosen
        // through it (Recents lists both) is still copied in its own right.
        QTemporaryDir from, to;
        QDir(from.path()).mkpath("real/inner");
        write(from.filePath("real/inner/a.txt"), "a");
        QVERIFY(QFile::link("real", from.filePath("link")));
        QDir(from.path()).mkdir("box");
        QVERIFY(QFile::link("../real", from.filePath("box/through")));
        QString error;
        const QStringList chosen{from.filePath("link"), from.filePath("link/inner/a.txt"),
                                 from.filePath("box"), from.filePath("box/through/inner/a.txt")};
        QCOMPARE(FileOps::transfer(chosen, to.path(), FileOps::Copy, &error),
                 QStringList({"a.txt", "a (copy).txt", "link", "box"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo(to.filePath("link")).isSymLink());
        QVERIFY(QFileInfo(to.filePath("box/through")).isSymLink());

        // Moved, what's beyond the link goes before the link does, while
        // it can still be reached through it.
        QTemporaryDir moves;
        QCOMPARE(FileOps::transfer({from.filePath("link"), from.filePath("link/inner/a.txt")}, moves.path(), FileOps::Move, &error),
                 QStringList({"a.txt", "link"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(moves.filePath("a.txt")));
        QVERIFY(QFileInfo(moves.filePath("link")).isSymLink());
    }

    void fileOpsRestoreAcrossDrivesLeavesWhatArrivesMeanwhile()
    {
        // Restoring across drives moves the way a move across drives does:
        // what turns up in the trashed folder meanwhile stays in the trash,
        // with the note that can bring it back.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir home;
        const auto restore = dataHomeAt(home.path());
        const QString trash = home.filePath("Trash");
        QDir().mkpath(trash + "/files/box");
        QDir().mkpath(trash + "/info");
        write(trash + "/info/box.trashinfo", "[Trash Info]\nPath=" + other->filePath("box").toUtf8() + "\n");
        const QByteArray pipe = QFile::encodeName(trash + "/files/box/pipe");
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        QString error;
        QFuture<QString> restored = QtConcurrent::run([&] { return FileOps::restoreOne(trash + "/files/box", &error); });
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            restored.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        write(trash + "/files/box/new.txt", "new");
        close(std::exchange(writer, -1));
        QCOMPARE(restored.result(), QString());
        QCOMPARE(error, QString("Restored box, but couldn't remove it from the trash."));
        QVERIFY(QFile::exists(trash + "/files/box/new.txt"));
        QVERIFY(QFile::exists(trash + "/info/box.trashinfo"));
        QVERIFY(QFileInfo::exists(other->filePath("box/pipe")));

        // With nothing new, all of it goes, and its note with it.
        QDir().mkpath(trash + "/files/album/inner");
        write(trash + "/files/album/inner/a.jpg", "picture");
        write(trash + "/info/album.trashinfo", "[Trash Info]\nPath=" + other->filePath("album").toUtf8() + "\n");
        error.clear();
        QCOMPARE(FileOps::restoreOne(trash + "/files/album", &error), other->filePath("album"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(trash + "/files/album"));
        QVERIFY(!QFile::exists(trash + "/info/album.trashinfo"));
        QVERIFY(QFile::exists(other->filePath("album/inner/a.jpg")));

        // One that can't be copied at all keeps its note.
        if (geteuid() != 0) {
            write(trash + "/files/locked.txt", "x");
            write(trash + "/info/locked.txt.trashinfo", "[Trash Info]\nPath=" + other->filePath("locked.txt").toUtf8() + "\n");
            QVERIFY(QFile::setPermissions(trash + "/files/locked.txt", QFile::WriteOwner));
            error.clear();
            QCOMPARE(FileOps::restoreOne(trash + "/files/locked.txt", &error), QString());
            QCOMPARE(error, QString("Couldn't copy locked.txt."));
            QVERIFY(QFile::exists(trash + "/info/locked.txt.trashinfo"));

            // One that stops partway says where what arrived went, and keeps
            // its note for the rest.
            QDir().mkpath(trash + "/files/part/a");
            write(trash + "/files/part/a/1.txt", "1");
            write(trash + "/files/part/m.txt", "m");
            QVERIFY(QFile::setPermissions(trash + "/files/part/m.txt", QFile::WriteOwner));
            write(trash + "/info/part.trashinfo", "[Trash Info]\nPath=" + other->filePath("part").toUtf8() + "\n");
            error.clear();
            QCOMPARE(FileOps::restoreOne(trash + "/files/part", &error), QString());
            QCOMPARE(error, QString("Restored only part of part, to %1: Couldn't copy m.txt.").arg(other->filePath("part")));
            QVERIFY(QFile::exists(other->filePath("part/a/1.txt")));
            QVERIFY(QFile::exists(trash + "/info/part.trashinfo"));
        }
    }

    void fileOpsWeedOutWhatsInsideQuickly()
    {
        // Select All in a big folder hands over every item. Comparing each
        // with every other to find what's inside another took most of a
        // minute for 20,000 photos before the first was touched.
        QTemporaryDir dir;
        QStringList paths{dir.filePath("gone")};
        for (int i = 0; i < 20000; ++i)
            paths << dir.filePath(QString("photos/%1.jpg").arg(i));
        QElapsedTimer timer;
        timer.start();
        // The first is missing, so it stops there, once they're weeded out.
        QString error;
        FileOps::transfer(paths, dir.path(), FileOps::Copy, &error);
        QCOMPARE(error, QString("gone is no longer there."));
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString("Took %1 ms.").arg(timer.elapsed())));
    }

    void fileOpsLetGoOfOnlyThePastedCut()
    {
        // Cut files are gone once pasted, so the clipboard lets go of them,
        // but not of something copied while they moved.
        QTemporaryDir from, to;
        write(from.filePath("a.txt"), "a");
        write(from.filePath("b.txt"), "b");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.cut({from.filePath("a.txt")});
        ops.paste(to.path());
        ops.copy({from.filePath("b.txt")});
        QVERIFY(done.wait(3000));
        bool cut = true;
        QCOMPARE(FileOps::pathsFrom(QGuiApplication::clipboard()->mimeData(), &cut), QStringList({from.filePath("b.txt")}));
        QVERIFY(!cut);

        ops.cut({from.filePath("b.txt")});
        ops.paste(to.path());
        QVERIFY(done.wait(3000));
        QVERIFY(!ops.canPaste());
    }

    void fileOpsDropMovesOnlyWhatsOnTheSameDrive()
    {
        // Each dropped file is moved from the folder's drive and copied from
        // any other, whichever comes first.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir here, folder;
        write(here.filePath("a.txt"), "a");
        write(other->filePath("b.txt"), "b");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.dropInto({QUrl::fromLocalFile(here.filePath("a.txt")), QUrl::fromLocalFile(other->filePath("b.txt"))},
                     folder.path());
        QVERIFY(done.wait(3000));
        QVERIFY(QFile::exists(folder.filePath("a.txt")));
        QVERIFY(QFile::exists(folder.filePath("b.txt")));
        QVERIFY(!QFile::exists(here.filePath("a.txt")));
        QVERIFY(QFile::exists(other->filePath("b.txt")));
    }

    void fileOpsDragUriLists()
    {
        // Names holding what means something in a URI ("#", "?", "%") are
        // dragged as the same paths they came from.
        FileOps ops;
        const QStringList paths{"/tmp/Track #1.mp3", "/tmp/what?.txt", "/tmp/100% done/caf\u00e9.txt"};
        QMimeData data;
        data.setData("text/uri-list", ops.uriList(paths).toUtf8());
        QCOMPARE(ops.localPaths(data.urls()), paths);
        // Only local files are dropped as paths.
        QCOMPARE(ops.localPaths({QUrl("https://example.com/a"), QUrl::fromLocalFile("/tmp/x")}), QStringList({"/tmp/x"}));
    }

    void fileOpsGuardDropsRenamesAndNames()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("album/inner");
        write(dir.filePath("a.txt"), "a");
        write(dir.filePath("b.txt"), "b");
        write(dir.filePath("c.txt"), "c");
        const auto urls = [&](const QString &name) { return QList<QUrl>{QUrl::fromLocalFile(dir.filePath(name))}; };
        FileOps ops;

        // A drop has to take something somewhere new: not where it already
        // is, not onto or into itself, and not onto a file.
        QVERIFY(ops.canDrop(urls("a.txt"), dir.filePath("album")));
        QVERIFY(!ops.canDrop(urls("a.txt"), dir.path()));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("album")));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("album/inner")));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("a.txt")));
        QVERIFY(QFile::link(dir.filePath("album/inner"), dir.filePath("shortcut")));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("shortcut")));
        // A link is moved as a link, so it can go into the folder it leads to.
        QVERIFY(ops.canDrop(urls("shortcut"), dir.filePath("album/inner")));
        // Moving something into the folder it's in leaves it alone rather
        // than making a "(copy)" of it.
        QString error;
        QVERIFY(FileOps::transfer({dir.filePath("a.txt")}, dir.path(), FileOps::Move, &error).isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(QFile::exists(dir.filePath("a.txt")));

        // A slash would move the file elsewhere, so it's refused; the same
        // name changes nothing, and spaces around a name are dropped.
        QVERIFY(!ops.rename(dir.filePath("a.txt"), "x/y").isEmpty());
        QCOMPARE(ops.rename(dir.filePath("a.txt"), "a.txt"), QString());
        QCOMPARE(ops.rename(dir.filePath("c.txt"), " d.txt "), QString());
        QVERIFY(QFile::exists(dir.filePath("d.txt")));

        QCOMPARE(FileOps::archiveName({dir.filePath("d.txt")}, dir.path()), QString("d.txt.zip"));
        QCOMPARE(FileOps::archiveName({dir.filePath("a.txt"), dir.filePath("b.txt")}, dir.path()), QString("Archive.zip"));

        QSignalSpy done(&ops, &FileOps::done);
        ops.newFolder(dir.path());
        ops.newFolder(dir.path());
        QCOMPARE(done.count(), 2);
        QCOMPARE(done.takeFirst().at(1).toString(), QString("New Folder"));
        QCOMPARE(done.takeFirst().at(1).toString(), QString("New Folder 2"));
    }

    void recentsReadGtkRecentFiles()
    {
        QTemporaryDir dir;
        write(dir.filePath("old.txt"), "x");
        write(dir.filePath("new.pdf"), "x");
        write(dir.filePath(".secret"), "x");
        const QByteArray url = QUrl::fromLocalFile(dir.path()).toEncoded();
        write(dir.filePath("recently-used.xbel"),
              "<?xml version=\"1.0\"?>\n<xbel version=\"1.0\">\n"
              "<bookmark href=\"" + url + "/old.txt\" modified=\"2026-01-01T10:00:00Z\" visited=\"2026-01-02T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/gone.txt\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/.secret\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"https://example.com/\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/new.pdf\" modified=\"2026-03-01T10:00:00Z\" visited=\"2026-03-01T10:00:00Z\"/>\n"
              "</xbel>\n");
        const auto entries = FolderModel::listRecent(dir.filePath("recently-used.xbel"), false);
        // Missing, hidden and web entries are left out; newest use first.
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).name, QString("new.pdf"));
        QCOMPARE(entries.at(1).name, QString("old.txt"));
        QCOMPARE(entries.at(0).kind, QString("pdf"));
        // Left before it's done, it stops checking what's on the list.
        int checks = 0;
        QVERIFY(FolderModel::listRecent(dir.filePath("recently-used.xbel"), false, 200, [&] { return ++checks > 3; }).size() == 1);
    }

    void folderSortsFiveWays()
    {
        QVector<FolderModel::Entry> e(4);
        e[0].name = "b.txt"; e[0].kind = "text"; e[0].extension = "TXT"; e[0].size = 10; e[0].modified = QDateTime::fromSecsSinceEpoch(300);
        e[1].name = "a.png"; e[1].kind = "image"; e[1].extension = "PNG"; e[1].size = 500; e[1].modified = QDateTime::fromSecsSinceEpoch(100);
        e[2].name = "c.txt"; e[2].kind = "text"; e[2].extension = "TXT"; e[2].size = 99; e[2].modified = QDateTime::fromSecsSinceEpoch(200);
        e[3].name = "Zeta"; e[3].isDir = true; e[3].modified = QDateTime::fromSecsSinceEpoch(50);
        auto order = [&](const QString &sort) {
            auto copy = e;
            FolderModel::sortEntries(copy, sort);
            QStringList names;
            for (const auto &x : copy) names << x.name;
            return names.join(' ');
        };
        QCOMPARE(order("name"), QString("Zeta a.png b.txt c.txt"));
        QCOMPARE(order("kind"), QString("Zeta a.png b.txt c.txt"));
        QCOMPARE(order("modified"), QString("Zeta b.txt c.txt a.png"));
        QCOMPARE(order("oldest"), QString("Zeta a.png c.txt b.txt"));
        QCOMPARE(order("size"), QString("Zeta a.png c.txt b.txt"));
    }

    void folderKeepsASelection()
    {
        QTemporaryDir dir;
        for (const char *name : {"a.txt", "b.txt", "c.txt", "d.txt"})
            write(dir.filePath(name), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));

        model.selectOnly(1);
        QCOMPARE(model.selectedCount(), 1);
        model.selectRange(1, 3, false);
        QCOMPARE(model.selectedPaths(), QStringList({dir.filePath("b.txt"), dir.filePath("c.txt"), dir.filePath("d.txt")}));
        model.toggleSelected(2);
        QCOMPARE(model.selectedCount(), 2);
        QVERIFY(!model.isSelected(2));
        model.selectRange(0, 0, true);
        QCOMPARE(model.selectedCount(), 3);

        // A file that goes away leaves the selection; the rest stay.
        QFile::remove(dir.filePath("b.txt"));
        model.reload();
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.selectedPaths(), QStringList({dir.filePath("a.txt"), dir.filePath("d.txt")}));

        model.selectAll();
        QCOMPARE(model.selectedCount(), 3);
        model.clearSelection();
        QCOMPARE(model.selectedCount(), 0);
    }

    void recentsSelectOneOfTwoSameNamedFiles()
    {
        // Recents can hold two files of the same name from different
        // folders; selecting one leaves the other alone.
        QTemporaryDir data;
        QDir(data.path()).mkpath("a");
        QDir(data.path()).mkpath("b");
        write(data.filePath("a/README.md"), "x");
        write(data.filePath("b/README.md"), "x");
        const QByteArray url = QUrl::fromLocalFile(data.path()).toEncoded();
        write(data.filePath("recently-used.xbel"),
              "<?xml version=\"1.0\"?>\n<xbel version=\"1.0\">\n"
              "<bookmark href=\"" + url + "/a/README.md\" modified=\"2026-01-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/b/README.md\" modified=\"2026-02-01T10:00:00Z\"/>\n"
              "</xbel>\n");
        const auto restore = dataHomeAt(data.path());

        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(FolderModel::recentPath());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        model.selectOnly(0);
        QCOMPARE(model.selectedCount(), 1);
        QVERIFY(!model.isSelected(1));
        QCOMPARE(model.selectedPaths(), QStringList({data.filePath("b/README.md")}));
        // The cursor comes back by path, which tells them apart too.
        QCOMPARE(model.indexOfPath(data.filePath("a/README.md")), 1);
        QCOMPARE(model.indexOfPath(data.filePath("b/README.md")), 0);
    }

    void trashRestoresAndEmpties()
    {
        QTemporaryDir home;
        const auto restore = dataHomeAt(home.path());
        const QString trash = home.filePath("Trash");
        QDir().mkpath(trash + "/files");
        QDir().mkpath(trash + "/info");
        QDir().mkpath(home.filePath("Docs"));
        auto trashed = [&](const QString &name, const QString &from) {
            write(trash + "/files/" + name, "x");
            write(trash + "/info/" + name + ".trashinfo",
                  "[Trash Info]\nPath=" + QUrl::toPercentEncoding(from, "/") + "\nDeletionDate=2026-11-10T12:00:00\n");
        };
        trashed("notes 1.txt", home.filePath("Docs/notes 1.txt"));
        trashed("gone.txt", home.filePath("Gone/Deep/gone.txt"));
        trashed("clash.txt", home.filePath("Docs/clash.txt"));
        write(home.filePath("Docs/clash.txt"), "newer");

        QCOMPARE(FileOps::originalPath(trash + "/files/notes 1.txt"), home.filePath("Docs/notes 1.txt"));
        QCOMPARE(FileOps::deletedAt(trash + "/files/notes 1.txt"), QDateTime(QDate(2026, 11, 10), QTime(12, 0)));

        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/notes 1.txt", &error), home.filePath("Docs/notes 1.txt"));
        QVERIFY(!QFile::exists(trash + "/info/notes 1.txt.trashinfo"));
        // A folder that's gone is made again; a name that's taken isn't overwritten.
        QCOMPARE(FileOps::restoreOne(trash + "/files/gone.txt", &error), home.filePath("Gone/Deep/gone.txt"));
        QCOMPARE(FileOps::restoreOne(trash + "/files/clash.txt", &error), home.filePath("Docs/clash (copy).txt"));
        QFile kept(home.filePath("Docs/clash.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("newer"));

        trashed("a.txt", home.filePath("a.txt"));
        QDir().mkpath(trash + "/files/folder/inner");
        write(trash + "/info/folder.trashinfo", "[Trash Info]\nPath=" + home.filePath("folder").toUtf8() + "\n");
        // Something being trashed gets its note first, then its file, so a
        // note without a file may be for something arriving.
        write(trash + "/info/arriving.txt.trashinfo", "[Trash Info]\nPath=" + home.filePath("arriving.txt").toUtf8() + "\n");
        QCOMPARE(FileOps::emptyTrashDir(trash), 2);
        QVERIFY(QDir(trash + "/files").isEmpty());
        QCOMPARE(QDir(trash + "/info").entryList(QDir::Files), QStringList({"arriving.txt.trashinfo"}));
    }

    void trashKeepsTheNoteOfWhatStays()
    {
        // A note goes only once its item is gone, so what couldn't be
        // deleted, or removed from the trash once restored to another drive,
        // can still be restored.
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files/box");
        QDir().mkpath(trash + "/info");
        write(trash + "/files/box/x.txt", "x");
        const auto other = elsewhere();
        const QString original = other ? other->filePath("box") : data.filePath("box");
        write(trash + "/info/box.trashinfo", "[Trash Info]\nPath=" + QUrl::toPercentEncoding(original, "/") + "\n");
        // Nothing can be taken out of a folder that can't be written to.
        const auto locked = QFile::ReadOwner | QFile::ExeOwner;
        QVERIFY(QFile::setPermissions(trash + "/files/box", locked));
        const auto unlock = qScopeGuard([&] {
            for (const QString &box : {trash + "/files/box", original})
                QFile::setPermissions(box, locked | QFile::WriteOwner);
        });
        QVERIFY(!FileOps::deleteTrashed(trash + "/files/box"));
        QVERIFY(QFile::exists(trash + "/info/box.trashinfo"));

        if (!other)
            QSKIP("Needs a second drive.");
        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/box", &error), QString());
        QCOMPARE(error, QString("Restored box, but couldn't remove it from the trash."));
        QVERIFY(QFile::exists(original + "/x.txt"));
        QVERIFY(QFile::exists(trash + "/info/box.trashinfo"));
    }

    void trashFindsNotesOnlyForWhatWasTrashed()
    {
        // Only what's directly in Trash/files has a note in Trash/info.
        // Something inside a trashed folder came along with it and has none;
        // the same name in Trash/files/info is a file in a trashed folder
        // called "info".
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files/photos");
        QDir().mkpath(trash + "/files/info");
        write(trash + "/files/photos/a", "a");
        write(trash + "/files/info/a.trashinfo", "[Trash Info]\nPath=" + data.filePath("elsewhere/a").toUtf8() + "\n");

        QCOMPARE(FileOps::originalPath(trash + "/files/photos/a"), QString());
        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/photos/a", &error), QString());
        QCOMPARE(error, QString("a has no record of where it came from."));
        QVERIFY(QFile::exists(trash + "/files/photos/a"));
        QVERIFY(FileOps::deleteTrashed(trash + "/files/photos/a"));
        QVERIFY(QFile::exists(trash + "/files/info/a.trashinfo"));
    }

    void trashAndRestoreRunInTheBackground()
    {
        // Trashing asks GIO about each item, and restoring can copy a whole
        // folder from another drive, so neither holds up the window: each
        // shows that it's busy, and says how it went once it's done.
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files");
        QDir().mkpath(trash + "/info");
        write(trash + "/files/a.txt", "a");
        write(trash + "/info/a.txt.trashinfo", "[Trash Info]\nPath=" + data.filePath("a.txt").toUtf8() + "\n");
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), failed(&ops, &FileOps::failed);
        ops.restore({trash + "/files/a.txt"});
        QVERIFY(!ops.busy().isEmpty());
        QCOMPARE(notice.count(), 0);
        QTRY_COMPARE(notice.count(), 1);
        QVERIFY(ops.busy().isEmpty());
        QCOMPARE(notice.first().first().toString(), QString("Restored a.txt to %1.").arg(data.path()));
        QVERIFY(QFile::exists(data.filePath("a.txt")));

        // What's no longer there fails before GIO goes near a trash.
        ops.trash({data.filePath("gone.txt")});
        QVERIFY(!ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(ops.busy().isEmpty());
        QVERIFY(failed.first().first().toString().startsWith("Couldn't move to the trash: gone.txt ("));

        // One piece of work runs at a time. What's asked for meanwhile
        // doesn't start, and says so rather than doing nothing.
        ops.trash({data.filePath("gone.txt")});
        ops.restore({trash + "/files/a.txt"});
        QCOMPARE(failed.count(), 2);
        QCOMPARE(failed.last().first().toString(), QString("Try again once the work under way is done."));
        QTRY_COMPARE(failed.count(), 3);
    }

    void trashTakesNothingButDeletes()
    {
        // A paste or a drop into the trash would leave items with no note
        // saying where they came from.
        QTemporaryDir data, from;
        const auto restore = dataHomeAt(data.path());
        const QString files = data.filePath("Trash/files");
        QDir().mkpath(files);
        write(from.filePath("a.txt"), "a");
        FileOps ops;
        QVERIFY(ops.inTrash(files));
        QVERIFY(!ops.canDrop({QUrl::fromLocalFile(from.filePath("a.txt"))}, files));
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.cut({from.filePath("a.txt")});
        ops.paste(files);
        QCOMPARE(failed.count(), 1);
        QVERIFY(QDir(files).isEmpty());
    }

    void fileOpsCancelStopsACopyPartway()
    {
        // Cancel stops at the next file, or the next piece of a big one: a
        // file copied partway is removed, and what was copied before stays.
        // Copying a pipe reads it as its writer writes, so the copy is
        // partway for as long as the test likes.
        QTemporaryDir from, to;
        write(from.filePath("a.txt"), "a");
        const QByteArray pipe = QFile::encodeName(from.filePath("pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        FileOps::Job job;
        QString error;
        QFuture<QStringList> copied = QtConcurrent::run([&] {
            return FileOps::transfer({from.filePath("a.txt"), from.filePath("pipe")}, to.path(), FileOps::Copy, &error, {}, &job);
        });
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            copied.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        QCOMPARE(::write(writer, "partial", 7), 7);
        QTRY_VERIFY(job.bytes >= 8);
        job.cancelled = true;
        QCOMPARE(::write(writer, "more", 4), 4);
        close(std::exchange(writer, -1));
        QCOMPARE(copied.result(), QStringList({"a.txt"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(job.done.load(), 1);
        QCOMPARE(job.total.load(), 2);
        QVERIFY(QFile::exists(to.filePath("a.txt")));
        QVERIFY(!QFileInfo::exists(to.filePath("pipe")));

        // From the window, Cancel says how far it got.
        QTemporaryDir again;
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice);
        ops.copy({from.filePath("a.txt"), from.filePath("pipe")});
        ops.paste(again.path());
        QVERIFY(!ops.busy().isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        ops.cancel();
        QCOMPARE(::write(writer, "x", 1), 1);
        close(std::exchange(writer, -1));
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(notice.count(), 1);
        QCOMPARE(notice.first().first().toString(), QString("Stopped after 1 of 2 items."));
        QVERIFY(!QFileInfo::exists(again.filePath("pipe")));
    }

    void fileOpsCancelledMoveAcrossDrivesKeepsOriginals()
    {
        // Across drives each original goes once it's copied, so a move
        // cancelled partway removes only what fully arrived: the file it
        // was on stays, and so does what it never reached.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir from;
        QDir(from.path()).mkdir("box");
        write(from.filePath("box/a.txt"), "a");
        write(from.filePath("box/z.txt"), "z");
        const QByteArray pipe = QFile::encodeName(from.filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        FileOps::Job job;
        QString error;
        QFuture<QStringList> moved = QtConcurrent::run([&] {
            return FileOps::transfer({from.filePath("box")}, other->path(), FileOps::Move, &error, {}, &job);
        });
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            moved.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        QCOMPARE(::write(writer, "partial", 7), 7);
        QTRY_VERIFY(job.bytes >= 8);
        job.cancelled = true;
        QCOMPARE(::write(writer, "more", 4), 4);
        close(std::exchange(writer, -1));
        // What arrived is the only copy of a.txt now, so it's shown.
        QCOMPARE(moved.result(), QStringList({"box"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        // A single folder counts what's in it.
        QCOMPARE(job.total.load(), 3);
        QCOMPARE(job.done.load(), 1);
        QVERIFY(QFile::exists(other->filePath("box/a.txt")));
        QVERIFY(!QFile::exists(from.filePath("box/a.txt")));
        QVERIFY(QFileInfo::exists(from.filePath("box/pipe")));
        QVERIFY(!QFileInfo::exists(other->filePath("box/pipe")));
        QVERIFY(QFile::exists(from.filePath("box/z.txt")));
        QVERIFY(!QFile::exists(other->filePath("box/z.txt")));
    }

    void fileOpsAskWhichClashes()
    {
        // Only a name that's taken by something else clashes: not a copy
        // onto itself (kept beside it, as always), nor a move into the
        // folder it's in.
        QTemporaryDir from, to;
        write(from.filePath("a.txt"), "a");
        write(from.filePath("b.txt"), "b");
        write(to.filePath("a.txt"), "old");
        QCOMPARE(FileOps::clashes({from.filePath("a.txt"), from.filePath("b.txt")}, to.path()),
                 QStringList({from.filePath("a.txt")}));
        QCOMPARE(FileOps::clashes({from.filePath("a.txt")}, from.path()), QStringList());
    }

    void fileOpsCopyOntoItselfKeepsBoth()
    {
        // Copied into its own folder, an item is kept beside itself, never
        // asked about, and never replaced by itself, also when the folder
        // is reached through a link to it, where replacing would send the
        // original to the trash before copying it.
        QTemporaryDir data, dir, links;
        const auto restore = dataHomeAt(data.path());
        write(dir.filePath("a.txt"), "a");
        QVERIFY(QFile::link(dir.path(), links.filePath("same")));
        const QString same = links.filePath("same");
        QCOMPARE(FileOps::clashes({dir.filePath("a.txt")}, same), QStringList());
        QString error;
        QCOMPARE(FileOps::transfer({dir.filePath("a.txt")}, dir.path(), FileOps::Copy, &error),
                 QStringList({"a (copy).txt"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(FileOps::transfer({dir.filePath("a.txt")}, same, FileOps::Copy, &error,
                                   {{dir.filePath("a.txt"), FileOps::Replace}}),
                 QStringList({"a (copy 2).txt"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        // Moved into where it already is, it stays.
        FileOps::transfer({dir.filePath("a.txt")}, same, FileOps::Move, &error, {{dir.filePath("a.txt"), FileOps::Replace}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(contents(dir.filePath("a.txt")), QByteArray("a"));
        QVERIFY(!QFileInfo::exists(data.filePath("Trash/files/a.txt")));
    }

    void fileOpsReplaceSkipOrKeepBoth()
    {
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        for (const char *name : {"replace.txt", "skip.txt", "both.txt"}) {
            write(from.filePath(name), "new");
            write(to.filePath(name), "old");
        }
        const QStringList paths{from.filePath("replace.txt"), from.filePath("skip.txt"), from.filePath("both.txt")};
        QString error;
        QList<FileOps::Step> steps;
        const QStringList made = FileOps::transfer(paths, to.path(), FileOps::Copy, &error,
                                                   {{paths[0], FileOps::Replace}, {paths[1], FileOps::Skip}, {paths[2], FileOps::KeepBoth}},
                                                   nullptr, &steps);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(made, QStringList({"replace.txt", "both (copy).txt"}));
        QCOMPARE(contents(to.filePath("replace.txt")), QByteArray("new"));
        QCOMPARE(contents(to.filePath("skip.txt")), QByteArray("old"));
        QCOMPARE(contents(to.filePath("both.txt")), QByteArray("old"));
        QCOMPARE(contents(to.filePath("both (copy).txt")), QByteArray("new"));
        // What's replaced isn't deleted: it's in the trash, with its note.
        const QString trashed = data.filePath("Trash/files/replace.txt");
        QCOMPARE(contents(trashed), QByteArray("old"));
        QCOMPARE(FileOps::originalPath(trashed), to.filePath("replace.txt"));
        QCOMPARE(steps.size(), 3);
        QCOMPARE(steps[0].other, trashed);
    }

    void fileOpsMergeFolders()
    {
        // A folder onto a folder merges: what's new goes in, and a clash
        // inside is replaced the same way, the old one to the trash.
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        QDir(from.path()).mkpath("album/sub");
        QDir(to.path()).mkpath("album/sub");
        write(from.filePath("album/a.jpg"), "new");
        write(from.filePath("album/sub/c.jpg"), "c");
        write(to.filePath("album/a.jpg"), "old");
        write(to.filePath("album/keep.jpg"), "keep");
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("album")}, to.path(), FileOps::Move, &error,
                                   {{from.filePath("album"), FileOps::Replace}}),
                 QStringList({"album"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(contents(to.filePath("album/a.jpg")), QByteArray("new"));
        QCOMPARE(contents(to.filePath("album/keep.jpg")), QByteArray("keep"));
        QCOMPARE(contents(to.filePath("album/sub/c.jpg")), QByteArray("c"));
        QCOMPARE(contents(data.filePath("Trash/files/a.jpg")), QByteArray("old"));
        // Moved, the emptied folder goes.
        QVERIFY(!QFileInfo::exists(from.filePath("album")));
    }

    void fileOpsAskAboutEachClashOrAll()
    {
        // Pasting asks about each clash before anything is copied; the
        // answer can go for the rest, and Escape cancels the lot.
        QTemporaryDir from, to;
        for (const char *name : {"x.txt", "y.txt", "z.txt"})
            write(from.filePath(name), "new");
        write(to.filePath("x.txt"), "old");
        write(to.filePath("y.txt"), "old");
        QDir(from.path()).mkdir("box");
        QDir(to.path()).mkdir("box");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.copy({from.filePath("box"), from.filePath("x.txt"), from.filePath("y.txt"), from.filePath("z.txt")});
        ops.paste(to.path());
        QVERIFY(ops.busy().isEmpty());
        QCOMPARE(ops.clash().value("name").toString(), QString("box"));
        QCOMPARE(ops.clash().value("folder").toString(), QFileInfo(to.path()).fileName());
        QVERIFY(ops.clash().value("merge").toBool());
        QCOMPARE(ops.clash().value("others").toInt(), 2);
        ops.answerClash("both", false);
        QCOMPARE(ops.clash().value("name").toString(), QString("x.txt"));
        QVERIFY(!ops.clash().value("merge").toBool());
        QCOMPARE(ops.clash().value("others").toInt(), 1);
        // Nothing is copied until the last answer.
        QVERIFY(!QFile::exists(to.filePath("z.txt")));
        ops.answerClash("skip", true);
        QVERIFY(ops.clash().isEmpty());
        QVERIFY(done.wait(3000));
        QVERIFY(QFileInfo(to.filePath("box (copy)")).isDir());
        QCOMPARE(contents(to.filePath("x.txt")), QByteArray("old"));
        QCOMPARE(contents(to.filePath("y.txt")), QByteArray("old"));
        QCOMPARE(contents(to.filePath("z.txt")), QByteArray("new"));
        QVERIFY(!QFile::exists(to.filePath("x (copy).txt")));

        // Cancelled, nothing happens at all.
        QTemporaryDir fresh;
        write(fresh.filePath("x.txt"), "old");
        ops.copy({from.filePath("x.txt"), from.filePath("z.txt")});
        ops.paste(fresh.path());
        QVERIFY(!ops.clash().isEmpty());
        ops.dropClash();
        QVERIFY(ops.clash().isEmpty());
        QVERIFY(ops.busy().isEmpty());
        QVERIFY(!QFile::exists(fresh.filePath("z.txt")));
    }

    void undoAndRedoRenameMoveAndTrash()
    {
        QTemporaryDir data, dir;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(dir.filePath("a.txt"), "a");
        write(dir.filePath("c.txt"), "c");
        QDir(dir.path()).mkdir("sub");
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), failed(&ops, &FileOps::failed);
        QCOMPARE(ops.undoName(), QString());

        QCOMPARE(ops.rename(dir.filePath("a.txt"), "b.txt"), QString());
        QCOMPARE(ops.undoName(), QString("Undo Rename"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("a.txt")));
        QVERIFY(!QFile::exists(dir.filePath("b.txt")));
        QCOMPARE(notice.last().first().toString(), QString("Undid renaming a.txt."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("b.txt")));
        QCOMPARE(notice.last().first().toString(), QString("Redid renaming a.txt."));

        ops.cut({dir.filePath("b.txt"), dir.filePath("c.txt")});
        ops.paste(dir.filePath("sub"));
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("sub/b.txt")));
        QCOMPARE(ops.undoName(), QString("Undo Move"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("b.txt")));
        QVERIFY(QFile::exists(dir.filePath("c.txt")));
        QVERIFY(QDir(dir.filePath("sub")).isEmpty());
        QCOMPARE(notice.last().first().toString(), QString("Undid moving 2 items."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("sub/b.txt")));
        QVERIFY(QFile::exists(dir.filePath("sub/c.txt")));

        ops.trash({dir.filePath("sub/b.txt"), dir.filePath("sub/c.txt")});
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QDir(dir.filePath("sub")).isEmpty());
        QCOMPARE(ops.undoName(), QString("Undo Delete"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(dir.filePath("sub/b.txt")), QByteArray("a"));
        QCOMPARE(contents(dir.filePath("sub/c.txt")), QByteArray("c"));
        QVERIFY(QDir(data.filePath("Trash/files")).isEmpty());
        QVERIFY(QDir(data.filePath("Trash/info")).isEmpty());
        QCOMPARE(notice.last().first().toString(), QString("Undid moving 2 items to the trash."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QDir(dir.filePath("sub")).isEmpty());
        QCOMPARE(QDir(data.filePath("Trash/files")).entryList(QDir::Files), QStringList({"b.txt", "c.txt"}));
        QCOMPARE(failed.count(), 0);

        // Undo goes back through them all, the newest first.
        for (int i = 0; i < 3; ++i) {
            ops.undo();
            QTRY_VERIFY(ops.busy().isEmpty());
        }
        QCOMPARE(contents(dir.filePath("a.txt")), QByteArray("a"));
        QCOMPARE(contents(dir.filePath("c.txt")), QByteArray("c"));
        QCOMPARE(ops.undoName(), QString());
        QCOMPARE(failed.count(), 0);
    }

    void undoAndRedoPasteAndReplace()
    {
        // A copy is undone by moving it to the trash, and redone by
        // bringing it back; a replace puts the replaced one back too.
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(from.filePath("a.txt"), "new");
        write(from.filePath("b.txt"), "new");
        write(from.filePath("r.txt"), "new");
        write(to.filePath("r.txt"), "old");
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), failed(&ops, &FileOps::failed);
        ops.copy({from.filePath("a.txt"), from.filePath("b.txt")});
        ops.paste(to.path());
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(ops.undoName(), QString("Undo Copy"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(!QFile::exists(to.filePath("a.txt")));
        QVERIFY(!QFile::exists(to.filePath("b.txt")));
        QCOMPARE(contents(data.filePath("Trash/files/a.txt")), QByteArray("new"));
        QCOMPARE(notice.last().first().toString(), QString("Undid copying 2 items."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(to.filePath("a.txt")), QByteArray("new"));
        QCOMPARE(contents(to.filePath("b.txt")), QByteArray("new"));
        QVERIFY(QDir(data.filePath("Trash/files")).isEmpty());

        ops.copy({from.filePath("r.txt")});
        ops.paste(to.path());
        ops.answerClash("replace", false);
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(to.filePath("r.txt")), QByteArray("new"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(to.filePath("r.txt")), QByteArray("old"));
        // The new one went to the trash while the old one was still there.
        QCOMPARE(QDir(data.filePath("Trash/files")).entryList(QDir::Files), QStringList({"r.txt.2"}));
        QCOMPARE(contents(data.filePath("Trash/files/r.txt.2")), QByteArray("new"));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(to.filePath("r.txt")), QByteArray("new"));
        QCOMPARE(contents(data.filePath("Trash/files/r.txt")), QByteArray("old"));
        QCOMPARE(failed.count(), 0);
    }

    void undoRefusesWhatsChangedSince()
    {
        // Undo checks everything is still where the change left it, and
        // that nothing's in the way, before it touches anything; refused,
        // the change stays there to undo.
        QTemporaryDir dir;
        FileOps::forgetHistory();
        write(dir.filePath("a.txt"), "a");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        QCOMPARE(ops.rename(dir.filePath("a.txt"), "b.txt"), QString());
        QVERIFY(QFile::rename(dir.filePath("b.txt"), dir.filePath("c.txt")));
        ops.undo();
        QVERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.last().first().toString(), QString("Can't undo that: b.txt isn't where it was left."));
        QCOMPARE(ops.undoName(), QString("Undo Rename"));

        QVERIFY(QFile::rename(dir.filePath("c.txt"), dir.filePath("b.txt")));
        write(dir.filePath("a.txt"), "newer");
        ops.undo();
        QCOMPARE(failed.last().first().toString(),
                 QString("Can't undo that: a.txt already exists in %1.").arg(QFileInfo(dir.path()).fileName()));
        QCOMPARE(contents(dir.filePath("a.txt")), QByteArray("newer"));
        QVERIFY(QFile::exists(dir.filePath("b.txt")));
        QCOMPARE(ops.undoName(), QString("Undo Rename"));
    }

    void undoRefusesATrashedItemThatsSinceAnother()
    {
        // Deleted from the trash for good, an item's name there can go to
        // the next one trashed, by another app. Undoing the first trashing
        // would then bring that one back in its place.
        QTemporaryDir data, dir, elsewhere;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(dir.filePath("a.txt"), "mine");
        write(elsewhere.filePath("a.txt"), "theirs");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.trash({dir.filePath("a.txt")});
        QTRY_VERIFY(ops.busy().isEmpty());
        const QString trashed = data.filePath("Trash/files/a.txt");
        QVERIFY(FileOps::deleteTrashed(trashed));
        QString error;
        QCOMPARE(FileOps::trashOne(elsewhere.filePath("a.txt"), &error), trashed);
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.last().first().toString(), QString("Can't undo that: a.txt isn't where it was left."));
        QVERIFY(!QFileInfo::exists(dir.filePath("a.txt")));
        QCOMPARE(FileOps::originalPath(trashed), elsewhere.filePath("a.txt"));
    }

    void extractOneItemBesideTheArchive()
    {
        // An archive holding one folder (or file) puts it beside the
        // archive, under its own name; nothing else is left behind.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir, source;
        QDir(source.path()).mkpath("album/sub");
        write(source.filePath("album/a.jpg"), "a");
        write(source.filePath("album/sub/b.jpg"), "b");
        pack(dir.filePath("photos.tar.gz"), source.path(), {"album"});
        FileOps ops;
        QVERIFY(ops.canExtract({dir.filePath("photos.tar.gz")}));
        QSignalSpy done(&ops, &FileOps::done), failed(&ops, &FileOps::failed);
        ops.extract({dir.filePath("photos.tar.gz")});
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first(), QList<QVariant>({dir.path(), "album"}));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(dir.filePath("album/a.jpg")), QByteArray("a"));
        QCOMPARE(contents(dir.filePath("album/sub/b.jpg")), QByteArray("b"));
        QCOMPARE(everything(dir.path()), QStringList({"album", "photos.tar.gz"}));
    }

    void extractManyItemsIntoAFolder()
    {
        // Several items go into a new folder named after the archive,
        // without its extensions, so they don't spill over the folder.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir, source;
        write(source.filePath("a.txt"), "a");
        write(source.filePath("b.txt"), "b");
        pack(dir.filePath("notes.tar.xz"), source.path(), {"a.txt", "b.txt"});
        pack(dir.filePath("more.zip"), source.path(), {"a.txt", "b.txt"});
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.extract({dir.filePath("notes.tar.xz"), dir.filePath("more.zip")});
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first(), QList<QVariant>({dir.path(), "notes"}));
        QCOMPARE(contents(dir.filePath("notes/a.txt")), QByteArray("a"));
        QCOMPARE(contents(dir.filePath("notes/b.txt")), QByteArray("b"));
        QCOMPARE(contents(dir.filePath("more/b.txt")), QByteArray("b"));
        QCOMPARE(everything(dir.path()), QStringList({"more", "more.zip", "notes", "notes.tar.xz"}));
        // Only archives are offered: not a lone compressed file, nor a
        // folder.
        write(dir.filePath("single.txt.gz"), "x");
        QVERIFY(ops.canExtract({dir.filePath("notes.tar.xz"), dir.filePath("more.zip")}));
        QVERIFY(!ops.canExtract({dir.filePath("notes.tar.xz"), dir.filePath("single.txt.gz")}));
        QVERIFY(!ops.canExtract({dir.filePath("notes")}));
        QVERIFY(!ops.canExtract({}));
    }

    void extractAsksWhenTheNameIsTaken()
    {
        // The name that would appear is asked about like a paste's, Keep
        // Both first; cancelling leaves nothing, not even the hidden
        // extraction.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir data, dir, source;
        const auto restore = dataHomeAt(data.path());
        write(source.filePath("a.txt"), "new");
        write(source.filePath("b.txt"), "new");
        pack(dir.filePath("notes.zip"), source.path(), {"a.txt", "b.txt"});
        QDir(dir.path()).mkdir("notes");
        write(dir.filePath("notes/a.txt"), "old");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done), clash(&ops, &FileOps::clashChanged);
        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(clash.wait(5000));
        QCOMPARE(ops.clash().value("name").toString(), QString("notes"));
        QCOMPARE(ops.clash().value("merge").toBool(), true);
        ops.dropClash();
        QTRY_COMPARE(everything(dir.path()), QStringList({"notes", "notes.zip"}));
        QCOMPARE(everything(dir.filePath("notes")), QStringList({"a.txt"}));

        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(clash.wait(5000));
        ops.answerClash("both", false);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.first(), QList<QVariant>({dir.path(), "notes (copy)"}));
        QCOMPARE(contents(dir.filePath("notes (copy)/a.txt")), QByteArray("new"));
        QCOMPARE(contents(dir.filePath("notes/a.txt")), QByteArray("old"));

        // Merged, what's replaced inside goes to the trash.
        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(clash.wait(5000));
        ops.answerClash("replace", false);
        QCOMPARE(contents(dir.filePath("notes/a.txt")), QByteArray("new"));
        QCOMPARE(contents(dir.filePath("notes/b.txt")), QByteArray("new"));
        QCOMPARE(contents(data.filePath("Trash/files/a.txt")), QByteArray("old"));
        QCOMPARE(everything(dir.path()), QStringList({"notes", "notes (copy)", "notes.zip"}));
    }

    void extractRefusesWhatWouldLandOutside()
    {
        // bsdtar refuses ".." and links leading out, and an archive with
        // either is extracted not at all: nothing escapes, and nothing is
        // left half-made.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir outer;
        QDir(outer.path()).mkpath("dir");
        QDir(outer.path()).mkpath("target");
        const QString dir = outer.filePath("dir");
        write(outer.filePath("f"), "evil");
        const auto mtree = [&](const QString &archive, const QByteArray &spec) {
            write(outer.filePath("spec"), "#mtree\n" + spec);
            QProcess tar;
            tar.start("bsdtar", {"-cf", archive, "@" + outer.filePath("spec")});
            QVERIFY(tar.waitForFinished() && tar.exitCode() == 0);
        };
        const QByteArray f = QFile::encodeName(outer.filePath("f"));
        mtree(dir + "/dots.tar", "./ok.txt type=file contents=" + f + "\n../evil.txt type=file contents=" + f + "\n");
        mtree(dir + "/link.tar", "./l type=link link=" + QFile::encodeName(outer.filePath("target"))
                                     + "\n./l/evil.txt type=file contents=" + f + "\n");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.extract({dir + "/dots.tar"});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().first().toString(), QString("Couldn't extract dots.tar: it would put files outside its folder."));
        QTRY_VERIFY(ops.busy().isEmpty());
        ops.extract({dir + "/link.tar"});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().first().toString(), QString("Couldn't extract link.tar: it would put files outside its folder."));
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(everything(outer.path()), QStringList({"dir", "f", "spec", "target"}));
        QCOMPARE(everything(dir), QStringList({"dots.tar", "link.tar"}));
        QCOMPARE(everything(outer.filePath("target")), QStringList());
    }

    void extractSaysWhenItCant()
    {
        // A password, or a damaged archive, is said as such.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir;
        write(dir.filePath("secret.txt"), "secret");
        QProcess tar;
        tar.start("bsdtar", {"--options", "zip:encryption=zipcrypt", "--passphrase", "x", "-acf",
                             dir.filePath("locked.zip"), "-C", dir.path(), "secret.txt"});
        QVERIFY(tar.waitForFinished() && tar.exitCode() == 0);
        QFile::remove(dir.filePath("secret.txt"));
        write(dir.filePath("broken.zip"), QByteArray(300, 'x'));
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.extract({dir.filePath("locked.zip"), dir.filePath("broken.zip")});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.first().first().toString(),
                 QString("Couldn't extract locked.zip: it's protected by a password. "
                         "Couldn't extract broken.zip: it's damaged, or isn't an archive Folio can read."));
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(everything(dir.path()), QStringList({"broken.zip", "locked.zip"}));
    }

    void extractCancelLeavesNothing()
    {
        // Cancel stops bsdtar partway, and what it had made goes with it.
        // Reading the archive from a pipe keeps it partway for as long as
        // the test likes.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir, source;
        write(source.filePath("a.txt"), QByteArray(4096, 'a'));
        write(source.filePath("b.txt"), QByteArray(1 << 20, 'b'));
        pack(source.filePath("whole.tar"), source.path(), {"a.txt", "b.txt"});
        const QByteArray archive = contents(source.filePath("whole.tar"));
        const QByteArray pipe = QFile::encodeName(dir.filePath("slow.tar"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice);
        ops.extract({dir.filePath("slow.tar")});
        int writer = -1;
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        // The first file whole, and some of the second: bsdtar reads ahead,
        // so it takes more than the first file to see it through.
        fcntl(writer, F_SETFL, 0);
        QCOMPARE(::write(writer, archive.constData(), 256 << 10), 256 << 10);
        QTRY_VERIFY(ops.busy().contains("1"));
        ops.cancel();
        close(writer);
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(notice.count(), 1);
        QCOMPARE(notice.first().first().toString(), QString("Stopped extracting slow.tar; nothing was extracted."));
        QCOMPARE(everything(dir.path()), QStringList({"slow.tar"}));
    }

    void undoAndRedoExtract()
    {
        // Undo sends what was extracted to the trash; redo brings it back.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir data, dir, source;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(source.filePath("a.txt"), "a");
        write(source.filePath("b.txt"), "b");
        pack(dir.filePath("notes.zip"), source.path(), {"a.txt", "b.txt"});
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done), notice(&ops, &FileOps::notice);
        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(done.wait(5000));
        QCOMPARE(ops.undoName(), QString("Undo Extract"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(everything(dir.path()), QStringList({"notes.zip"}));
        QCOMPARE(contents(data.filePath("Trash/files/notes/a.txt")), QByteArray("a"));
        QCOMPARE(notice.last().first().toString(), QString("Undid extracting notes.zip."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(dir.filePath("notes/b.txt")), QByteArray("b"));
        QCOMPARE(notice.last().first().toString(), QString("Redid extracting notes.zip."));
    }

    void kindsFromMimeTypes()
    {
        QCOMPARE(FolderModel::kindForMime("application/pdf"), QString("pdf"));
        QCOMPARE(FolderModel::kindForMime("video/mp4"), QString("video"));
        QCOMPARE(FolderModel::kindForMime("audio/mp4"), QString("audio"));
        QCOMPARE(FolderModel::kindForMime("application/zip"), QString("archive"));
        QCOMPARE(FolderModel::kindForMime("application/x-xz-compressed-tar"), QString("archive"));
        // Whatever reads as text is text: code, config and data formats too.
        for (const char *mime : {"application/x-ruby", "application/json", "application/x-shellscript",
                                 "application/toml", "application/x-yaml", "application/javascript"})
            QCOMPARE(FolderModel::kindForMime(mime), QString("text"));
        QCOMPARE(FolderModel::kindForMime("application/octet-stream"), QString("other"));
    }

    void backendKeepsHistoryAndCrumbs()
    {
        QTemporaryDir home;
        QDir(home.path()).mkpath("Pictures/Trip");
        Backend backend(home.path());
        backend.navigate(home.path());
        backend.navigate(home.filePath("Pictures/Trip"));
        QCOMPARE(backend.title(), QString("Trip"));
        const auto crumbs = backend.crumbs();
        QCOMPARE(crumbs.size(), 3);
        QCOMPARE(crumbs.first().toMap().value("name").toString(), QString("~"));

        backend.goUp();
        QCOMPARE(backend.path(), home.filePath("Pictures"));
        // Coming back up selects the folder we left.
        QCOMPARE(backend.takeSelection(), QString("Trip"));
        backend.back();
        QCOMPARE(backend.path(), home.filePath("Pictures/Trip"));
        QVERIFY(backend.canForward());
    }

    void backendStaysPutWhenTheHistoryIsGone()
    {
        QTemporaryDir home;
        QDir(home.path()).mkpath("Old");
        QDir(home.path()).mkpath("New");
        Backend backend(home.path());
        backend.navigate(home.filePath("Old"));
        backend.navigate(home.filePath("New"));
        QDir(home.filePath("Old")).removeRecursively();

        // Going back to a folder that's gone says so and leaves the history
        // on the folder shown, with what it was to select next untouched.
        backend.expectSelection("pasted.txt");
        backend.back();
        QCOMPARE(backend.path(), home.filePath("New"));
        QVERIFY(!backend.message().isEmpty());
        QVERIFY(backend.canBack());
        QVERIFY(!backend.canForward());
        QCOMPARE(backend.takeSelection(), QString("pasted.txt"));

        QDir(home.path()).mkpath("Old");
        backend.back();
        QCOMPARE(backend.path(), home.filePath("Old"));
        QDir(home.filePath("New")).removeRecursively();
        backend.forward();
        QCOMPARE(backend.path(), home.filePath("Old"));
        QVERIFY(!backend.canBack());
        QVERIFY(backend.canForward());
    }

    void backendCompletesTypedPaths()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work/folio", "Work/forge", "Documents", ".config"})
            QDir(home.path()).mkpath(dir);
        const QString h = home.path();
        QCOMPARE(Backend::expandTyped("~/Work", h, h), h + "/Work");
        QCOMPARE(Backend::expandTyped("Work/folio", h, h), h + "/Work/folio");
        QCOMPARE(Backend::expandTyped("/etc", h, h), QString("/etc"));
        QCOMPARE(Backend::expandTyped("Work", h, "recent:"), h + "/Work");
        QCOMPARE(Backend::complete("~/Doc", h, h), QString("~/Documents/"));
        QCOMPARE(Backend::complete("~/Work/fo", h, h), QString("~/Work/fo"));   // folio or forge
        QCOMPARE(Backend::complete("~/Work/fol", h, h), QString("~/Work/folio/"));
        QCOMPARE(Backend::complete("~/.co", h, h), QString("~/.config/"));
        QCOMPARE(Backend::complete("~/c", h, h), QString("~/c"));               // hidden stays hidden
        QCOMPARE(Backend::complete("Wo", h, h), QString("Work/"));
        QCOMPARE(Backend::complete("Wo", h, "recent:"), QString("Work/"));
    }

    void backendKeepsTheWorkingDirectoryOnItsFolder()
    {
        const QString before = QDir::currentPath();
        const auto restore = qScopeGuard([before] { QDir::setCurrent(before); });
        QTemporaryDir home;
        QDir(home.path()).mkpath("Work");
        const QString work = QFileInfo(home.filePath("Work")).canonicalFilePath();
        Backend kept(home.path()), other(home.path());
        kept.navigate(work);
        QCOMPARE(QDir::currentPath(), before);   // Not until it's asked to.

        kept.keepWorkingDirectory();
        QCOMPARE(QDir::currentPath(), work);
        kept.navigate(home.path());
        QCOMPARE(QDir::currentPath(), QFileInfo(home.path()).canonicalFilePath());
        kept.navigate(work);
        // A view isn't a folder to work in: it stays where it was.
        kept.navigate(FolderModel::recentPath());
        QCOMPARE(QDir::currentPath(), work);
        // Only the pane used last moves it, until another one takes it.
        other.navigate(home.path());
        QCOMPARE(QDir::currentPath(), work);
        other.keepWorkingDirectory();
        QCOMPARE(QDir::currentPath(), QFileInfo(home.path()).canonicalFilePath());
        kept.navigate(work);
        QCOMPARE(QDir::currentPath(), QFileInfo(home.path()).canonicalFilePath());
    }

    void backendShowItemSelectsTheFile()
    {
        QTemporaryDir home;
        write(home.filePath("report.pdf"), "x");
        Backend backend(home.path());
        backend.showItem(home.filePath("report.pdf"));
        QCOMPARE(backend.path(), home.path());
        QCOMPARE(backend.takeSelection(), QString("report.pdf"));
    }

    void backendReadsDetailsOffTheUiThread()
    {
        // A picture's size and a text's opening lines are read in the
        // background: on a slow mount, reading them as the selection moves
        // would hold up the window.
        QTemporaryDir dir;
        const QString photo = dir.filePath("photo.png"), notes = dir.filePath("notes.txt");
        QVERIFY(QImage(40, 30, QImage::Format_RGB32).save(photo));
        write(notes, "hello");
        Backend backend(dir.path());
        backend.describe({{"path", photo}, {"kind", "image"}, {"mime", "image/png"}});
        QCOMPARE(backend.details().value("type").toString(), QMimeDatabase().mimeTypeForName("image/png").comment());
        QVERIFY(!backend.details().contains("dimensions"));
        QTRY_COMPARE(backend.details().value("dimensions").toString(), QString("40 × 30"));

        // The next item starts from its type alone, and what was read for
        // one left behind is dropped.
        backend.describe({{"path", photo}, {"kind", "image"}, {"mime", "image/png"}});
        backend.describe({{"path", notes}, {"kind", "text"}, {"mime", "text/plain"}});
        QVERIFY(!backend.details().contains("dimensions"));
        QTRY_COMPARE(backend.details().value("text").toString(), QString("hello"));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(!backend.details().contains("dimensions"));

        // A picture replaced by a folder of the same name doesn't keep the
        // picture's size.
        backend.describe({{"path", photo}, {"kind", "image"}, {"mime", "image/png"}});
        QTRY_VERIFY(backend.details().contains("dimensions"));
        backend.describe({{"path", photo}, {"kind", "folder"}, {"mime", "inode/directory"}});
        QVERIFY(!backend.details().contains("dimensions"));
    }

    void backendReadsTrashNotesOffTheUiThread()
    {
        // Finding a trashed item's note looks over every mounted drive, so
        // it's read with the rest of the details.
        QTemporaryDir home;
        const auto restore = dataHomeAt(home.path());
        QDir().mkpath(home.filePath("Trash/files"));
        QDir().mkpath(home.filePath("Trash/info"));
        write(home.filePath("Trash/files/old.pdf"), "x");
        write(home.filePath("Trash/info/old.pdf.trashinfo"),
              "[Trash Info]\nPath=" + home.filePath("Docs/old.pdf").toUtf8() + "\nDeletionDate=2026-11-10T12:00:00\n");
        Backend backend(home.path());
        backend.describe({{"path", home.filePath("Trash/files/old.pdf")}, {"kind", "pdf"}, {"mime", "application/pdf"}}, true);
        QVERIFY(!backend.details().contains("origin"));
        QTRY_COMPARE(backend.details().value("origin").toString(), home.filePath("Docs/old.pdf"));
        QCOMPARE(backend.details().value("deleted").toDateTime(), QDateTime(QDate(2026, 11, 10), QTime(12, 0)));
    }

    void mounterOutlivedByGiosReply()
    {
        // A local folder can't be mounted, so GIO answers with an error on
        // a later turn of the main loop, as it does for a slow tailnet
        // machine. By then the first Mounter's window has closed.
        QTemporaryDir dir;
        const QString uri = QUrl::fromLocalFile(dir.path()).toString();
        auto *closed = new Mounter;
        closed->mount(uri);
        delete closed;
        Mounter open;
        QSignalSpy failed(&open, &Mounter::failed);
        open.mount(uri);
        QVERIFY(failed.wait(2000));
        QVERIFY(!open.busy());
    }

    void trashFollowsXdgDataHome()
    {
        // g_file_trash puts things in $XDG_DATA_HOME/Trash, wherever that
        // is, so the sidebar's Trash opens there and is called "Trash".
        QTemporaryDir home, data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash/files");
        Places places(home.path());
        QCOMPARE(places.views().last().toMap().value("path").toString(), trash);
        Backend backend(home.path());
        backend.navigate(trash);
        QCOMPARE(backend.path(), trash);
        QCOMPARE(backend.title(), QString("Trash"));
    }

    void placesListEveryFolderInHome()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work", "Documents", "omarchy", ".config", "project10", "project9"})
            QDir(home.path()).mkpath(dir);
        write(home.filePath("notes.txt"), "not a folder");
        auto names = [](const Places &p) {
            QStringList list;
            for (const auto &v : p.places()) list << v.toMap().value("name").toString();
            return list;
        };
        QCOMPARE(names(Places(home.path())),
                 QStringList({"Documents", "Omarchy", "project9", "project10", "Work"}));

        // Dropbox shows once its folder exists, with its own mark.
        QDir(home.path()).mkpath("Dropbox");
        const Places withDropbox(home.path());
        QCOMPARE(names(withDropbox).at(1), QString("Dropbox"));
        QCOMPARE(withDropbox.places().at(1).toMap().value("icon").toString(), QString("dropbox"));
        QCOMPARE(withDropbox.places().at(0).toMap().value("icon").toString(), QString("documents"));
        QCOMPARE(withDropbox.places().at(5).toMap().value("icon").toString(), QString("folder"));
    }

    void placesPinFoldersAsGtkBookmarks()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work/folio", "Downloads", "Work/hype"})
            QDir(home.path()).mkpath(dir);
        const QString bookmarks = home.filePath(".config/gtk-3.0/bookmarks");
        QDir().mkpath(QFileInfo(bookmarks).absolutePath());
        // Omarchy's seeded home folders and a remote bookmark are kept in
        // the file but not shown as pins.
        write(bookmarks, QUrl::fromLocalFile(home.filePath("Downloads")).toEncoded() + " Downloads\n"
                         "sftp://studio/srv Studio\n"
                         + QUrl::fromLocalFile(home.filePath("Work/hype")).toEncoded() + " Slides\n");

        Places places(home.path());
        QCOMPARE(places.pinned().size(), 1);
        QCOMPARE(places.pinned().at(0).toMap().value("name").toString(), QString("Slides"));

        QCOMPARE(places.pin(home.filePath("Work/folio")), QString());
        QVERIFY(places.isPinned(home.filePath("Work/folio")));
        QCOMPARE(places.pinned().size(), 2);
        QVERIFY(!places.pin(home.filePath("Downloads")).isEmpty()); // already under Home
        // Hidden folders in home aren't listed under Home, so they can be
        // pinned.
        QCOMPARE(places.pin(home.filePath(".config")), QString());
        QVERIFY(places.isPinned(home.filePath(".config")));
        places.unpin(home.filePath(".config"));
        QVERIFY(!places.isPinned(home.filePath(".config")));

        places.unpin(home.filePath("Work/hype"));
        QFile file(bookmarks);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.contains("sftp://studio/srv Studio"));
        QVERIFY(saved.contains("Downloads"));
        QVERIFY(saved.contains("Work/folio"));
        QVERIFY(!saved.contains("Work/hype"));
        QCOMPARE(places.pinned().size(), 1);
    }

    void placesParseTailscalePeers()
    {
        const QByteArray json = R"({"Peer":{
            "a":{"HostName":"nas","DNSName":"nas.example.ts.net.","OS":"linux","Online":false},
            "b":{"HostName":"studio","DNSName":"studio.example.ts.net.","OS":"linux","Online":true},
            "c":{"HostName":"phone","DNSName":"phone.example.ts.net.","OS":"iOS","Online":true}}})";
        const auto peers = Places::parseTailscale(json);
        // Offline machines and phones are left out.
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers.at(0).toMap().value("name").toString(), QString("studio"));
        QCOMPARE(peers.at(0).toMap().value("host").toString(), QString("studio.example.ts.net"));
        QCOMPARE(peers.at(0).toMap().value("os").toString(), QString("Linux"));
    }

    void placesRememberOpenedMachines()
    {
        QTemporaryDir home;
        Places places(home.path());
        places.remember("studio.example.ts.net");
        places.remember("nas.example.ts.net");
        // Most recently opened first; opening one again moves it up.
        QCOMPARE(places.opened(), QStringList({"nas.example.ts.net", "studio.example.ts.net"}));
        places.remember("studio.example.ts.net");
        QCOMPARE(places.opened(), QStringList({"studio.example.ts.net", "nas.example.ts.net"}));
        // Kept in Folio's settings, with when.
        QSettings settings(home.filePath(".config/folio/folio.conf"), QSettings::IniFormat);
        QCOMPARE(settings.beginReadArray("opened-machines"), 2);
        settings.setArrayIndex(0);
        QVERIFY(QDateTime::fromString(settings.value("when").toString(), Qt::ISODate).isValid());
        settings.endArray();
        QCOMPARE(Places(home.path()).opened().size(), 2);

        places.forget("studio.example.ts.net");
        QCOMPARE(places.opened(), QStringList({"nas.example.ts.net"}));

        // Only the last 20 are kept.
        for (int i = 0; i < 25; ++i)
            places.remember(QString("machine%1.example.ts.net").arg(i));
        QCOMPARE(places.opened().size(), 20);
        QCOMPARE(places.opened().first(), QString("machine24.example.ts.net"));
        QCOMPARE(places.opened().last(), QString("machine5.example.ts.net"));
    }

    void placesShowOnlyOnlineOpenedMachines()
    {
        const QVariantList peers = Places::parseTailscale(tailnet());
        // The offline nas and the phone aren't shown, however recently opened.
        const QStringList opened{"nas.example.ts.net", "studio.example.ts.net", "phone.example.ts.net",
                                 "build.example.ts.net"};
        const auto shown = Places::onlineOf(opened, peers);
        QCOMPARE(shown.size(), 2);
        QCOMPARE(shown.at(0).toMap().value("name").toString(), QString("studio"));
        QCOMPARE(shown.at(1).toMap().value("name").toString(), QString("build"));
        QCOMPARE(Places::onlineOf(opened, peers, 1).size(), 1);

        // The sidebar shows a handful at most.
        QVariantList many;
        QStringList hosts;
        for (int i = 0; i < 10; ++i) {
            const QString host = QString("machine%1.example.ts.net").arg(i);
            many << QVariantMap{{"name", QString("machine%1").arg(i)}, {"host", host}, {"os", "Linux"}};
            hosts << host;
        }
        QCOMPARE(Places::onlineOf(hosts, many).size(), 6);
    }

    void tailnetViewListsOnlineDesktopsByName()
    {
        const auto entries = FolderModel::listPeers(Places::parseTailscale(tailnet()));
        QStringList names;
        for (const auto &e : entries) names << e.name;
        // Offline machines and phones are left out; the rest go by name,
        // whatever their case.
        QCOMPARE(names, QStringList({"build", "Conference-Room", "studio"}));
        const auto &build = entries.at(0);
        QCOMPARE(build.kind, QString("peer"));
        QCOMPARE(build.path, FolderModel::tailnetPath() + "build.example.ts.net");
        QCOMPARE(build.extension, QString("Windows"));
        QVERIFY(!build.isDir);
    }

    void tailnetViewIsAPlaceOfItsOwn()
    {
        QTemporaryDir home;
        Backend backend(home.path());
        backend.navigate(FolderModel::tailnetPath());
        QCOMPARE(backend.path(), FolderModel::tailnetPath());
        QCOMPARE(backend.title(), QString("Tailnet"));
        QCOMPARE(backend.crumbs().size(), 1);
        QCOMPARE(backend.crumbs().at(0).toMap().value("name").toString(), QString("Tailnet"));
        backend.goUp();
        QCOMPARE(backend.path(), FolderModel::tailnetPath());
    }

    void typedViewsGoToTheirView()
    {
        // The path bar shows a view as its own name, so Return on it, or
        // the name typed, goes there rather than to a folder it isn't.
        QTemporaryDir home;
        Backend backend(home.path());
        backend.navigate(FolderModel::tailnetPath());
        QVERIFY(backend.goTyped(backend.typedPath()));
        QCOMPARE(backend.path(), FolderModel::tailnetPath());
        QVERIFY(backend.message().isEmpty());
        QVERIFY(backend.goTyped(" recent: "));
        QCOMPARE(backend.path(), FolderModel::recentPath());
    }

    void tailnetViewSearchesMachineNames()
    {
        const QVariantList peers = Places::parseTailscale(tailnet());
        QCOMPARE(FolderModel::listPeers(peers, "STU").size(), 1);

        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPeers(peers);
        model.setPath(FolderModel::tailnetPath());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 3);
        model.setQuery("o");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.get(0).value("name").toString(), QString("Conference-Room"));
        QCOMPARE(model.get(1).value("name").toString(), QString("studio"));
        // A machine going away leaves the view at the next poll.
        QVariantList without = peers;
        without.removeIf([](const QVariant &p) { return p.toMap().value("name") == "studio"; });
        model.setPeers(without);
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 1);
        // A machine renamed keeps its host, and so its path; the view still
        // shows the new name.
        QVariantList renamed = without;
        QVariantMap peer = renamed[0].toMap();
        const QString host = peer.value("host").toString();
        peer["name"] = "Boardroom";
        renamed[0] = peer;
        model.setQuery("");
        QVERIFY(loaded.wait(3000));
        model.setPeers(renamed);
        QVERIFY(loaded.wait(3000));
        bool found = false;
        for (int i = 0; i < model.count(); ++i)
            found |= model.get(i).value("name").toString() == "Boardroom";
        QVERIFY2(found, qPrintable(host));
    }

    void tailnetMountsGoByTheMachinesName()
    {
        QCOMPARE(Backend::mountLabel("sftp:host=studio.example.ts.net"), QString("studio"));
        QCOMPARE(Backend::mountLabel("sftp:host=studio.example.ts.net,user=me"), QString("studio"));
        QCOMPARE(Backend::mountLabel("sftp:host=files.example.com"), QString("files.example.com"));
        QCOMPARE(Backend::mountLabel("smb-share:server=nas,share=media"), QString("nas"));
        QCOMPARE(Backend::mountLabel("ftp:host=ftp.example.com"), QString("ftp.example.com"));
        QCOMPARE(Backend::mountLabel("dav:host=cloud.example.com,ssl=true,prefix=%2Fremote.php"), QString("cloud.example.com"));
    }

    void typedServerAddressesAreMounted()
    {
        // What's typed is what's mounted, cleaned up for GIO: GVfs knows
        // where on the mount the path is, which for WebDAV can be part of
        // the mount itself.
        QCOMPARE(Backend::serverAddress("sftp://nas"), QString("sftp://nas/"));
        QCOMPARE(Backend::serverAddress("  SFTP://me@nas:2222/srv/My Files "), QString("sftp://me@nas:2222/srv/My%20Files"));
        QCOMPARE(Backend::serverAddress("smb://nas/media/Films"), QString("smb://nas/media/Films"));
        QCOMPARE(Backend::serverAddress("ftp://ftp.example.com/pub/"), QString("ftp://ftp.example.com/pub/"));
        QCOMPARE(Backend::serverAddress("davs://cloud.example.com/remote.php/dav/files/me"),
                 QString("davs://cloud.example.com/remote.php/dav/files/me"));
        QCOMPARE(Backend::serverAddress("dav://10.0.0.5:8080"), QString("dav://10.0.0.5:8080/"));
        // Paths, other schemes and addresses without a server are not.
        for (const char *text : {"~/Work", "/etc", "Work/folio", "file:///etc", "http://example.com", "sftp://", "smb:/nas"})
            QVERIFY2(Backend::serverAddress(text).isEmpty(), text);
    }

    void placesListOnlyPhysicalDrives()
    {
        const QList<Places::Mount> mounts{
            {"/", "/dev/mapper/root", "btrfs", ""},
            {"/home", "/dev/mapper/root", "btrfs", ""},
            {"/.snapshots", "/dev/mapper/root", "btrfs", ""},
            {"/boot", "/dev/nvme0n1p1", "vfat", "EFI"},
            {"/tmp", "tmpfs", "tmpfs", ""},
            {"/var/lib/snapd/snap/core", "/dev/loop3", "squashfs", ""},
            {"/mnt/archive", "/dev/sda1", "ext4", ""},
            {"/run/media/me/Photos", "/dev/sdb1", "exfat", "Photos"},
            {"/run/media/me/Photos/again", "/dev/sdb1", "exfat", "Photos"},
            {"/run/user/1000/gvfs", "gvfsd-fuse", "fuse.gvfsd-fuse", ""},
        };
        const auto drives = Places::physicalDrives(mounts, "/dev/mapper/root");
        QCOMPARE(drives.size(), 2);
        QCOMPARE(drives.at(0).toMap().value("name").toString(), QString("archive"));
        QCOMPARE(drives.at(1).toMap().value("name").toString(), QString("Photos"));
        QCOMPARE(drives.at(1).toMap().value("path").toString(), QString("/run/media/me/Photos"));
    }

    void drivesSayHowTheyreLetGoOf()
    {
        // A stick or card is ejected, so it's safe to pull out; a disk that
        // can only be unmounted is unmounted; one GIO can't let go of (only
        // fstab knows it) has no button at all.
        Places::Mount stick{"/run/media/me/Photos", "/dev/sdb1", "exfat", "Photos"};
        stick.canEject = stick.canUnmount = true;
        stick.free = 120'000'000'000;
        Places::Mount disk{"/run/media/me/Backup", "/dev/sdc1", "ext4", "Backup"};
        disk.canUnmount = true;
        const Places::Mount fixed{"/mnt/archive", "/dev/sda1", "ext4", ""};
        const auto drives = Places::physicalDrives({stick, disk, fixed}, "/dev/nvme0n1p2");
        QCOMPARE(drives.size(), 3);
        QCOMPARE(drives.at(0).toMap().value("release").toString(), QString("eject"));
        QCOMPARE(drives.at(1).toMap().value("release").toString(), QString("unmount"));
        QCOMPARE(drives.at(2).toMap().value("release").toString(), QString());
        QCOMPARE(drives.at(0).toMap().value("free").toLongLong(), 120'000'000'000);
        QCOMPARE(drives.at(2).toMap().value("free").toLongLong(), -1);
    }

    void drivesSayWhatsFreeInTheLanguagesDigits()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \"Format.js\" as Format\n"
                          "QtObject { function free(bytes) { return Format.free(bytes) } }",
                          QUrl::fromLocalFile(QFINDTESTDATA("../src/Format.js")));
        std::unique_ptr<QObject> format(component.create());
        QVERIFY(format);
        auto free = [&](qint64 bytes) {
            QVariant text;
            QMetaObject::invokeMethod(format.get(), "free", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, bytes));
            return text.toString();
        };
        QCOMPARE(free(120'000'000'000), QString("120.0 GB free"));
        QCOMPARE(free(512), QString("512 B free"));

        const QLocale before;
        const auto restore = qScopeGuard([before] { QLocale::setDefault(before); });
        QLocale::setDefault(QLocale("ar_EG"));
        QCOMPARE(free(120'000'000'000), QLocale("ar_EG").toString(120.0, 'f', 1) + " GB free");
    }

    void placesParseDropboxStatus()
    {
        QCOMPARE(Places::parseDropboxStatus("Up to date\n"), QString("synced"));
        QCOMPARE(Places::parseDropboxStatus("Syncing 3 files\n"), QString("syncing"));
        QCOMPARE(Places::parseDropboxStatus("Dropbox isn't running!\n"), QString("stopped"));
    }

    void thumbnailsFollowTheFreedesktopCache()
    {
        QTemporaryDir dir;
        QImage picture(800, 400, QImage::Format_RGB32);
        picture.fill(Qt::red);
        const QString file = dir.filePath("wide.png");
        QVERIFY(picture.save(file));

        Thumbnails thumbnails(dir.filePath("cache"), {dir.filePath("none")});
        const QImage thumb = thumbnails.load(file, "image/png", 256);
        QCOMPARE(thumb.size(), QSize(256, 128));

        // Cached under the spec's name, tagged so other apps trust it.
        const QString cached = dir.filePath("cache/large/" + Thumbnails::cacheKey(file));
        QVERIFY(QFile::exists(cached));
        const QImage stored(cached);
        QCOMPARE(stored.text("Thumb::URI"), QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded));
        QVERIFY(!stored.text("Thumb::MTime").isEmpty());

        // A fresh cache entry is served as is: overwrite it with a marker
        // color and the next load must return the cached pixels.
        QImage marked = stored;
        marked.fill(Qt::blue);
        QVERIFY(marked.save(cached));
        QCOMPARE(thumbnails.load(file, "image/png", 256).pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(Thumbnails::bucketFor(128), QString("normal"));
        QCOMPARE(Thumbnails::bucketFor(512), QString("x-large"));
    }

    void thumbnailsFollowAPictureSavedOver()
    {
        // Qt keeps the images it has shown by their URL, and a re-read makes
        // new tiles that ask again, so a picture saved over must ask with a
        // URL of its own, and the cache must make it anew.
        QTemporaryDir dir;
        const QString file = dir.filePath("photo.png");
        QVERIFY(QImage(64, 32, QImage::Format_RGB32).save(file));
        const QDateTime then = QFileInfo(file).lastModified();

        Thumbnails thumbnails(dir.filePath("cache"), {dir.filePath("none")});
        QQmlEngine engine;
        engine.addImageProvider("thumb", new ThumbnailProvider(&thumbnails));
        QQmlComponent tile(&engine);
        tile.setData("import QtQuick\nimport \"Format.js\" as Format\n"
                     "Image { required property string path; required property var modified\n"
                     "        asynchronous: true; source: Format.thumb(path, \"image/png\", 128, modified) }",
                     QUrl::fromLocalFile(QFINDTESTDATA("../src/Format.js")));
        auto shownWidth = [&](const QDateTime &modified) {
            std::unique_ptr<QObject> image(tile.createWithInitialProperties({{"path", file}, {"modified", modified}}));
            const bool shown = image && QTest::qWaitFor([&] { return image->property("implicitWidth").toInt() > 0; }, 5000);
            return shown ? image->property("implicitWidth").toInt() : -1;
        };
        QCOMPARE(shownWidth(then), 128);

        // Saved over twice within one second, as an editor's quick saves
        // can be: the spec's whole seconds can't tell them apart.
        const QDateTime second = QDateTime::fromSecsSinceEpoch(then.toSecsSinceEpoch() + 60);
        auto saveAt = [&](const QSize &size, const QDateTime &when) {
            QVERIFY(QImage(size, QImage::Format_RGB32).save(file));
            QFile saved(file);
            QVERIFY(saved.open(QIODevice::ReadWrite));
            QVERIFY(saved.setFileTime(when, QFileDevice::FileModificationTime));
        };
        saveAt({32, 64}, second.addMSecs(100));
        QCOMPARE(shownWidth(second.addMSecs(100)), 64);
        saveAt({64, 32}, second.addMSecs(600));
        QCOMPARE(shownWidth(second.addMSecs(600)), 128);
    }

    void thumbnailsReadInstalledThumbnailers()
    {
        QTemporaryDir dir;
        write(dir.filePath("demo.thumbnailer"),
              "[Thumbnailer Entry]\nTryExec=/bin/sh\nExec=/bin/sh -c \"exit 1\" %i %o %s\nMimeType=video/mp4;video/webm;\n");
        Thumbnails thumbnails(dir.filePath("cache"), {dir.path()});
        QVERIFY(thumbnails.thumbnailerFor("video/webm").startsWith("/bin/sh"));
        QVERIFY(thumbnails.thumbnailerFor("video/ogg").isEmpty());

        // A thumbnailer that fails leaves a failure marker, so it isn't retried.
        write(dir.filePath("clip.mp4"), "not a video");
        QVERIFY(thumbnails.load(dir.filePath("clip.mp4"), "video/mp4", 128).isNull());
        QVERIFY(QFile::exists(dir.filePath("cache/fail/folio/" + Thumbnails::cacheKey(dir.filePath("clip.mp4")))));
    }

    void thumbnailsRememberOnlyWhatFailed()
    {
        QTemporaryDir dir;
        QImage frame(64, 32, QImage::Format_RGB32);
        frame.fill(Qt::green);
        QVERIFY(frame.save(dir.filePath("frame.png")));
        write(dir.filePath("heif.thumbnailer"),
              "[Thumbnailer Entry]\nExec=/bin/cp \"" + dir.filePath("frame.png").toUtf8() + "\" %o\nMimeType=image/heif;\n");
        Thumbnails thumbnails(dir.filePath("cache"), {dir.path()});

        // Nothing could make one, so nothing failed: a thumbnailer installed
        // later still gets its turn.
        write(dir.filePath("bundle.zip"), "not a zip");
        QVERIFY(thumbnails.load(dir.filePath("bundle.zip"), "application/zip", 128).isNull());
        QVERIFY(!QFile::exists(dir.filePath("cache/fail/folio/" + Thumbnails::cacheKey(dir.filePath("bundle.zip")))));

        // A folder's peek asks for any picture, and gets the thumbnailer for
        // the one it names.
        write(dir.filePath("photo.heic"), "not a real heic");
        QCOMPARE(thumbnails.load(dir.filePath("photo.heic"), "image/*", 128).size(), QSize(64, 32));
    }

    void thumbnailsSetTextOnAPage()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.md"), "# Packing\n\n- passport\n- charger\n");
        const QImage page = Thumbnails::textPage(dir.filePath("notes.md"), 256, QColor("#fafaf7"), QColor("#303034"));
        QCOMPARE(page.size(), QSize(218, 256));
        // Dark ink near the top left, where the first line is; plain paper
        // at the bottom right.
        bool ink = false;
        for (int y = 0; y < 40 && !ink; ++y)
            for (int x = 0; x < 120 && !ink; ++x)
                ink = qGray(page.pixel(x, y)) < 128;
        QVERIFY(ink);
        QVERIFY(qGray(page.pixel(214, 252)) > 240);

        // A FIFO named like text is never opened: reading it would block.
        const QString fifo = dir.filePath("events.txt");
        QCOMPARE(mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QVERIFY(Thumbnails::textPage(fifo, 256, Qt::white, Qt::black).isNull());

        // Binary that claims to be text gets the stand-in.
        write(dir.filePath("blob.txt"), QByteArray("abc\0def", 7));
        QVERIFY(Thumbnails::textPage(dir.filePath("blob.txt"), 256, Qt::white, Qt::black).isNull());
    }

    void thumbnailsDrawAPdfsFirstPage()
    {
        if (QStandardPaths::findExecutable("pdftoppm").isEmpty())
            QSKIP("pdftoppm (Poppler) isn't installed");
        QTemporaryDir dir;
        {
            QPdfWriter pdf(dir.filePath("itinerary.pdf"));
            pdf.setPageSize(QPageSize(QPageSize::A4));
            QPainter painter(&pdf);
            painter.fillRect(QRect(0, 0, 2000, 2000), Qt::black);
        }
        Thumbnails thumbnails(dir.filePath("cache"), {dir.path()});
        const QImage page = thumbnails.load(dir.filePath("itinerary.pdf"), "application/pdf", 256);
        QCOMPARE(page.height(), 256);
        QVERIFY(page.width() > 150 && page.width() < 200);
        QVERIFY(qGray(page.pixel(5, 5)) < 64);
        QVERIFY(qGray(page.pixel(page.width() - 3, page.height() - 3)) > 200);
    }

    void previewerPicksAViewerForEachType()
    {
        QCOMPARE(Previewer::viewerFor("image/png", false), QString("image"));
        QCOMPARE(Previewer::viewerFor("image/gif", false), QString("animated"));
        QCOMPARE(Previewer::viewerFor("image/x-canon-cr2", false), QString("thumbnail"));
        QCOMPARE(Previewer::viewerFor("video/mp4", false), QString("video"));
        QCOMPARE(Previewer::viewerFor("audio/mpeg", false), QString("audio"));
        QCOMPARE(Previewer::viewerFor("application/pdf", false), QString("pdf"));
        QCOMPARE(Previewer::viewerFor("text/markdown", false), QString("markdown"));
        QCOMPARE(Previewer::viewerFor("text/x-c++src", false), QString("text"));
        QCOMPARE(Previewer::viewerFor("application/x-shellscript", false), QString("text"));
        QCOMPARE(Previewer::viewerFor("application/zip", false), QString("other"));
        QCOMPARE(Previewer::viewerFor("inode/directory", true), QString("folder"));
    }

    void previewerReadsTextButNotBinaries()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.txt"), "hello\nworld\n");
        write(dir.filePath("blob.txt"), QByteArray("abc\0def", 7));
        QCOMPARE(Previewer::readText(dir.filePath("notes.txt")), QString("hello\nworld\n"));
        QVERIFY(Previewer::readText(dir.filePath("blob.txt")).isEmpty());
        QCOMPARE(Previewer::readText(dir.filePath("notes.txt"), 5), QString("hello"));

        // A binary with a text name falls back to the plain file view.
        Previewer previewer;
        previewer.start(dir.filePath("blob.txt"), false, "name");
        QTRY_COMPARE(previewer.viewer(), QString("other"));
    }

    void previewerStepsThroughTheFolder()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("a.md"), "# A");
        write(dir.filePath("b.txt"), "b");
        write(dir.filePath("c.pdf"), "%PDF");
        write(dir.filePath(".hidden"), "h");

        Previewer previewer;
        QSignalSpy changed(&previewer, &Previewer::changed);
        previewer.start(dir.filePath("b.txt"), false, "name");
        // Shown at once, before the folder is listed.
        QCOMPARE(previewer.item().value("name").toString(), QString("b.txt"));
        QTRY_COMPARE(previewer.text(), QString("b"));

        // The listing moves the index without touching what's shown, which
        // would restart a playing video.
        const int shown = changed.count();
        QTRY_COMPARE(previewer.count(), 4);
        QCOMPARE(previewer.index(), 2);
        QCOMPARE(changed.count(), shown);
        previewer.previous();
        QCOMPARE(previewer.viewer(), QString("markdown"));
        previewer.previous();
        QCOMPARE(previewer.viewer(), QString("folder"));
        QTRY_COMPARE(previewer.item().value("itemCount").toInt(), 0);
        previewer.previous();
        QCOMPARE(previewer.index(), 0);
        previewer.next();
        previewer.next();
        previewer.next();
        QCOMPARE(previewer.viewer(), QString("pdf"));
        previewer.next();
        QCOMPARE(previewer.index(), 3);

        // Folio pointing at a sibling keeps the list; another folder starts over.
        previewer.show(dir.filePath("a.md"));
        QCOMPARE(previewer.index(), 1);
        QCOMPARE(previewer.count(), 4);
        QTemporaryDir other;
        write(other.filePath("x.txt"), "x");
        write(other.filePath("y.txt"), "y");
        previewer.show(other.filePath("y.txt"));
        QCOMPARE(previewer.count(), 1);
        QCOMPARE(previewer.index(), 0);
        QTRY_COMPARE(previewer.count(), 2);
        QCOMPARE(previewer.index(), 1);
        QVERIFY(changed.count() > 0);
    }

    void previewerReadsOffTheUiThread()
    {
        // A big folder, or any file on a slow mount, would hold up the
        // window for as long as reading it takes, so an item shows at once
        // and its text, or a folder's count and first pictures, follow.
        QTemporaryDir dir;
        write(dir.filePath("notes.txt"), "hello");
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("album/a.jpg"), "x");
        Previewer previewer;
        previewer.start(dir.filePath("notes.txt"), false, "name");
        QCOMPARE(previewer.text(), QString());
        QTRY_COMPARE(previewer.text(), QString("hello"));
        previewer.show(dir.filePath("album"));
        QCOMPARE(previewer.item().value("itemCount").toInt(), -1);
        QTRY_COMPARE(previewer.item().value("itemCount").toInt(), 1);

        // What's read for an item left behind is dropped.
        previewer.show(dir.filePath("notes.txt"));
        previewer.show(dir.filePath("album"));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(previewer.text(), QString());
    }

    void previewerCountsHiddenFilesWhenTheyShow()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("dots");
        write(dir.filePath("dots/.config"), "c");

        // The first folder shown counts what Folio's tile counts.
        Previewer previewer;
        previewer.start(dir.filePath("dots"), true, "name");
        QTRY_COMPARE(previewer.item().value("itemCount").toInt(), 1);
    }

    void previewerPeeksIntoFolders()
    {
        // A folder's first pictures are for the preview window, so its item
        // has them and the folder model's (which never fills them) doesn't.
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("album/a.jpg"), "x");
        Previewer previewer;
        previewer.start(dir.filePath("album"), false, "name");
        QTRY_COMPARE(previewer.item().value("peek").toStringList(), QStringList({dir.filePath("album/a.jpg")}));
        QVERIFY(!FolderModel::toMap(FolderModel::Entry()).contains("peek"));
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
    }

    void themeReadsThePalette()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"light\"\naccent = \"#112233\"\nbackground = \"#FFFCF0\"\nforeground = \"#100F0F\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#FFFCF0"));
        // Missing derived backgrounds fall back to the base.
        QCOMPARE(theme.darkBackground(), QColor("#FFFCF0"));
        QVERIFY(theme.light());
    }

    void themeDerivesALighterBackgroundWhenItMatches()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.filePath("theme/colors.toml"), "background = \"#101315\"\nforeground = \"#e0e0e0\"\nlighter_background = \"#101315\"\n");
        Theme same(dir.path());
        QCOMPARE(same.background(), QColor("#101315"));
        QVERIFY(same.lighterBackground() != same.background());
        QVERIFY(same.lighterBackground().lightnessF() > same.background().lightnessF());

        write(dir.filePath("theme/colors.toml"), "background = \"#101315\"\nforeground = \"#e0e0e0\"\nlighter_background = \"#2a2e36\"\n");
        Theme distinct(dir.path());
        QCOMPARE(distinct.lighterBackground(), QColor("#2a2e36"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":4}");

        Theme theme(dir.path(), nullptr, command);
        QTRY_COMPARE(theme.radius(), 4);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void themeAsksForRoundingOnlyWhileActive()
    {
        // The fake hyprctl lives outside the theme folder: Theme watches that
        // folder and its parent, and a change there re-reads the rounding
        // whatever the state. Both sit in a private parent, so other programs'
        // temporary files can't count as a change.
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("theme") && QDir(root.path()).mkpath("bin"));
        const QString themeDir = root.filePath("theme");
        const QString response = root.filePath("bin/response.json"), command = root.filePath("bin/hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":3}");

        Theme theme(themeDir, nullptr, command);
        QTRY_COMPARE(theme.radius(), 3);
        // Away from Folio, a change goes unnoticed for longer than the poll,
        // and it's picked up once Folio is back.
        emit qGuiApp->applicationStateChanged(Qt::ApplicationInactive);
        // A read already under way may still finish (it's killed within
        // 500ms), so the change comes after it.
        QTest::qWait(600);
        write(response, "{\"int\":6}");
        QTest::qWait(1500);
        QCOMPARE(theme.radius(), 3);
        emit qGuiApp->applicationStateChanged(Qt::ApplicationActive);
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Open").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n items": {"one": "%n item", "other": "%n items"}})");
        write(dir.path() + "/ru.json", R"({"%n items": {"one": "%n элемент", "few": "%n элемента", "many": "%n элементов", "other": "%n элемента"}})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n items", nullptr, 21), QString("%n элемент"));
        QCOMPARE(translator.translate("Main", "%n items", nullptr, 25), QString("%n элементов"));
    }

private:
    // Packs `items`, from `folder`, into `archive`, its kind from its name.
    static void pack(const QString &archive, const QString &folder, const QStringList &items)
    {
        QProcess tar;
        tar.start("bsdtar", QStringList{"-acf", archive, "-C", folder} + items);
        QVERIFY(tar.waitForFinished() && tar.exitCode() == 0);
    }

    // Everything in a folder, the hidden too, by name.
    static QStringList everything(const QString &folder)
    {
        return QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    }

    static QByteArray contents(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(FolioTests)
#include "folio_tests.moc"
