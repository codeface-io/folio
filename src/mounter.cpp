#include "mounter.h"

#include <QPointer>

// GIO's headers use `signals` as a struct member, which Qt defines as a
// keyword.
#undef signals
#include <gio/gio.h>
#define signals Q_SIGNALS

struct MounterCallbacks {
    // GMountOperation's own handlers run after these and answer
    // "unhandled" at once, which GVfs reports as a cancelled login; stopping
    // the emission leaves the operation waiting for the user's reply.

    // GVfs's SFTP backend asks two questions, about a host seen for the
    // first time and about a host whose key differs from the key for its IP
    // address, and both list "Log In Anyway" first. A tailnet machine is
    // reached over WireGuard and already authenticated by Tailscale, so the
    // answer is always yes. A changed key for a known machine never gets
    // here: ssh refuses it outright. Any other server's questions (a WebDAV
    // server's certificate that can't be verified, too) are the user's.
    static void question(GMountOperation *op, const char *message, const char **choices, gpointer data) {
        g_signal_stop_emission_by_name(op, "ask-question");
        auto *self = static_cast<Mounter *>(data);
        if (self->m_trusted) {
            g_mount_operation_set_choice(op, 0);
            g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
            return;
        }
        self->m_prompt = QString::fromUtf8(message).trimmed();
        self->m_choices.clear();
        for (const char **choice = choices; choice && *choice; ++choice)
            self->m_choices << QString::fromUtf8(*choice);
        self->setAsking(true);
    }

    static void password(GMountOperation *op, const char *message, const char *user, const char *,
                         GAskPasswordFlags, gpointer data) {
        g_signal_stop_emission_by_name(op, "ask-password");
        auto *self = static_cast<Mounter *>(data);
        self->m_prompt = QString::fromUtf8(message).trimmed();
        self->m_user = QString::fromUtf8(user ? user : "");
        self->setAsking(true);
    }

    // GVfs gave up waiting for an answer.
    static void aborted(GMountOperation *, gpointer data) {
        static_cast<Mounter *>(data)->setAsking(false);
    }

    static void mounted(GObject *source, GAsyncResult *result, gpointer data) {
        auto *guard = static_cast<QPointer<Mounter> *>(data);
        Mounter *self = guard->data();
        delete guard;
        GError *error = nullptr;
        const bool ok = g_file_mount_enclosing_volume_finish(G_FILE(source), result, &error);
        if (self) {
            // Mounted earlier (by Folio or another app) is as good as now.
            const bool already = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED);
            if (ok || already) {
                char *path = g_file_get_path(G_FILE(source));
                const QString local = QString::fromUtf8(path ? path : "");
                g_free(path);
                self->finish();
                if (local.isEmpty())
                    emit self->failed(QObject::tr("GVfs mounted it but gave no local folder for it"));
                else
                    emit self->mounted(local);
            } else {
                const bool cancelled = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED);
                const QString message = cancelled ? QString() : QString::fromUtf8(error ? error->message : "");
                self->finish();
                emit self->failed(message);
            }
        }
        if (error)
            g_error_free(error);
    }
};

Mounter::Mounter(QObject *parent) : QObject(parent) {}

Mounter::~Mounter() {
    // A prompt nobody will answer any more would keep GVfs waiting.
    cancel();
    if (m_operation)
        g_signal_handlers_disconnect_by_data(m_operation, this);
    finish();
}

void Mounter::mount(const QString &uri, bool trusted) {
    if (m_operation)
        return;
    m_trusted = trusted;
    GFile *file = g_file_new_for_uri(uri.toUtf8().constData());
    m_operation = g_mount_operation_new();
    g_signal_connect(m_operation, "ask-question", G_CALLBACK(MounterCallbacks::question), this);
    g_signal_connect(m_operation, "ask-password", G_CALLBACK(MounterCallbacks::password), this);
    g_signal_connect(m_operation, "aborted", G_CALLBACK(MounterCallbacks::aborted), this);
    // Qt runs on GLib's main loop on Linux, so the reply arrives on the UI
    // thread like any other event. The window can close before GVfs answers,
    // and GIO calls back regardless, so the callback gets a guard, not `this`.
    g_file_mount_enclosing_volume(file, G_MOUNT_MOUNT_NONE, m_operation, nullptr,
                                  MounterCallbacks::mounted, new QPointer<Mounter>(this));
    // The pending mount holds its own reference to the file.
    g_object_unref(file);
}

void Mounter::providePassword(const QString &user, const QString &password) {
    if (!m_operation || !m_asking)
        return;
    setAsking(false);
    if (!user.isEmpty())
        g_mount_operation_set_username(m_operation, user.toUtf8().constData());
    g_mount_operation_set_password(m_operation, password.toUtf8().constData());
    g_mount_operation_set_password_save(m_operation, G_PASSWORD_SAVE_FOR_SESSION);
    g_mount_operation_reply(m_operation, G_MOUNT_OPERATION_HANDLED);
}

void Mounter::choose(int choice) {
    if (!m_operation || !m_asking || choice < 0 || choice >= m_choices.size())
        return;
    setAsking(false);
    g_mount_operation_set_choice(m_operation, choice);
    g_mount_operation_reply(m_operation, G_MOUNT_OPERATION_HANDLED);
}

void Mounter::cancel() {
    if (!m_operation || !m_asking)
        return;
    setAsking(false);
    g_mount_operation_reply(m_operation, G_MOUNT_OPERATION_ABORTED);
}

void Mounter::setAsking(bool asking) {
    m_asking = asking;
    if (!asking) {
        m_prompt.clear();
        m_choices.clear();
    }
    emit askingChanged();
}

void Mounter::finish() {
    if (m_asking)
        setAsking(false);
    if (m_operation) {
        g_object_unref(m_operation);
        m_operation = nullptr;
    }
}
