// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PaneOverlay.h"

#include "Pane.h"
#include "scripting/ScriptFiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

namespace Afterglow {

PaneOverlay::PaneOverlay(Pane& host)
    : QObject(&host)
    , m_host(&host)
{
}

PaneOverlay* PaneOverlay::start(Pane& host, const OverlaySpec& spec, const Launch& launch, QString* error)
{
    // Deleted on failure before it answers or joins its host.
    std::unique_ptr<PaneOverlay> overlay(new PaneOverlay(host));
    overlay->m_capture = spec.capture;
    overlay->m_title = spec.title;
    AfterglowPty::SpawnOptions options = launch.spawn;
    for (const QString& arg : spec.argv) options.argv.push_back(arg.toStdString());
    // A path that a remote shell reported is no directory here.
    const bool local = !spec.cwd.isEmpty() && QDir::isAbsolutePath(spec.cwd) && QFileInfo(spec.cwd).isDir();
    options.workingDirectory = (local ? spec.cwd : QDir::homePath()).toStdString();
    for (auto it = spec.env.cbegin(); it != spec.env.cend(); ++it) {
        const std::string name = it.key().toStdString();
        std::erase_if(options.environment, [&name](const auto& entry) { return entry.first == name; });
        std::erase(options.unsetEnvironment, name);
        if (it.value().metaType().id() == QMetaType::Bool) options.unsetEnvironment.push_back(name);
        else options.environment.emplace_back(name, it.value().toString().toStdString());
    }
    std::erase_if(options.environment, [](const auto& entry) { return entry.first == "AFTERGLOW_RESULT"; });
    std::erase(options.unsetEnvironment, std::string("AFTERGLOW_RESULT"));
    if (spec.capture == OverlaySpec::Capture::File) {
        // Created 0600, and removed with the overlay.
        overlay->m_resultFile = std::make_unique<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/afterglow-overlay-XXXXXX"));
        if (!overlay->m_resultFile->open()) {
            if (error) *error = QStringLiteral("cannot create the result file: %1").arg(overlay->m_resultFile->errorString());
            return nullptr;
        }
        overlay->m_resultFile->close();
        options.environment.emplace_back("AFTERGLOW_RESULT", QFile::encodeName(overlay->m_resultFile->fileName()).toStdString());
    } else {
        options.unsetEnvironment.emplace_back("AFTERGLOW_RESULT");
    }
    if (spec.input) options.input = spec.input->toStdString();
    options.captureOutput = spec.capture == OverlaySpec::Capture::Stdout;

    auto* pane = new Pane(launch.terminal, overlay.get());
    overlay->m_pane = pane;
    pane->setOverlayHost(&host);
    pane->setOutputLimit(std::size_t(OverlaySpec::MaxOutput) + 1);
    pane->setLaunch(QString::fromStdString(options.workingDirectory), spec.argv);
    if (!spec.argv.isEmpty()) pane->setLaunchedProgram(QFileInfo(spec.argv.first()).fileName());
    if (!spec.title.isEmpty()) pane->setUserTitle(spec.title);
    // Queued from the pty thread: the reply is set before it arrives.
    connect(pane, &Pane::closed, overlay.get(), &PaneOverlay::programExited);
    if (!pane->start(options, launch.term, launch.cols, launch.rows, launch.cellWidth, launch.cellHeight, error)) return nullptr;
    host.setOverlay(overlay.get());
    return overlay.release();
}

PaneOverlay::~PaneOverlay()
{
    // Still on its host, which deletes it first thing when it closes.
    if (!m_answered) finish(QStringLiteral("The pane under the overlay closed"), QStringLiteral("cancelled"), {});
}

void PaneOverlay::setReply(const QString& owner, const QString& request, Reply reply)
{
    m_owner = owner;
    m_request = request;
    m_reply = std::move(reply);
}

void PaneOverlay::cancel(const QString& message)
{
    if (m_answered) return;
    finish(message, QStringLiteral("cancelled"), {});
    Q_EMIT ended();
}

void PaneOverlay::programExited()
{
    if (m_answered) return;
    bool truncated = false;
    QByteArray output;
    if (m_capture == OverlaySpec::Capture::Stdout) {
        output = QByteArray::fromStdString(m_pane->takeOutput(&truncated));
    } else if (m_capture == OverlaySpec::Capture::File && m_resultFile) {
        QFile file(m_resultFile->fileName());
        if (file.open(QIODevice::ReadOnly)) output = file.read(OverlaySpec::MaxOutput + 1);
    }
    if (output.size() > OverlaySpec::MaxOutput) {
        truncated = true;
        output = utf8Prefix(std::move(output), OverlaySpec::MaxOutput);
    }
    finish({}, {},
           {{QStringLiteral("exit_code"), m_pane->exitCode()}, {QStringLiteral("stdout"), QString::fromUtf8(output)},
            {QStringLiteral("truncated"), truncated}});
    Q_EMIT ended();
}

void PaneOverlay::finish(const QString& error, const QString& code, const QVariantMap& result)
{
    if (m_host && m_host->overlay() == this) m_host->setOverlay(nullptr);
    if (m_answered) return;
    m_answered = true;
    if (auto reply = std::exchange(m_reply, {})) reply(error, code, result);
}

} // namespace Afterglow
