// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace Afterglow {

/// A rule of Notifications.filters, as kitty's filter_notification: terms
/// field:regexp on the title, body, app and type of a notification, joined
/// with and, or, not and parentheses, or the word all. Terms side by side
/// join with and. Keywords and fields are in any case. A notification that a
/// rule matches is not shown.
class NotificationFilter
{
public:
    /// What a rule can match: an OSC 9 or OSC 777 notification has no app
    /// and no types.
    struct Fields
    {
        QString title;
        QString body;
        QString app;
        QStringList types;
    };

    /// The rule of @p text, or nullopt with the reason in @p error.
    static std::optional<NotificationFilter> parse(const QString& text, QString* error);

    bool matches(const Fields& fields) const;
    const QString& text() const
    {
        return m_text;
    }
    /// Two rules of one text are the same rule.
    bool operator==(const NotificationFilter& other) const
    {
        return m_text == other.m_text;
    }

    struct Node;

private:
    QString m_text;
    std::shared_ptr<const Node> m_root;
};

} // namespace Afterglow
