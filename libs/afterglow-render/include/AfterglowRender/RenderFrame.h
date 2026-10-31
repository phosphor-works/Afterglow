// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <rhi/qrhi.h>
#include <functional>
#include <utility>

namespace AfterglowRender {

// A frame can fail while a shader pipeline or texture is being created.
// Close its active pass before ending the frame, including during unwinding.
class RenderFrame
{
public:
    RenderFrame(QRhiCommandBuffer* commands, std::function<void()> end)
        : m_commands(commands), m_end(std::move(end)) {}
    ~RenderFrame() { finish(); }
    RenderFrame(const RenderFrame&) = delete;
    RenderFrame& operator=(const RenderFrame&) = delete;

    QRhiCommandBuffer* commands() const { return m_commands; }
    void beginPass(QRhiRenderTarget* target, const QColor& clear,
                   const QRhiDepthStencilClearValue& depth,
                   QRhiResourceUpdateBatch* updates = nullptr)
    {
        m_commands->beginPass(target, clear, depth, updates);
        m_passActive = true;
    }
    void endPass(QRhiResourceUpdateBatch* updates = nullptr)
    {
        m_commands->endPass(updates);
        m_passActive = false;
    }
    void finish()
    {
        if (std::exchange(m_finished, true)) return;
        if (m_passActive) endPass();
        m_end();
    }

private:
    QRhiCommandBuffer* m_commands;
    std::function<void()> m_end;
    bool m_passActive = false;
    bool m_finished = false;
};

} // namespace AfterglowRender
