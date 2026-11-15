// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OverlayInputs.h"

#include <QRegularExpression>

namespace Afterglow {

bool OverlayInputs::validName(const QString& name)
{
    static const QRegularExpression rule(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return rule.match(name).hasMatch();
}

QString OverlayInputs::add(const QString& owner, const QString& name, const QString& chunk, bool eof)
{
    if (!validName(name)) return QStringLiteral("an input name is 1 to 64 letters, digits, _ and -");
    // Looked up, not made: a refused first part leaves no books behind.
    const auto books = m_inputs.constFind(owner);
    const qsizetype waitingInputs = books == m_inputs.cend() ? 0 : books->size();
    const auto known = books == m_inputs.cend() ? nullptr : &*books;
    const auto entry = known ? known->constFind(name) : QHash<QString, Waiting>::const_iterator();
    const bool exists = known && entry != known->cend();
    if (!exists && waitingInputs >= MaxWaiting) return QStringLiteral("%1 inputs wait for their overlay already").arg(MaxWaiting);
    if (exists && entry->eof) return QStringLiteral("the input %1 has ended").arg(name);
    const QByteArray bytes = chunk.toUtf8();
    const qsizetype size = exists ? entry->bytes.size() : 0;
    if (size + bytes.size() > OverlaySpec::MaxInput) {
        drop(owner, name);
        return QStringLiteral("the overlay's stdin is over %1 MiB").arg(OverlaySpec::MaxInput >> 20);
    }
    if (m_bytes + bytes.size() > MaxWaitingBytes)
        return QStringLiteral("afterglow keeps at most %1 MiB of input for overlays that have not started").arg(MaxWaitingBytes >> 20);
    Waiting& waiting = m_inputs[owner][name];
    waiting.bytes += bytes;
    waiting.eof = eof;
    m_bytes += bytes.size();
    return {};
}

QString OverlayInputs::resolve(const QString& owner, QVariantMap& options)
{
    if (!options.contains(QStringLiteral("input"))) return {};
    const QString name = options.take(QStringLiteral("input")).toString();
    QByteArray bytes;
    bool complete = false;
    if (const auto inputs = m_inputs.constFind(owner); inputs != m_inputs.cend()) {
        if (const auto input = inputs->constFind(name); input != inputs->cend() && input->eof) {
            bytes = input->bytes;
            complete = true;
        }
    }
    drop(owner, name);
    if (options.contains(QStringLiteral("stdin"))) return QStringLiteral("the overlay options stdin and input exclude each other");
    if (!complete) return QStringLiteral("no complete input %1 on this connection").arg(name);
    options.insert(QStringLiteral("stdin"), QString::fromUtf8(bytes));
    return {};
}

void OverlayInputs::drop(const QString& owner, const QString& name)
{
    const auto inputs = m_inputs.find(owner);
    if (inputs == m_inputs.end()) return;
    m_bytes -= inputs->take(name).bytes.size();
    if (inputs->isEmpty()) m_inputs.erase(inputs);
}

void OverlayInputs::drop(const QString& owner)
{
    for (const Waiting& input : m_inputs.take(owner)) m_bytes -= input.bytes.size();
}

void OverlayInputs::dropPrefix(const QString& prefix)
{
    for (auto it = m_inputs.begin(); it != m_inputs.end();) {
        if (!it.key().startsWith(prefix)) {
            ++it;
            continue;
        }
        for (const Waiting& input : *it) m_bytes -= input.bytes.size();
        it = m_inputs.erase(it);
    }
}

void OverlayInputs::clear()
{
    m_inputs.clear();
    m_bytes = 0;
}

} // namespace Afterglow
