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
#include <QTest>
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

// Review-derived checks use stable failure IDs for runtime negative controls.
// Sources and the limits of the simulated backend are in README.retry.md.
#define REVIEW_CHECK(id, expression) do { \
    if (!(expression)) { \
        std::cerr << "REVIEW ASSERTION: " id "\n" << __LINE__ << ": " #expression "\n"; \
        std::exit(1); \
    } \
} while (false)

namespace Review
{
static void passwordButton(QDialogButtonBox::StandardButton which)
{
    CHECK(gui());
    auto *buttons = gui()->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
    CHECK(buttons && buttons->button(which));
    QTest::mouseClick(buttons->button(which), Qt::LeftButton);
    pump();
}

static void messageButton(QMessageBox::StandardButton which)
{
    CHECK(box() && box()->button(which));
    QTest::mouseClick(box()->button(which), Qt::LeftButton);
    pump();
}

static void submit(const QString &text = QStringLiteral("test-response"))
{
    waitFor([] { return gui() && !box(); });
    auto *edit = gui()->findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    CHECK(edit);
    edit->setText(text);
    passwordButton(QDialogButtonBox::Ok);
}

static void question()
{
    waitFor([] { return box(); });
    REVIEW_CHECK("retry_cancel_button", box()->standardButtons() == (QMessageBox::Ok | QMessageBox::Cancel));
    REVIEW_CHECK("retry_question_text", box()->text() == QStringLiteral("Authentication failed. Trying again?"));
    CHECK(box()->defaultButton() == box()->button(QMessageBox::Ok));
}

static void completedRequest(const Result &result, std::size_t starts)
{
    pump();
    REVIEW_CHECK("cancel_completes_request", result.calls == 1);
    REVIEW_CHECK("cancel_starts_no_retry", helpers.size() == starts);
    REVIEW_CHECK("cancel_leaves_no_dialog", !gui() && !box());
    for (const auto &h : helpers)
        REVIEW_CHECK("cancel_retires_helpers", !h->object);
}

// stefonarch: save a root file, cancel three times, then observe a lockout.
// Each save is a distinct request to the SAME agent, with a separate result
// and cookie. Keep all results alive to detect delayed duplicate completion.
static void threeSaves(const std::string &name)
{
    std::vector<std::unique_ptr<Result>> results;
    LXQtPolicykit::PolicykitAgent agent;
    const bool multiple = name == "review_three_saves_multi_identity";
    const bool failedFirst = name == "review_three_saves_retry_cancel";
    for (int save = 0; save < 3; ++save)
    {
        results.push_back(std::make_unique<Result>());
        auto &result = *results.back();
        const std::size_t first = helpers.size();
        const std::size_t starts = first + (multiple ? 2 : 1);
        begin(agent, result, QStringLiteral("review-save-%1").arg(save), multiple);
        waitFor([] { return gui(); });
        CHECK(helpers.size() == starts && result.calls == 0);
        if (failedFirst)
        {
            submit();
            waitFor([] { return box(); });
            REVIEW_CHECK("per_request_retry_budget", box()->standardButtons() == (QMessageBox::Ok | QMessageBox::Cancel));
            question();
            messageButton(QMessageBox::Cancel);
        }
        else
        {
            auto *edit = gui()->findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
            CHECK(edit);
            if (name == "review_three_saves_typed_cancel")
            {
                QTest::keyClicks(edit, "not-submitted");
                CHECK(edit->text() == QStringLiteral("not-submitted"));
            }
            if (name == "review_three_saves_escape")
                QTest::keyClick(gui(), Qt::Key_Escape);
            else if (name == "review_three_saves_close")
                CHECK(gui()->close());
            else
                passwordButton(QDialogButtonBox::Cancel);
        }
        pump();
        for (std::size_t i = first; i < starts; ++i)
        {
            REVIEW_CHECK("cancel_never_submits_password", helpers[i]->responses == (failedFirst ? 1 : 0));
            CHECK(helpers[i]->cancels == (failedFirst ? 0 : 1));
        }
        completedRequest(result, starts);
    }

    // The fourth save is still usable, with all three attempts available.
    results.push_back(std::make_unique<Result>());
    auto &fresh = *results.back();
    const std::size_t first = helpers.size();
    begin(agent, fresh, QStringLiteral("review-fourth-save"));
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        waitFor([] { return gui() && !box(); });
        auto &h = *helpers.back();
        CHECK(h.cookie == QStringLiteral("review-fourth-save"));
        CHECK(h.identity == helpers[first]->identity);
        CHECK(h.responses == 0);
        h.gain = attempt == 2;
        submit();
        CHECK(h.responses == 1);
        if (attempt < 2)
        {
            waitFor([] { return box(); });
            REVIEW_CHECK("per_request_retry_budget", box()->standardButtons() == (QMessageBox::Ok | QMessageBox::Cancel));
            question();
            CHECK(fresh.calls == 0 && helpers.size() == first + attempt + 1);
            messageButton(QMessageBox::Ok);
        }
    }
    completedRequest(fresh, first + 3);
    for (const auto &result : results)
        CHECK(result->calls == 1);
}

