// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
// Files between programs and the user: the questions of file transfers,
// downloads and uploads, and the output of a command that goes to the
// pager as a file.
#include "TerminalWindow.h"
#include "PaneDialog.h"
#include "PaneLaunch.h"
#include "app/NotificationCenter.h"
#include "desktop/Download.h"
#include "desktop/FileTransfer.h"
#include "desktop/UploadArchive.h"
#include "mux/MuxClient.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QtConcurrentRun>

#include <memory>

namespace Afterglow {

namespace {
/// A question about what a program of a pane asks, over nothing yet: the
/// caller adds its buttons, then puts it over the pane (keepDialogOverPane).
/// It goes when it closes. Its lambdas must not capture the window: the
/// dialog follows the pane to another window, and outlives this one.
QMessageBox* paneQuestion(const QString& title, const QString& text, QMessageBox::StandardButtons buttons = QMessageBox::NoButton)
{
    auto* dialog = new QMessageBox(QMessageBox::Question, title, text, buttons);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // A native message box (Plasma's) is a second box that this one
    // opens later, and that adds its buttons again each time it shows:
    // keepDialogOverPane moves a native dialog by a hide and a show.
    dialog->setOption(QMessageBox::Option::DontUseNativeDialog);
    // The program chose some of the words.
    dialog->setTextFormat(Qt::PlainText);
    return dialog;
}

/// @p name in @p directory, or "name (N).ext" when that is taken: by a
/// file, or by the part file of a download that streams to it, which
/// DownloadFile makes at the answer, on this thread. Empty when every name
/// of DownloadNames is taken. A file that takes the name later does not go:
/// DownloadFile renames to the next free name then.
QString uniqueDownloadPath(const QString& directory, const QString& name)
{
    QDir().mkpath(directory);
    const QDir dir(directory);
    const auto taken = [](const QString& path) {
        return QFileInfo::exists(path) || QFileInfo(path).isSymLink() || QFileInfo::exists(path + QStringLiteral(".part"));
    };
    for (int n = 0; n < DownloadNames; ++n) {
        const QString path = dir.filePath(numberedDownloadName(name, n));
        if (!taken(path)) return path;
    }
    return {};
}

/// Answers download question @p serial of @p pane with a free name for
/// @p file in @p directory, or refuses it when there is none.
void saveDownloadIn(Pane* pane, const QString& directory, const QString& file, quint64 serial)
{
    const QString path = uniqueDownloadPath(directory, file);
    if (path.isEmpty())
        NotificationCenter::instance().showNotice(pane, TerminalWindow::tr("Download not saved"),
                                                  TerminalWindow::tr("%1 has no free name for “%2”.").arg(directory, sanitizedText(file, 200)));
    pane->answerDownload(path, serial);
}
} // namespace

void TerminalWindow::askDownload(Pane* pane, const QString& name, qint64 size, quint64 serial)
{
    const Settings& settings = m_settings.settings();
    // Settings::read expanded a ~ of the config.
    QString directory = settings.downloadDirectory;
    if (directory.isEmpty()) directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString file = name.isEmpty() ? QStringLiteral("download.bin") : name;
    switch (settings.downloads) {
    case Settings::Permission::Deny:
        pane->answerDownload({}, serial);
        return;
    case Settings::Permission::Allow:
        saveDownloadIn(pane, directory, file, serial);
        return;
    case Settings::Permission::Ask:
        break;
    }
    // One question, before the data comes, as iTerm2 asks: the program
    // waits. It starts on Refuse, so a Return typed for the shell refuses.
    const QString shown = sanitizedText(file, 200);
    const QString text = size >= 0
        ? tr("%1 wants to save the file “%2” (%3).").arg(clipboardAsker(pane), shown, QLocale().formattedDataSize(size))
        : tr("%1 wants to save the file “%2”. Its size is not known.").arg(clipboardAsker(pane), shown);
    QMessageBox* dialog = paneQuestion(tr("Download"), text);
    // The root directory has no name of its own.
    const QString place = QDir(directory).dirName().isEmpty() ? QDir::toNativeSeparators(directory) : QDir(directory).dirName();
    QPushButton* save = dialog->addButton(tr("Save to %1").arg(place), QMessageBox::AcceptRole);
    QPushButton* saveAs = dialog->addButton(tr("Save As…"), QMessageBox::ActionRole);
    QPushButton* refuse = dialog->addButton(tr("Refuse"), QMessageBox::RejectRole);
    dialog->setDefaultButton(refuse);
    dialog->setEscapeButton(refuse);
    keepDialogOverPane(dialog, pane);
    const QPointer<Pane> guard(pane);
    // The answer names its question: a later download of the pane waits
    // for an answer of its own.
    connect(dialog, &QDialog::finished, dialog, [guard, dialog, save, saveAs, directory, file, serial] {
        if (!guard) return;
        if (dialog->clickedButton() == save) saveDownloadIn(guard, directory, file, serial);
        else if (dialog->clickedButton() == saveAs) saveDownloadAs(guard, directory, file, serial);
        else guard->answerDownload({}, serial);
    });
    // The download ended before the answer, or the pane closed.
    connect(pane, &Pane::downloadCancelled, dialog, &QDialog::reject);
    connect(pane, &QObject::destroyed, dialog, &QDialog::reject);
    dialog->open();
    if (!isActive()) alert(0);
}

void TerminalWindow::askUpload(Pane* pane)
{
    const Settings& settings = m_settings.settings();
    if (settings.uploads == Settings::Permission::Deny) {
        // Nothing goes to the program: Ctrl+C from a setting would signal
        // whatever runs in the foreground then.
        pane->refuseUpload();
        return;
    }
    // The limit of the question that the user answers, also when the
    // settings change before the files are chosen.
    const std::size_t limit = settings.uploadLimit;
    // The picker is the consent, as in iTerm2. A question first: a folder
    // and files are two pickers, and Refuse is the default, so a Return
    // typed for the shell refuses.
    const QString text = tr("%1 asks for files. The files that you choose go to it as typed input, at most %2 MiB compressed.")
                             .arg(clipboardAsker(pane), QString::number(limit >> 20));
    QMessageBox* dialog = paneQuestion(tr("Upload"), text);
    QPushButton* files = dialog->addButton(tr("Choose Files…"), QMessageBox::AcceptRole);
    QPushButton* folder = dialog->addButton(tr("Choose a Folder…"), QMessageBox::ActionRole);
    QPushButton* refuse = dialog->addButton(tr("Refuse"), QMessageBox::RejectRole);
    dialog->setDefaultButton(refuse);
    dialog->setEscapeButton(refuse);
    keepDialogOverPane(dialog, pane);
    const QPointer<Pane> guard(pane);
    connect(dialog, &QDialog::finished, dialog, [guard, dialog, files, folder, limit] {
        if (!guard) return;
        const bool directory = dialog->clickedButton() == folder;
        if (!directory && dialog->clickedButton() != files) {
            guard->answerUpload(std::nullopt);
            return;
        }
        auto* picker = new QFileDialog(nullptr, tr("Files to upload to %1").arg(sanitizedText(guard->title(), 100)));
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setAcceptMode(QFileDialog::AcceptOpen);
        picker->setFileMode(directory ? QFileDialog::Directory : QFileDialog::ExistingFiles);
        picker->setDirectory(QDir::homePath());
        keepDialogOverPane(picker, guard);
        // Answered once: the files, or a cancel when it closes without them.
        auto answered = std::make_shared<bool>(false);
        connect(picker, &QFileDialog::filesSelected, picker, [guard, answered, limit](const QStringList& paths) {
            *answered = true;
            if (guard) sendUpload(guard, paths, limit);
        });
        connect(picker, &QDialog::finished, picker, [guard, answered] {
            if (!*answered && guard) guard->answerUpload(std::nullopt);
        });
        connect(guard, &Pane::uploadCancelled, picker, &QDialog::reject);
        connect(guard, &QObject::destroyed, picker, &QDialog::reject);
        picker->open();
    });
    // The command that asked ended: no answer goes, and no question stays.
    connect(pane, &Pane::uploadCancelled, dialog, &QDialog::reject);
    connect(pane, &QObject::destroyed, dialog, &QDialog::reject);
    dialog->open();
    if (!isActive()) alert(0);
}

void TerminalWindow::sendUpload(Pane* pane, const QStringList& paths, std::size_t limit)
{
    struct Made
    {
        std::optional<std::string> answer;
        QString error;
    };
    // The pane's: it answers in whichever window shows the pane then, and
    // goes with the pane, which no longer waits.
    auto* watcher = new QFutureWatcher<Made>(pane);
    connect(watcher, &QFutureWatcherBase::finished, watcher, [watcher, pane] {
        const Made made = watcher->result();
        watcher->deleteLater();
        if (!made.answer) {
            pane->answerUpload(std::nullopt);
            NotificationCenter::instance().showNotice(pane, tr("Upload refused"), made.error);
        } else if (!pane->answerUpload(made.answer)) {
            NotificationCenter::instance().showNotice(pane, tr("Upload refused"),
                                                      tr("The files did not go to the program. Its input was full, and it got "
                                                         "Ctrl+C, or the command that asked for them ended."));
        }
    });
    watcher->setFuture(QtConcurrent::run([paths, limit] {
        Made made;
        if (const auto archive = UploadArchive::build(paths, limit, made.error)) made.answer = UploadArchive::answer(*archive);
        return made;
    }));
}

void TerminalWindow::saveDownloadAs(Pane* pane, const QString& directory, const QString& name, quint64 serial)
{
    auto* dialog = new QFileDialog(nullptr, tr("Save file received from %1").arg(sanitizedText(pane->title(), 100)));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setDirectory(directory);
    dialog->selectFile(name);
    keepDialogOverPane(dialog, pane);
    const QPointer<Pane> guard(pane);
    // Answered once: a chosen file, or a refusal when the dialog closes
    // without one.
    auto answered = std::make_shared<bool>(false);
    // The dialog asked before it replaces a file: the download replaces it.
    connect(dialog, &QFileDialog::fileSelected, dialog, [guard, answered, serial](const QString& path) {
        *answered = true;
        if (guard) guard->answerDownload(path, serial, true);
    });
    connect(dialog, &QDialog::finished, dialog, [guard, answered, serial] {
        if (!*answered && guard) guard->answerDownload({}, serial);
    });
    connect(pane, &Pane::downloadCancelled, dialog, &QDialog::reject);
    connect(pane, &QObject::destroyed, dialog, &QDialog::reject);
    dialog->open();
}

void TerminalWindow::askFileTransfer(Pane* pane, const QString& token, bool writing, const QStringList& paths, int total,
                                     bool othersRefused)
{
    // Asked by the transfer itself, so the worker exists already.
    const QPointer<FileTransfer> guard = pane->fileTransfers();
    // A request that the pane sent before its tab or its overlay closed:
    // nobody is left to answer it.
    if (!takesInput(pane)) {
        guard->approve(token, false);
        return;
    }
    // The dialog keeps the paths that it shows: a Yes approves those, and
    // the files that come later get a dialog of their own, unless the
    // worker refuses them.
    const FileTransfer::Question question = FileTransfer::question(writing, paths, total, othersRefused, clipboardAsker(pane));
    QMessageBox* dialog = paneQuestion(tr("File transfer"), question.text, QMessageBox::Yes | QMessageBox::No);
    dialog->setDefaultButton(QMessageBox::No);
    if (!question.detail.isEmpty()) dialog->setDetailedText(question.detail);
    // This window closes. A pane that moved to another window still asks, over that one.
    keepDialogOverPane(dialog, pane);
    connect(dialog, &QDialog::finished, dialog, [guard, token](int result) {
        if (guard) guard->approve(token, result == QMessageBox::Yes);
    });
    if (guard) {
        connect(guard, &QObject::destroyed, dialog, &QDialog::reject);
        connect(guard, &FileTransfer::sessionClosed, dialog, [dialog, token](const QString& closed) { if (closed == token) dialog->reject(); });
    }
    dialog->open();
}

bool TerminalWindow::showOutputInPager(Pane* pane, std::optional<std::uint64_t> region)
{
    // Handled from here on: a failure is a notification, not a key for the program.
    const auto fail = [pane](const QString& title, const QString& body) {
        NotificationCenter::instance().showNotice(pane, title, body);
        return true;
    };
    // The pager would run on the host of the session, where the file of
    // this computer is not.
    if (m_session && m_session->remote()) {
        return fail(tr("Could not show the output"),
                    tr("Session %1 runs on another host, and a pane there cannot read a file of this computer.").arg(m_session->session()));
    }
    auto command = QProcess::splitCommand(m_settings.settings().pager);
    if (command.isEmpty() || QStandardPaths::findExecutable(command.front()).isEmpty()) {
        return fail(tr("No pager"), command.isEmpty() ? tr("Terminal.pager is empty.")
                                                      : tr("The pager %1 was not found.").arg(command.front()));
    }
    const std::size_t limit = m_settings.settings().pagerOutputLimit;
    // The last output, or a command's output region: the first on the
    // screen, the one last visited, or the one under the pointer.
    const auto output = region ? pane->readRegion(*region, limit) : pane->readText(AfterglowVt::Terminal::TextScope::LastOutput, limit);
    if (!region && pane->lastOutputArriving()) {
        return fail(tr("No command output"), tr("The output of the command is still on its way from the session. Try again in a moment."));
    }
    if (!output.value(QStringLiteral("available")).toBool()) {
        // A session pane's region can be on its way from the daemon, as the last output can.
        if (region && pane->regionArriving(*region)) {
            return fail(tr("No command output"), tr("The output of the command is still on its way from the session. Try again in a moment."));
        }
        return fail(tr("No command output"), region ? tr("The output of the command is no longer in the history.")
                                                    : tr("Shell integration has not recorded a command yet."));
    }
    auto file = std::make_unique<QTemporaryFile>();
    const QByteArray bytes = output.value(QStringLiteral("text")).toString().toUtf8();
    if (!file->open() || file->write(bytes) != bytes.size() || !file->flush()) {
        return fail(tr("Could not show the output"), tr("The temporary file could not be written."));
    }
    if (output.value(QStringLiteral("truncated")).toBool()) {
        file->write(tr("\n[Afterglow: output limited to %1]\n").arg(QLocale().formattedDataSize(qint64(limit))).toUtf8());
    }
    const QString path = file->fileName();
    file->close();
    const QString pager = launchedProgramName(command);
    if (m_session) {
        // The pager runs under the daemon, and can outlive this window and
        // Afterglow. A shell opens the file, removes it, and gives the
        // pager the open file (/dev/fd/4): nothing is left whatever ends
        // the pager. A trap could not do it: the end of a session hangs
        // up only the shell, which runs no trap while it waits for the
        // pager, and the shell gets SIGKILL soon after. The pager gets
        // the file through a redirection of its own command, which every
        // sh passes on (ksh closes what a bare exec opened). The path and
        // the pager's words go as arguments, never into the script.
        command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                              QStringLiteral("exec 3<\"$1\"; rm -f -- \"$1\"; shift; exec \"$@\" /dev/fd/4 4<&3 3<&-"),
                              QStringLiteral("sh"), path}
            + command;
    } else {
        command << path;
    }
    QString error;
    // A project saves the tab as a shell in its directory, not as a
    // pager of a file that is gone. The daemon of a session does not
    // keep the command either, and names the pager as the pane's job.
    m_unsavedLaunch = pager;
    Pane* created = newTab(focusedDirectory(), command, {}, &error);
    m_unsavedLaunch.reset();
    if (!created) return fail(tr("Could not show the output"), error);
    created->setLaunch(created->launchDirectory(), {});
    // The file goes with the pane, whose program ends with it. In a
    // session the shell of the pane removes it once it is open, and the
    // pager outlives the pane: the pane's close removes it still when that
    // shell never ran.
    file->setParent(created);
    file.release();
    return true;
}

} // namespace Afterglow
