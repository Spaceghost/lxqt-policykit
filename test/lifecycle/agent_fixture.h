// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef LXQT_POLICYKIT_TEST_AGENT_FIXTURE_H
#define LXQT_POLICYKIT_TEST_AGENT_FIXTURE_H

// Real agent, Qt widgets, PolkitQt Session and AsyncResult. Only registration
// and the native helper transport are replaced; no PAM policy is exercised.
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE 1
#include <polkitagent/polkitagent.h>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "policykitagent.h"
#include "policykitagentgui.h"

#define CHECK(x) do { \
    if (!(x)) { \
        std::cerr << __LINE__ << ": " #x "\n"; \
        std::exit(1); \
    } \
} while (false)

struct Helper
{
    GObject *object = nullptr;
    QString cookie;
    QString identity;
    QString response;
    bool initiated = false;
    bool completed = false;
    bool gain = false;
    int responses = 0;
    int cancels = 0;
    int prompts = 1;
    bool automaticCompletion = true;
};
static std::vector<std::unique_ptr<Helper>> helpers;
static bool synchronousCompletion = false;
static bool automaticPrompts = true;
static bool insideNativeCompletion = false;
static int registrations = 0;
struct AuditSession
{
    GObject parent;
};
struct AuditSessionClass
{
    GObjectClass parent;
};
G_DEFINE_TYPE(AuditSession, audit_session, G_TYPE_OBJECT)
static void audit_session_init(AuditSession *)
{
}
static void audit_session_class_init(AuditSessionClass *klass)
{
    g_signal_new("completed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                 nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
    g_signal_new("request", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                 nullptr, nullptr, nullptr, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
    for (const char *name : {"show-error", "show-info"})
        g_signal_new(name, G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                     nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_STRING);
}
static Helper &helper(PolkitAgentSession *session)
{
    CHECK(session);
    for (auto &entry : helpers)
        if (entry->object == reinterpret_cast<GObject *>(session))
            return *entry;
    std::abort();
}
static void complete(Helper &h, bool gain = false)
{
    CHECK(h.object && !h.completed);
    h.completed = true;
    insideNativeCompletion = true;
    g_signal_emit_by_name(h.object, "completed", gain);
    insideNativeCompletion = false;
}
extern "C"
{
gpointer polkit_agent_listener_register(PolkitAgentListener *, PolkitAgentRegisterFlags,
    PolkitSubject *, const gchar *, GCancellable *, GError **)
{
    ++registrations;
    return reinterpret_cast<gpointer>(1);
}
void polkit_agent_listener_unregister(gpointer)
{
}
PolkitAgentSession *polkit_agent_session_new(PolkitIdentity *identity, const gchar *cookie)
{
    auto h = std::make_unique<Helper>();
    h->object = G_OBJECT(g_object_new(audit_session_get_type(), nullptr));
    h->cookie = QString::fromUtf8(cookie);
    auto *name = polkit_identity_to_string(identity);
    h->identity = QString::fromUtf8(name);
    g_free(name);
    g_object_weak_ref(h->object, [](gpointer data, GObject *)
    {
        static_cast<Helper *>(data)->object = nullptr;
    }, h.get());
    auto *object = h->object;
    helpers.push_back(std::move(h));
    return reinterpret_cast<PolkitAgentSession *>(object);
}
void polkit_agent_session_initiate(PolkitAgentSession *session)
{
    auto &h = helper(session);
    CHECK(!h.initiated);
    h.initiated = true;
    if (synchronousCompletion)
    {
        complete(h);
        return;
    }
    if (!automaticPrompts)
        return;
    QTimer::singleShot(0, qApp, [p = &h]
    {
        if (p->object && !p->completed)
            g_signal_emit_by_name(p->object, "request", "Password:", FALSE);
    });
}
void polkit_agent_session_response(PolkitAgentSession *session, const gchar *response)
{
    auto &h = helper(session);
    CHECK(!h.completed);
    h.response = QString::fromUtf8(response);
    ++h.responses;
    if (h.responses < h.prompts)
        g_signal_emit_by_name(h.object, "request", "Verification code:", FALSE);
    else if (h.automaticCompletion)
        complete(h, h.gain);
}
void polkit_agent_session_cancel(PolkitAgentSession *session)
{
    auto &h = helper(session);
    CHECK(!h.completed);
    ++h.cancels;
    complete(h);
}
}

static void pump()
{
    for (int i = 0; i < 8; ++i)
    {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}
static void waitFor(const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 2000)
    {
        pump();
        QThread::msleep(1);
    }
    CHECK(ready());
}
static LXQtPolicykit::PolicykitAgentGUI *gui()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<LXQtPolicykit::PolicykitAgentGUI *>(widget))
            if (dialog->isVisible())
                return dialog;
    return nullptr;
}
static QMessageBox *box()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<QMessageBox *>(widget))
            if (dialog->isVisible())
                return dialog;
    return nullptr;
}
static void button(QMessageBox::StandardButton which)
{
    CHECK(box() && box()->button(which));
    box()->button(which)->click();
    pump();
}
static void retryQuestion()
{
    waitFor([] { return box() != nullptr; });
    CHECK(box()->text() == QStringLiteral("Authentication failed. Trying again?"));
    CHECK(box()->windowTitle() == QStringLiteral("Authorization Failed"));
    CHECK(box()->standardButtons() == (QMessageBox::Ok | QMessageBox::Cancel));
    CHECK(box()->defaultButton() == box()->button(QMessageBox::Ok));
}
struct Result
{
    int calls = 0;
    QString error;
    std::function<void()> during;
    std::unique_ptr<PolkitQt1::Agent::AsyncResult> value;
    Result()
    {
        auto *simple = g_simple_async_result_new(nullptr, [](GObject *, GAsyncResult *r, gpointer data)
        {
            auto &self = *static_cast<Result *>(data);
            CHECK(++self.calls == 1);
            GError *error = nullptr;
            if (g_simple_async_result_propagate_error(G_SIMPLE_ASYNC_RESULT(r), &error))
            {
                self.error = QString::fromUtf8(error->message);
                g_error_free(error);
            }
            if (self.during)
                self.during();
        }, this, nullptr);
        value = std::make_unique<PolkitQt1::Agent::AsyncResult>(simple);
    }
};
static PolkitQt1::Identity::List identities(bool multiple = false)
{
    PolkitQt1::Identity::List ids;
    ids << PolkitQt1::UnixUserIdentity(1000);
    if (multiple)
        ids << PolkitQt1::UnixUserIdentity(2000);
    return ids;
}
static void begin(LXQtPolicykit::PolicykitAgent &agent, Result &r,
                  const QString &cookie = QStringLiteral("request-one"), bool multiple = false)
{
    agent.initiateAuthentication(QStringLiteral("audit.action"), QStringLiteral("Audit action"),
        QString(), PolkitQt1::Details(), cookie, identities(multiple), r.value.get());
}
struct Fixture
{
    Result result;
    std::unique_ptr<LXQtPolicykit::PolicykitAgent> agent = std::make_unique<LXQtPolicykit::PolicykitAgent>();
    explicit Fixture(bool multiple = false)
    {
        CHECK(registrations == 1);
        begin(*agent, result, QStringLiteral("request-one"), multiple);
        waitFor([] { return gui() != nullptr; });
        gui()->findChild<QComboBox *>(QStringLiteral("identityComboBox"))->setCurrentIndex(0);
    }
    ~Fixture()
    {
        agent.reset();
        pump();
    }
    void submit()
    {
        waitFor([] { return gui() && !box(); });
        gui()->findChild<QLineEdit *>(QStringLiteral("passwordEdit"))->setText(QStringLiteral("test-response"));
        gui()->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"))->button(QDialogButtonBox::Ok)->click();
        pump();
    }
    void ended(int starts = 1)
    {
        pump();
        CHECK(result.calls == 1 && int(helpers.size()) == starts);
        CHECK(!gui() && !box());
        for (const auto &h : helpers)
            CHECK(!h->object);
    }
};

#endif
