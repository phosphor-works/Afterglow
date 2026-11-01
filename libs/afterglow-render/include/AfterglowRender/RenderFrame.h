// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <rhi/qrhi.h>
#include <QRect>
#include <QSize>
#include <functional>
#include <utility>

namespace AfterglowRender {

/// @p rect (top-left origin) as a QRhi scissor, which has a bottom-left
/// origin, clipped to @p target.
inline QRhiScissor scissorFor(const QRect& rect, const QSize& target)
{
    const QRect c = rect.intersected(QRect(QPoint(0, 0), target));
    return QRhiScissor(c.x(), target.height() - (c.y() + c.height()), c.width(), c.height());
}

// A frame can fail while a shader pipeline or texture is being created.
// Close its active pass before ending the frame, including during unwinding.
// An update batch staged for the next pass is submitted then too: its uploads
// are already counted as done (atlas dirty rectangles, new image textures).
class RenderFrame
{
public:
    RenderFrame(QRhiCommandBuffer* commands, std::function<void()> end)
        : m_commands(commands), m_end(std::move(end)) {}
    ~RenderFrame() { finish(); }
    RenderFrame(const RenderFrame&) = delete;
    RenderFrame& operator=(const RenderFrame&) = delete;

    QRhiCommandBuffer* commands() const { return m_commands; }
    /// Holds @p updates until a pass begins with it, and submits it if the
    /// frame ends first. Returns @p updates; one batch is held at a time.
    QRhiResourceUpdateBatch* stage(QRhiResourceUpdateBatch* updates)
    {
        if (m_staged && m_staged != updates) {
            // Keep the earlier batch's work, in order, ahead of the new one.
            m_staged->merge(updates);
            updates->release();
            return m_staged;
        }
        m_staged = updates;
        return updates;
    }
    void beginPass(QRhiRenderTarget* target, const QColor& clear,
                   const QRhiDepthStencilClearValue& depth,
                   QRhiResourceUpdateBatch* updates = nullptr)
    {
        if (updates == m_staged) m_staged = nullptr;
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
        if (m_staged) m_commands->resourceUpdate(std::exchange(m_staged, nullptr));
        m_end();
    }

private:
    QRhiCommandBuffer* m_commands;
    std::function<void()> m_end;
    QRhiResourceUpdateBatch* m_staged = nullptr;
    bool m_passActive = false;
    bool m_finished = false;
};

} // namespace AfterglowRender
