// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Delta.h>
#include <AfterglowProto/Fd.h>
#include <AfterglowProto/FdDelta.h>
#include <AfterglowProto/Random.h>
#include <AfterglowProto/TransferClient.h>
#include <AfterglowProto/TransferPath.h>
#include <AfterglowProto/Zlib.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace AfterglowProto {

namespace {

using Type = TransferItem::Type;
using Fields = std::vector<std::pair<std::string_view, std::string_view>>;

// The most file data in one data message, before base64 (kitty's chunk).
constexpr std::size_t PacketData = TransferMaxData;
// Raw bytes read from a file for one step of compression or of a delta.
constexpr std::size_t ReadStep = 64 * 1024;
// Bytes of a delta scanned in one step: a refill reads up to two blocks more.
constexpr std::size_t DeltaStep = 1024 * 1024;
// Bytes that one pull() reads from files for deltas and signatures. A delta
// of a file that did not change makes almost no output, so the output
// budget alone would let one call read the whole file.
constexpr std::size_t WorkStep = 16 * 1024 * 1024;
// Replies that may wait while a delta is applied in steps. Each step reads
// at most one batch of the terminal's output, so the queue stays far below.
constexpr std::size_t MaxWaiting = 64 * 1024 * 1024;
// A signature of the largest block size for a file of 1 TiB is 20 MiB.
constexpr std::size_t MaxSignature = 64 * 1024 * 1024;
// One literal of a delta that the receiver buffers whole.
constexpr std::size_t MaxLiteral = 16 * 1024 * 1024;
// Afterglow opens a regular file at its first data, but holds the old file of
// a delta upload, and its signature, from the file command on. It refuses a
// file with EBUSY when 128 are open in one pane. A client keeps the delta
// uploads that wait well below it, and names its other files at once: a
// terminal that asks the user can list those in one question. A delta upload
// that it names when another finishes can get a question of its own. A receive has at
// most this many files requested and not finished at a time: the terminal
// holds each one open while it sends it, and this side its destination.
constexpr std::size_t MaxOpenFiles = 32;
constexpr std::size_t MaxListing = 1 << 20;
// The memory of a listing: the names and ids, and what each entry costs here.
// Afterglow sends at most 8 MiB.
constexpr std::size_t MaxListingBytes = 256 * 1024 * 1024;
// PATH_MAX. The terminal could not open a longer path either.
constexpr std::size_t MaxPath = 4096;
// Directory levels under a received root. A path of MaxPath bytes has fewer.
constexpr std::size_t MaxDepth = MaxPath / 2;
constexpr std::size_t MaxLinkTarget = 4096;
// Files of at most this size go whole and uncompressed, as kitten sends them.
constexpr std::uint64_t SmallFile = 4096;

std::string number(std::int64_t value)
{
    char buffer[24];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, end);
}

std::optional<std::int64_t> parseNumber(const TransferFields& f, const char* key)
{
    const auto it = f.find(key);
    if (it == f.end() || it->second.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto& text = it->second;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string field(const TransferFields& f, const char* key)
{
    const auto it = f.find(key);
    return it == f.end() ? std::string() : it->second;
}

std::string_view typeName(Type type)
{
    switch (type) {
    case Type::Directory:
        return "directory";
    case Type::Symlink:
        return "symlink";
    case Type::Link:
        return "link";
    case Type::Regular:
        break;
    }
    return "regular";
}

std::string errnoText(int error)
{
    return std::string(transferErrorName(error)) + ":" + std::strerror(error);
}

// The error of a status that names none: an empty st is not a success.
std::string statusError(const std::string& status)
{
    return status.empty() ? "EIO:the terminal sent an empty status" : status;
}

// The modification time of @p st in nanoseconds, if it fits in 64 bits.
std::optional<std::int64_t> nanoseconds(const struct stat& st)
{
    std::int64_t ns = 0;
    if (__builtin_mul_overflow(std::int64_t(st.st_mtim.tv_sec), std::int64_t(1000000000), &ns)
        || __builtin_add_overflow(ns, std::int64_t(st.st_mtim.tv_nsec), &ns)) {
        return std::nullopt;
    }
    return ns;
}

// Access and modification times for futimens() and utimensat(): both @p ns.
struct Times
{
    timespec values[2];
    explicit Times(std::int64_t ns)
    {
        timespec time { time_t(ns / 1000000000), long(ns % 1000000000) };
        if (time.tv_nsec < 0) {
            time.tv_nsec += 1000000000;
            --time.tv_sec;
        }
        values[0] = values[1] = time;
    }
};

// Whether a file of @p path is worth compressing under @p strategy: Auto
// leaves out archives and the image, audio and video formats, as kitten
// transfer does.
bool shouldCompress(std::string_view path, TransferClientOptions::Compress strategy)
{
    if (strategy != TransferClientOptions::Compress::Auto) {
        return strategy == TransferClientOptions::Compress::Always;
    }
    const auto name = TransferPath::baseName(path);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return true;
    }
    std::string extension = name.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    static const std::set<std::string, std::less<>> compressed {
        "zip", "odt", "odp", "ods", "pptx", "docx", "xlsx", "epub", "jar", "apk", "gz", "tgz", "bz2", "xz", "txz",
        "zst", "lz", "lz4", "lzma", "7z", "rar", "svgz", "png", "jpg", "jpeg", "gif", "webp", "avif", "heic",
        "heif", "jxl", "bmp", "tif", "tiff", "ico", "mp4", "m4v", "mkv", "webm", "mov", "avi", "mpg", "mpeg",
        "mp3", "m4a", "ogg", "opus", "flac", "aac",
    };
    return !compressed.contains(extension);
}

// The children of a directory, sorted as Go's os.ReadDir sorts them.
bool listDirectory(const std::string& path, std::vector<std::string>& names, int& error)
{
    DIR* dir = ::opendir(path.c_str());
    if (!dir) {
        error = errno;
        return false;
    }
    errno = 0;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (name != "." && name != "..") {
            names.emplace_back(name);
        }
    }
    error = errno;
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return error == 0;
}

// mkdir -p: the directories that the user named get the mode of the umask.
bool makeDirectories(const std::string& path)
{
    struct stat st { };
    if (::stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode) || (errno = ENOTDIR, false);
    }
    if (errno != ENOENT) {
        return false;
    }
    const auto parent = TransferPath::dirName(path);
    if (parent != path && !makeDirectories(parent)) {
        return false;
    }
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

// The umask of the process. /proc reads it without a change: umask(2) can
// read it only by setting it, and a file that another thread of the
// process (a test's terminal, say) creates in between would get mode 0777.
mode_t processUmask()
{
    const int fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::string text;
        char buffer[4096];
        ssize_t n;
        while (text.size() < 65536 && ((n = ::read(fd, buffer, sizeof buffer)) > 0 || (n < 0 && errno == EINTR))) {
            text.append(buffer, std::size_t(std::max<ssize_t>(n, 0)));
        }
        ::close(fd);
        if (const auto at = text.find("\nUmask:"); at != std::string::npos) {
            auto begin = at + 7;
            while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
                ++begin;
            }
            unsigned value = 0;
            const auto [end, ec] = std::from_chars(text.data() + begin, text.data() + text.size(), value, 8);
            if (ec == std::errc() && end != text.data() + begin) {
                return mode_t(value & 0777);
            }
        }
    }
    // Without /proc. aglow has a single thread, so nothing creates a file
    // while the mask is 0.
    const mode_t mask = ::umask(0);
    ::umask(mask);
    return mask & 0777;
}

// Deflates or passes through the bytes of one file, and cuts them into data messages.
class Packer
{
public:
    explicit Packer(bool compress)
    {
        if (compress) {
            m_deflater = std::make_unique<ZlibStream>(ZlibStream::Mode::Deflate);
        }
    }
    bool valid() const { return !m_deflater || m_deflater->valid(); }
    bool add(std::string_view raw, bool last)
    {
        // Drop what went out only before more is added: less than one packet moves.
        m_ready.erase(0, m_sent);
        m_sent = 0;
        if (!m_deflater) {
            m_ready.append(raw);
            return true;
        }
        m_deflater->setInput(raw);
        char buffer[ReadStep];
        for (;;) {
            if (m_deflater->inputLeft() == 0 && !last) {
                return true;
            }
            std::size_t produced = 0;
            const auto result = m_deflater->step(buffer, sizeof buffer, produced, last);
            m_ready.append(buffer, produced);
            if (result == ZlibStream::Result::End) {
                return true;
            }
            if (result == ZlibStream::Result::Error) {
                return false;
            }
        }
    }
    std::string_view ready() const { return std::string_view(m_ready).substr(m_sent); }
    void consume(std::size_t count) { m_sent += count; }

private:
    std::unique_ptr<ZlibStream> m_deflater;
    std::string m_ready;
    std::size_t m_sent = 0;
};

} // namespace

TransferClient::TransferClient(TransferClientOptions options, std::string id)
    : m_options(std::move(options))
    , m_id(std::move(id))
{
}

TransferClient::~TransferClient() = default;

bool TransferClient::over() const
{
    return m_output.empty() && (m_state == State::Done || m_state == State::Failed || m_state == State::Canceled);
}

