// SPDX-License-Identifier: LGPL-2.1-or-later
// The runner inserts the production completed() method below. Session/result
// are test doubles; --qt uses real Qt dialogs and real QPointer lifetimes.
#include <cassert>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __LINE__ << ": " #condition "\n"; std::exit(1); \
} } while (false)

#ifdef USE_QT
#include <QApplication>
#include <QAbstractButton>
#include <QKeyEvent>
#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#else
class QString {
    std::string text;
public:
    QString(const char *s = "") : text(s) {}
    void clear() { text.clear(); }
    bool isEmpty() const { return text.empty(); }
    bool operator==(const QString &other) const { return text == other.text; }
};
#define Q_ASSERT assert
template<class T> T qobject_cast(void *p) { return static_cast<T>(p); }
template<class T> class QPointer {
    std::weak_ptr<int> life;
public:
    QPointer(T *p) : life(p->life) {}
    explicit operator bool() const { return !life.expired(); }
};
class QMessageBox {
public:
    enum StandardButton { NoButton = 0, Ok = 0x400, Yes = 0x4000, Cancel = 0x400000 };
    static StandardButton information(void *, const QString &, const QString &,
                                     int = Ok, StandardButton = NoButton);
};
#endif

namespace PolkitQt1 {
struct Identity {
    QString name = "unix-user:1000";
    QString toString() const { return name; }
};
namespace Agent {
struct AsyncResult {
    int completions = 0;
    std::function<void()> onComplete;
    void setCompleted() { ++completions; if (onComplete) onComplete(); }
};
class Session
#ifdef USE_QT
    : public QObject
#endif
{
#ifdef USE_QT
    Q_OBJECT
#endif
    AsyncResult *r;
public:
    std::shared_ptr<int> life = std::make_shared<int>(0);
    explicit Session(AsyncResult *result) : r(result) {}
    AsyncResult *result() { return r; }
    // No cancel(): completed() must not cancel an already-finished session.
};
}
}

template<class K, class V> struct IdentityMap : std::map<K, V> {
    V value(K key) const { return this->at(key); }
};
namespace LXQtPolicykit {
struct PolicykitAgentGUI {
    QString selected = "unix-user:1000";
    QString identity() const { return selected; }
};
struct PolicykitAgent {
    bool m_inProgress = true, m_userCancelled = false;
    bool m_errorShown = false, m_infoShown = false;
    int m_authenticationAttempts = 0;
    QString m_lastError, m_cookie = "original-cookie";
    PolicykitAgentGUI gui;
    PolicykitAgentGUI *m_gui = &gui;
    IdentityMap<PolkitQt1::Agent::Session *, PolkitQt1::Identity> m_SessionIdentity;
    std::vector<std::unique_ptr<PolkitQt1::Agent::Session>> sessions;
    PolkitQt1::Agent::Session *current = nullptr;
    int starts = 0;
    QString lastCookie, lastIdentity;
    PolkitQt1::Agent::AsyncResult *lastResult = nullptr;
    auto sender() { return current; }
    static QString tr(const char *s) { return s; }
    void createSession(const PolkitQt1::Identity &identity, const QString &cookie,
                       PolkitQt1::Agent::AsyncResult *result) {
        ++starts;
        lastCookie = cookie; lastIdentity = identity.toString(); lastResult = result;
        sessions.emplace_back(std::make_unique<PolkitQt1::Agent::Session>(result));
        current = sessions.back().get(); m_SessionIdentity[current] = identity;
    }
    void completed(bool gainedAuthorization);
};
// PRODUCTION_IMPLEMENTATION
}

