// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../lifecycle/agent_fixture.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QScreen>
#include <QSocketNotifier>
#include <QVBoxLayout>
#include <QWindow>
#include <unistd.h>

// This endpoint can initiate requests and control the fake authentication backend.
// It cannot click, type, accept, reject, focus, or close an authentication dialog.
// The external runner performs those operations through the display server.
class InputAudit : public QObject
{
public:
    int keys = 0;
    int clicks = 0;
    int closes = 0;

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->spontaneous())
        {
            if (event->type() == QEvent::KeyPress)
                ++keys;
            if (event->type() == QEvent::MouseButtonPress)
                ++clicks;
            if (event->type() == QEvent::Close)
                ++closes;
        }
        return false;
    }
};

static QJsonObject control(QWidget *widget, QWidget *window)
{
    const QPoint center = widget->mapTo(window, widget->rect().center());
    return {{"x", center.x()}, {"y", center.y()},
            {"enabled", widget->isEnabled()}, {"focused", widget->hasFocus()}};
}

static QJsonObject snapshot(const std::vector<std::unique_ptr<Result>> &results,
                            const InputAudit &audit)
{
    QJsonArray windows;
    for (auto *widget : QApplication::topLevelWidgets())
    {
        if (!widget->isVisible())
            continue;
        auto *window = widget->windowHandle();
        if (!window)
            continue;
        QJsonObject item{{"title", widget->windowTitle()},
                         {"id", QString::number(widget->winId())},
                         {"active", widget->isActiveWindow()},
                         {"exposed", window->isExposed()},
                         {"width", widget->width()}, {"height", widget->height()},
                         {"modal", QApplication::activeModalWidget() == widget}};
        QJsonObject controls;
        if (auto *password = qobject_cast<LXQtPolicykit::PolicykitAgentGUI *>(widget))
        {
            item["kind"] = "password";
            auto *edit = password->findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
            auto *buttons = password->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"));
            auto *choices = password->findChild<QComboBox *>(QStringLiteral("identityComboBox"));
            controls["password"] = control(edit, widget);
            controls["ok"] = control(buttons->button(QDialogButtonBox::Ok), widget);
            controls["cancel"] = control(buttons->button(QDialogButtonBox::Cancel), widget);
            controls["identity"] = control(choices, widget);
            item["identity"] = choices->currentText();
            item["input_length"] = edit->text().size();
        }
        else if (auto *message = qobject_cast<QMessageBox *>(widget))
        {
            item["kind"] = "message";
            item["text"] = message->text();
            item["buttons"] = int(message->standardButtons());
            for (const auto choice : {QMessageBox::Ok, QMessageBox::Cancel})
            {
                if (auto *button = message->button(choice))
                    controls[choice == QMessageBox::Ok ? "ok" : "cancel"] = control(button, widget);
            }
        }
        else if (widget->windowType() == Qt::Popup)
        {
            item["kind"] = "popup";
            item["popup_active"] = QApplication::activePopupWidget() == widget;
        }
        else
        {
            item["kind"] = "requester";
            auto *edit = widget->findChild<QLineEdit *>(QStringLiteral("requesterEdit"));
            if (edit)
            {
                controls["edit"] = control(edit, widget);
                item["input_length"] = edit->text().size();
            }
        }
        item["controls"] = controls;
        windows.append(item);
    }
    QJsonArray conversations;
    for (const auto &h : helpers)
        conversations.append(QJsonObject{{"responses", h->responses}, {"cancels", h->cancels},
                                         {"alive", h->object != nullptr}, {"identity", h->identity}});
    QJsonArray completions;
    for (const auto &result : results)
        completions.append(result->calls);
    return {{"platform", QGuiApplication::platformName()}, {"pid", int(getpid())},
            {"windows", windows}, {"helpers", conversations}, {"results", completions},
            {"native_keys", audit.keys}, {"native_clicks", audit.clicks},
            {"native_closes", audit.closes}};
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QString expected = qEnvironmentVariable("DESKTOP_EXPECTED_QPA");
    if ((expected != QLatin1String("xcb") && expected != QLatin1String("wayland"))
        || QGuiApplication::platformName() != expected)
    {
        std::cerr << "DESKTOP ASSERTION: real_platform_required\n";
        return 2;
    }
    const bool requester = app.arguments().contains(QStringLiteral("--requester"));
    QGuiApplication::setDesktopFileName(requester
        ? QStringLiteral("org.lxqt.PolicyKit1.DesktopRequester")
        : QStringLiteral("org.lxqt.PolicyKit1.DesktopTest"));
    InputAudit audit;
    app.installEventFilter(&audit);
    // Results must outlive the agent and all its completion callbacks.
    std::vector<std::unique_ptr<Result>> results;
    std::unique_ptr<LXQtPolicykit::PolicykitAgent> agent;
    std::unique_ptr<QWidget> requesterWindow;
    if (requester)
    {
        requesterWindow = std::make_unique<QWidget>();
        requesterWindow->setWindowTitle(QStringLiteral("Desktop test requester"));
        auto *layout = new QVBoxLayout(requesterWindow.get());
        layout->addWidget(new QLabel(QStringLiteral("Independent requesting application")));
        auto *edit = new QLineEdit;
        edit->setObjectName(QStringLiteral("requesterEdit"));
        layout->addWidget(edit);
        requesterWindow->resize(480, 220);
        requesterWindow->show();
        edit->setFocus();
    }
    else
        agent = std::make_unique<LXQtPolicykit::PolicykitAgent>();

    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&]
    {
        std::string line;
        if (!std::getline(std::cin, line))
        {
            app.quit();
            return;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
        {
            std::cerr << "DESKTOP ASSERTION: invalid_control_command\n";
            std::exit(2);
        }
        const auto command = document.object();
        const QString operation = command["op"].toString();
        if (operation == QLatin1String("begin") && agent)
        {
            results.push_back(std::make_unique<Result>());
            begin(*agent, *results.back(), QStringLiteral("desktop-%1").arg(results.size()),
                  command["multiple"].toBool());
        }
        else if (operation == QLatin1String("backend") && agent)
        {
            const int index = command["index"].toInt(-1);
            CHECK(index >= 0 && index < int(helpers.size()));
            auto &h = *helpers.at(index);
            CHECK(h.object && !h.completed);
            if (command.contains("gain"))
                h.gain = command["gain"].toBool();
            if (command.contains("automatic"))
                h.automaticCompletion = command["automatic"].toBool();
            if (command.contains("error"))
            {
                const auto text = command["error"].toString().toUtf8();
                g_signal_emit_by_name(h.object, "show-error", text.constData());
            }
            if (command["complete"].toBool())
                complete(h, h.gain);
        }
        else if (operation == QLatin1String("cancel_request") && agent)
            agent->cancelAuthentication();
        else if (operation == QLatin1String("capture") && expected == QLatin1String("xcb"))
        {
            const auto image = app.primaryScreen()->grabWindow(0);
            CHECK(!image.isNull() && image.save(command["path"].toString()));
        }
        else if (operation != QLatin1String("snapshot"))
        {
            std::cerr << "DESKTOP ASSERTION: unsupported_control_command\n";
            std::exit(2);
        }
        auto response = snapshot(results, audit);
        response["sequence"] = command["sequence"];
        std::cout << QJsonDocument(response).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
    });
    return app.exec();
}
