// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef LXQT_POLICYKIT_TEST_ATTEMPT_CASES_H
#define LXQT_POLICYKIT_TEST_ATTEMPT_CASES_H

#include "review_cases.h"

// Additional scenarios share the real-agent fixture in test_agent_lifecycle.cpp.
// No production source is extracted, patched or replaced by this header.

namespace AttemptCases
{
static void terminalFailure()
{
    waitFor([] { return box(); });
    REVIEW_CHECK("unsubmitted_failure_is_terminal", box()->standardButtons() == QMessageBox::Ok);
    CHECK(box()->text() == QStringLiteral("Authentication failed"));
    Review::messageButton(QMessageBox::Ok);
}

static void noOwnedObjects(const LXQtPolicykit::PolicykitAgent &agent)
{
    pump();
    REVIEW_CHECK("session_wrappers_retired", agent.findChildren<PolkitQt1::Agent::Session *>().isEmpty());
    for (auto *widget : QApplication::topLevelWidgets())
    {
        REVIEW_CHECK("dialogs_destroyed", !qobject_cast<LXQtPolicykit::PolicykitAgentGUI *>(widget));
        REVIEW_CHECK("dialogs_destroyed", !qobject_cast<QMessageBox *>(widget));
    }
}

static bool run(const std::string &name)
{
    if (name == "no_response_failure")
    {
        Fixture f;
        complete(*helpers[0]);
        terminalFailure();
        f.ended();
        CHECK(helpers[0]->responses == 0);
    }
    else if (name == "no_response_error")
    {
        Fixture f;
        g_signal_emit_by_name(helpers[0]->object, "show-error", "Helper failed before submission");
        complete(*helpers[0]);
        pump();
        CHECK(box() && box()->text() == QStringLiteral("Helper failed before submission"));
        Review::messageButton(QMessageBox::Ok);
        REVIEW_CHECK("unsubmitted_error_does_not_retry", f.result.calls == 1 && helpers.size() == 1);
        f.ended();
    }
    else if (name == "retry_startup_failure")
    {
        Fixture f;
        Review::submit();
        Review::question();
        synchronousCompletion = true;
        Review::messageButton(QMessageBox::Ok);
        terminalFailure();
        f.ended(2);
        CHECK(helpers[0]->responses == 1 && helpers[1]->responses == 0);
        synchronousCompletion = false;
    }
    else if (name == "success_without_response")
    {
        Fixture f;
        complete(*helpers[0], true);
        f.ended();
        CHECK(helpers[0]->responses == 0);
    }
    else if (name == "cancel_before_first_prompt")
    {
        Result cancelled;
        Result next;
        LXQtPolicykit::PolicykitAgent agent;
        begin(agent, cancelled);
        // Cancel before the queued native prompt has reached the agent.
        agent.cancelAuthentication();
        begin(agent, next, QStringLiteral("after-early-cancel"));
        waitFor([] { return gui(); });
        CHECK(cancelled.calls == 1 && next.calls == 0 && helpers.size() == 2);
        CHECK(helpers[0]->responses == 0 && helpers[0]->cancels == 1);
        helpers[1]->gain = true;
        Review::submit();
        Review::completedRequest(next, 2);
        noOwnedObjects(agent);
    }
    else if (name == "multi_challenge_attempt_budget")
    {
        Fixture f;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            waitFor([] { return gui() && !box(); });
            auto &h = *helpers.back();
            h.prompts = 3;
            h.gain = attempt == 2;
            for (int challenge = 0; challenge < 3; ++challenge)
            {
                Review::submit();
                CHECK(h.responses == challenge + 1);
                if (challenge < 2)
                {
                    REVIEW_CHECK("challenges_do_not_consume_retries", !box() && f.result.calls == 0);
                    CHECK(helpers.size() == std::size_t(attempt + 1));
                }
            }
            if (attempt < 2)
            {
                Review::question();
                Review::messageButton(QMessageBox::Ok);
            }
        }
        f.ended(3);
    }
    else if (name == "cancel_second_challenge")
    {
        Fixture f;
        helpers[0]->prompts = 3;
        Review::submit();
        Review::passwordButton(QDialogButtonBox::Cancel);
        f.ended();
        CHECK(helpers[0]->responses == 1 && helpers[0]->cancels == 1);
    }
    else if (name == "unselected_failure_no_budget")
    {
        Fixture f(true);
        complete(*helpers[1]);
        pump();
        CHECK(!box() && f.result.calls == 0);
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            auto &h = *helpers[attempt == 0 ? 0 : attempt + 1];
            h.gain = attempt == 2;
            Review::submit();
            if (attempt < 2)
            {
                Review::question();
                Review::messageButton(QMessageBox::Ok);
            }
        }
        f.ended(4);
        CHECK(helpers[1]->responses == 0);
    }
    else if (name == "delayed_identity_prompt")
    {
        automaticPrompts = false;
        Result result;
        LXQtPolicykit::PolicykitAgent agent;
        begin(agent, result, QStringLiteral("delayed-identities"), true);
        g_signal_emit_by_name(helpers[0]->object, "request", "First password:", FALSE);
        waitFor([] { return gui(); });
        gui()->findChild<QComboBox *>(QStringLiteral("identityComboBox"))->setCurrentIndex(1);
        // The second conversation only becomes ready after identity selection.
        g_signal_emit_by_name(helpers[1]->object, "request", "Second password:", FALSE);
        pump();
        helpers[1]->gain = true;
        Review::submit();
        Review::completedRequest(result, 2);
        CHECK(helpers[0]->responses == 0 && helpers[0]->cancels == 1);
        CHECK(helpers[1]->responses == 1 && helpers[1]->cancels == 0);
        noOwnedObjects(agent);
    }
    else if (name == "owned_object_churn")
    {
        std::vector<std::unique_ptr<Result>> results;
        LXQtPolicykit::PolicykitAgent agent;
        for (int request = 0; request < 150; ++request)
        {
            results.push_back(std::make_unique<Result>());
            auto &result = *results.back();
            begin(agent, result, QStringLiteral("churn-%1").arg(request));
            waitFor([] { return gui(); });
            const auto sessions = agent.findChildren<PolkitQt1::Agent::Session *>();
            REVIEW_CHECK("agent_owns_session_wrappers", sessions.size() == 1);
            QPointer<PolkitQt1::Agent::Session> wrapper = sessions.first();
            QPointer<LXQtPolicykit::PolicykitAgentGUI> password = gui();
            if (request % 3 == 0)
            {
                Review::passwordButton(QDialogButtonBox::Cancel);
            }
            else if (request % 3 == 1)
            {
                Review::submit();
                Review::question();
                Review::messageButton(QMessageBox::Cancel);
            }
            else
            {
                helpers.back()->gain = true;
                Review::submit();
            }
            Review::completedRequest(result, std::size_t(request + 1));
            REVIEW_CHECK("session_wrappers_retired", !wrapper);
            REVIEW_CHECK("dialogs_destroyed", !password);
            noOwnedObjects(agent);
        }
        for (const auto &result : results)
            CHECK(result->calls == 1);
    }
    else if (name == "shutdown_owns_wrappers")
    {
        Fixture f;
        const auto sessions = f.agent->findChildren<PolkitQt1::Agent::Session *>();
        REVIEW_CHECK("agent_owns_session_wrappers", sessions.size() == 1);
        QPointer<PolkitQt1::Agent::Session> wrapper = sessions.first();
        f.agent.reset();
        // QObject ownership must work even before a deferred-delete event runs.
        REVIEW_CHECK("shutdown_retires_wrappers", !wrapper);
        f.ended();
    }
    else if (name == "stale_backend_acknowledgement")
    {
        Fixture f;
        Result next;
        helpers[0]->automaticCompletion = false;
        Review::submit();
        g_signal_emit_by_name(helpers[0]->object, "show-error", "Old backend error");
        complete(*helpers[0]);
        pump();
        QPointer<QMessageBox> old = box();
        CHECK(old);
        f.agent->cancelAuthentication();
        begin(*f.agent, next, QStringLiteral("after-backend-cancel"));
        old->done(QMessageBox::Ok);
        pump();
        REVIEW_CHECK("stale_backend_cannot_retry", helpers.size() == 2 && next.calls == 0 && !box());
        helpers[1]->gain = true;
        Review::submit();
        Review::completedRequest(next, 2);
        CHECK(f.result.calls == 1);
    }
    else if (name == "repeated_retry_answer")
    {
        Fixture f;
        Review::submit();
        Review::question();
        QPointer<QMessageBox> old = box();
        old->done(QMessageBox::Ok);
        old->done(QMessageBox::Ok);
        pump();
        REVIEW_CHECK("one_consent_one_helper", helpers.size() == 2 && f.result.calls == 0);
        helpers[1]->gain = true;
        Review::submit();
        f.ended(2);
    }
    else
        return false;
    return true;
}
} // namespace AttemptCases

#endif