void TransferClient::start(std::string_view action, std::string_view count)
{
    Fields f { { "ac", action }, { "id", m_id } };
    if (!count.empty()) {
        f.emplace_back("sz", count);
    }
    std::string hash;
    if (!m_options.password.empty()) {
        hash = transferPasswordHash(m_id, m_options.password);
        f.emplace_back("pw", hash);
    }
    queue(encodeTransfer(f));
}

void TransferClient::fail(std::string error)
{
    if (m_error.empty()) {
        m_error = std::move(error);
    }
    if (m_state != State::Canceled) {
        m_state = State::Failed;
    }
}

void TransferClient::receive(std::string_view payload)
{
    // Work that a reply started goes first: later replies wait their turn,
    // in order, so that each call does a bounded amount of work.
    if (busy() || !m_waiting.empty()) {
        if (m_waitingBytes + payload.size() > MaxWaiting) {
            fail("The terminal sent more than " + number(std::int64_t(MaxWaiting >> 20))
                 + " MiB while aglow applied a delta");
            abandon();
            m_waiting.clear();
            m_waitingBytes = 0;
            return;
        }
        m_waitingBytes += payload.size();
        m_waiting.emplace_back(payload);
        m_hasWork = true;
        return;
    }
    handle(payload);
}

void TransferClient::handle(std::string_view payload)
{
    const auto decoded = decodeTransfer(payload);
    if (!decoded || field(*decoded, "id") != m_id) {
        return;
    }
    if (m_state == State::Done || m_state == State::Failed || m_state == State::Canceled) {
        return;
    }
    const auto& f = *decoded;
    // Only the session's own CANCELED ends it: one with a file id is that file's status.
    if (field(f, "ac") == "status" && field(f, "st") == TransferStatus::Canceled && field(f, "fid").empty()) {
        m_state = State::Canceled;
        return;
    }
    if (m_state == State::Canceling) {
        return;
    }
    onReply(f);
}

void TransferClient::pull(std::string& out, std::size_t budget)
{
    const std::size_t start = out.size();
    out += m_output;
    m_output.clear();
    m_hasWork = false;
    if (busy()) {
        if (m_state == State::Running) {
            resume();
        } else {
            abandon();
        }
    }
    while (!busy() && !m_waiting.empty()) {
        const std::string payload = std::move(m_waiting.front());
        m_waiting.pop_front();
        m_waitingBytes -= payload.size();
        handle(payload);
    }
    if (busy() || !m_waiting.empty()) {
        m_hasWork = true;
    } else if (m_state == State::Running && out.size() - start < budget) {
        produce(out, budget - (out.size() - start));
    }
    // A cancel that produce() queued goes out now, not after the next reply.
    out += m_output;
    m_output.clear();
}

void TransferClient::confirm(bool proceed)
{
    if (m_state != State::Confirm) {
        return;
    }
    if (!proceed) {
        cancel("Canceled by the user");
        return;
    }
    m_state = State::Running;
    this->proceed();
}

void TransferClient::cancel(std::string_view reason)
{
    if (m_state == State::Done || m_state == State::Failed || m_state == State::Canceled || m_state == State::Canceling) {
        return;
    }
    if (!reason.empty() && m_error.empty()) {
        m_error = std::string(reason);
    }
    const Fields f { { "ac", "cancel" }, { "id", m_id } };
    queue(encodeTransfer(f));
    m_state = State::Canceling;
}

namespace {

// ── Sending ─────────────────────────────────────────────────────────────────

class Sender final : public TransferClient
{
public:
    struct File
    {
        std::string fid;
        std::string path; // expanded local path
        struct stat st { };
        std::string linkData;
        // The file of this transfer that a link names.
        std::size_t linkTarget = SIZE_MAX;
        bool compress = false;
        bool rsyncCapable = false;
        enum class Phase : std::uint8_t { Unsent, Waiting, Signature, Ready, Sending, Sent, Acked } phase = Phase::Unsent;
        bool metadataSent = false;
        UniqueFd fd;
        std::string signature;
        // Bytes of signature that this file holds against options.signatureMemory.
        std::size_t signatureHeld = 0;
        std::unique_ptr<FdSource> source;
        std::unique_ptr<DeltaGenerator> generator;
        std::unique_ptr<Packer> packer;
        bool inputDone = false;
        // The metadata asked for a delta (tt=rsync): only then may a signature come.
        bool deltaAsked = false;
        bool deltaUsed = false;
    };

    Sender(TransferClientOptions options, std::string id)
        : TransferClient(std::move(options), std::move(id))
    {
    }

    bool scan(const std::vector<std::string>& args, std::string& error);
    void begin() { start("send"); }

protected:
    void onReply(const TransferFields& f) override;
    void produce(std::string& out, std::size_t budget) override;
    void proceed() override { m_transmitting = true; }

private:
    using Phase = File::Phase;
    void addPath(const std::string& local, const std::string& remote, int& counter, std::string& error);
    void sendMetadata(std::string& out, std::size_t end);
    void setPhase(std::size_t index, Phase phase);
    void finishItem(std::size_t index, std::string error);
    void releaseData(File& file);
    bool allAcked() const { return m_metadataSent == m_files.size() && m_acked == m_files.size(); }
    bool pack(std::size_t index, std::string& out, std::size_t budget, std::size_t& work);
    std::size_t nextReady();

    std::vector<File> m_files;
    std::map<std::string, std::size_t> m_byFid;
    // The links that name each file, for a target that fails.
    std::multimap<std::size_t, std::size_t> m_linksTo;
    std::deque<std::size_t> m_ready;
    // Links whose target's metadata has not gone out: kitty fails a link
    // that names a file it does not know yet.
    std::vector<std::size_t> m_deferred;
    bool m_granted = false;
    bool m_transmitting = false;
    bool m_finishSent = false;
    std::size_t m_metadataSent = 0;
    // Delta uploads whose metadata went out and that the terminal has not finished.
    std::size_t m_openDeltas = 0;
    std::size_t m_waiting = 0;
    std::size_t m_acked = 0;
    std::size_t m_signaturesHeld = 0;
    std::size_t m_active = SIZE_MAX;
    std::string m_raw;
};

void Sender::addPath(const std::string& local, const std::string& remote, int& counter, std::string& error)
{
    if (!error.empty()) {
        return;
    }
    File file;
    if (::lstat(local.c_str(), &file.st) != 0) {
        error = "Failed to stat " + local + ": " + std::strerror(errno);
        return;
    }
    TransferItem item;
    item.local = local;
    item.remote = remote;
    if (S_ISDIR(file.st.st_mode)) {
        item.type = Type::Directory;
    } else if (S_ISLNK(file.st.st_mode)) {
        item.type = Type::Symlink;
    } else if (S_ISREG(file.st.st_mode)) {
        item.type = Type::Regular;
        item.size = std::uint64_t(file.st.st_size);
    } else {
        // Devices, sockets and pipes are not transferred.
        return;
    }
    file.path = local;
    char id[20];
    const auto [end, ec] = std::to_chars(id, id + sizeof id, ++counter, 16);
    file.fid.assign(id, end);
    file.rsyncCapable = item.type == Type::Regular && item.size > SmallFile;
    file.compress = item.type == Type::Regular && item.size > SmallFile && shouldCompress(local, m_options.compress);
    m_items.push_back(item);
    m_files.push_back(std::move(file));
    if (item.type != Type::Directory) {
        return;
    }
    std::vector<std::string> children;
    int failure = 0;
    if (!listDirectory(local, children, failure)) {
        error = "Failed to read the directory " + local + ": " + std::strerror(failure);
        return;
    }
    // The children go under the directory's own remote path, wherever that is.
    for (const auto& child : children) {
        addPath(TransferPath::join(local, child), TransferPath::join(remote, child), counter, error);
    }
}

bool Sender::scan(const std::vector<std::string>& args, std::string& error)
{
    if (std::any_of(args.begin(), args.end(), [](const std::string& arg) { return arg.empty(); })) {
        error = "A path cannot be empty";
        return false;
    }
    // Each source: its path here and its path on the terminal's computer.
    std::vector<std::pair<std::string, std::string>> sources;
    if (m_options.mode == TransferClientOptions::Mode::Mirror) {
        if (args.empty()) {
            error = "Must specify at least one file to transfer";
            return false;
        }
        const auto home = TransferPath::clean(m_options.home);
        for (const auto& arg : args) {
            const auto path = TransferPath::absolute(arg, m_options.home, m_options.cwd);
            // The home and the files under it go to the home of the other computer.
            std::string remote = path;
            if (home != "/" && path == home) {
                remote = "~";
            } else if (home != "/" && path.starts_with(home + "/")) {
                remote = "~/" + TransferPath::relative(home, path);
            }
            sources.emplace_back(path, remote);
        }
    } else {
        if (args.size() < 2) {
            error = "Must specify at least one local path and one remote path";
            return false;
        }
        std::string remoteBase = args.back();
        if (args.size() > 2 && !remoteBase.ends_with('/')) {
            remoteBase += '/';
        }
        // kitten: the remote path of a source is the destination itself, or
        // the destination directory (ending in /) and the source's name.
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            const auto path = TransferPath::absolute(args[i], m_options.home, m_options.cwd);
            sources.emplace_back(path,
                                 remoteBase.ends_with('/') ? TransferPath::join(remoteBase, TransferPath::baseName(path)) : remoteBase);
        }
    }
    int counter = 0;
    for (const auto& [local, remote] : sources) {
        addPath(local, remote, counter, error);
    }
    if (!error.empty()) {
        return false;
    }
    if (m_files.empty()) {
        error = "No files to transfer";
        return false;
    }
    // The first copy of a regular file is sent. The others become hard links to it.
    std::map<std::pair<dev_t, ino_t>, std::size_t> inodes;
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        auto& file = m_files[i];
        const auto key = std::make_pair(file.st.st_dev, file.st.st_ino);
        const auto [it, added] = inodes.emplace(key, i);
        if (!added && m_items[i].type == Type::Regular && m_items[it->second].type == Type::Regular) {
            m_items[i].type = Type::Link;
            m_items[i].size = 0;
            file.linkData = "fid:" + m_files[it->second].fid;
            file.linkTarget = it->second;
            file.compress = file.rsyncCapable = false;
        }
    }
    // A symlink to a file of the transfer points at its copy.
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        auto& file = m_files[i];
        if (m_items[i].type != Type::Symlink) {
            continue;
        }
        std::string target(MaxLinkTarget, '\0');
        const auto n = ::readlink(file.path.c_str(), target.data(), target.size());
        if (n < 0 || std::size_t(n) >= target.size()) {
            m_items[i].error = "EIO:could not read the link";
            continue;
        }
        target.resize(std::size_t(n));
        file.linkData = "path:" + target;
        // The kernel resolves the link: a lexical join would go wrong when
        // the target passes through another link, or through "..".
        struct stat st { };
        if (::stat(file.path.c_str(), &st) == 0) {
            if (const auto it = inodes.find({ st.st_dev, st.st_ino }); it != inodes.end()) {
                file.linkData = (target.starts_with('/') ? "fid_abs:" : "fid:") + m_files[it->second].fid;
                file.linkTarget = it->second;
            }
        }
    }
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        m_byFid[m_files[i].fid] = i;
        if (m_files[i].linkTarget != SIZE_MAX) {
            m_linksTo.emplace(m_files[i].linkTarget, i);
        }
        if (!m_items[i].error.empty()) {
            m_items[i].finished = true;
            setPhase(i, Phase::Acked);
        }
    }
    return true;
}

