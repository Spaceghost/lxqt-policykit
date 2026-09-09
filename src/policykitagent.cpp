/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * LXQt - a lightweight, Qt based, desktop toolset
 * https://lxqt.org
 *
 * Copyright: 2011-2012 Razor team
 * Authors:
 *   Petr Vanek <petr@scribus.info>
 *
 * This program or library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 *
 * END_COMMON_COPYRIGHT_HEADER */

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE 1

#include <polkitagent/polkitagent.h>
#include <PolkitQt1/Subject>

#include <QMessageBox>

#include "policykitagent.h"
#include "policykitagentgui.h"


namespace LXQtPolicykit
{

namespace
{
constexpr int maximumAuthenticationAttempts = 3;
}

PolicykitAgent::PolicykitAgent(QObject *parent)
    : PolkitQt1::Agent::Listener(parent),
      m_inProgress(false),
      m_inProgressAlert(false),
      m_userCancelled(false),
      m_errorShown(false),
      m_infoShown(false),
      m_shuttingDown(false),
      m_authenticationAttempts(0),
      m_requestId(0),
      m_gui(nullptr),
      m_result(nullptr)
{
    PolkitQt1::UnixSessionSubject session(getpid());
    registerListener(session, QStringLiteral("/org/lxqt/PolicyKit1/AuthenticationAgent"));
}

PolicykitAgent::~PolicykitAgent()
{
    m_shuttingDown = true;
    finishAuthentication(m_result);
}

void PolicykitAgent::deleteSessions()
{
    const auto sessions = m_SessionIdentity.keys();
    const auto activeSessions = m_activeSessions;
    m_SessionIdentity.clear();
    m_activeSessions.clear();
    m_submittedSessions.clear();
    for (auto *session : sessions)
    {
        disconnect(session, nullptr, this, nullptr);
        if (m_gui)
            disconnect(m_gui, nullptr, session, nullptr);
        // PolkitQt releases the native session after emitting completed().
        // Never cancel a completed session or destroy its wrapper in that signal.
        if (activeSessions.contains(session))
            session->cancel();
        session->deleteLater();
    }
}

void PolicykitAgent::createSession(const PolkitQt1::Identity &identity,
                                  const QString &cookie,
                                  PolkitQt1::Agent::AsyncResult *result)
{
    auto *session = new PolkitQt1::Agent::Session(identity, cookie, result, this);
    m_SessionIdentity[session] = identity;
    m_activeSessions.insert(session);
    connect(session, &PolkitQt1::Agent::Session::completed, this, [this, session]
    {
        m_activeSessions.remove(session);
        if (m_gui)
            disconnect(m_gui, nullptr, session, nullptr);
    });
    // Let the native callback finish before entering UI code or completing a request.
    connect(session, &PolkitQt1::Agent::Session::request, this, &PolicykitAgent::request, Qt::QueuedConnection);
    connect(session, &PolkitQt1::Agent::Session::completed, this, &PolicykitAgent::completed, Qt::QueuedConnection);
    connect(session, &PolkitQt1::Agent::Session::showError, this, &PolicykitAgent::showError, Qt::QueuedConnection);
    connect(session, &PolkitQt1::Agent::Session::showInfo, this, &PolicykitAgent::showInfo, Qt::QueuedConnection);
    session->initiate();
}

void PolicykitAgent::initiateAuthentication(const QString &actionId,
                                            const QString &message,
                                            const QString &iconName,
                                            const PolkitQt1::Details &details,
                                            const QString &cookie,
                                            const PolkitQt1::Identity::List &identities,
                                            PolkitQt1::Agent::AsyncResult *result)
{
    if (m_inProgress || m_shuttingDown)
    {
        const QString info = tr("Another authentication is in progress. Please try again later.");
        if (!m_inProgressAlert && !m_shuttingDown)
        {
            m_inProgressAlert = true;
            auto *box = new QMessageBox(QMessageBox::Information, tr("PolicyKit Information"),
                                       info, QMessageBox::Ok, m_gui);
            box->setAttribute(Qt::WA_DeleteOnClose);
            connect(box, &QObject::destroyed, this, [this]
            {
                m_inProgressAlert = false;
            });
            box->show();
        }
        result->setError(info);
        result->setCompleted();
        return;
    }
    ++m_requestId;
    m_inProgress = true;
    m_result = result;
    m_userCancelled = false;
    m_errorShown = false;
    m_infoShown = false;
    m_authenticationAttempts = 0;
    m_lastError.clear();
    m_cookie = cookie;

    if (identities.isEmpty())
    {
        result->setError(tr("Authentication failed"));
        finishAuthentication(result);
        return;
    }
    m_gui = new PolicykitAgentGUI(actionId, message, iconName, details, identities);
    // Rejecting the password dialog cancels the request, including every identity.
    connect(m_gui, &QDialog::rejected, this, &PolicykitAgent::cancelAuthentication);
    for (const PolkitQt1::Identity &identity : identities)
        createSession(identity, cookie, result);
}

bool PolicykitAgent::initiateAuthenticationFinish()
{
    // Per-request state is already cleared by finishAuthentication(). A finish
    // callback for a rejected/previous request must not clear the current one.
    return true;
}

void PolicykitAgent::cancelAuthentication()
{
    if (!m_result)
        return;
    m_userCancelled = true;
    finishAuthentication(m_result);
}

void PolicykitAgent::finishAuthentication(PolkitQt1::Agent::AsyncResult *result)
{
    if (!result || m_result != result)
        return;
    // Invalidate callbacks before cancelling helpers or notifying the caller.
    ++m_requestId;
    m_result = nullptr;
    m_inProgress = false;
    m_cookie.clear();
    deleteSessions();
    if (m_messageBox)
    {
        m_messageBox->hide();
        m_messageBox = nullptr;
    }
    if (m_gui)
    {
        m_gui->blockSignals(true);
        m_gui->hide();
        m_gui->deleteLater();
        m_gui = nullptr;
    }
    // The shared AsyncResult is completed exactly once, including cancellation.
    // Do not access request state after this potentially reentrant callback.
    result->setCompleted();
}

void PolicykitAgent::request(const QString &request, bool echo)
{
    auto *session = qobject_cast<PolkitQt1::Agent::Session *>(sender());
    if (!m_inProgress || !m_gui || !m_activeSessions.contains(session))
        return;
    if (m_infoShown)
    {
        session->cancel();
        return;
    }
    const auto identity = m_SessionIdentity.value(session);
    m_gui->setPrompt(identity, request, echo);
    // A new prompt replaces, rather than accumulates, this session's callback.
    disconnect(m_gui, &QDialog::finished, session, nullptr);
    auto *result = m_result;
    const auto requestId = m_requestId;
    connect(m_gui, &QDialog::finished, session, [this, session, result, requestId] (int choice)
    {
        if (m_requestId != requestId || m_result != result || !m_activeSessions.contains(session))
            return;
        if (choice == QDialog::Accepted)
        {
            if (m_gui->identity() == m_SessionIdentity.value(session).toString())
            {
                // Mark before setResponse(), which can complete synchronously.
                // Multiple challenges in one conversation still count as one attempt.
                m_submittedSessions.insert(session);
                session->setResponse(m_gui->response());
            }
            else
                session->cancel();
        }
    }, Qt::SingleShotConnection);
    m_gui->show();
    m_gui->activateWindow();
    m_gui->raise();
}

QMessageBox *PolicykitAgent::createMessage(QMessageBox::Icon icon, const QString &title,
                                         const QString &text, QMessageBox::StandardButtons buttons)
{
    auto *box = new QMessageBox(icon, title, text, buttons, m_gui);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::ApplicationModal);
    box->setDefaultButton(QMessageBox::Ok);
    m_messageBox = box;
    connect(box, &QDialog::finished, this, [this, box]
    {
        if (m_messageBox == box)
            m_messageBox = nullptr;
    });
    return box;
}

