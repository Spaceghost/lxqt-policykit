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

#ifndef POLICYKITAGENT_H
#define POLICYKITAGENT_H

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE 1

#include <PolkitQt1/Agent/Listener>
#include <PolkitQt1/Agent/Session>
#include <PolkitQt1/Details>
#include <PolkitQt1/Identity>

#include <QApplication>
#include <QHash>
#include <QMessageBox>
#include <QPointer>
#include <QSet>

namespace LXQtPolicykit
{

class PolicykitAgentGUI;

class PolicykitAgent : public PolkitQt1::Agent::Listener
{
    Q_OBJECT

public:
    PolicykitAgent(QObject *parent = 0);
    ~PolicykitAgent() override;

public slots:
    void initiateAuthentication(const QString &actionId,
                                const QString &message,
                                const QString &iconName,
                                const PolkitQt1::Details &details,
                                const QString &cookie,
                                const PolkitQt1::Identity::List &identities,
                                PolkitQt1::Agent::AsyncResult *result) override;
    bool initiateAuthenticationFinish() override;
    void cancelAuthentication() override;

    void request(const QString &request, bool echo);
    void completed(bool gainedAuthorization);
    void showError(const QString &text);
    void showInfo(const QString &text);

private:
    void createSession(const PolkitQt1::Identity &identity,
                       const QString &cookie,
                       PolkitQt1::Agent::AsyncResult *result);
    void deleteSessions();
    void finishAuthentication(PolkitQt1::Agent::AsyncResult *result);
    void completeAttempt(const PolkitQt1::Identity &identity,
                         PolkitQt1::Agent::AsyncResult *result,
                         bool gainedAuthorization, quint64 requestId);
    QMessageBox *createMessage(QMessageBox::Icon icon, const QString &title,
                              const QString &text,
                              QMessageBox::StandardButtons buttons = QMessageBox::Ok);
    void showBackendMessage(const QString &text, bool informational);

    bool m_inProgress;
    bool m_inProgressAlert;
    bool m_userCancelled;
    bool m_errorShown;
    bool m_infoShown;
    bool m_shuttingDown;
    int m_authenticationAttempts;
    quint64 m_requestId;
    QString m_lastError;
    QString m_cookie;
    PolicykitAgentGUI * m_gui;
    PolkitQt1::Agent::AsyncResult *m_result;
    QPointer<QMessageBox> m_messageBox;
    QHash<PolkitQt1::Agent::Session*,PolkitQt1::Identity> m_SessionIdentity;
    QSet<PolkitQt1::Agent::Session*> m_activeSessions;
};

} // namespace

#endif