static bool run(const std::string &name)
{
    if (name == "review_three_saves_cancel" || name == "review_three_saves_typed_cancel"
        || name == "review_three_saves_escape" || name == "review_three_saves_close"
        || name == "review_three_saves_multi_identity" || name == "review_three_saves_retry_cancel")
    {
        threeSaves(name);
    }
    else if (name == "review_retry_contract")
    {
        // Observe the dialog inside its event loop as well, so this same test
        // can reject the original PR's blocking, OK-only failure notice.
        Fixture f;
        bool answered = false;
        QTimer observer;
        QObject::connect(&observer, &QTimer::timeout, [&] {
            if (!box())
                return;
            observer.stop();
            question();
            CHECK(helpers.size() == 1 && f.result.calls == 0);
            QTest::mouseClick(box()->button(QMessageBox::Cancel), Qt::LeftButton);
            answered = true;
        });
        observer.start(1);
        submit();
        waitFor([&] { return answered; });
        completedRequest(f.result, 1);
        CHECK(helpers[0]->responses == 1 && helpers[0]->cancels == 0);
    }
    else if (name == "review_retry_enter")
    {
        Fixture f;
        submit();
        question();
        QTest::keyClick(box(), Qt::Key_Return);
        pump();
        REVIEW_CHECK("one_consent_one_helper", helpers.size() == 2 && f.result.calls == 0);
        CHECK(helpers[1]->cookie == helpers[0]->cookie && helpers[1]->identity == helpers[0]->identity);
        helpers[1]->gain = true;
        submit();
        completedRequest(f.result, 2);
    }
    else if (name == "review_second_retry_cancel")
    {
        Fixture f;
        submit(); question(); messageButton(QMessageBox::Ok);
        submit(); question(); messageButton(QMessageBox::Cancel);
        completedRequest(f.result, 2);
        CHECK(helpers[0]->responses == 1 && helpers[1]->responses == 1);
    }
    else if (name == "review_backend_error_consent")
    {
        Fixture f;
        g_signal_emit_by_name(helpers[0]->object, "show-error", "Rejected by backend");
        complete(*helpers[0]);
        pump();
        CHECK(box() && box()->text() == QStringLiteral("Rejected by backend"));
        messageButton(QMessageBox::Ok);
        REVIEW_CHECK("backend_error_does_not_auto_retry", helpers.size() == 1 && f.result.calls == 0);
        question();
        // Pump a later event-loop turn without answering the question.
        bool idle = false;
        QTimer::singleShot(0, qApp, [&] { idle = true; });
        waitFor([&] { return idle; });
        REVIEW_CHECK("backend_error_does_not_auto_retry", helpers.size() == 1 && f.result.calls == 0);
        messageButton(QMessageBox::Cancel);
        completedRequest(f.result, 1);
    }
    else if (name == "review_empty_submit_is_attempt")
    {
        Fixture f;
        submit(QString());
        REVIEW_CHECK("empty_submit_is_not_cancel", helpers[0]->responses == 1 && helpers[0]->response.isEmpty());
        question(); messageButton(QMessageBox::Cancel);
        completedRequest(f.result, 1);
    }
    else if (name == "review_replaced_prompt_once")
    {
        Fixture f;
        helpers[0]->prompts = 2;
        // Two pending challenges must replace the dialog's handler rather
        // than send the same click twice (the PR's one-shot callback promise).
        g_signal_emit_by_name(helpers[0]->object, "request", "Replacement password:", FALSE);
        pump();
        submit();
        REVIEW_CHECK("one_click_one_response", helpers[0]->responses == 1);
        CHECK(f.result.calls == 0 && helpers.size() == 1);
        passwordButton(QDialogButtonBox::Cancel);
        completedRequest(f.result, 1);
        CHECK(helpers[0]->responses == 1 && helpers[0]->cancels == 1);
    }
    else if (name == "review_cancelled_password_stays_cancelled")
    {
        Fixture f;
        Result next;
        QPointer<LXQtPolicykit::PolicykitAgentGUI> old = gui();
        old->findChild<QLineEdit *>(QStringLiteral("passwordEdit"))->setText(QStringLiteral("not-submitted"));
        auto *buttons = old->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
        // Use click() here to avoid processing deferred deletion before the
        // deliberately stale signal. Other review cases use mouse events.
        buttons->button(QDialogButtonBox::Cancel)->click();
        CHECK(f.result.calls == 1 && old);
        begin(*f.agent, next, QStringLiteral("replacement-save"));
        old->done(QDialog::Accepted);
        pump();
        CHECK(helpers.size() == 2 && next.calls == 0 && !box());
        REVIEW_CHECK("stale_password_cannot_submit", helpers[0]->responses == 0 && helpers[1]->responses == 0);
        helpers[1]->gain = true; submit();
        completedRequest(next, 2);
    }
    else if (name == "review_backend_lockout_respected")
    {
        Fixture f;
        // A supplied terminal message is NOT a simulation of PAM accounting.
        const char *text = "Account locked by the backend after failed authentication";
        g_signal_emit_by_name(helpers[0]->object, "show-info", text);
        complete(*helpers[0]);
        pump();
        REVIEW_CHECK("backend_lockout_is_preserved", box() && box()->text() == QString::fromUtf8(text));
        CHECK(box()->standardButtons() == QMessageBox::Ok && helpers.size() == 1);
        messageButton(QMessageBox::Ok);
        completedRequest(f.result, 1);
        CHECK(helpers[0]->responses == 0);
    }
    else
        return false;
    return true;
}
} // namespace Review

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    CHECK(argc == 2);
    const std::string name(argv[1]);
    if (Review::run(name)) {}
    else if (name == "cancel_password" || name == "escape_password" || name == "close_password") {
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
