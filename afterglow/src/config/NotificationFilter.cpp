// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "NotificationFilter.h"

#include <QRegularExpression>

#include <algorithm>
#include <vector>

namespace Afterglow {

struct NotificationFilter::Node
{
    enum class Kind {
        All,
        Term,
        Not,
        And,
        Or
    };
    enum class Field {
        Title,
        Body,
        App,
        Type
    };
    Kind kind = Kind::All;
    Field field = Field::Title;
    QRegularExpression pattern;
    std::vector<std::shared_ptr<const Node>> children;
};

namespace {

using Node = NotificationFilter::Node;

// Parentheses deeper than this are a mistake, and would use the stack up.
// Nothing else nests: a chain of and or or is one node, and a chain of not
// one at most.
constexpr int MaxDepth = 32;

struct Token
{
    enum class Kind {
        Open,
        Close,
        Word,
        Term,
        End
    };
    Kind kind = Kind::End;
    QString word; ///< a keyword, or the field of a term
    QString value; ///< the pattern of a term
};

class Parser
{
public:
    explicit Parser(const QString& text)
        : m_text(text)
    {
    }

    std::shared_ptr<const Node> parse(QString* error)
    {
        m_token = next();
        auto root = orExpression(0);
        if (root && m_token.kind != Token::Kind::End)
            fail(QStringLiteral("unexpected text after the rule"));
        if (!m_error.isEmpty()) {
            if (error)
                *error = m_error;
            return nullptr;
        }
        return root;
    }

private:
    void fail(const QString& message)
    {
        if (m_error.isEmpty())
            m_error = message;
    }

    Token next()
    {
        while (m_at < m_text.size() && m_text.at(m_at).isSpace())
            ++m_at;
        if (m_at >= m_text.size())
            return {};
        const QChar c = m_text.at(m_at);
        if (c == QLatin1Char('(')) {
            ++m_at;
            return {Token::Kind::Open, {}, {}};
        }
        if (c == QLatin1Char(')')) {
            ++m_at;
            return {Token::Kind::Close, {}, {}};
        }
        const qsizetype start = m_at;
        while (m_at < m_text.size() && !m_text.at(m_at).isSpace() && m_text.at(m_at) != QLatin1Char(':')
               && m_text.at(m_at) != QLatin1Char('(') && m_text.at(m_at) != QLatin1Char(')'))
            ++m_at;
        // Keywords and fields in any case, as kitty reads them.
        const QString word = m_text.mid(start, m_at - start).toLower();
        if (m_at >= m_text.size() || m_text.at(m_at) != QLatin1Char(':'))
            return {Token::Kind::Word, word, {}};
        ++m_at; // the colon
        // kitty takes a quoted value after white space too: title: "a b".
        qsizetype quote = m_at;
        while (quote < m_text.size() && m_text.at(quote).isSpace())
            ++quote;
        if (quote > m_at && quote < m_text.size() && m_text.at(quote) == QLatin1Char('"'))
            m_at = quote;
        return {Token::Kind::Term, word, value()};
    }

    /// A quoted value, where \" and \\ stand for themselves, or the text up
    /// to a space or to a parenthesis that closes a group of the rule.
    QString value()
    {
        QString out;
        if (m_at < m_text.size() && m_text.at(m_at) == QLatin1Char('"')) {
            ++m_at;
            while (m_at < m_text.size() && m_text.at(m_at) != QLatin1Char('"')) {
                if (m_text.at(m_at) == QLatin1Char('\\') && m_at + 1 < m_text.size()
                    && (m_text.at(m_at + 1) == QLatin1Char('"') || m_text.at(m_at + 1) == QLatin1Char('\\'))) {
                    ++m_at;
                }
                out += m_text.at(m_at++);
            }
            if (m_at >= m_text.size())
                fail(QStringLiteral("a quote is not closed"));
            else
                ++m_at;
            return out;
        }
        int depth = 0;
        while (m_at < m_text.size() && !m_text.at(m_at).isSpace()) {
            const QChar c = m_text.at(m_at);
            if (c == QLatin1Char('('))
                ++depth;
            if (c == QLatin1Char(')') && depth-- == 0)
                break;
            out += c;
            ++m_at;
        }
        return out;
    }

    /// One node for a whole chain of @p kind, so that a long rule neither
    /// nests nor recurses.
    static std::shared_ptr<const Node> join(Node::Kind kind, std::vector<std::shared_ptr<const Node>> children)
    {
        if (children.size() == 1)
            return std::move(children.front());
        auto node = std::make_shared<Node>();
        node->kind = kind;
        node->children = std::move(children);
        return node;
    }

    bool isWord(const char* word) const
    {
        return m_token.kind == Token::Kind::Word && m_token.word == QLatin1String(word);
    }

    std::shared_ptr<const Node> orExpression(int depth)
    {
        std::vector<std::shared_ptr<const Node>> terms;
        terms.push_back(andExpression(depth));
        while (terms.back() && isWord("or")) {
            m_token = next();
            terms.push_back(andExpression(depth));
        }
        return terms.back() ? join(Node::Kind::Or, std::move(terms)) : nullptr;
    }

