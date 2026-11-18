// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProgramImages.h"

#include "shell/Sibling.h"

#include <ImageDecoder.h>

#include <QLoggingCategory>

#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>

#include <unistd.h>

Q_LOGGING_CATEGORY(lcProgramImages, "afterglow.images", QtInfoMsg)

namespace Afterglow::ProgramImages {

namespace {

std::mutex g_mutex;
std::string g_executable;
std::unique_ptr<AfterglowMux::ImageDecoder> g_decoder;
bool g_tried = false;

} // namespace

AfterglowMux::ImageDecoder* decoder()
{
    std::lock_guard lock(g_mutex);
    if (g_tried) return g_decoder.get();
    g_tried = true;
    std::string error;
    const std::string executable = g_executable.empty() ? Aglow::siblingExecutable("aglow-mux", &error) : g_executable;
    if (error.empty() && executable.empty()) error = "no aglow-mux";
    // Why it cannot run: missing, or there and not executable.
    if (error.empty() && ::access(executable.c_str(), X_OK) != 0) error = executable + ": " + std::strerror(errno);
    if (!error.empty()) {
        qCWarning(lcProgramImages).noquote() << "no image decoder:" << QString::fromStdString(error)
                                             << "- the images that programs send do not show";
        return nullptr;
    }
    // The reports of the decoder's client go to the log, as the window's
    // other warnings.
    g_decoder = std::make_unique<AfterglowMux::ImageDecoder>(executable, AfterglowMux::ImageDecoder::WaitMs, [](const std::string& line) {
        qCWarning(lcProgramImages).noquote() << QString::fromStdString(line);
    });
    return g_decoder.get();
}

void setServerExecutable(std::string path)
{
    std::lock_guard lock(g_mutex);
    g_executable = std::move(path);
}

} // namespace Afterglow::ProgramImages