struct Dialogs {
    enum Action { Cancel, Ok, Escape, Close, Reject, Unexpected } action = Cancel;
    struct Notice { QString title, text; int buttons; };
    std::vector<Notice> notices;
    std::function<void()> during;
    static inline Dialogs *active = nullptr;
#ifdef USE_QT
    QTimer timer;
#endif
    Dialogs() {
        active = this;
#ifdef USE_QT
        QObject::connect(&timer, &QTimer::timeout, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box) return;
            record(box->windowTitle(), box->text(), int(box->standardButtons()));
            if (box->text() == QString("Authentication failed. Trying again?"))
                CHECK(box->defaultButton() == box->button(QMessageBox::Ok));
            if (action == Escape) {
                QKeyEvent event(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(box, &event);
            } else if (action == Close) {
                CHECK(box->close());
            } else if (action == Reject || action == Unexpected) {
                box->done(action == Reject ? 0 : QMessageBox::Yes);
            } else {
                const auto button = action == Ok ? QMessageBox::Ok : QMessageBox::Cancel;
                CHECK(box->button(button)); box->button(button)->click();
            }
        });
        timer.start(1);
#endif
    }
    ~Dialogs() { active = nullptr; }
    void record(const QString &title, const QString &text, int buttons) {
        CHECK(notices.size() < 4); // Fail, rather than hang, on unwanted extra dialogs.
        notices.push_back({title, text, buttons});
        auto callback = std::move(during);
        during = nullptr;
        if (callback) callback();
    }
    void checkRetry() const {
        CHECK(notices.size() == 1);
        CHECK(notices[0].title == QString("Authorization Failed"));
        CHECK(notices[0].text == QString("Authentication failed. Trying again?"));
        CHECK(notices[0].buttons == (QMessageBox::Ok | QMessageBox::Cancel));
    }
};
#ifndef USE_QT
QMessageBox::StandardButton QMessageBox::information(void *, const QString &title,
    const QString &text, int buttons, StandardButton defaultButton) {
    CHECK(Dialogs::active);
    auto &d = *Dialogs::active;
    if (text == QString("Authentication failed. Trying again?")) CHECK(defaultButton == Ok);
    d.record(title, text, buttons);
    if (d.action == Dialogs::Ok) return Ok;
    if (d.action == Dialogs::Reject) return NoButton;
    if (d.action == Dialogs::Unexpected) return Yes;
    return Cancel;
}
#endif

struct Fixture {
    Dialogs dialogs;
    PolkitQt1::Agent::AsyncResult result;
    LXQtPolicykit::PolicykitAgent agent;
    Fixture() { agent.createSession({}, agent.m_cookie, &result); }
    void ended() {
        CHECK(agent.starts == 1); CHECK(result.completions == 1);
        CHECK(!agent.m_inProgress); CHECK(agent.m_cookie.isEmpty());
    }
};

