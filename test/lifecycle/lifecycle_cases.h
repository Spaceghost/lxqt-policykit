// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef LXQT_POLICYKIT_TEST_LIFECYCLE_CASES_H
#define LXQT_POLICYKIT_TEST_LIFECYCLE_CASES_H

#include "agent_fixture.h"

namespace LifecycleCases
{
static bool run(const std::string &name)
{
    if (name == "cancel_password" || name == "escape_password" || name == "close_password")
    {
        Fixture f;
        if (name == "cancel_password")
            gui()->reject();
        if (name == "escape_password")
        {
            QKeyEvent e(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(gui(), &e);
        }
        if (name == "close_password")
            CHECK(gui()->close());
        f.ended();
        CHECK(helpers[0]->cancels == 1 && helpers[0]->responses == 0);
    }
    else if (name == "cancel_retry" || name == "escape_retry" || name == "close_retry"
               || name == "reject_retry" || name == "unexpected_retry")
    {
        Fixture f;
        f.submit();
        retryQuestion();
        if (name == "cancel_retry")
            button(QMessageBox::Cancel);
        if (name == "escape_retry")
        {
            QKeyEvent e(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(box(), &e);
        }
        if (name == "close_retry")
            CHECK(box()->close());
        if (name == "reject_retry")
            box()->reject();
        if (name == "unexpected_retry")
            box()->done(QMessageBox::Yes);
        f.ended();
        CHECK(helpers[0]->responses == 1 && helpers[0]->cancels == 0);
    }
    else if (name == "retry_success")
    {
        Fixture f;
        f.submit();
        retryQuestion();
        button(QMessageBox::Ok);
        CHECK(helpers.size() == 2 && f.result.calls == 0);
        CHECK(helpers[1]->cookie == helpers[0]->cookie && helpers[1]->identity == helpers[0]->identity);
        helpers[1]->gain = true;
        f.submit();
        f.ended(2);
    }
    else if (name == "retry_limit")
    {
        Fixture f;
        for (int i = 0; i < 2; ++i)
        {
            f.submit();
            retryQuestion();
            button(QMessageBox::Ok);
        }
        f.submit();
        waitFor([] { return box(); });
        CHECK(box()->standardButtons() == QMessageBox::Ok);
        CHECK(box()->text() == QStringLiteral("Authentication failed"));
        button(QMessageBox::Ok);
        f.ended(3);
    }
    else if (name == "backend_error" || name == "backend_info")
    {
        Fixture f;
        helpers[0]->automaticCompletion = false;
        f.submit();
        g_signal_emit_by_name(helpers[0]->object, name == "backend_error" ? "show-error" : "show-info", "Backend explanation");
        complete(*helpers[0]);
        pump();
        CHECK(box() && box()->text() == QStringLiteral("Backend explanation"));
        CHECK(f.result.calls == 0);
        button(QMessageBox::Ok);
        if (name == "backend_error")
        {
            retryQuestion();
            button(QMessageBox::Cancel);
        }
        f.ended();
    }
    else if (name == "external_cancel_password" || name == "external_cancel_retry")
    {
        Fixture f;
        if (name == "external_cancel_retry")
        {
            f.submit();
            retryQuestion();
        }
        f.agent->cancelAuthentication();
        CHECK(f.result.calls == 1);
        f.agent->cancelAuthentication();
        f.ended();
    }
    else if (name == "cancel_before_completion_delivery")
    {
        Fixture f;
        complete(*helpers[0]);
        f.agent->cancelAuthentication();
        f.ended();
    }
    else if (name == "stale_delivery")
    {
        Fixture f;
        Result next;
        g_signal_emit_by_name(helpers[0]->object, "show-info", "Obsolete information");
        f.agent->cancelAuthentication();
        begin(*f.agent, next, QStringLiteral("request-two"));
        pump();
        CHECK(f.result.calls == 1 && next.calls == 0 && !box());
        CHECK(helpers.size() == 2 && helpers[1]->cookie == QStringLiteral("request-two"));
        helpers[1]->gain = true;
        f.submit();
        CHECK(next.calls == 1);
    }
    else if (name == "stale_dialog")
    {
        Fixture f;
        Result next;
        f.submit();
        retryQuestion();
        QPointer<QMessageBox> old = box();
        f.agent->cancelAuthentication();
        begin(*f.agent, next, QStringLiteral("request-two"));
        // Deliver a stale answer before deferred deletion, after state was reset.
        CHECK(old);
        old->done(QMessageBox::Ok);
        pump();
        CHECK(helpers.size() == 2 && next.calls == 0 && f.result.calls == 1);
        helpers[1]->gain = true;
        f.submit();
        CHECK(next.calls == 1);
    }
    else if (name == "request_after_native_completion")
    {
        Fixture f;
        g_signal_emit_by_name(helpers[0]->object, "request", "Late prompt", FALSE);
        complete(*helpers[0]);
        pump();
        CHECK(box() && box()->standardButtons() == QMessageBox::Ok);
        button(QMessageBox::Ok);
        f.ended();
        CHECK(helpers[0]->responses == 0);
    }
    else if (name == "new_request" || name == "reentrant_result")
    {
        Fixture f;
        Result next;
        if (name == "reentrant_result")
            f.result.during = [&] { begin(*f.agent, next, QStringLiteral("request-two")); };
        f.agent->cancelAuthentication();
        if (name == "new_request")
            begin(*f.agent, next, QStringLiteral("request-two"));
        waitFor([] { return gui(); });
        helpers[1]->gain = true;
        f.submit();
        CHECK(f.result.calls == 1 && next.calls == 1);
    }
    else if (name == "finish_callback")
    {
        Fixture f;
        CHECK(f.agent->initiateAuthenticationFinish());
        helpers[0]->gain = true;
        f.submit();
        f.ended();
    }
    else if (name == "other_identity")
    {
        Fixture f(true);
        g_signal_emit_by_name(helpers[1]->object, "show-info", "Other account locked");
        complete(*helpers[1]);
        pump();
        CHECK(!box());
        helpers[0]->gain = true;
        f.submit();
        f.ended(2);
    }
    else if (name == "multiple_identities")
    {
        Fixture f(true);
        gui()->reject();
        f.ended(2);
        CHECK(helpers[0]->cancels == 1 && helpers[1]->cancels == 1);
        CHECK(helpers[0]->responses == 0 && helpers[1]->responses == 0);
    }
    else if (name == "dead_identity")
    {
        Fixture f(true);
        complete(*helpers[1]);
        pump();
        auto *choices = gui()->findChild<QComboBox *>(QStringLiteral("identityComboBox"));
        CHECK(choices->count() == 1);
        CHECK(choices->currentText() == helpers[0]->identity);
        helpers[0]->gain = true;
        f.submit();
        f.ended(2);
    }
    else if (name == "second_identity_retry")
    {
        Fixture f(true);
        gui()->findChild<QComboBox *>(QStringLiteral("identityComboBox"))->setCurrentIndex(1);
        f.submit();
        retryQuestion();
        button(QMessageBox::Ok);
        waitFor([] { return gui(); });
        auto *choices = gui()->findChild<QComboBox *>(QStringLiteral("identityComboBox"));
        CHECK(choices->count() == 1);
        CHECK(choices->currentText() == helpers[1]->identity);
        CHECK(helpers.size() == 3 && helpers[2]->identity == helpers[1]->identity);
        CHECK(helpers[0]->responses == 0 && helpers[0]->cancels == 1);
        helpers[2]->gain = true;
        f.submit();
        f.ended(3);
    }
    else if (name == "multi_prompt")
    {
        Fixture f;
        helpers[0]->prompts = 2;
        helpers[0]->gain = true;
        f.submit();
        CHECK(helpers[0]->responses == 1 && f.result.calls == 0);
        f.submit();
        f.ended();
        CHECK(helpers[0]->responses == 2);
    }
    else if (name == "synchronous_completion")
    {
        synchronousCompletion = true;
        Result result;
        LXQtPolicykit::PolicykitAgent agent;
        begin(agent, result);
        pump();
        CHECK(box() && box()->standardButtons() == QMessageBox::Ok);
        button(QMessageBox::Ok);
        CHECK(result.calls == 1 && helpers.size() == 1);
    }
    else if (name == "destruction")
    {
        Fixture f;
        f.agent.reset();
        f.ended();
        CHECK(helpers[0]->cancels == 1);
    }
    else if (name == "empty_identities")
    {
        Result result;
        LXQtPolicykit::PolicykitAgent agent;
        agent.initiateAuthentication({}, {}, {}, {}, QStringLiteral("empty"), {}, result.value.get());
        CHECK(result.calls == 1 && !result.error.isEmpty() && helpers.empty());
    }
    else if (name == "busy_request")
    {
        Fixture f;
        Result second;
        begin(*f.agent, second);
        CHECK(second.calls == 1 && !second.error.isEmpty() && f.result.calls == 0);
        CHECK(helpers.size() == 1);
        f.agent->cancelAuthentication();
        f.ended();
    }
    else if (name == "native_callback_boundary")
    {
        Fixture f;
        helpers[0]->automaticCompletion = false;
        f.submit();
        bool observed = false, unsafe = false;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&]
        {
            if (!box())
                return;
            observed = true;
            unsafe = insideNativeCompletion;
            box()->button(QMessageBox::Cancel)->click();
        });
        timer.start(1);
        complete(*helpers[0]);
        waitFor([&] { return observed; });
        CHECK(!unsafe);
        f.ended();
    }
    else if (name == "repeated_cancel")
    {
        LXQtPolicykit::PolicykitAgent agent;
        for (int i = 0; i < 30; ++i)
        {
            Result result;
            begin(agent, result, QString::number(i));
            waitFor([] { return gui(); });
            gui()->reject();
            pump();
            CHECK(result.calls == 1 && !gui() && !box());
            CHECK(helpers.back()->responses == 0 && helpers.back()->cancels == 1);
        }
    }
    else
        return false;
    return true;
}

} // namespace LifecycleCases

#endif
