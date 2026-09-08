// SPDX-License-Identifier: LGPL-2.1-or-later
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
#include <QThread>
#include <QTimer>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <vector>
#include "policykitagent.h"
#include "policykitagentgui.h"

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

struct Helper {
    GObject *object = nullptr;
    QString cookie, identity, response;
    bool initiated = false, completed = false, gain = false;
    int responses = 0, cancels = 0, prompts = 1;
};
static std::vector<std::unique_ptr<Helper>> helpers;
static bool synchronousCompletion = false;
static bool insideNativeCompletion = false;
static int registrations = 0;
struct AuditSession { GObject parent; };
struct AuditSessionClass { GObjectClass parent; };
G_DEFINE_TYPE(AuditSession, audit_session, G_TYPE_OBJECT)
static void audit_session_init(AuditSession *) {}
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
        if (entry->object == reinterpret_cast<GObject *>(session)) return *entry;
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
extern "C" {
gpointer polkit_agent_listener_register(PolkitAgentListener *, PolkitAgentRegisterFlags,
    PolkitSubject *, const gchar *, GCancellable *, GError **)
{
    ++registrations;
    return reinterpret_cast<gpointer>(1);
}
void polkit_agent_listener_unregister(gpointer) {}
PolkitAgentSession *polkit_agent_session_new(PolkitIdentity *identity, const gchar *cookie)
{
    auto h = std::make_unique<Helper>();
    h->object = G_OBJECT(g_object_new(audit_session_get_type(), nullptr));
    h->cookie = QString::fromUtf8(cookie);
    auto *name = polkit_identity_to_string(identity);
    h->identity = QString::fromUtf8(name);
    g_free(name);
    g_object_weak_ref(h->object, [](gpointer data, GObject *) {
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
    if (synchronousCompletion) { complete(h); return; }
    QTimer::singleShot(0, qApp, [p = &h] {
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
    else
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
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}
static void waitFor(const std::function<bool()> &ready)
{
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < 2000) { pump(); QThread::msleep(1); }
    CHECK(ready());
}
static LXQtPolicykit::PolicykitAgentGUI *gui()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<LXQtPolicykit::PolicykitAgentGUI *>(widget))
            if (dialog->isVisible()) return dialog;
    return nullptr;
}
static QMessageBox *box()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<QMessageBox *>(widget))
            if (dialog->isVisible()) return dialog;
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
struct Result {
    int calls = 0;
    QString error;
    std::function<void()> during;
    std::unique_ptr<PolkitQt1::Agent::AsyncResult> value;
    Result() {
        auto *simple = g_simple_async_result_new(nullptr, [](GObject *, GAsyncResult *r, gpointer data) {
            auto &self = *static_cast<Result *>(data);
            CHECK(++self.calls == 1);
            GError *error = nullptr;
            if (g_simple_async_result_propagate_error(G_SIMPLE_ASYNC_RESULT(r), &error)) {
                self.error = QString::fromUtf8(error->message); g_error_free(error);
            }
            if (self.during) self.during();
        }, this, nullptr);
        value = std::make_unique<PolkitQt1::Agent::AsyncResult>(simple);
    }
};
static PolkitQt1::Identity::List identities(bool multiple = false)
{
    PolkitQt1::Identity::List ids;
    ids << PolkitQt1::UnixUserIdentity(1000);
    if (multiple) ids << PolkitQt1::UnixUserIdentity(2000);
    return ids;
}
static void begin(LXQtPolicykit::PolicykitAgent &agent, Result &r,
                  const QString &cookie = QStringLiteral("request-one"), bool multiple = false)
{
    agent.initiateAuthentication(QStringLiteral("audit.action"), QStringLiteral("Audit action"),
        QString(), PolkitQt1::Details(), cookie, identities(multiple), r.value.get());
}
struct Fixture {
    Result result;
    std::unique_ptr<LXQtPolicykit::PolicykitAgent> agent = std::make_unique<LXQtPolicykit::PolicykitAgent>();
    explicit Fixture(bool multiple = false) {
        CHECK(registrations == 1);
        begin(*agent, result, QStringLiteral("request-one"), multiple);
        waitFor([] { return gui() != nullptr; });
        gui()->findChild<QComboBox *>(QStringLiteral("identityComboBox"))->setCurrentIndex(0);
    }
    ~Fixture() { agent.reset(); pump(); }
    void submit() {
        waitFor([] { return gui() && !box(); });
        gui()->findChild<QLineEdit *>(QStringLiteral("passwordEdit"))->setText(QStringLiteral("test-response"));
        gui()->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"))->button(QDialogButtonBox::Ok)->click();
        pump();
    }
    void ended(int starts = 1) {
        pump();
        CHECK(result.calls == 1 && int(helpers.size()) == starts);
        CHECK(!gui() && !box());
        for (const auto &h : helpers) CHECK(!h->object);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    CHECK(argc == 2);
    const std::string name(argv[1]);
    if (name == "cancel_password" || name == "escape_password" || name == "close_password") {
        Fixture f;
        if (name == "cancel_password") gui()->reject();
        if (name == "escape_password") { QKeyEvent e(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(gui(), &e); }
        if (name == "close_password") CHECK(gui()->close());
        f.ended(); CHECK(helpers[0]->cancels == 1 && helpers[0]->responses == 0);
    } else if (name == "cancel_retry" || name == "escape_retry" || name == "close_retry"
               || name == "reject_retry" || name == "unexpected_retry") {
        Fixture f; f.submit(); retryQuestion();
        if (name == "cancel_retry") button(QMessageBox::Cancel);
        if (name == "escape_retry") { QKeyEvent e(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(box(), &e); }
        if (name == "close_retry") CHECK(box()->close());
        if (name == "reject_retry") box()->reject();
        if (name == "unexpected_retry") box()->done(QMessageBox::Yes);
        f.ended(); CHECK(helpers[0]->responses == 1 && helpers[0]->cancels == 0);
    } else if (name == "retry_success") {
        Fixture f; f.submit(); retryQuestion(); button(QMessageBox::Ok);
        CHECK(helpers.size() == 2 && f.result.calls == 0);
        CHECK(helpers[1]->cookie == helpers[0]->cookie && helpers[1]->identity == helpers[0]->identity);
        helpers[1]->gain = true; f.submit(); f.ended(2);
    } else if (name == "retry_limit") {
        Fixture f;
        for (int i = 0; i < 2; ++i) { f.submit(); retryQuestion(); button(QMessageBox::Ok); }
        f.submit(); waitFor([] { return box(); });
        CHECK(box()->standardButtons() == QMessageBox::Ok);
        CHECK(box()->text() == QStringLiteral("Authentication failed"));
        button(QMessageBox::Ok); f.ended(3);
    } else if (name == "backend_error" || name == "backend_info") {
        Fixture f;
        g_signal_emit_by_name(helpers[0]->object, name == "backend_error" ? "show-error" : "show-info", "Backend explanation");
        complete(*helpers[0]); pump();
        CHECK(box() && box()->text() == QStringLiteral("Backend explanation"));
        CHECK(f.result.calls == 0); button(QMessageBox::Ok);
        if (name == "backend_error") { retryQuestion(); button(QMessageBox::Cancel); }
        f.ended();
    } else if (name == "external_cancel_password" || name == "external_cancel_retry") {
        Fixture f;
        if (name == "external_cancel_retry") { f.submit(); retryQuestion(); }
        f.agent->cancelAuthentication(); CHECK(f.result.calls == 1);
        f.agent->cancelAuthentication(); f.ended();
    } else if (name == "cancel_before_completion_delivery") {
        Fixture f; complete(*helpers[0]); f.agent->cancelAuthentication(); f.ended();
    } else if (name == "stale_delivery") {
        Fixture f; Result next;
        g_signal_emit_by_name(helpers[0]->object, "show-info", "Obsolete information");
        f.agent->cancelAuthentication();
        begin(*f.agent, next, QStringLiteral("request-two")); pump();
        CHECK(f.result.calls == 1 && next.calls == 0 && !box());
        CHECK(helpers.size() == 2 && helpers[1]->cookie == QStringLiteral("request-two"));
        helpers[1]->gain = true; f.submit(); CHECK(next.calls == 1);
    } else if (name == "stale_dialog") {
        Fixture f; Result next; f.submit(); retryQuestion();
        QPointer<QMessageBox> old = box();
        f.agent->cancelAuthentication();
        begin(*f.agent, next, QStringLiteral("request-two"));
        // Deliver a stale answer before deferred deletion, after state was reset.
        CHECK(old); old->done(QMessageBox::Ok); pump();
        CHECK(helpers.size() == 2 && next.calls == 0 && f.result.calls == 1);
        helpers[1]->gain = true; f.submit(); CHECK(next.calls == 1);
    } else if (name == "request_after_native_completion") {
        Fixture f;
        g_signal_emit_by_name(helpers[0]->object, "request", "Late prompt", FALSE);
        complete(*helpers[0]); pump(); retryQuestion();
        button(QMessageBox::Cancel); f.ended(); CHECK(helpers[0]->responses == 0);
    } else if (name == "new_request" || name == "reentrant_result") {
        Fixture f; Result next;
        if (name == "reentrant_result") f.result.during = [&] { begin(*f.agent, next, QStringLiteral("request-two")); };
        f.agent->cancelAuthentication();
        if (name == "new_request") begin(*f.agent, next, QStringLiteral("request-two"));
        waitFor([] { return gui(); }); helpers[1]->gain = true; f.submit();
        CHECK(f.result.calls == 1 && next.calls == 1);
    } else if (name == "finish_callback") {
        Fixture f; CHECK(f.agent->initiateAuthenticationFinish());
        helpers[0]->gain = true; f.submit(); f.ended();
    } else if (name == "other_identity") {
        Fixture f(true);
        g_signal_emit_by_name(helpers[1]->object, "show-info", "Other account locked");
        complete(*helpers[1]); pump(); CHECK(!box());
        helpers[0]->gain = true; f.submit(); f.ended(2);
    } else if (name == "multiple_identities") {
        Fixture f(true); gui()->reject(); f.ended(2);
        CHECK(helpers[0]->cancels == 1 && helpers[1]->cancels == 1);
        CHECK(helpers[0]->responses == 0 && helpers[1]->responses == 0);
    } else if (name == "dead_identity") {
        Fixture f(true);
        complete(*helpers[1]); pump();
        auto *choices = gui()->findChild<QComboBox *>(QStringLiteral("identityComboBox"));
        CHECK(choices->count() == 1);
        CHECK(choices->currentText() == helpers[0]->identity);
        helpers[0]->gain = true; f.submit(); f.ended(2);
    } else if (name == "second_identity_retry") {
        Fixture f(true);
        gui()->findChild<QComboBox *>(QStringLiteral("identityComboBox"))->setCurrentIndex(1);
        f.submit(); retryQuestion(); button(QMessageBox::Ok);
        waitFor([] { return gui(); });
        auto *choices = gui()->findChild<QComboBox *>(QStringLiteral("identityComboBox"));
        CHECK(choices->count() == 1);
        CHECK(choices->currentText() == helpers[1]->identity);
        CHECK(helpers.size() == 3 && helpers[2]->identity == helpers[1]->identity);
        CHECK(helpers[0]->responses == 0 && helpers[0]->cancels == 1);
        helpers[2]->gain = true; f.submit(); f.ended(3);
    } else if (name == "multi_prompt") {
        Fixture f; helpers[0]->prompts = 2; helpers[0]->gain = true;
        f.submit(); CHECK(helpers[0]->responses == 1 && f.result.calls == 0);
        f.submit(); f.ended(); CHECK(helpers[0]->responses == 2);
    } else if (name == "synchronous_completion") {
        synchronousCompletion = true;
        Result result; LXQtPolicykit::PolicykitAgent agent;
        begin(agent, result); pump(); retryQuestion(); button(QMessageBox::Cancel);
        CHECK(result.calls == 1 && helpers.size() == 1);
    } else if (name == "destruction") {
        Fixture f; f.agent.reset(); f.ended(); CHECK(helpers[0]->cancels == 1);
    } else if (name == "empty_identities") {
        Result result; LXQtPolicykit::PolicykitAgent agent;
        agent.initiateAuthentication({}, {}, {}, {}, QStringLiteral("empty"), {}, result.value.get());
        CHECK(result.calls == 1 && !result.error.isEmpty() && helpers.empty());
    } else if (name == "busy_request") {
        Fixture f; Result second; begin(*f.agent, second);
        CHECK(second.calls == 1 && !second.error.isEmpty() && f.result.calls == 0);
        CHECK(helpers.size() == 1); f.agent->cancelAuthentication(); f.ended();
    } else if (name == "native_callback_boundary") {
        Fixture f; bool observed = false, unsafe = false;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            if (!box()) return;
            observed = true; unsafe = insideNativeCompletion;
            box()->button(QMessageBox::Cancel)->click();
        });
        timer.start(1);
        complete(*helpers[0]); waitFor([&] { return observed; });
        CHECK(!unsafe); f.ended();
    } else if (name == "repeated_cancel") {
        LXQtPolicykit::PolicykitAgent agent;
        for (int i = 0; i < 30; ++i) {
            Result result; begin(agent, result, QString::number(i));
            waitFor([] { return gui(); }); gui()->reject(); pump();
            CHECK(result.calls == 1 && !gui() && !box());
            CHECK(helpers.back()->responses == 0 && helpers.back()->cancels == 1);
        }
    } else { std::cerr << "Unknown scenario: " << name << '\n'; return 2; }
    pump();
    std::cout << "PASS: " << name << '\n';
}