    std::shared_ptr<const Node> andExpression(int depth)
    {
        std::vector<std::shared_ptr<const Node>> terms;
        terms.push_back(notExpression(depth));
        while (terms.back()) {
            // Terms side by side join with and, as in kitty's queries.
            if (isWord("and"))
                m_token = next();
            else if (m_token.kind == Token::Kind::End || m_token.kind == Token::Kind::Close || isWord("or"))
                break;
            terms.push_back(notExpression(depth));
        }
        return terms.back() ? join(Node::Kind::And, std::move(terms)) : nullptr;
    }

    std::shared_ptr<const Node> notExpression(int depth)
    {
        // A chain of not, read in a loop: two cancel out.
        bool negated = false;
        while (isWord("not")) {
            m_token = next();
            negated = !negated;
        }
        auto operand = primary(depth);
        if (!operand || !negated)
            return operand;
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Not;
        node->children = {std::move(operand)};
        return node;
    }

    std::shared_ptr<const Node> primary(int depth)
    {
        if (depth > MaxDepth) {
            fail(QStringLiteral("the rule nests more than %1 levels").arg(MaxDepth));
            return nullptr;
        }
        switch (m_token.kind) {
        case Token::Kind::Open: {
            m_token = next();
            auto inner = orExpression(depth + 1);
            if (!inner)
                return nullptr;
            if (m_token.kind != Token::Kind::Close) {
                fail(QStringLiteral("a parenthesis is not closed"));
                return nullptr;
            }
            m_token = next();
            return inner;
        }
        case Token::Kind::Word:
            if (m_token.word == QLatin1String("all")) {
                m_token = next();
                return std::make_shared<Node>();
            }
            fail(QStringLiteral("\"%1\" is not a term: use title:, body:, app: or type:, or all").arg(m_token.word));
            return nullptr;
        case Token::Kind::Term: {
            auto node = std::make_shared<Node>();
            node->kind = Node::Kind::Term;
            const QString& field = m_token.word;
            if (field == QLatin1String("title"))
                node->field = Node::Field::Title;
            else if (field == QLatin1String("body"))
                node->field = Node::Field::Body;
            else if (field == QLatin1String("app"))
                node->field = Node::Field::App;
            else if (field == QLatin1String("type"))
                node->field = Node::Field::Type;
            else {
                fail(QStringLiteral("\"%1\" is not a field: use title, body, app or type").arg(field));
                return nullptr;
            }
            if (m_token.value.isEmpty()) {
                fail(QStringLiteral("%1: has no pattern").arg(field));
                return nullptr;
            }
            // \w, \d and \b of all scripts, as in kitty's Python re.
            node->pattern = QRegularExpression(m_token.value, QRegularExpression::UseUnicodePropertiesOption);
            if (!node->pattern.isValid()) {
                fail(QStringLiteral("%1 is not a regular expression: %2")
                         .arg(m_token.value, node->pattern.errorString()));
                return nullptr;
            }
            m_token = next();
            return node;
        }
        case Token::Kind::Close:
            fail(QStringLiteral("a parenthesis closes no group"));
            return nullptr;
        case Token::Kind::End:
            fail(QStringLiteral("the rule ends where a term should be"));
            return nullptr;
        }
        return nullptr;
    }

    const QString& m_text;
    qsizetype m_at = 0;
    Token m_token;
    QString m_error;
};

bool evaluate(const Node& node, const NotificationFilter::Fields& fields)
{
    switch (node.kind) {
    case Node::Kind::All:
        return true;
    case Node::Kind::Not:
        return !evaluate(*node.children[0], fields);
    case Node::Kind::And:
        return std::all_of(node.children.begin(), node.children.end(), [&](const auto& child) {
            return evaluate(*child, fields);
        });
    case Node::Kind::Or:
        return std::any_of(node.children.begin(), node.children.end(), [&](const auto& child) {
            return evaluate(*child, fields);
        });
    case Node::Kind::Term:
        break;
    }
    // A search anywhere in the field, as kitty's (Python's re.search).
    switch (node.field) {
    case Node::Field::Title:
        return node.pattern.match(fields.title).hasMatch();
    case Node::Field::Body:
        return node.pattern.match(fields.body).hasMatch();
    case Node::Field::App:
        return node.pattern.match(fields.app).hasMatch();
    case Node::Field::Type:
        for (const QString& type : fields.types) {
            if (node.pattern.match(type).hasMatch())
                return true;
        }
        return false;
    }
    return false;
}

} // namespace

std::optional<NotificationFilter> NotificationFilter::parse(const QString& text, QString* error)
{
    Parser parser(text);
    auto root = parser.parse(error);
    if (!root)
        return std::nullopt;
    NotificationFilter filter;
    filter.m_text = text;
    filter.m_root = std::move(root);
    return filter;
}

bool NotificationFilter::matches(const Fields& fields) const
{
    return m_root && evaluate(*m_root, fields);
}

} // namespace Afterglow