void PolicykitAgent::completed(bool gainedAuthorization)
{
    auto *session = qobject_cast<PolkitQt1::Agent::Session *>(sender());
    if (!m_inProgress || !m_gui || !m_SessionIdentity.contains(session))
        return;
    const auto identity = m_SessionIdentity.take(session);
    const bool responseSubmitted = m_submittedSessions.remove(session);
    disconnect(session, nullptr, this, nullptr);
    disconnect(m_gui, nullptr, session, nullptr);
    session->deleteLater();
    if (m_gui->identity() != identity.toString())
    {
        // There is no live conversation to answer for this choice anymore.
        m_gui->removeIdentity(identity);
        return;
    }

    auto *result = m_result;
    const auto requestId = m_requestId;
    if (m_messageBox)
    {
        // Preserve the backend message and wait for acknowledgement without
        // keeping a native signal callback or a nested event loop on the stack.
        connect(m_messageBox, &QDialog::finished, this,
                [this, identity, result, gainedAuthorization, responseSubmitted, requestId]
        {
            completeAttempt(identity, result, gainedAuthorization, responseSubmitted, requestId);
        }, Qt::SingleShotConnection);
        return;
    }
    completeAttempt(identity, result, gainedAuthorization, responseSubmitted, requestId);
}

void PolicykitAgent::completeAttempt(const PolkitQt1::Identity &identity,
                                    PolkitQt1::Agent::AsyncResult *result,
                                    bool gainedAuthorization, bool responseSubmitted,
                                    quint64 requestId)
{
    if (m_requestId != requestId || m_result != result || !m_inProgress)
        return;
    // A helper failure without a submitted response is not a password retry.
    if (!gainedAuthorization && responseSubmitted && !m_userCancelled && !m_infoShown
        && ++m_authenticationAttempts < maximumAuthenticationAttempts)
    {
        auto *box = createMessage(QMessageBox::Information, tr("Authorization Failed"),
                                 tr("Authentication failed. Trying again?"),
                                 QMessageBox::Ok | QMessageBox::Cancel);
        connect(box, &QDialog::finished, this, [this, identity, result, requestId] (int choice)
        {
            if (m_requestId != requestId || m_result != result)
                return;
            if (choice == QMessageBox::Ok && !m_userCancelled && !m_infoShown)
            {
                m_errorShown = false;
                m_infoShown = false;
                m_lastError.clear();
                createSession(identity, m_cookie, result);
            }
            else
            {
                m_userCancelled = true;
                finishAuthentication(result);
            }
        }, Qt::SingleShotConnection);
        box->show();
        return;
    }
    if (!gainedAuthorization && !m_userCancelled && !m_errorShown)
    {
        const QString text = m_lastError.isEmpty()
            ? tr("Authentication failed")
            : m_lastError;
        auto *box = createMessage(QMessageBox::Information, tr("Authorization Failed"), text);
        connect(box, &QDialog::finished, this, [this, result, requestId]
        {
            if (m_requestId == requestId)
                finishAuthentication(result);
        }, Qt::SingleShotConnection);
        box->show();
        return;
    }
    finishAuthentication(result);
}

void PolicykitAgent::showBackendMessage(const QString &text, bool informational)
{
    auto *session = qobject_cast<PolkitQt1::Agent::Session *>(sender());
    if (!m_inProgress || !m_gui || !m_SessionIdentity.contains(session)
        || m_gui->identity() != m_SessionIdentity.value(session).toString())
        return;
    m_lastError = text;
    m_errorShown = true;
    m_infoShown = m_infoShown || informational;
    if (m_messageBox)
    {
        const QString previous = m_messageBox->informativeText();
        m_messageBox->setInformativeText(previous.isEmpty() ? text : previous + QLatin1Char('\n') + text);
        return;
    }
    auto *box = createMessage(informational ? QMessageBox::Information : QMessageBox::Warning,
                             informational ? tr("PolicyKit Information") : tr("PolicyKit Error"), text);
    box->show();
}

void PolicykitAgent::showError(const QString &text)
{
    showBackendMessage(text, false);
}

void PolicykitAgent::showInfo(const QString &text)
{
    showBackendMessage(text, true);
}

} //namespace
