// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef LXQT_POLICYKIT_TEST_REVIEW_CASES_H
#define LXQT_POLICYKIT_TEST_REVIEW_CASES_H

#include "agent_fixture.h"

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
        QObject::connect(&observer, &QTimer::timeout, [&]
        {
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
        submit();
        question();
        messageButton(QMessageBox::Ok);
        submit();
        question();
        messageButton(QMessageBox::Cancel);
        completedRequest(f.result, 2);
        CHECK(helpers[0]->responses == 1 && helpers[1]->responses == 1);
    }
    else if (name == "review_backend_error_consent")
    {
        Fixture f;
        helpers[0]->automaticCompletion = false;
        submit();
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
        question();
        messageButton(QMessageBox::Cancel);
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
        helpers[1]->gain = true;
        submit();
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

#endif
