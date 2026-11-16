// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProgramImages.h"

#include "shell/Sibling.h"

#include <ImageDecoder.h>

#include <QLoggingCategory>

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
    if (executable.empty() || ::access(executable.c_str(), X_OK) != 0) {
        qCWarning(lcProgramImages).noquote() << "no image decoder:"
            << (error.empty() ? QString::fromStdString(executable) + QStringLiteral(" is not there") : QString::fromStdString(error))
            << "- the images that programs send do not show";
        return nullptr;
    }
    // As the daemon's: its reports name the program that decodes.
    g_decoder = std::make_unique<AfterglowMux::ImageDecoder>(executable, AfterglowMux::ImageDecoder::WaitMs, "afterglow");
    return g_decoder.get();
}

void setServerExecutable(std::string path)
{
    std::lock_guard lock(g_mutex);
    g_executable = std::move(path);
}

} // namespace Afterglow::ProgramImages