void Sender::setPhase(std::size_t index, Phase phase)
{
    auto& file = m_files[index];
    if (file.phase == phase) {
        return;
    }
    if (file.phase == Phase::Waiting) {
        --m_waiting;
    }
    if (phase == Phase::Waiting) {
        ++m_waiting;
    }
    if (phase == Phase::Ready) {
        m_ready.push_back(index);
    }
    if (phase == Phase::Acked) {
        ++m_acked;
        if (file.metadataSent && file.deltaAsked) {
            --m_openDeltas;
        }
    }
    file.phase = phase;
}

void Sender::sendMetadata(std::string& out, std::size_t end)
{
    // The metadata of a delta upload goes out as the terminal finishes
    // files, so that a large tree never holds more than MaxOpenFiles of
    // them open there.
    while (m_metadataSent < m_files.size() && out.size() < end) {
        const std::size_t i = m_metadataSent;
        auto& file = m_files[i];
        const auto& item = m_items[i];
        if (file.phase == Phase::Acked) {
            ++m_metadataSent;
            continue;
        }
        const bool rsync = m_options.deltas && file.rsyncCapable;
        if (rsync && m_openDeltas >= MaxOpenFiles) {
            break;
        }
        ++m_metadataSent;
        const std::string mode = number(file.st.st_mode & 07777);
        const auto mtime = nanoseconds(file.st);
        const std::string modified = mtime ? number(*mtime) : std::string();
        const std::string size = number(std::int64_t(item.size));
        Fields f { { "ac", "file" }, { "id", m_id }, { "fid", file.fid }, { "n", item.remote },
            { "ft", typeName(item.type) }, { "prm", mode } };
        // A time past the year 2262 does not fit: the terminal keeps its own.
        if (mtime) {
            f.emplace_back("mod", modified);
        }
        if (item.type == Type::Regular) {
            f.emplace_back("sz", size);
        }
        if (file.compress) {
            f.emplace_back("zip", "zlib");
        }
        if (rsync) {
            f.emplace_back("tt", "rsync");
        }
        file.deltaAsked = rsync;
        out += encodeTransfer(f);
        file.metadataSent = true;
        if (rsync) {
            ++m_openDeltas;
        }
        setPhase(i, Phase::Waiting);
    }
    std::erase_if(m_deferred, [&](std::size_t link) {
        if (m_files[link].phase != Phase::Ready || m_files[m_files[link].linkTarget].metadataSent) {
            if (m_files[link].phase == Phase::Ready) {
                m_ready.push_back(link);
            }
            return true;
        }
        return false;
    });
}

void Sender::releaseData(File& file)
{
    file.generator.reset();
    file.source.reset();
    file.packer.reset();
    std::string().swap(file.signature);
    m_signaturesHeld -= file.signatureHeld;
    file.signatureHeld = 0;
    file.fd.reset();
}

void Sender::finishItem(std::size_t index, std::string error)
{
    auto& file = m_files[index];
    auto& item = m_items[index];
    if (file.phase == Phase::Acked) {
        return;
    }
    item.finished = true;
    if (item.error.empty()) {
        item.error = std::move(error);
    }
    if (item.error.empty() && file.deltaUsed) {
        m_deltaFileBytes += item.size;
    }
    releaseData(file);
    setPhase(index, Phase::Acked);
    if (m_active == index) {
        m_active = SIZE_MAX;
    }
    if (item.error.empty()) {
        return;
    }
    // A link to a file that failed cannot be made, and a terminal may hold it
    // without an answer until the session finishes: the session would wait.
    const auto [first, last] = m_linksTo.equal_range(index);
    std::vector<std::size_t> links;
    for (auto it = first; it != last; ++it) {
        links.push_back(it->second);
    }
    for (const auto link : links) {
        finishItem(link, "ENOENT:the link's target failed");
    }
}

void Sender::onReply(const TransferFields& f)
{
    const auto action = field(f, "ac");
    const auto status = field(f, "st");
    const auto fid = field(f, "fid");
    if (!m_granted) {
        // Nothing but the permission can come before it.
        if (action != "status" || !fid.empty()) {
            cancel("Unexpected response from the terminal");
            return;
        }
        if (status != TransferStatus::Ok) {
            fail("Permission denied for this transfer: " + status);
            return;
        }
        m_granted = true;
        m_state = State::Running;
        return;
    }
    if (fid.empty()) {
        if (action == "status" && !transferSuccess(status)) {
            fail(statusError(status));
        }
        return;
    }
    const auto found = m_byFid.find(fid);
    if (found == m_byFid.end()) {
        return;
    }
    const std::size_t i = found->second;
    auto& file = m_files[i];
    auto& item = m_items[i];
    if (file.phase == Phase::Acked) {
        return;
    }
    if (action == "data" || action == "end_data") {
        if (file.phase != Phase::Signature) {
            return;
        }
        const auto data = field(f, "d");
        m_signatureBytes += data.size();
        if (file.signature.size() + data.size() > MaxSignature) {
            cancel("The terminal sent a signature that is too large");
            return;
        }
        if (m_signaturesHeld + data.size() > m_options.signatureMemory) {
            cancel("The terminal sent more signatures than this side can hold");
            return;
        }
        file.signature += data;
        file.signatureHeld += data.size();
        m_signaturesHeld += data.size();
        if (action == "end_data") {
            if (!signatureBlockSize(file.signature, DeltaMaxBlockSize)) {
                cancel("The terminal sent an invalid signature");
                return;
            }
            file.generator = std::make_unique<DeltaGenerator>(std::move(file.signature));
            if (!file.generator->valid()) {
                cancel("Out of memory");
                return;
            }
            file.deltaUsed = true;
            setPhase(i, Phase::Ready);
        }
        return;
    }
    if (action != "status") {
        return;
    }
    if (status == TransferStatus::Started) {
        if (file.phase != Phase::Waiting) {
            return;
        }
        if (item.type == Type::Directory) {
            return;
        }
        // A terminal that sends a signature for a file sent whole is refused:
        // it would make this side hash files that nobody asked to compare.
        if (field(f, "tt") == "rsync" && !file.deltaAsked) {
            cancel("The terminal asked for a delta that was not offered");
            return;
        }
        setPhase(i, file.deltaAsked && field(f, "tt") == "rsync" ? Phase::Signature : Phase::Ready);
        return;
    }
    if (status == TransferStatus::Progress) {
        if (const auto size = parseNumber(f, "sz"); size && *size >= 0) {
            item.done = std::uint64_t(*size);
        }
        return;
    }
    if (status == TransferStatus::Ok) {
        if (const auto size = parseNumber(f, "sz"); size && *size > 0) {
            item.done = std::uint64_t(*size);
        }
        finishItem(i, {});
        return;
    }
    finishItem(i, statusError(status));
}