int main(int argc, char **argv) {
#ifdef USE_QT
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
#else
    (void)argc; (void)argv;
#endif
    int cases = 0;
    for (const auto action : {Dialogs::Cancel, Dialogs::Escape, Dialogs::Close,
                              Dialogs::Reject, Dialogs::Unexpected}) {
        Fixture f; f.dialogs.action = action;
        f.agent.completed(false); f.dialogs.checkRetry(); f.ended();
        CHECK(f.agent.m_userCancelled); CHECK(f.agent.m_authenticationAttempts == 1);
        f.agent.completed(false); CHECK(f.result.completions == 1);
        ++cases;
    }
    for (bool priorError : {false, true}) {
        Fixture f; f.agent.m_errorShown = priorError; f.agent.m_lastError = "PAM error";
        f.agent.completed(false); f.dialogs.checkRetry(); f.ended(); ++cases;
    }
    for (bool priorError : {false, true}) {
        Fixture f; f.dialogs.action = Dialogs::Ok;
        f.agent.m_errorShown = priorError; f.agent.m_lastError = "PAM error";
        f.agent.completed(false); f.dialogs.checkRetry();
        CHECK(f.agent.starts == 2); CHECK(f.result.completions == 0);
        CHECK(f.agent.lastCookie == QString("original-cookie"));
        CHECK(f.agent.lastIdentity == QString("unix-user:1000"));
        CHECK(f.agent.lastResult == &f.result);
        CHECK(!f.agent.m_errorShown); CHECK(f.agent.m_lastError.isEmpty());
        f.agent.completed(true); CHECK(f.result.completions == 1); ++cases;
    }
    {
        Fixture f; f.dialogs.action = Dialogs::Ok;
        f.agent.completed(false); f.agent.completed(false); f.agent.completed(false);
        CHECK(f.agent.starts == 3); CHECK(f.result.completions == 1);
        CHECK(f.agent.m_authenticationAttempts == 3);
        CHECK(f.dialogs.notices.size() == 3);
        CHECK(f.dialogs.notices.back().text == QString("Authentication failed"));
        CHECK(f.dialogs.notices.back().buttons == QMessageBox::Ok); ++cases;
    }
    {
        Fixture f; f.agent.completed(true); f.ended();
        CHECK(f.dialogs.notices.empty()); CHECK(f.agent.m_authenticationAttempts == 0); ++cases;
    }
    {
        Fixture f; f.agent.m_userCancelled = true; f.agent.completed(false); f.ended();
        CHECK(f.dialogs.notices.empty()); CHECK(f.agent.m_authenticationAttempts == 0); ++cases;
    }
    {
        Fixture f; f.agent.m_infoShown = f.agent.m_errorShown = true;
        f.agent.completed(false); f.ended();
        CHECK(f.dialogs.notices.empty()); CHECK(f.agent.m_authenticationAttempts == 0); ++cases;
    }
    {
        Fixture f; f.agent.gui.selected = "unix-user:2000"; f.agent.completed(false);
        CHECK(f.agent.starts == 1); CHECK(f.result.completions == 0);
        CHECK(f.dialogs.notices.empty()); CHECK(f.agent.m_authenticationAttempts == 0); ++cases;
    }
    for (int event : {0, 1, 2}) {
        Fixture f; f.dialogs.action = Dialogs::Ok;
        f.dialogs.during = [&] {
            if (event == 0) { f.agent.m_inProgress = false; f.agent.m_cookie.clear(); }
            if (event == 1) f.agent.m_userCancelled = true;
            if (event == 2) f.agent.m_infoShown = f.agent.m_errorShown = true;
        };
        f.agent.completed(false); f.ended(); f.dialogs.checkRetry(); ++cases;
    }
    for (bool terminal : {false, true}) {
        Fixture f; PolkitQt1::Agent::AsyncResult replacement;
        f.dialogs.action = Dialogs::Ok;
        if (terminal) f.agent.m_authenticationAttempts = 2;
        f.dialogs.during = [&] {
            f.agent.sessions.clear(); f.agent.m_SessionIdentity.clear();
            f.agent.m_cookie = "replacement-cookie";
            f.agent.m_authenticationAttempts = 0;
            f.agent.createSession({}, f.agent.m_cookie, &replacement);
        };
        f.agent.completed(false);
        CHECK(f.agent.starts == 2); CHECK(f.agent.m_inProgress);
        CHECK(f.agent.m_cookie == QString("replacement-cookie"));
        CHECK(f.agent.m_authenticationAttempts == 0);
        CHECK(!f.agent.m_userCancelled);
        CHECK(replacement.completions == 0); CHECK(f.result.completions == 0); ++cases;
    }
    {
        Fixture f;
        f.result.onComplete = [&] {
            CHECK(!f.agent.m_inProgress); CHECK(f.agent.m_cookie.isEmpty());
            f.agent.completed(false);
        };
        f.agent.completed(false); f.ended(); f.dialogs.checkRetry(); ++cases;
    }
    {
        Fixture f; PolkitQt1::Agent::AsyncResult replacement;
        f.result.onComplete = [&] {
            f.agent.m_inProgress = true; f.agent.m_cookie = "replacement-cookie";
            f.agent.m_userCancelled = false; f.agent.m_authenticationAttempts = 0;
            f.agent.createSession({}, f.agent.m_cookie, &replacement);
        };
        f.agent.completed(false);
        CHECK(f.result.completions == 1); CHECK(replacement.completions == 0);
        CHECK(f.agent.m_inProgress); CHECK(f.agent.m_cookie == QString("replacement-cookie"));
        CHECK(f.agent.m_authenticationAttempts == 0); ++cases;
    }
    std::cout << "PASS: " << cases << " completion scenarios"
#ifdef USE_QT
              << " (real Qt dialogs and QPointer)"
#else
              << " (control-flow doubles; no Qt/PAM)"
#endif
              << '\n';
}
#ifdef USE_QT
#include "test_retry_completion.moc"
#endif
