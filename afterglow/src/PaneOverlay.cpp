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
    overlay->m_borderTitle = spec.title.toStdString();
    overlay->m_shape.width = spec.width;
    overlay->m_shape.height = spec.height;
    overlay->m_shape.position = spec.position;
    overlay->m_shape.border = spec.border;
    overlay->m_placement = overlay->m_shape.place(launch.cols, launch.rows);
    AfterglowPty::SpawnOptions options = launch.spawn;
    for (const QString& arg : spec.argv) options.argv.push_back(arg.toStdString());
    QString cwd = spec.cwd;
    if (cwd == QLatin1String("~") || cwd.startsWith(QLatin1String("~/"))) cwd = QDir::homePath() + cwd.mid(1);
    // A path that a remote shell reported is no directory here.
    const bool local = !cwd.isEmpty() && QDir::isAbsolutePath(cwd) && QFileInfo(cwd).isDir();
    options.workingDirectory = (local ? cwd : QDir::homePath()).toStdString();
    applyLaunchEnvironment(options, spec.env);
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
    // An ssh session that the program runs answers as the host's would
    // (Pane::setRemoteSocket keeps it current).
    pane->setRemoteSocket(host.remoteSocketAnswer());
    pane->setOutputLimit(std::size_t(OverlaySpec::MaxOutput) + 1);
    pane->setLaunch(QString::fromStdString(options.workingDirectory), spec.argv);
    if (!spec.argv.isEmpty()) pane->setLaunchedProgram(QFileInfo(spec.argv.first()).fileName());
    if (!spec.title.isEmpty()) pane->setUserTitle(spec.title);
    // Queued from the pty thread: the reply is set before it arrives.
    connect(pane, &Pane::closed, overlay.get(), &PaneOverlay::programExited);
    const QRect grid = overlay->m_placement.grid;
    if (!pane->start(std::move(options), launch.term, grid.width(), grid.height(), launch.cellWidth, launch.cellHeight, error))
        return nullptr;
    host.setOverlay(overlay.get());
    return overlay.release();
}

PaneOverlay::~PaneOverlay()
{
    // Still on its host, which deletes it first thing when it closes.
    if (!m_answered) finish(QStringLiteral("The pane under the overlay closed"), QStringLiteral("cancelled"), {});
}

void PaneOverlay::place(int cols, int rows, int cellWidth, int cellHeight)
{
    m_placement = m_shape.place(cols, rows);
    m_pane->resize(m_placement.grid.width(), m_placement.grid.height(), cellWidth, cellHeight);
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
        output = readResult(m_resultFile->fileName());
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

QByteArray PaneOverlay::readResult(const QString& path)
{
    // The program can put anything at the path: a FIFO would hang the GUI
    // thread, and a link could name another file of the user. As
    // readScriptFile, but the text is cut at the limit, not refused.
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return {};
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    struct stat info {};
    if (::fstat(fd, &info) || !S_ISREG(info.st_mode)) return {};
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) return {};
    return file.read(OverlaySpec::MaxOutput + 1);
}

void PaneOverlay::finish(const QString& error, const QString& code, const QVariantMap& result)
{
    if (m_host && m_host->overlay() == this) m_host->setOverlay(nullptr);
    if (m_answered) return;
    m_answered = true;
    if (auto reply = std::exchange(m_reply, {})) reply(error, code, result);
}

} // namespace Afterglow