bool Sender::pack(std::size_t index, std::string& out, std::size_t budget, std::size_t& work)
{
    auto& file = m_files[index];
    auto& item = m_items[index];
    const std::size_t start = out.size();
    if (item.type == Type::Symlink || item.type == Type::Link) {
        // A target of MaxLinkTarget bytes and its prefix take two packets.
        std::string_view data = file.linkData;
        do {
            const auto take = std::min(data.size(), PacketData);
            const bool last = take == data.size();
            const Fields f { { "ac", last ? "end_data" : "data" }, { "id", m_id }, { "fid", file.fid },
                { "d", data.substr(0, take) } };
            out += encodeTransfer(f);
            data.remove_prefix(take);
        } while (!data.empty());
        setPhase(index, Phase::Sent);
        return true;
    }
    const auto failFile = [&](std::string error) {
        // The terminal holds the file open: end it there too.
        const Fields f { { "ac", "end_data" }, { "id", m_id }, { "fid", file.fid } };
        out += encodeTransfer(f);
        finishItem(index, std::move(error));
        return true;
    };
    if (!file.fd.valid()) {
        // O_NONBLOCK: a file that became a pipe since the scan must not block the open.
        file.fd.reset(::open(file.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat st { };
        if (!file.fd.valid() || ::fstat(file.fd.get(), &st) != 0) {
            return failFile(errnoText(errno));
        }
        if (!S_ISREG(st.st_mode)) {
            return failFile("EINVAL:the file is no longer a regular file");
        }
        // Only the announced size is sent, also when the file grows meanwhile.
        file.source = std::make_unique<FdSource>(file.fd.get(), item.size);
        file.packer = std::make_unique<Packer>(file.compress);
        if (!file.packer->valid()) {
            cancel("Out of memory");
            return false;
        }
    }
    while (out.size() - start < budget) {
        // Refill until a whole packet is ready or the input ends.
        while (file.packer->ready().size() <= PacketData && !file.inputDone) {
            if (work >= WorkStep) {
                m_hasWork = true;
                return false;
            }
            m_raw.clear();
            const auto before = file.source->consumed();
            bool ok = true;
            errno = 0;
            if (file.generator) {
                ok = file.generator->produce(*file.source, m_raw, ReadStep, DeltaStep);
                file.inputDone = ok && file.generator->done();
            } else {
                ok = file.source->read(m_raw, ReadStep);
                file.inputDone = ok && file.source->atEnd();
            }
            work += std::max<std::size_t>(std::size_t(file.source->consumed() - before), ReadStep);
            if (!ok) {
                // A generator fails without errno on a signature that it cannot use.
                const int error = errno ? errno : file.generator ? EINVAL : EIO;
                return failFile(errnoText(error));
            }
            if (file.inputDone && file.source->consumed() != item.size) {
                return failFile("EIO:the file shrank while it was read");
            }
            if (!file.packer->add(m_raw, file.inputDone)) {
                cancel("Compression failed");
                return false;
            }
        }
        const auto ready = file.packer->ready();
        const std::size_t take = std::min(ready.size(), PacketData);
        const bool last = file.inputDone && take == ready.size();
        Fields f { { "ac", last ? "end_data" : "data" }, { "id", m_id }, { "fid", file.fid } };
        if (take) {
            f.emplace_back("d", ready.substr(0, take));
        }
        out += encodeTransfer(f);
        if (file.deltaUsed) {
            m_deltaBytes += take;
        }
        file.packer->consume(take);
        if (last) {
            setPhase(index, Phase::Sent);
            releaseData(file);
            return true;
        }
    }
    return false;
}

std::size_t Sender::nextReady()
{
    while (!m_ready.empty()) {
        const auto index = m_ready.front();
        m_ready.pop_front();
        auto& file = m_files[index];
        if (file.phase != Phase::Ready) {
            continue;
        }
        if (file.linkTarget != SIZE_MAX && !m_files[file.linkTarget].metadataSent) {
            m_deferred.push_back(index);
            continue;
        }
        return index;
    }
    return SIZE_MAX;
}

void Sender::produce(std::string& out, std::size_t budget)
{
    const std::size_t end = out.size() + budget;
    std::size_t work = 0;
    if (m_granted) {
        sendMetadata(out, end);
    }
    if (m_options.confirmPaths && !m_transmitting) {
        // Ask once each file whose metadata went out has its answer: that is
        // every file, or as many as the cap on delta uploads lets out first.
        if ((m_metadataSent == m_files.size() || m_openDeltas >= MaxOpenFiles) && m_waiting == 0) {
            m_state = State::Confirm;
        }
        return;
    }
    m_transmitting = true;
    while (out.size() < end && m_state == State::Running) {
        if (m_active == SIZE_MAX) {
            m_active = nextReady();
            if (m_active == SIZE_MAX) {
                break;
            }
            setPhase(m_active, Phase::Sending);
        }
        const std::size_t index = m_active;
        if (pack(index, out, end - out.size(), work) && m_active == index) {
            m_active = SIZE_MAX;
        }
        // A cancel in pack() ends the session: no more files may open there.
        if (m_state != State::Running || m_hasWork) {
            break;
        }
        sendMetadata(out, end);
    }
    if (m_state == State::Running && !m_finishSent && allAcked()) {
        m_finishSent = true;
        const Fields f { { "ac", "finish" }, { "id", m_id } };
        out += encodeTransfer(f);
        m_state = State::Done;
    }
}

// ── Receiving ───────────────────────────────────────────────────────────────
//
// The terminal is not trusted here: its listing names the files, and a
// hostile one could name them to make this side write outside the
// destination. Each entry is placed as a name in the local copy of its
// parent entry, under a base directory that the user named. Every write
// goes through directory descriptors that are opened one level at a time
// below the base with O_NOFOLLOW: no link under the base is followed,
// whether the transfer made it or it was there before. The base itself is
// opened by its path, as the user gave it.

// A name for an entry: a single path component.
bool validName(std::string_view name)
{
    return !name.empty() && name.size() <= 255 && name != "." && name != ".." && name.find('/') == std::string_view::npos
        && name.find('\0') == std::string_view::npos;
}

// Creates a new file with a random name in @p dir; it never replaces one.
int createTemporary(int dir, std::string_view prefix, std::string& name)
{
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto suffix = randomHex(6);
        if (!suffix) {
            errno = EIO;
            return -1;
        }
        name = std::string(prefix) + *suffix;
        const int fd = ::openat(dir, name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }
    }
    errno = EEXIST;
    return -1;
}

class Receiver final : public TransferClient
{
public:
    struct Remote
    {
        std::size_t spec = 0;
        std::string remoteId; // st: the terminal's id of the entry
        std::string parentId; // pr
        std::string targetId; // d: the id of a link's target
        std::optional<std::int64_t> mtime;
        std::optional<std::int64_t> mode;
        // Where the entry goes: a name in the directory of its parent entry,
        // or in a base directory for a root.
        std::size_t base = SIZE_MAX;
        std::size_t parent = SIZE_MAX;
        std::size_t target = SIZE_MAX;
        std::size_t depth = 0;
        std::string name;
        std::string fid; // the id of the data request
        bool requested = false;
        bool rsync = false;
        bool compressed = false;
        bool opened = false;
        bool streamEnded = false;
        // A directory that this session made: only those get the listed mode and time.
        bool created = false;
        UniqueFd dir; // the directory that the file goes in
        UniqueFd out;
        UniqueFd basis;
        std::string temporary;
        std::string linkValue;
        std::unique_ptr<ZlibStream> inflater;
        std::unique_ptr<DeltaApplier> applier;
        std::unique_ptr<FdDeltaTarget> deltaTarget;
        std::string pending;
        // The packet that the inflater reads: a delta step can stop it midway.
        std::string inflateInput;
        std::uint32_t blockSize = 0;
        std::uint64_t basisSize = 0;

        Remote() = default;
        // A moved-from entry holds no directory, so it removes nothing.
        Remote(Remote&&) noexcept = default;
        Remote& operator=(Remote&&) = delete;
        // A transfer that ends early, for any reason, leaves no temporary file.
        ~Remote() { discard(); }
        void discard()
        {
            if (!temporary.empty() && dir.valid()) {
                ::unlinkat(dir.get(), temporary.c_str(), 0);
            }
            temporary.clear();
            deltaTarget.reset();
            out.reset();
            basis.reset();
            dir.reset();
        }
    };

    Receiver(TransferClientOptions options, std::string id)
        : TransferClient(std::move(options), std::move(id))
        , m_umask(processUmask())
    {
    }

    ~Receiver() override { m_remote.clear(); }

    bool setUp(const std::vector<std::string>& args, std::string& error)
    {
        if (std::any_of(args.begin(), args.end(), [](const std::string& arg) { return arg.empty(); })) {
            error = "A path cannot be empty";
            return false;
        }
        if (m_options.mode == TransferClientOptions::Mode::Mirror) {
            if (args.empty()) {
                error = "Must specify at least one file to transfer";
                return false;
            }
            m_specs = args;
        } else {
            if (args.size() < 2) {
                error = "Must specify at least one source and a destination file to transfer";
                return false;
            }
            m_specs.assign(args.begin(), args.end() - 1);
            m_destination = args.back();
        }
        m_counts.assign(m_specs.size(), 0);
        m_specErrors.assign(m_specs.size(), {});
        start("receive", number(std::int64_t(m_specs.size())));
        for (std::size_t i = 0; i < m_specs.size(); ++i) {
            const std::string fid = number(std::int64_t(i));
            const Fields request { { "ac", "file" }, { "id", m_id }, { "fid", fid }, { "n", m_specs[i] } };
            queue(encodeTransfer(request));
        }
        return true;
    }

protected:
    void onReply(const TransferFields& f) override;
    void produce(std::string& out, std::size_t budget) override;
    void proceed() override { openBases(); }

private:
    struct Base
    {
        std::string path;
    };
    // The entry size that a listing costs here, beyond its names.
    static constexpr std::size_t EntryCost = sizeof(Remote) + sizeof(TransferItem);

    std::optional<std::size_t> specOf(const std::string& fid) const;
    void listed(const TransferFields& f);
    bool collect();
    bool link();
    bool placeRoots();
    std::size_t addBase(const std::string& path);
    bool openBases();
    int baseFd(std::size_t base);
    UniqueFd openDirectory(std::size_t index, bool create);
    void data(std::size_t index, const TransferFields& f, bool end);
    bool inflate(std::size_t index);
    void endData(std::size_t index, bool end, bool ok);
    bool busy() const override { return m_busy != SIZE_MAX; }
    void resume() override;
    void abandon() override;
    bool write(std::size_t index, std::string_view chunk);
    void done(std::size_t index, std::string error);
    void finalize();
    bool makeLink(std::size_t index, std::string& error);
    bool request(std::size_t index, std::string& out, std::size_t budget, std::size_t& work);
    std::string expectedSpec(const std::string& spec) const;
    mode_t modeOf(const Remote& remote, mode_t fallback) const
    {
        // The permission bits under the umask. Set-user-ID, set-group-ID
        // and the sticky bit are dropped.
        return (remote.mode && *remote.mode >= 0 ? mode_t(*remote.mode) : fallback) & 0777 & ~m_umask;
    }

    std::vector<std::string> m_specs;
    std::string m_destination;
    std::vector<std::size_t> m_counts;
    // One error for each spec at most, whatever the terminal repeats.
    std::vector<std::string> m_specErrors;
    std::string m_remoteHome;
    mode_t m_umask;
    bool m_granted = false;
    bool m_listed = false;
    std::size_t m_listingBytes = 0;
    std::vector<Remote> m_remote;
    std::vector<Base> m_bases;
    std::map<std::string, std::size_t> m_baseIndex;
    // One base open at a time: a mirror of many paths has many bases.
    UniqueFd m_baseFd;
    std::size_t m_baseFdIndex = SIZE_MAX;
    // The directory of the last entry opened: its siblings reuse it, and
    // need no walk from the base.
    UniqueFd m_dirCache;
    std::size_t m_dirCacheIndex = SIZE_MAX;
    std::map<std::string, std::size_t, std::less<>> m_byRemoteId;
    std::vector<std::size_t> m_order; // every entry, parents first
    std::map<std::string, std::size_t> m_byFid;
    std::size_t m_nextRequest = 0;
    std::size_t m_inFlight = 0;
    std::size_t m_outstanding = 0;
    // A delta request whose signature is being sent, from the entry's basis.
    std::size_t m_signing = SIZE_MAX;
    // The file whose delta goes on in steps, and whether its last packet came.
    std::size_t m_busy = SIZE_MAX;
    bool m_busyEnd = false;
    std::unique_ptr<SignatureBuilder> m_signer;
    std::uint64_t m_signSize = 0;
    std::uint64_t m_signOffset = 0;
    std::string m_signBuffer;
    std::size_t m_signSent = 0;
    std::string m_block;
    bool m_finishSent = false;
};

std::optional<std::size_t> Receiver::specOf(const std::string& fid) const
{
    std::size_t spec = 0;
    const auto [end, ec] = std::from_chars(fid.data(), fid.data() + fid.size(), spec);
    if (ec != std::errc() || end != fid.data() + fid.size() || spec >= m_specs.size()) {
        return std::nullopt;
    }
    return spec;
}

void Receiver::onReply(const TransferFields& f)
{
    const auto action = field(f, "ac");
    const auto status = field(f, "st");
    const auto fid = field(f, "fid");
    if (!m_granted) {
        // Nothing but the permission can come before it.
        if (action != "status" || !fid.empty()) {
            cancel("Unexpected response from the terminal");
            return;
        }
        if (status != TransferStatus::Ok) {
            fail("Permission denied for this transfer: " + status);
            return;
        }
        m_granted = true;
        return;
    }
    if (!m_listed) {
        if (action == "file") {
            listed(f);
            return;
        }
        if (action != "status") {
            cancel("Unexpected response from the terminal");
            return;
        }
        if (!fid.empty()) {
            const auto spec = specOf(fid);
            if (!spec) {
                cancel("Unexpected response from the terminal (unknown file id)");
                return;
            }
            if (m_specErrors[*spec].empty()) {
                m_specErrors[*spec] = status.starts_with("ENOENT") ? "File not found" : statusError(status);
            }
            return;
        }
        if (status != TransferStatus::Ok) {
            fail(statusError(status));
            return;
        }
        m_listed = true;
        m_remoteHome = field(f, "n");
        std::string message;
        for (std::size_t i = 0; i < m_specs.size(); ++i) {
            if (!m_specErrors[i].empty()) {
                message += "\n  " + m_specs[i] + ": " + m_specErrors[i];
            }
        }
        std::vector<std::string>().swap(m_specErrors);
        if (!message.empty()) {
            cancel("Failed to process some sources:" + message);
            return;
        }
        for (std::size_t i = 0; i < m_specs.size(); ++i) {
            if (m_counts[i] == 0) {
                cancel("No matches found for: " + m_specs[i]);
                return;
            }
        }
        if (!collect()) {
            return;
        }
        if (m_options.confirmPaths) {
            m_state = State::Confirm;
            return;
        }
        m_state = State::Running;
        openBases();
        return;
    }
    if (fid.empty()) {
        // The terminal ended the session, for a limit of its own or an error.
        if (action == "status" && !transferSuccess(status)) {
            fail(statusError(status));
        }
        return;
    }
    const auto found = m_byFid.find(fid);
    if (found == m_byFid.end()) {
        return;
    }
    const std::size_t index = found->second;
    if (m_items[index].finished) {
        return;
    }
    if (!m_remote[index].requested) {
        cancel("The terminal answered for a file before its request");
        return;
    }
    if (action == "data" || action == "end_data") {
        data(index, f, action == "end_data");
    } else if (action == "status" && !transferSuccess(status)) {
        done(index, statusError(status));
    }
}

void Receiver::listed(const TransferFields& f)
{
    const auto spec = specOf(field(f, "fid"));
    if (!spec) {
        cancel("Unexpected response from the terminal (unknown file id)");
        return;
    }
    Remote remote;
    remote.spec = *spec;
    remote.remoteId = field(f, "st");
    remote.parentId = field(f, "pr");
    remote.targetId = field(f, "d");
    remote.mtime = parseNumber(f, "mod");
    remote.mode = parseNumber(f, "prm");
    TransferItem item;
    item.remote = field(f, "n");
    m_listingBytes += item.remote.size() + remote.remoteId.size() + remote.parentId.size() + remote.targetId.size() + EntryCost;
    if (m_remote.size() >= MaxListing || m_listingBytes > MaxListingBytes) {
        cancel("The terminal listed too many files");
        return;
    }
    const auto type = field(f, "ft");
    item.type = type == "directory" ? Type::Directory : type == "symlink" ? Type::Symlink : type == "link" ? Type::Link : Type::Regular;
    // The terminal lists absolute paths. Only the last name of each is used here.
    remote.name = TransferPath::baseName(item.remote);
    if (remote.remoteId.empty() || !item.remote.starts_with('/') || item.remote.size() > MaxPath || !validName(remote.name)) {
        cancel("The terminal listed an invalid path: " + item.remote);
        return;
    }
    if (item.type == Type::Regular) {
        // The size bounds what is written: a file without one could fill the disk.
        const auto size = parseNumber(f, "sz");
        if (!size || *size < 0) {
            cancel("The terminal listed a file without a size: " + item.remote);
            return;
        }
        item.size = std::uint64_t(*size);
    }
    ++m_counts[*spec];
    m_remote.push_back(std::move(remote));
    m_items.push_back(std::move(item));
}

std::size_t Receiver::addBase(const std::string& path)
{
    const auto [it, added] = m_baseIndex.emplace(path, m_bases.size());
    if (added) {
        m_bases.push_back({ path });
    }
    return it->second;
}

// The remote path that a spec names, as the terminal resolves it: ~ and
// relative paths under its home.
std::string Receiver::expectedSpec(const std::string& spec) const
{
    if (spec.starts_with('/')) {
        return TransferPath::clean(spec);
    }
    if (!m_remoteHome.starts_with('/')) {
        return {};
    }
    if (spec == "~") {
        return TransferPath::clean(m_remoteHome);
    }
    return TransferPath::join(m_remoteHome, spec.starts_with("~/") ? std::string_view(spec).substr(2) : std::string_view(spec));
}

// Each entry gets its parent, its depth and its link target. The parent
// must be a directory of the same spec, and the entry's path its parent's
// path and its name: so the paths written here are as long as the paths
// that the terminal listed, and the tree has no cycle.
bool Receiver::link()
{
    for (std::size_t i = 0; i < m_remote.size(); ++i) {
        if (!m_byRemoteId.emplace(m_remote[i].remoteId, i).second) {
            return false;
        }
    }
    for (std::size_t i = 0; i < m_remote.size(); ++i) {
        auto& remote = m_remote[i];
        if (!remote.targetId.empty()) {
            if (const auto it = m_byRemoteId.find(remote.targetId); it != m_byRemoteId.end()) {
                remote.target = it->second;
            }
        }
        if (remote.parentId.empty()) {
            continue;
        }
        const auto it = m_byRemoteId.find(remote.parentId);
        if (it == m_byRemoteId.end() || it->second == i) {
            return false;
        }
        const auto parent = it->second;
        if (m_remote[parent].spec != remote.spec || m_items[parent].type != Type::Directory
            || TransferPath::clean(m_items[i].remote) != TransferPath::join(m_items[parent].remote, remote.name)) {
            return false;
        }
        remote.parent = parent;
    }
    // The order walks the parent links from the roots, so a parent always
    // comes before its children, whatever the listed paths look like: the
    // terminal may list a directory as /a/./b and its child as /a/b/c. The
    // children of one directory stay together, for the directory cache.
    std::vector<std::size_t> firstChild(m_remote.size(), SIZE_MAX);
    std::vector<std::size_t> lastChild(m_remote.size(), SIZE_MAX);
    std::vector<std::size_t> nextSibling(m_remote.size(), SIZE_MAX);
    m_order.clear();
    m_order.reserve(m_remote.size());
    for (std::size_t i = 0; i < m_remote.size(); ++i) {
        const auto parent = m_remote[i].parent;
        if (parent == SIZE_MAX) {
            m_order.push_back(i);
        } else if (firstChild[parent] == SIZE_MAX) {
            firstChild[parent] = lastChild[parent] = i;
        } else {
            lastChild[parent] = nextSibling[lastChild[parent]] = i;
        }
    }
    for (std::size_t k = 0; k < m_order.size(); ++k) {
        for (auto child = firstChild[m_order[k]]; child != SIZE_MAX; child = nextSibling[child]) {
            m_order.push_back(child);
        }
    }
    // An entry that no root reaches is in a cycle of parents.
    if (m_order.size() != m_remote.size()) {
        return false;
    }
    std::set<std::pair<std::size_t, std::string_view>> names;
    for (const auto i : m_order) {
        auto& remote = m_remote[i];
        if (remote.parent == SIZE_MAX) {
            continue;
        }
        const auto& parent = m_remote[remote.parent];
        remote.depth = parent.depth + 1;
        remote.base = parent.base;
        // Two entries may not take one local path.
        if (remote.depth > MaxDepth || !names.emplace(remote.parent, remote.name).second) {
            return false;
        }
        m_items[i].local = TransferPath::join(m_items[remote.parent].local, remote.name);
    }
    return true;
}

// Sets the base and the local name of the root of each spec. The local name
// comes from what the user typed, never from the listing alone: the root
// that the terminal lists must be the path that the spec names.
bool Receiver::placeRoots()
{
    std::vector<std::size_t> roots(m_specs.size(), SIZE_MAX);
    for (std::size_t i = 0; i < m_remote.size(); ++i) {
        if (!m_remote[i].parentId.empty()) {
            continue;
        }
        const auto spec = m_remote[i].spec;
        if (roots[spec] != SIZE_MAX) {
            cancel("The terminal listed more than one root for " + m_specs[spec]);
            return false;
        }
        roots[spec] = i;
    }
    for (std::size_t s = 0; s < m_specs.size(); ++s) {
        if (roots[s] == SIZE_MAX) {
            cancel("The terminal listed no root for " + m_specs[s]);
            return false;
        }
        const auto expected = expectedSpec(m_specs[s]);
        const auto& root = m_items[roots[s]].remote;
        if (expected.empty() || TransferPath::clean(root) != expected) {
            cancel("The terminal listed " + root + " for " + m_specs[s]);
            return false;
        }
    }
    const auto local = [&](const std::string& path) { return TransferPath::absolute(path, m_options.home, m_options.cwd); };
    // The local path of each root, by spec.
    std::vector<std::string> where(m_specs.size());
    if (m_options.mode == TransferClientOptions::Mode::Mirror) {
        for (std::size_t s = 0; s < m_specs.size(); ++s) {
            const auto& spec = m_specs[s];
            // An absolute spec is the same path here. ~, ~/x and a relative
            // spec are under the home on both sides: the remote home does
            // not choose where they go.
            where[s] = spec.starts_with('/') ? TransferPath::clean(spec)
                : spec == "~"                ? TransferPath::clean(m_options.home)
                                             : TransferPath::join(m_options.home, spec.starts_with("~/") ? std::string_view(spec).substr(2) : std::string_view(spec));
        }
        // Two specs that overlap would write one path twice.
        std::vector<std::size_t> sorted(m_specs.size());
        for (std::size_t s = 0; s < sorted.size(); ++s) {
            sorted[s] = s;
        }
        std::sort(sorted.begin(), sorted.end(), [&](std::size_t a, std::size_t b) { return where[a] < where[b]; });
        for (std::size_t k = 1; k < sorted.size(); ++k) {
            const auto& a = where[sorted[k - 1]];
            const auto& b = where[sorted[k]];
            if (a == b || b.starts_with(a == "/" ? a : a + "/")) {
                cancel("The sources " + m_specs[sorted[k - 1]] + " and " + m_specs[sorted[k]] + " overlap");
                return false;
            }
        }
    } else {
        struct stat st { };
        const auto destination = local(m_destination);
        const bool intoDirectory = m_destination.ends_with('/') || m_specs.size() > 1
            || (::stat(destination.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        std::map<std::string, std::size_t> names;
        for (std::size_t s = 0; s < m_specs.size(); ++s) {
            if (!intoDirectory) {
                where[s] = destination;
                continue;
            }
            const auto name = TransferPath::baseName(expectedSpec(m_specs[s]));
            if (const auto [it, added] = names.emplace(name, s); !added) {
                cancel("The sources " + m_specs[it->second] + " and " + m_specs[s] + " have the same name");
                return false;
            }
            where[s] = TransferPath::join(destination, name);
        }
    }
    for (std::size_t s = 0; s < m_specs.size(); ++s) {
        auto& root = m_remote[roots[s]];
        root.name = TransferPath::baseName(where[s]);
        if (where[s] == "/" || !validName(root.name)) {
            cancel("Cannot receive into " + where[s]);
            return false;
        }
        root.base = addBase(TransferPath::dirName(where[s]));
        m_items[roots[s]].local = where[s];
    }
    return true;
}

bool Receiver::collect()
{
    if (!placeRoots()) {
        return false;
    }
    if (!link()) {
        cancel("The terminal sent a listing whose entries do not form a tree");
        return false;
    }
    for (const auto i : m_order) {
        auto& item = m_items[i];
        const auto& remote = m_remote[i];
        // Directories are made at the end, and a hard link to a listed file is made locally.
        if (item.type == Type::Directory || (item.type == Type::Link && remote.target != SIZE_MAX)) {
            continue;
        }
        if (item.type == Type::Link) {
            item.finished = true;
            item.error = remote.targetId.empty() ? "EINVAL:the terminal listed a hard link without its target"
                                                 : "ENOENT:the link's target is not in the transfer";
            continue;
        }
        m_remote[i].fid = "r" + number(std::int64_t(i));
        m_byFid[m_remote[i].fid] = i;
        ++m_outstanding;
    }
    return true;
}

// The bases are the directories that the user named, or their parents. They
// are made only once the transfer goes ahead: a "no" at the confirmation
// leaves nothing behind.
bool Receiver::openBases()
{
    for (const auto& base : m_bases) {
        const bool made = makeDirectories(base.path);
        const UniqueFd fd(made ? ::open(base.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1);
        if (!fd.valid()) {
            cancel("Cannot open " + base.path + ": " + std::strerror(errno));
            return false;
        }
    }
    return true;
}

int Receiver::baseFd(std::size_t base)
{
    if (m_baseFdIndex != base || !m_baseFd.valid()) {
        m_baseFd.reset(::open(m_bases[base].path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        m_baseFdIndex = m_baseFd.valid() ? base : SIZE_MAX;
    }
    return m_baseFd.get();
}

// Opens the directory that holds entry @p index, making the directories on
// the way when @p create is set. Invalid, with errno set, when a level is
// missing, is not a directory, or is a symbolic link.
UniqueFd Receiver::openDirectory(std::size_t index, bool create)
{
    const auto parent = m_remote[index].parent;
    if (parent != SIZE_MAX && parent == m_dirCacheIndex) {
        return UniqueFd(::fcntl(m_dirCache.get(), F_DUPFD_CLOEXEC, 0));
    }
    const int base = baseFd(m_remote[index].base);
    if (base < 0) {
        return {};
    }
    UniqueFd fd(::fcntl(base, F_DUPFD_CLOEXEC, 0));
    if (parent == SIZE_MAX || !fd.valid()) {
        return fd;
    }
    std::vector<std::size_t> chain;
    for (auto j = parent; j != SIZE_MAX; j = m_remote[j].parent) {
        chain.push_back(j);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        auto& level = m_remote[*it];
        if (create) {
            // 0700 while the transfer runs. finalize() sets the listed mode.
            if (::mkdirat(fd.get(), level.name.c_str(), 0700) == 0) {
                level.created = true;
            } else if (errno != EEXIST) {
                return {};
            }
        }
        UniqueFd next(::openat(fd.get(), level.name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next.valid()) {
            return {};
        }
        fd = std::move(next);
    }
    m_dirCache.reset(::fcntl(fd.get(), F_DUPFD_CLOEXEC, 0));
    m_dirCacheIndex = m_dirCache.valid() ? parent : SIZE_MAX;
    return fd;
}

bool Receiver::request(std::size_t index, std::string& out, std::size_t budget, std::size_t& work)
{
    auto& remote = m_remote[index];
    const auto& item = m_items[index];
    const std::size_t start = out.size();
    if (!remote.requested) {
        remote.requested = true;
        ++m_inFlight;
        // A delta needs the old file at the destination: a regular file, not a link.
        UniqueFd existing;
        if (m_options.deltas && item.type == Type::Regular) {
            if (const auto dir = openDirectory(index, false); dir.valid()) {
                existing.reset(::openat(dir.get(), remote.name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            }
        }
        struct stat st { };
        remote.rsync = existing.valid() && ::fstat(existing.get(), &st) == 0 && S_ISREG(st.st_mode)
            && std::uint64_t(st.st_size) > SmallFile;
        remote.compressed = item.type == Type::Regular && item.size > SmallFile && shouldCompress(item.remote, m_options.compress);
        Fields f { { "ac", "file" }, { "id", m_id }, { "fid", remote.fid }, { "n", item.remote },
            { "tt", remote.rsync ? "rsync" : "simple" } };
        if (remote.compressed) {
            f.emplace_back("zip", "zlib");
        }
        out += encodeTransfer(f);
        if (!remote.rsync) {
            return true;
        }
        // The signature reads the basis with pread(2), and so does the delta later.
        remote.basis = std::move(existing);
        m_signing = index;
        m_signSize = std::uint64_t(st.st_size);
        m_signOffset = 0;
        m_signer = std::make_unique<SignatureBuilder>(m_signSize);
        m_signBuffer = m_signer->header();
        m_signSent = 0;
        remote.blockSize = m_signer->blockSize();
        remote.basisSize = m_signSize;
    }
    // The signature, a few blocks at a time.
    const std::uint32_t block = m_signer->blockSize();
    while (out.size() - start < budget) {
        while (m_signBuffer.size() - m_signSent < PacketData && m_signOffset < m_signSize) {
            if (work >= WorkStep) {
                m_hasWork = true;
                return false;
            }
            const auto want = std::size_t(std::min<std::uint64_t>(block, m_signSize - m_signOffset));
            m_block.resize(want);
            std::size_t have = 0;
            while (have < want) {
                const ssize_t n = ::pread(remote.basis.get(), m_block.data() + have, want - have, off_t(m_signOffset + have));
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    break;
                }
                have += std::size_t(n);
            }
            if (have != want) {
                // The file shrank: the signature would not match it.
                cancel("Failed to read " + item.local);
                return true;
            }
            m_signBuffer.erase(0, m_signSent);
            m_signSent = 0;
            m_signBuffer += m_signer->record(m_block);
            m_signOffset += want;
            work += want;
        }
        const std::string_view pending = std::string_view(m_signBuffer).substr(m_signSent);
        const bool last = m_signOffset >= m_signSize && pending.size() <= PacketData;
        const auto data = pending.substr(0, PacketData);
        Fields f { { "ac", last ? "end_data" : "data" }, { "id", m_id }, { "fid", remote.fid } };
        if (!data.empty()) {
            f.emplace_back("d", data);
        }
        out += encodeTransfer(f);
        m_signatureBytes += data.size();
        m_signSent += data.size();
        if (last) {
            m_signer.reset();
            std::string().swap(m_signBuffer);
            m_signSent = 0;
            m_signing = SIZE_MAX;
            return true;
        }
    }
    return false;
}

void Receiver::produce(std::string& out, std::size_t budget)
{
    const std::size_t start = out.size();
    std::size_t work = 0;
    while (out.size() - start < budget && m_state == State::Running) {
        if (m_signing != SIZE_MAX) {
            if (!request(m_signing, out, budget - (out.size() - start), work)) {
                return;
            }
            continue;
        }
        if (m_inFlight >= MaxOpenFiles) {
            break;
        }
        // Entries without a request, and the ones that the terminal ended already.
        while (m_nextRequest < m_order.size()
               && (m_remote[m_order[m_nextRequest]].fid.empty() || m_items[m_order[m_nextRequest]].finished)) {
            ++m_nextRequest;
        }
        if (m_nextRequest >= m_order.size()) {
            break;
        }
        request(m_order[m_nextRequest++], out, budget - (out.size() - start), work);
        if (m_hasWork) {
            return;
        }
    }
    if (m_state == State::Running && m_outstanding == 0 && !m_finishSent) {
        finalize();
        m_finishSent = true;
        const Fields f { { "ac", "finish" }, { "id", m_id } };
        out += encodeTransfer(f);
        m_state = State::Done;
    }
}

// Writes decompressed bytes of a file: as they are, or as delta operations.
// Never past the size that the listing gave. False with errno set.
bool Receiver::write(std::size_t index, std::string_view chunk)
{
    auto& remote = m_remote[index];
    auto& item = m_items[index];
    if (!remote.rsync) {
        if (chunk.size() > item.size - item.done) {
            errno = EFBIG;
            return false;
        }
        if (!writeAll(remote.out.get(), chunk)) {
            return false;
        }
        item.done += chunk.size();
        return true;
    }
    if (remote.pending.size() + chunk.size() > MaxLiteral + 64) {
        errno = EFBIG;
        return false;
    }
    remote.pending += chunk;
    // The target sets errno when it stops. An invalid operation or a wrong hash leaves it 0.
    errno = 0;
    const auto used = remote.applier->apply(remote.pending, *remote.deltaTarget, WorkStep);
    if (!used) {
        if (errno == 0) {
            errno = EINVAL;
        }
        return false;
    }
    remote.pending.erase(0, *used);
    item.done = remote.deltaTarget->written();
    return true;
}

void Receiver::data(std::size_t index, const TransferFields& f, bool end)
{
    auto& remote = m_remote[index];
    auto& item = m_items[index];
    if (index == m_signing) {
        cancel("The terminal sent data for a file before its request");
        return;
    }
    const auto bytes = field(f, "d");
    if (bytes.size() > PacketData) {
        done(index, "EFBIG:a data packet holds more than " + number(std::int64_t(PacketData)) + " bytes");
        return;
    }
    if (remote.rsync) {
        m_deltaBytes += bytes.size();
    }
    if (item.type == Type::Symlink) {
        if (remote.linkValue.size() + bytes.size() > MaxLinkTarget) {
            done(index, "EFBIG:the link target is too long");
            return;
        }
        remote.linkValue += bytes;
        if (end) {
            done(index, {});
        }
        return;
    }
    if (item.type != Type::Regular) {
        done(index, "EINVAL:the terminal sent data for a " + std::string(typeName(item.type)));
        return;
    }
    if (!remote.opened) {
        remote.opened = true;
        remote.dir = openDirectory(index, true);
        if (!remote.dir.valid()) {
            done(index, errnoText(errno));
            return;
        }
        remote.out.reset(createTemporary(remote.dir.get(), ".aglow-transfer-", remote.temporary));
        if (!remote.out.valid()) {
            remote.temporary.clear();
            done(index, errnoText(errno));
            return;
        }
        if (remote.compressed) {
            remote.inflater = std::make_unique<ZlibStream>(ZlibStream::Mode::Inflate);
            if (!remote.inflater->valid()) {
                done(index, "ENOMEM");
                return;
            }
        }
        if (remote.rsync) {
            // The block size and the size of the signature that went out.
            remote.applier = std::make_unique<DeltaApplier>(remote.blockSize, remote.basisSize, MaxLiteral);
            remote.deltaTarget = std::make_unique<FdDeltaTarget>(remote.basis.get(), remote.out.get(), remote.blockSize, item.size);
            if (!remote.applier->valid()) {
                done(index, "ENOMEM");
                return;
            }
        }
    }
    bool ok = true;
    errno = 0;
    if (remote.compressed) {
        // Nothing may follow the end of the stream, in this packet or a later one.
        if (remote.streamEnded && !bytes.empty()) {
            errno = EINVAL;
            ok = false;
        }
        if (ok && !remote.streamEnded) {
            // The inflater reads the packet in place, and a delta step can
            // stop it before it took all: the bytes stay with the file.
            remote.inflateInput.assign(bytes);
            remote.inflater->setInput(remote.inflateInput);
            ok = inflate(index);
        }
    } else if (!bytes.empty()) {
        ok = write(index, bytes);
    }
    if (ok && remote.applier && remote.applier->hasWork()) {
        // The delta goes on in pull(), one step at a time, and the end of
        // the file is checked once it is applied.
        m_busy = index;
        m_busyEnd = end;
        m_hasWork = true;
        return;
    }
    endData(index, end, ok);
}

// Inflates the input of a compressed file until it is used, or until a step
// of its delta stops. False with errno set.
bool Receiver::inflate(std::size_t index)
{
    auto& remote = m_remote[index];
    char buffer[ReadStep];
    for (;;) {
        std::size_t produced = 0;
        const auto result = remote.inflater->step(buffer, sizeof buffer, produced);
        if (produced && !write(index, std::string_view(buffer, produced))) {
            return false;
        }
        if (result == ZlibStream::Result::End) {
            remote.streamEnded = true;
            if (remote.inflater->inputLeft() != 0) {
                errno = EINVAL;
                return false;
            }
            return true;
        }
        // Out of input: zlib reports no progress as an error.
        if (remote.inflater->inputLeft() == 0 && produced < sizeof buffer) {
            return true;
        }
        if (result == ZlibStream::Result::Error) {
            errno = EINVAL;
            return false;
        }
        if (remote.applier && remote.applier->hasWork()) {
            return true;
        }
    }
}

void Receiver::resume()
{
    const std::size_t index = m_busy;
    auto& remote = m_remote[index];
    errno = 0;
    bool ok = write(index, {});
    if (ok && !remote.applier->hasWork() && remote.compressed && !remote.streamEnded
        && remote.inflater->inputLeft() > 0) {
        ok = inflate(index);
    }
    if (ok && remote.applier->hasWork()) {
        return;
    }
    m_busy = SIZE_MAX;
    endData(index, m_busyEnd, ok);
}

void Receiver::abandon()
{
    if (m_busy != SIZE_MAX) {
        done(m_busy, errnoText(ECANCELED));
    }
}

// The rest of a data packet, once its bytes are written: the checks of the
// last packet, and the file taking its name. @p ok: the bytes went in.
void Receiver::endData(std::size_t index, bool end, bool ok)
{
    auto& remote = m_remote[index];
    auto& item = m_items[index];
    if (ok && end && remote.compressed && !remote.streamEnded) {
        errno = EINVAL;
        ok = false;
    }
    if (ok && end && remote.rsync && (!remote.applier->finished() || !remote.pending.empty())) {
        errno = EINVAL;
        ok = false;
    }
    if (ok && end && item.done != item.size) {
        errno = EIO;
        ok = false;
    }
    if (!ok) {
        done(index, errnoText(errno ? errno : EIO));
        return;
    }
    if (!end) {
        return;
    }
    // Mode and times go on the descriptor, before the file takes its name.
    // close() reports a write that failed late (on NFS, say): a file that
    // did not reach the disk whole never replaces the destination.
    remote.deltaTarget.reset();
    bool committed = ::fchmod(remote.out.get(), modeOf(remote, 0666)) == 0;
    if (committed && remote.mtime) {
        committed = ::futimens(remote.out.get(), Times(*remote.mtime).values) == 0;
    }
    committed = committed && ::close(remote.out.release()) == 0;
    committed = committed && ::renameat(remote.dir.get(), remote.temporary.c_str(), remote.dir.get(), remote.name.c_str()) == 0;
    if (!committed) {
        done(index, errnoText(errno));
        return;
    }
    remote.temporary.clear();
    done(index, {});
}

void Receiver::done(std::size_t index, std::string error)
{
    auto& remote = m_remote[index];
    auto& item = m_items[index];
    if (item.finished) {
        return;
    }
    item.finished = true;
    item.error = std::move(error);
    if (index == m_busy) {
        m_busy = SIZE_MAX;
    }
    if (index == m_signing) {
        m_signer.reset();
        std::string().swap(m_signBuffer);
        m_signSent = 0;
        m_signing = SIZE_MAX;
    }
    remote.discard();
    remote.inflater.reset();
    remote.applier.reset();
    std::string().swap(remote.pending);
    if (remote.requested) {
        --m_inFlight;
    }
    if (remote.rsync && item.error.empty()) {
        m_deltaFileBytes += item.size;
    }
    if (!remote.fid.empty()) {
        --m_outstanding;
    }
}

// A symbolic link or a hard link, made at a temporary name and renamed over
// the entry's name. A hard link names a file of this transfer that arrived.
bool Receiver::makeLink(std::size_t index, std::string& error)
{
    const auto& remote = m_remote[index];
    const auto& item = m_items[index];
    if (!remote.targetId.empty() && remote.target == SIZE_MAX) {
        error = "ENOENT:the link's target is not in the transfer";
        return false;
    }
    if (item.type == Type::Link
        && (m_items[remote.target].type != Type::Regular || !m_items[remote.target].finished || !m_items[remote.target].error.empty())) {
        error = "EINVAL:a hard link must name a file of the transfer that arrived";
        return false;
    }
    std::string text = remote.linkValue;
    if (item.type == Type::Symlink && remote.target != SIZE_MAX) {
        text = m_items[remote.target].local;
        if (!remote.linkValue.starts_with('/')) {
            text = TransferPath::relative(TransferPath::dirName(item.local), text);
        }
    }
    if (item.type == Type::Symlink && (text.empty() || text.find('\0') != std::string::npos)) {
        error = "EINVAL:the link has no target";
        return false;
    }
    UniqueFd from;
    if (item.type == Type::Link) {
        from = openDirectory(remote.target, false);
        if (!from.valid()) {
            error = errnoText(errno);
            return false;
        }
    }
    const auto dir = openDirectory(index, true);
    if (!dir.valid()) {
        error = errnoText(errno);
        return false;
    }
    std::string temporary;
    int result = -1;
    for (int attempt = 0; attempt < 64 && result != 0; ++attempt) {
        temporary = ".aglow-link-" + randomHex(6).value_or("0");
        result = item.type == Type::Link
            ? ::linkat(from.get(), m_remote[remote.target].name.c_str(), dir.get(), temporary.c_str(), 0)
            : ::symlinkat(text.c_str(), dir.get(), temporary.c_str());
        if (result != 0 && errno != EEXIST) {
            break;
        }
    }
    if (result == 0 && ::renameat(dir.get(), temporary.c_str(), dir.get(), remote.name.c_str()) != 0) {
        const int saved = errno;
        ::unlinkat(dir.get(), temporary.c_str(), 0);
        errno = saved;
        result = -1;
    }
    if (result != 0) {
        error = errnoText(errno);
        return false;
    }
    if (item.type == Type::Symlink && remote.mtime) {
        ::utimensat(dir.get(), remote.name.c_str(), Times(*remote.mtime).values, AT_SYMLINK_NOFOLLOW);
    }
    return true;
}

void Receiver::finalize()
{
    // Directories first, parents before children, so that links can go in them.
    for (const auto i : m_order) {
        auto& item = m_items[i];
        if (item.type != Type::Directory) {
            continue;
        }
        item.finished = true;
        auto& remote = m_remote[i];
        const auto dir = openDirectory(i, true);
        struct stat st { };
        if (!dir.valid()) {
            item.error = errnoText(errno);
        } else if (::mkdirat(dir.get(), remote.name.c_str(), 0700) == 0) {
            remote.created = true;
        } else if (errno != EEXIST) {
            item.error = errnoText(errno);
        } else if (::fstatat(dir.get(), remote.name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            item.error = errnoText(errno);
        } else if (!S_ISDIR(st.st_mode)) {
            item.error = errnoText(ENOTDIR);
        }
    }
    for (const auto i : m_order) {
        auto& item = m_items[i];
        if (item.type == Type::Regular || item.type == Type::Directory || !item.error.empty()) {
            continue;
        }
        item.finished = true;
        std::string error;
        if (!makeLink(i, error)) {
            item.error = error;
        }
    }
    // Children change the times of their directory: the deepest directories
    // go first. A directory that was there before keeps its mode and time:
    // a mirror of ~ must not make the home directory 0777.
    for (auto it = m_order.rbegin(); it != m_order.rend(); ++it) {
        const auto i = *it;
        const auto& remote = m_remote[i];
        if (m_items[i].type != Type::Directory || !m_items[i].error.empty() || !remote.created) {
            continue;
        }
        const auto dir = openDirectory(i, false);
        const UniqueFd fd(dir.valid() ? ::openat(dir.get(), remote.name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1);
        if (!fd.valid()) {
            continue;
        }
        ::fchmod(fd.get(), modeOf(remote, 0777));
        if (remote.mtime) {
            ::futimens(fd.get(), Times(*remote.mtime).values);
        }
    }
}

bool absolutePaths(const TransferClientOptions& options, std::string& error)
{
    if (!options.home.starts_with('/') || !options.cwd.starts_with('/')) {
        error = "The home and working directories must be absolute paths";
        return false;
    }
    return true;
}

} // namespace

std::unique_ptr<TransferClient> makeTransferSender(const TransferClientOptions& options,
                                                   const std::vector<std::string>& args, std::string& error)
{
    if (!absolutePaths(options, error)) {
        return nullptr;
    }
    const auto id = randomHex(8);
    if (!id) {
        error = "The kernel gave no random bytes for the session id";
        return nullptr;
    }
    auto sender = std::make_unique<Sender>(options, *id);
    if (!sender->scan(args, error)) {
        return nullptr;
    }
    sender->begin();
    return sender;
}

std::unique_ptr<TransferClient> makeTransferReceiver(const TransferClientOptions& options,
                                                     const std::vector<std::string>& args, std::string& error)
{
    if (!absolutePaths(options, error)) {
        return nullptr;
    }
    const auto id = randomHex(8);
    if (!id) {
        error = "The kernel gave no random bytes for the session id";
        return nullptr;
    }
    auto receiver = std::make_unique<Receiver>(options, *id);
    if (!receiver->setUp(args, error)) {
        return nullptr;
    }
    return receiver;
}

} // namespace AfterglowProto
