// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Transfer.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The client side of a file transfer session (OSC 5113), as kitten transfer
/// runs it: the file listing, the ac= sequences, compression and deltas.
///
/// A session does no terminal I/O. The caller writes what pull() gives to the
/// terminal, and passes each reply of the terminal to receive(). The files
/// are read and written here, with POSIX calls. Paths that start with ~/
/// name the home directory on their own computer.
namespace AfterglowProto {

struct TransferClientOptions
{
    enum class Mode : std::uint8_t {
        Normal, ///< the last argument is the destination
        Mirror, ///< every argument is a source, copied to the same path on the other computer
    };
    enum class Compress : std::uint8_t { Auto, Never, Always };
    Mode mode = Mode::Normal;
    Compress compress = Compress::Auto;
    /// Send deltas against the files that exist at the destination (tt=rsync).
    bool deltas = false;
    /// Stop in State::Confirm before any file data moves.
    bool confirmPaths = false;
    /// The password that skips the terminal's prompt, sent as its SHA-256
    /// form (transferPasswordHash()). Empty: the terminal asks the user.
    std::string password;
    /// This computer's home directory and working directory. Relative paths
    /// and ~/ resolve against them.
    std::string home;
    std::string cwd;
};

/// One file, directory or link of a session.
struct TransferItem
{
    enum class Type : std::uint8_t { Regular, Directory, Symlink, Link };
    Type type = Type::Regular;
    /// The path on this computer, as the user named it (send) or as it is
    /// written (receive).
    std::string local;
    /// The path on the terminal's computer.
    std::string remote;
    std::uint64_t size = 0;
    /// Bytes that the terminal has written (send) or that were written here (receive).
    std::uint64_t done = 0;
    bool finished = false;
    /// The terminal's status or the local error; empty on success.
    std::string error;
    /// The file goes as a delta.
    bool delta = false;
};

class TransferClient
{
public:
    enum class State : std::uint8_t {
        Asking, ///< waiting for the terminal (the permission, a listing)
        Confirm, ///< options.confirmPaths: waiting for confirm()
        Running,
        Done, ///< the session finished; items() tells which files failed
        Failed, ///< the session ended with error()
        Canceling, ///< a cancel was sent; waiting for CANCELED
        Canceled,
    };

    virtual ~TransferClient();
    TransferClient(const TransferClient&) = delete;
    TransferClient& operator=(const TransferClient&) = delete;

    const std::string& id() const { return m_id; }
    State state() const { return m_state; }
    /// Done, Failed or Canceled, and nothing is left to send.
    bool over() const;
    /// Why the session failed or was canceled.
    const std::string& error() const { return m_error; }
    const std::vector<TransferItem>& items() const { return m_items; }
    /// Bytes of signatures and deltas that crossed the terminal, and the size
    /// of the files that went as deltas.
    std::uint64_t signatureBytes() const { return m_signatureBytes; }
    std::uint64_t deltaBytes() const { return m_deltaBytes; }
    std::uint64_t deltaFileBytes() const { return m_deltaFileBytes; }

    /// Reads one reply of the terminal: the text after "5113;". Replies of
    /// other sessions are ignored.
    void receive(std::string_view payload);
    /// Appends complete OSC sequences to @p out, until it holds about
    /// @p budget more bytes or nothing is ready. File data is read here, so
    /// the caller bounds the memory and the work of one call.
    void pull(std::string& out, std::size_t budget);
    /// The answer in State::Confirm: go on, or cancel the session.
    void confirm(bool proceed);
    /// Asks the terminal to end the session. It ends when the terminal
    /// answers CANCELED; a caller that waits no longer may stop earlier.
    void cancel(std::string_view reason = {});

protected:
    TransferClient(TransferClientOptions options, std::string id);
    void queue(std::string message) { m_output += std::move(message); }
    void fail(std::string error);
    virtual void onReply(const TransferFields& reply) = 0;
    virtual void produce(std::string& out, std::size_t budget) = 0;
    virtual void proceed() = 0;

    TransferClientOptions m_options;
    std::string m_id;
    State m_state = State::Asking;
    std::string m_error;
    std::string m_output;
    std::vector<TransferItem> m_items;
    std::uint64_t m_signatureBytes = 0;
    std::uint64_t m_deltaBytes = 0;
    std::uint64_t m_deltaFileBytes = 0;
};

/// Copies files from this computer to the terminal's computer (ac=send).
/// @p args are the paths as kitten transfer takes them. nullptr, with
/// @p error set, when a source cannot be read or the arguments are wrong.
std::unique_ptr<TransferClient> makeTransferSender(const TransferClientOptions& options,
                                                   const std::vector<std::string>& args, std::string& error);
/// Copies files from the terminal's computer to this one (ac=receive).
std::unique_ptr<TransferClient> makeTransferReceiver(const TransferClientOptions& options,
                                                     const std::vector<std::string>& args, std::string& error);

/// Whether a file of @p path is worth compressing under @p strategy: Auto
/// leaves out archives, images and video, as kitten transfer does.
bool transferShouldCompress(std::string_view path, TransferClientOptions::Compress strategy);

/// Path helpers with the meaning of Go's path/filepath, which kitten
/// transfer uses to map paths: clean() folds "." and ".." and repeated
/// slashes, baseName() and dirName() split the cleaned path.
namespace TransferPath {
std::string clean(std::string_view path);
std::string join(std::string_view a, std::string_view b);
std::string baseName(std::string_view path);
std::string dirName(std::string_view path);
/// @p target relative to @p base, both absolute and clean.
std::string relative(std::string_view base, std::string_view target);
/// ~ and ~/x under @p home; a relative path under @p cwd.
std::string absolute(std::string_view path, std::string_view home, std::string_view cwd);
} // namespace TransferPath

} // namespace AfterglowProto
