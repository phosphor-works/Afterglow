// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Delta.h>
#include <AfterglowProto/FdDelta.h>
#include <AfterglowProto/Random.h>
#include <AfterglowProto/TransferClient.h>
#include <AfterglowProto/Zlib.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace AfterglowProto {

namespace TransferPath {

std::string clean(std::string_view path)
{
    if (path.empty()) {
        return ".";
    }
    const bool rooted = path.front() == '/';
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = std::min(path.find('/', start), path.size());
        const auto part = path.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!rooted) {
                parts.push_back(part);
            }
            continue;
        }
        parts.push_back(part);
    }
    std::string out = rooted ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            out += '/';
        }
        out += parts[i];
    }
    return out.empty() ? "." : out;
}

std::string join(std::string_view a, std::string_view b)
{
    if (a.empty()) {
        return b.empty() ? std::string() : clean(b);
    }
    if (b.empty()) {
        return clean(a);
    }
    return clean(std::string(a) + "/" + std::string(b));
}

std::string baseName(std::string_view path)
{
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    if (path.empty()) {
        return ".";
    }
    if (path == "/") {
        return "/";
    }
    const auto at = path.rfind('/');
    return std::string(at == std::string_view::npos ? path : path.substr(at + 1));
}

std::string dirName(std::string_view path)
{
    const auto at = path.rfind('/');
    if (at == std::string_view::npos) {
        return ".";
    }
    return clean(path.substr(0, at + 1));
}

std::string relative(std::string_view base, std::string_view target)
{
    const auto b = clean(base);
    const auto t = clean(target);
    auto split = [](const std::string& p) {
        std::vector<std::string> parts;
        std::size_t start = 1;
        while (start < p.size()) {
            const auto end = std::min(p.find('/', start), p.size());
            parts.emplace_back(p.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    };
    const auto bp = split(b);
    const auto tp = split(t);
    std::size_t common = 0;
    while (common < bp.size() && common < tp.size() && bp[common] == tp[common]) {
        ++common;
    }
    std::string out;
    for (std::size_t i = common; i < bp.size(); ++i) {
        out += out.empty() ? ".." : "/..";
    }
    for (std::size_t i = common; i < tp.size(); ++i) {
        if (!out.empty()) {
            out += '/';
        }
        out += tp[i];
    }
    return out.empty() ? "." : out;
}

std::string absolute(std::string_view path, std::string_view home, std::string_view cwd)
{
    if (path == "~") {
        return clean(home);
    }
    if (path.starts_with("~/")) {
        return join(home, path.substr(2));
    }
    if (path.starts_with("/")) {
        return clean(path);
    }
    return join(cwd, path);
}

} // namespace TransferPath

namespace {

using Type = TransferItem::Type;
using Fields = std::vector<std::pair<std::string_view, std::string_view>>;

// The most file data in one data message, before base64 (kitty's chunk).
constexpr std::size_t PacketData = TransferMaxData;
// Raw bytes read from a file for one step of compression or of a delta.
constexpr std::size_t ReadStep = 64 * 1024;
// Bytes of a delta scanned in one step: a refill reads up to two blocks more.
constexpr std::size_t DeltaStep = 1024 * 1024;
// A signature of the largest block size for a file of 1 TiB is 20 MiB.
constexpr std::size_t MaxSignature = 64 * 1024 * 1024;
// One literal of a delta that the receiver buffers whole.
constexpr std::size_t MaxLiteral = 16 * 1024 * 1024;
// Afterglow refuses a regular file with EBUSY when 128 are open in one pane.
// A client keeps well below it: files wait for their metadata or their request.
constexpr std::size_t MaxOpenFiles = 32;
constexpr std::size_t MaxListing = 1 << 20;
constexpr std::size_t MaxLinkTarget = 4096;

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
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

// A temporary file next to @p path, renamed over it when complete. It gets
// the mode of a new file, which the listing's mode replaces at the end.
int temporaryNear(const std::string& path, std::string& name)
{
    name = TransferPath::dirName(path) + "/.aglow-transfer-XXXXXX";
    const int fd = ::mkostemp(name.data(), O_CLOEXEC);
    if (fd >= 0) {
        const mode_t mask = ::umask(0);
        ::umask(mask);
        ::fchmod(fd, 0666 & ~mask);
    }
    return fd;
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
    std::string& ready() { return m_ready; }

private:
    std::unique_ptr<ZlibStream> m_deflater;
    std::string m_ready;
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
    const auto decoded = decodeTransfer(payload);
    if (!decoded || field(*decoded, "id") != m_id) {
        return;
    }
    const auto& f = *decoded;
    if (field(f, "ac") == "status" && field(f, "st") == TransferStatus::Canceled) {
        m_state = State::Canceled;
        return;
    }
    if (m_state == State::Done || m_state == State::Failed || m_state == State::Canceled || m_state == State::Canceling) {
        return;
    }
    onReply(f);
}

void TransferClient::pull(std::string& out, std::size_t budget)
{
    const std::size_t start = out.size();
    out += m_output;
    m_output.clear();
    if (m_state == State::Running && out.size() - start < budget) {
        produce(out, budget - (out.size() - start));
    }
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
        bool compress = false;
        bool rsyncCapable = false;
        enum class Phase : std::uint8_t { Unsent, Waiting, Signature, Ready, Sending, Sent, Acked } phase = Phase::Unsent;
        int fd = -1;
        std::string signature;
        std::unique_ptr<FdSource> source;
        std::unique_ptr<DeltaGenerator> generator;
        std::unique_ptr<Packer> packer;
        bool inputDone = false;
        bool ended = false;
        File() = default;
        File(File&&) = default;
        ~File()
        {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    };

    Sender(TransferClientOptions options, std::string id)
        : TransferClient(std::move(options), std::move(id))
    {
    }

    bool scan(const std::vector<std::string>& args, std::string& error);
    void start()
    {
        std::string bypass;
        Fields f { { "ac", "send" }, { "id", m_id } };
        if (!m_options.password.empty()) {
            bypass = transferPasswordHash(m_id, m_options.password);
            f.emplace_back("pw", bypass);
        }
        queue(encodeTransfer(f));
    }

protected:
    void onReply(const TransferFields& f) override;
    void produce(std::string& out, std::size_t budget) override;
    void proceed() override { m_transmitting = true; }

private:
    void addPath(const std::string& shown, const std::string& remoteBase, int& counter, std::string& error);
    void sendMetadata(std::string& out);
    void finishItem(std::size_t index, std::string error);
    bool allAcked() const;
    bool pack(std::size_t index, std::string& out, std::size_t budget);
    std::size_t openRegular() const;

    std::vector<File> m_files;
    std::map<std::string, std::size_t> m_byFid;
    bool m_granted = false;
    bool m_transmitting = false;
    bool m_finishSent = false;
    std::size_t m_metadataSent = 0;
    std::size_t m_active = SIZE_MAX;
};

void Sender::addPath(const std::string& shown, const std::string& remoteBase, int& counter, std::string& error)
{
    if (!error.empty()) {
        return;
    }
    const std::string expanded = TransferPath::absolute(shown, m_options.home, m_options.cwd);
    File file;
    if (::lstat(expanded.c_str(), &file.st) != 0) {
        error = "Failed to stat " + shown + ": " + std::strerror(errno);
        return;
    }
    // kitten: the remote path of a source is the base itself, or the base
    // directory (ending in /) and the source's name.
    std::string remote;
    if (remoteBase.empty()) {
        remote = shown;
    } else if (remoteBase.ends_with('/')) {
        remote = TransferPath::join(remoteBase, TransferPath::baseName(shown));
    } else {
        remote = remoteBase;
    }
    TransferItem item;
    item.local = shown;
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
    file.path = expanded;
    char id[20];
    const auto [end, ec] = std::to_chars(id, id + sizeof id, ++counter, 16);
    file.fid.assign(id, end);
    file.rsyncCapable = item.type == Type::Regular && item.size > 4096;
    file.compress = item.type == Type::Regular && item.size > 4096 && transferShouldCompress(expanded, m_options.compress);
    m_items.push_back(item);
    m_files.push_back(std::move(file));
    if (item.type != Type::Directory) {
        return;
    }
    std::vector<std::string> children;
    int failure = 0;
    if (!listDirectory(expanded, children, failure)) {
        error = "Failed to read the directory " + shown + ": " + std::strerror(failure);
        return;
    }
    std::string base = remoteBase;
    if (!base.empty()) {
        while (base.size() > 1 && base.ends_with('/')) {
            base.pop_back();
        }
        base += "/" + TransferPath::baseName(shown) + "/";
    } else {
        base = shown;
        while (base.size() > 1 && base.ends_with('/')) {
            base.pop_back();
        }
        base += "/";
    }
    for (const auto& child : children) {
        addPath(TransferPath::join(shown, child), base, counter, error);
    }
}

bool Sender::scan(const std::vector<std::string>& args, std::string& error)
{
    std::vector<std::string> paths;
    std::string remoteBase;
    if (m_options.mode == TransferClientOptions::Mode::Mirror) {
        if (args.empty()) {
            error = "Must specify at least one file to transfer";
            return false;
        }
        const auto home = TransferPath::clean(m_options.home);
        for (const auto& arg : args) {
            auto path = TransferPath::absolute(arg, m_options.home, m_options.cwd);
            // Files under this home go under the home of the other computer.
            if (home != "/" && path.starts_with(home + "/")) {
                path = "~/" + TransferPath::relative(home, path);
            }
            paths.push_back(path);
        }
    } else {
        if (args.size() < 2) {
            error = "Must specify at least one local path and one remote path";
            return false;
        }
        remoteBase = args.back();
        if (args.size() > 2 && !remoteBase.ends_with('/')) {
            remoteBase += '/';
        }
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            paths.push_back(TransferPath::absolute(args[i], m_options.home, m_options.cwd));
        }
    }
    int counter = 0;
    for (const auto& path : paths) {
        addPath(path, remoteBase, counter, error);
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
            file.compress = file.rsyncCapable = false;
        }
    }
    // A symlink to a file of the transfer points at its copy.
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        auto& file = m_files[i];
        if (m_items[i].type != Type::Symlink) {
            continue;
        }
        std::string target(4096, '\0');
        const auto n = ::readlink(file.path.c_str(), target.data(), target.size());
        if (n < 0 || std::size_t(n) >= target.size()) {
            m_items[i].error = "EIO:could not read the link";
            continue;
        }
        target.resize(std::size_t(n));
        file.linkData = "path:" + target;
        const bool absolute = target.starts_with('/');
        const auto resolved = absolute ? target : TransferPath::join(TransferPath::dirName(file.path), target);
        struct stat st { };
        if (::stat(resolved.c_str(), &st) == 0) {
            if (const auto it = inodes.find({ st.st_dev, st.st_ino }); it != inodes.end()) {
                file.linkData = (absolute ? "fid_abs:" : "fid:") + m_files[it->second].fid;
            }
        }
    }
    for (std::size_t i = 0; i < m_files.size(); ++i) {
        m_byFid[m_files[i].fid] = i;
        if (!m_items[i].error.empty()) {
            m_items[i].finished = true;
            m_files[i].phase = File::Phase::Acked;
        }
    }
    return true;
}

std::size_t Sender::openRegular() const
{
    std::size_t open = 0;
    for (std::size_t i = 0; i < m_metadataSent; ++i) {
        if (m_items[i].type == Type::Regular && m_files[i].phase != File::Phase::Acked) {
            ++open;
        }
    }
    return open;
}

void Sender::sendMetadata(std::string& out)
{
    // Metadata goes out as the terminal finishes files, so that a large
    // tree never holds more than MaxOpenFiles regular files open there.
    std::size_t open = openRegular();
    while (m_metadataSent < m_files.size()) {
        const std::size_t i = m_metadataSent;
        auto& file = m_files[i];
        const auto& item = m_items[i];
        if (item.type == Type::Regular && open >= MaxOpenFiles) {
            return;
        }
        ++m_metadataSent;
        if (file.phase == File::Phase::Acked) {
            continue;
        }
        const bool rsync = m_options.deltas && file.rsyncCapable;
        const std::string mode = number(file.st.st_mode & 07777);
        const std::string mtime
            = number(std::int64_t(file.st.st_mtim.tv_sec) * 1000000000 + std::int64_t(file.st.st_mtim.tv_nsec));
        const std::string size = number(std::int64_t(item.size));
        Fields f { { "ac", "file" }, { "id", m_id }, { "fid", file.fid }, { "n", item.remote },
            { "ft", typeName(item.type) }, { "prm", mode }, { "mod", mtime } };
        if (item.type == Type::Regular) {
            f.emplace_back("sz", size);
        }
        if (file.compress) {
            f.emplace_back("zip", "zlib");
        }
        if (rsync) {
            f.emplace_back("tt", "rsync");
        }
        out += encodeTransfer(f);
        file.phase = File::Phase::Waiting;
        if (item.type == Type::Regular) {
            ++open;
        }
    }
}

void Sender::finishItem(std::size_t index, std::string error)
{
    auto& file = m_files[index];
    auto& item = m_items[index];
    item.finished = true;
    if (item.error.empty()) {
        item.error = std::move(error);
    }
    file.phase = File::Phase::Acked;
    file.generator.reset();
    file.source.reset();
    file.packer.reset();
    std::string().swap(file.signature);
    if (file.fd >= 0) {
        ::close(file.fd);
        file.fd = -1;
    }
    if (m_active == index) {
        m_active = SIZE_MAX;
    }
}

bool Sender::allAcked() const
{
    return m_metadataSent == m_files.size()
        && std::all_of(m_files.begin(), m_files.end(), [](const File& f) { return f.phase == File::Phase::Acked; });
}

void Sender::onReply(const TransferFields& f)
{
    const auto action = field(f, "ac");
    const auto status = field(f, "st");
    const auto fid = field(f, "fid");
    if (fid.empty()) {
        if (action != "status") {
            return;
        }
        if (!m_granted && status == TransferStatus::Ok) {
            m_granted = true;
            m_state = State::Running;
            return;
        }
        fail(m_granted ? status : "Permission denied for this transfer: " + status);
        return;
    }
    const auto found = m_byFid.find(fid);
    if (found == m_byFid.end()) {
        return;
    }
    const std::size_t i = found->second;
    auto& file = m_files[i];
    auto& item = m_items[i];
    if (file.phase == File::Phase::Acked) {
        return;
    }
    if (action == "data" || action == "end_data") {
        if (file.phase != File::Phase::Signature) {
            return;
        }
        const auto data = field(f, "d");
        m_signatureBytes += data.size();
        if (file.signature.size() + data.size() > MaxSignature) {
            cancel("The terminal sent a signature that is too large");
            return;
        }
        file.signature += data;
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
            file.phase = File::Phase::Ready;
            item.delta = true;
            m_deltaFileBytes += item.size;
        }
        return;
    }
    if (action != "status") {
        return;
    }
    if (status == TransferStatus::Started) {
        if (file.phase != File::Phase::Waiting) {
            return;
        }
        if (item.type == Type::Directory) {
            return;
        }
        file.phase = field(f, "tt") == "rsync" && item.type == Type::Regular ? File::Phase::Signature : File::Phase::Ready;
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
    finishItem(i, status);
}

bool Sender::pack(std::size_t index, std::string& out, std::size_t budget)
{
    auto& file = m_files[index];
    auto& item = m_items[index];
    const std::size_t start = out.size();
    if (item.type == Type::Symlink || item.type == Type::Link) {
        const Fields f { { "ac", "end_data" }, { "id", m_id }, { "fid", file.fid }, { "d", file.linkData } };
        out += encodeTransfer(f);
        file.phase = File::Phase::Sent;
        return true;
    }
    if (file.fd < 0) {
        file.fd = ::open(file.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (file.fd < 0) {
            const int error = errno;
            // The terminal holds the file open: end it there too.
            const Fields f { { "ac", "end_data" }, { "id", m_id }, { "fid", file.fid } };
            out += encodeTransfer(f);
            finishItem(index, errnoText(error));
            return true;
        }
        // Only the announced size is sent, also when the file grows meanwhile.
        file.source = std::make_unique<FdSource>(file.fd, item.size);
        file.packer = std::make_unique<Packer>(file.compress);
        if (!file.packer->valid()) {
            cancel("Out of memory");
            return false;
        }
    }
    auto& ready = file.packer->ready();
    while (out.size() - start < budget) {
        // Refill until a whole packet is ready or the input ends.
        while (ready.size() <= PacketData && !file.inputDone) {
            std::string raw;
            bool ok = true;
            if (file.generator) {
                ok = file.generator->produce(*file.source, raw, ReadStep, DeltaStep);
                file.inputDone = ok && file.generator->done();
                m_deltaBytes += raw.size();
            } else {
                ok = file.source->read(raw, ReadStep);
                file.inputDone = ok && file.source->atEnd();
            }
            if (!ok) {
                const int error = errno;
                const Fields f { { "ac", "end_data" }, { "id", m_id }, { "fid", file.fid } };
                out += encodeTransfer(f);
                finishItem(index, errnoText(error ? error : EIO));
                return true;
            }
            if (!file.packer->add(raw, file.inputDone)) {
                cancel("Compression failed");
                return false;
            }
        }
        const std::size_t take = std::min(ready.size(), PacketData);
        const bool last = file.inputDone && take == ready.size();
        const std::string_view data(ready.data(), take);
        Fields f { { "ac", last ? "end_data" : "data" }, { "id", m_id }, { "fid", file.fid } };
        if (!data.empty()) {
            f.emplace_back("d", data);
        }
        out += encodeTransfer(f);
        ready.erase(0, take);
        if (last) {
            file.phase = File::Phase::Sent;
            ::close(file.fd);
            file.fd = -1;
            file.source.reset();
            file.generator.reset();
            file.packer.reset();
            return true;
        }
    }
    return false;
}

void Sender::produce(std::string& out, std::size_t budget)
{
    const std::size_t start = out.size();
    if (m_granted) {
        sendMetadata(out);
    }
    if (m_options.confirmPaths && !m_transmitting) {
        // Ask once every file has an answer, as kitten does, so that the list shows what exists.
        if ((m_metadataSent == m_files.size() || openRegular() >= MaxOpenFiles) && std::none_of(m_files.begin(), m_files.end(), [](const File& f) {
                return f.phase == File::Phase::Waiting;
            })) {
            m_state = State::Confirm;
        }
        return;
    }
    m_transmitting = true;
    while (out.size() - start < budget && m_state == State::Running) {
        if (m_active == SIZE_MAX) {
            for (std::size_t i = 0; i < m_files.size(); ++i) {
                if (m_files[i].phase == File::Phase::Ready) {
                    m_active = i;
                    m_files[i].phase = File::Phase::Sending;
                    break;
                }
            }
        }
        if (m_active == SIZE_MAX) {
            break;
        }
        const std::size_t index = m_active;
        if (pack(index, out, budget - (out.size() - start)) && m_active == index) {
            m_active = SIZE_MAX;
        }
        if (m_granted) {
            sendMetadata(out);
        }
    }
    if (m_state == State::Running && !m_finishSent && allAcked()) {
        m_finishSent = true;
        const Fields f { { "ac", "finish" }, { "id", m_id } };
        out += encodeTransfer(f);
        m_state = State::Done;
    }
}

// ── Receiving ───────────────────────────────────────────────────────────────

class Receiver final : public TransferClient
{
public:
    struct Remote
    {
        std::size_t spec = 0;
        std::string remoteId; // st: the terminal's id of the entry
        std::string parent; // pr
        std::string target; // d: the id of a link's target
        std::optional<std::int64_t> mtime;
        std::optional<std::int64_t> mode;
        std::string fid; // the id of the data request
        bool requested = false;
        bool rsync = false;
        bool compressed = false;
        bool opened = false;
        int out = -1;
        int basis = -1;
        std::string temporary;
        std::string linkValue;
        std::unique_ptr<ZlibStream> inflater;
        std::unique_ptr<DeltaApplier> applier;
        std::unique_ptr<FdDeltaTarget> deltaTarget;
        std::string pending;
        std::uint64_t received = 0;
        std::uint32_t blockSize = 0;
        std::uint64_t basisSize = 0;
        Remote() = default;
        Remote(Remote&&) = default;
        ~Remote() { close(); }
        void close()
        {
            if (out >= 0) {
                ::close(out);
                out = -1;
            }
            if (basis >= 0) {
                ::close(basis);
                basis = -1;
            }
        }
    };

    Receiver(TransferClientOptions options, std::string id)
        : TransferClient(std::move(options), std::move(id))
    {
    }

    bool setUp(const std::vector<std::string>& args, std::string& error)
    {
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
        std::string bypass;
        const std::string count = number(std::int64_t(m_specs.size()));
        Fields f { { "ac", "receive" }, { "id", m_id }, { "sz", count } };
        if (!m_options.password.empty()) {
            bypass = transferPasswordHash(m_id, m_options.password);
            f.emplace_back("pw", bypass);
        }
        queue(encodeTransfer(f));
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
    void proceed() override { }

private:
    void listed(const TransferFields& f);
    bool collect();
    bool place(std::vector<std::size_t>& order, const std::vector<std::size_t>& entries, const std::string& base);
    void data(std::size_t index, const TransferFields& f, bool end);
    void done(std::size_t index, std::string error);
    void finalize();
    bool request(std::size_t index, std::string& out, std::size_t budget);

    std::vector<std::string> m_specs;
    std::string m_destination;
    std::vector<std::size_t> m_counts;
    std::vector<std::string> m_failedSpecs;
    std::string m_remoteHome;
    bool m_granted = false;
    bool m_listed = false;
    std::vector<Remote> m_remote;
    std::vector<std::size_t> m_order; // the entries placed on this computer, parents first
    std::map<std::string, std::size_t> m_byFid;
    std::size_t m_nextRequest = 0;
    std::size_t m_inFlight = 0;
    std::size_t m_outstanding = 0;
    // A delta request whose signature is being sent.
    std::size_t m_signing = SIZE_MAX;
    int m_signFd = -1;
    std::unique_ptr<SignatureBuilder> m_signer;
    std::uint64_t m_signSize = 0;
    std::uint64_t m_signOffset = 0;
    std::string m_signBuffer;
    bool m_finishSent = false;
};

void Receiver::onReply(const TransferFields& f)
{
    const auto action = field(f, "ac");
    const auto status = field(f, "st");
    const auto fid = field(f, "fid");
    if (!m_granted) {
        if (action != "status") {
            fail("Unexpected response from the terminal");
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
            std::size_t spec = 0;
            const auto [end, ec] = std::from_chars(fid.data(), fid.data() + fid.size(), spec);
            if (ec != std::errc() || end != fid.data() + fid.size() || spec >= m_specs.size()) {
                cancel("Unexpected response from the terminal (unknown file id)");
                return;
            }
            m_failedSpecs.push_back(m_specs[spec] + ": " + (status.starts_with("ENOENT") ? "File not found" : status));
            return;
        }
        if (status != TransferStatus::Ok) {
            fail(status);
            return;
        }
        m_listed = true;
        m_remoteHome = field(f, "n");
        if (!m_failedSpecs.empty()) {
            std::string message = "Failed to process some sources:";
            for (const auto& failure : m_failedSpecs) {
                message += "\n  " + failure;
            }
            cancel(message);
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
        m_state = m_options.confirmPaths ? State::Confirm : State::Running;
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
    if (action == "data" || action == "end_data") {
        data(index, f, action == "end_data");
    } else if (action == "status" && !transferSuccess(status)) {
        done(index, status);
    }
}

void Receiver::listed(const TransferFields& f)
{
    const auto fid = field(f, "fid");
    std::size_t spec = 0;
    const auto [end, ec] = std::from_chars(fid.data(), fid.data() + fid.size(), spec);
    if (ec != std::errc() || end != fid.data() + fid.size() || spec >= m_specs.size()) {
        cancel("Unexpected response from the terminal (unknown file id)");
        return;
    }
    if (m_remote.size() >= MaxListing) {
        cancel("The terminal listed too many files");
        return;
    }
    Remote remote;
    remote.spec = spec;
    remote.remoteId = field(f, "st");
    remote.parent = field(f, "pr");
    remote.target = field(f, "d");
    remote.mtime = parseNumber(f, "mod");
    remote.mode = parseNumber(f, "prm");
    TransferItem item;
    item.remote = field(f, "n");
    const auto type = field(f, "ft");
    item.type = type == "directory" ? Type::Directory : type == "symlink" ? Type::Symlink : type == "link" ? Type::Link : Type::Regular;
    if (const auto size = parseNumber(f, "sz"); size && *size > 0 && item.type == Type::Regular) {
        item.size = std::uint64_t(*size);
    }
    const auto name = TransferPath::baseName(item.remote);
    if (remote.remoteId.empty() || item.remote.empty() || name == "." || name == ".." || name == "/"
        || item.remote.find('\0') != std::string::npos) {
        cancel("The terminal listed an invalid path: " + item.remote);
        return;
    }
    ++m_counts[spec];
    m_remote.push_back(std::move(remote));
    m_items.push_back(std::move(item));
}

// Places @p entries (one spec) under @p base, as kitten's make_tree does: each
// entry goes into the local copy of its parent.
bool Receiver::place(std::vector<std::size_t>& order, const std::vector<std::size_t>& entries, const std::string& base)
{
    std::map<std::string, std::size_t> byId;
    for (const auto i : entries) {
        byId[m_remote[i].remoteId] = i;
    }
    std::map<std::size_t, bool> placed;
    // Each entry needs its parent's path first. The listing can come in any
    // order, and a parent that is missing or a cycle stops the transfer.
    std::function<bool(std::size_t, int)> placeOne = [&](std::size_t i, int depth) {
        if (placed.contains(i)) {
            return true;
        }
        if (depth > 4096) {
            return false;
        }
        const auto& parent = m_remote[i].parent;
        std::string where = base;
        if (!parent.empty()) {
            const auto it = byId.find(parent);
            if (it == byId.end() || !placeOne(it->second, depth + 1)) {
                return false;
            }
            where = m_items[it->second].local;
        }
        m_items[i].local = TransferPath::join(where, TransferPath::baseName(m_items[i].remote));
        placed[i] = true;
        order.push_back(i);
        return true;
    };
    for (const auto i : entries) {
        if (!placeOne(i, 0)) {
            return false;
        }
    }
    return true;
}

bool Receiver::collect()
{
    std::vector<std::vector<std::size_t>> bySpec(m_specs.size());
    for (std::size_t i = 0; i < m_remote.size(); ++i) {
        bySpec[m_remote[i].spec].push_back(i);
    }
    // The root of a spec is its shortest path.
    std::vector<std::string> specPaths(m_specs.size());
    for (std::size_t s = 0; s < m_specs.size(); ++s) {
        auto& entries = bySpec[s];
        std::stable_sort(entries.begin(), entries.end(),
                         [&](std::size_t a, std::size_t b) { return m_items[a].remote.size() < m_items[b].remote.size(); });
        specPaths[s] = m_items[entries.front()].remote;
    }
    const auto local = [&](const std::string& path) { return TransferPath::absolute(path, m_options.home, m_options.cwd); };
    if (m_options.mode == TransferClientOptions::Mode::Mirror) {
        // Paths under the remote home go under the home of this computer.
        std::string home = m_remoteHome;
        while (home.size() > 1 && home.ends_with('/')) {
            home.pop_back();
        }
        std::string common = specPaths.front();
        for (const auto& path : specPaths) {
            while (!(path == common || path.starts_with(common + "/") || common == "/")) {
                common = TransferPath::dirName(common);
            }
        }
        if (!home.empty() && (common == home || common.starts_with(home + "/"))) {
            for (auto& path : specPaths) {
                path = "~/" + TransferPath::relative(home, path);
            }
        }
        for (std::size_t s = 0; s < m_specs.size(); ++s) {
            if (!place(m_order, bySpec[s], TransferPath::dirName(local(specPaths[s])))) {
                cancel("The terminal sent a listing without the parents of its files");
                return false;
            }
        }
    } else {
        struct stat st { };
        const auto destination = local(m_destination);
        const bool intoDirectory = m_destination.ends_with('/') || m_remote.size() > 1
            || (::stat(destination.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        for (std::size_t s = 0; s < m_specs.size(); ++s) {
            if (intoDirectory) {
                if (!place(m_order, bySpec[s], destination)) {
                    cancel("The terminal sent a listing without the parents of its files");
                    return false;
                }
            } else {
                const auto i = bySpec[s].front();
                m_items[i].local = destination;
                m_order.push_back(i);
            }
        }
    }
    for (const auto i : m_order) {
        const auto& item = m_items[i];
        // Directories are made at the end, and a hard link to a listed file is made locally.
        if (item.type == Type::Directory || (item.type == Type::Link && !m_remote[i].target.empty())) {
            continue;
        }
        m_remote[i].fid = "r" + number(std::int64_t(i));
        m_byFid[m_remote[i].fid] = i;
        ++m_outstanding;
    }
    return true;
}

bool Receiver::request(std::size_t index, std::string& out, std::size_t budget)
{
    auto& remote = m_remote[index];
    const auto& item = m_items[index];
    const std::size_t start = out.size();
    if (!remote.requested) {
        remote.requested = true;
        ++m_inFlight;
        struct stat st { };
        const bool exists = ::lstat(item.local.c_str(), &st) == 0;
        remote.rsync = m_options.deltas && item.type == Type::Regular && exists && S_ISREG(st.st_mode) && st.st_size > 4096;
        remote.compressed = item.type == Type::Regular && item.size > 4096 && transferShouldCompress(item.remote, m_options.compress);
        Fields f { { "ac", "file" }, { "id", m_id }, { "fid", remote.fid }, { "n", item.remote },
            { "tt", remote.rsync ? "rsync" : "simple" } };
        if (remote.compressed) {
            f.emplace_back("zip", "zlib");
        }
        out += encodeTransfer(f);
        if (!remote.rsync) {
            return true;
        }
        m_signFd = ::open(item.local.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (m_signFd < 0 || ::fstat(m_signFd, &st) != 0) {
            // The terminal waits for a signature: an empty one is invalid and ends the file there.
            const Fields end { { "ac", "end_data" }, { "id", m_id }, { "fid", remote.fid } };
            out += encodeTransfer(end);
            done(index, errnoText(errno));
            return true;
        }
        m_signing = index;
        m_signSize = std::uint64_t(st.st_size);
        m_signOffset = 0;
        m_signer = std::make_unique<SignatureBuilder>(m_signSize);
        m_signBuffer = m_signer->header();
        remote.blockSize = m_signer->blockSize();
        remote.basisSize = m_signSize;
        remote.basis = ::dup(m_signFd);
    }
    // The signature, a few blocks at a time.
    const std::uint32_t block = m_signer->blockSize();
    while (out.size() - start < budget) {
        while (m_signBuffer.size() < PacketData && m_signOffset < m_signSize) {
            const auto want = std::size_t(std::min<std::uint64_t>(block, m_signSize - m_signOffset));
            std::string bytes(want, '\0');
            std::size_t have = 0;
            while (have < want) {
                const ssize_t n = ::pread(m_signFd, bytes.data() + have, want - have, off_t(m_signOffset + have));
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
            m_signBuffer += m_signer->record(bytes);
            m_signOffset += want;
        }
        const bool last = m_signOffset >= m_signSize && m_signBuffer.size() <= PacketData;
        const std::string_view data(m_signBuffer.data(), std::min(m_signBuffer.size(), PacketData));
        Fields f { { "ac", last ? "end_data" : "data" }, { "id", m_id }, { "fid", remote.fid } };
        if (!data.empty()) {
            f.emplace_back("d", data);
        }
        out += encodeTransfer(f);
        m_signatureBytes += data.size();
        m_signBuffer.erase(0, data.size());
        if (last) {
            ::close(m_signFd);
            m_signFd = -1;
            m_signer.reset();
            m_signing = SIZE_MAX;
            return true;
        }
    }
    return false;
}

void Receiver::produce(std::string& out, std::size_t budget)
{
    const std::size_t start = out.size();
    while (out.size() - start < budget && m_state == State::Running) {
        if (m_signing != SIZE_MAX) {
            if (!request(m_signing, out, budget - (out.size() - start))) {
                return;
            }
            continue;
        }
        if (m_inFlight >= MaxOpenFiles) {
            break;
        }
        while (m_nextRequest < m_order.size() && m_remote[m_order[m_nextRequest]].fid.empty()) {
            ++m_nextRequest;
        }
        if (m_nextRequest >= m_order.size()) {
            break;
        }
        request(m_order[m_nextRequest++], out, budget - (out.size() - start));
    }
    if (m_state == State::Running && m_outstanding == 0 && !m_finishSent) {
        finalize();
        m_finishSent = true;
        const Fields f { { "ac", "finish" }, { "id", m_id } };
        out += encodeTransfer(f);
        m_state = State::Done;
    }
}

void Receiver::data(std::size_t index, const TransferFields& f, bool end)
{
    auto& remote = m_remote[index];
    auto& item = m_items[index];
    if (!remote.requested || index == m_signing) {
        cancel("The terminal sent data for a file before its request");
        return;
    }
    const auto bytes = field(f, "d");
    remote.received += bytes.size();
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
        if (!makeDirectories(TransferPath::dirName(item.local))) {
            done(index, errnoText(errno));
            return;
        }
        remote.out = temporaryNear(item.local, remote.temporary);
        if (remote.out < 0) {
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
            const std::uint32_t block = remote.blockSize;
            remote.applier = std::make_unique<DeltaApplier>(block, remote.basisSize, MaxLiteral);
            // The new file is no larger than its announced size, when the terminal gave one.
            const std::uint64_t limit = item.size ? item.size : std::numeric_limits<std::uint64_t>::max();
            remote.deltaTarget = std::make_unique<FdDeltaTarget>(remote.basis, remote.out, block, limit);
            if (!remote.applier->valid()) {
                done(index, "ENOMEM");
                return;
            }
        }
    }
    auto write = [&](std::string_view chunk) {
        if (!remote.rsync) {
            if (!writeAll(remote.out, chunk)) {
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
        const auto used = remote.applier->apply(remote.pending, *remote.deltaTarget);
        if (!used) {
            errno = EINVAL;
            return false;
        }
        remote.pending.erase(0, *used);
        item.done = remote.deltaTarget->written();
        return true;
    };
    bool ok = true;
    if (remote.inflater) {
        remote.inflater->setInput(bytes);
        char buffer[ReadStep];
        for (;;) {
            std::size_t produced = 0;
            const auto result = remote.inflater->step(buffer, sizeof buffer, produced);
            if (produced && !write(std::string_view(buffer, produced))) {
                ok = false;
                break;
            }
            if (result == ZlibStream::Result::End) {
                remote.inflater.reset();
                break;
            }
            // Out of input: zlib reports no progress as an error.
            if (remote.inflater->inputLeft() == 0 && produced < sizeof buffer) {
                break;
            }
            if (result == ZlibStream::Result::Error) {
                errno = EINVAL;
                ok = false;
                break;
            }
        }
        if (ok && end && remote.inflater) {
            errno = EINVAL;
            ok = false;
        }
    } else if (!bytes.empty()) {
        ok = write(bytes);
    }
    if (ok && end && remote.rsync && (!remote.applier->finished() || !remote.pending.empty())) {
        errno = EINVAL;
        ok = false;
    }
    if (!ok) {
        done(index, errnoText(errno ? errno : EIO));
        return;
    }
    if (!end) {
        return;
    }
    remote.close();
    if (::rename(remote.temporary.c_str(), item.local.c_str()) != 0) {
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
    if (index == m_signing) {
        ::close(m_signFd);
        m_signFd = -1;
        m_signer.reset();
        m_signing = SIZE_MAX;
    }
    remote.close();
    remote.inflater.reset();
    remote.applier.reset();
    remote.deltaTarget.reset();
    std::string().swap(remote.pending);
    if (!remote.temporary.empty()) {
        ::unlink(remote.temporary.c_str());
        remote.temporary.clear();
    }
    if (remote.requested) {
        --m_inFlight;
    }
    if (item.delta || remote.rsync) {
        item.delta = true;
        m_deltaFileBytes += item.size;
    }
    if (!remote.fid.empty()) {
        --m_outstanding;
    }
}

void Receiver::finalize()
{
    std::map<std::string, std::size_t> byId;
    for (const auto i : m_order) {
        byId[m_remote[i].remoteId] = i;
    }
    auto setTimes = [&](std::size_t i) {
        const auto& remote = m_remote[i];
        const auto& path = m_items[i].local;
        if (remote.mode && m_items[i].type != Type::Symlink && m_items[i].type != Type::Link) {
            ::chmod(path.c_str(), mode_t(*remote.mode & 07777));
        }
        if (remote.mtime) {
            const auto ns = *remote.mtime;
            timespec time { time_t(ns / 1000000000), long(ns % 1000000000) };
            if (time.tv_nsec < 0) {
                time.tv_nsec += 1000000000;
                --time.tv_sec;
            }
            const timespec times[2] = { time, time };
            ::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW);
        }
    };
    for (const auto i : m_order) {
        auto& item = m_items[i];
        const auto& remote = m_remote[i];
        if (item.type == Type::Directory) {
            if (!makeDirectories(item.local)) {
                item.error = errnoText(errno);
            }
            item.finished = true;
            continue;
        }
        if (item.type == Type::Regular || !item.error.empty()) {
            continue;
        }
        if (item.type == Type::Link && remote.target.empty()) {
            continue; // requested as a file, and failed or arrived
        }
        std::string target;
        if (!remote.target.empty()) {
            const auto it = byId.find(remote.target);
            if (it == byId.end()) {
                item.error = "ENOENT:the link's target is not in the transfer";
                item.finished = true;
                continue;
            }
            target = m_items[it->second].local;
            if (item.type == Type::Symlink && !remote.linkValue.starts_with('/')) {
                target = TransferPath::relative(TransferPath::dirName(item.local), target);
            }
        } else {
            target = remote.linkValue;
        }
        if (target.empty()) {
            item.error = "EINVAL:the link has no target";
            item.finished = true;
            continue;
        }
        item.finished = true;
        if (!makeDirectories(TransferPath::dirName(item.local))) {
            item.error = errnoText(errno);
            continue;
        }
        ::unlink(item.local.c_str());
        const int result = item.type == Type::Link ? ::link(target.c_str(), item.local.c_str()) : ::symlink(target.c_str(), item.local.c_str());
        if (result != 0) {
            item.error = errnoText(errno);
        }
    }
    for (const auto i : m_order) {
        if (m_items[i].type != Type::Directory && m_items[i].error.empty()) {
            setTimes(i);
        }
    }
    // Children change the times of their directory: the deepest directories go first.
    std::vector<std::size_t> directories;
    for (const auto i : m_order) {
        if (m_items[i].type == Type::Directory && m_items[i].error.empty()) {
            directories.push_back(i);
        }
    }
    std::stable_sort(directories.begin(), directories.end(), [&](std::size_t a, std::size_t b) {
        return std::count(m_items[a].local.begin(), m_items[a].local.end(), '/')
            > std::count(m_items[b].local.begin(), m_items[b].local.end(), '/');
    });
    for (const auto i : directories) {
        setTimes(i);
    }
}

} // namespace

bool transferShouldCompress(std::string_view path, TransferClientOptions::Compress strategy)
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
    // Archives, and the image, audio and video formats that are compressed
    // already: deflate would spend time for nothing.
    static const std::set<std::string, std::less<>> compressed {
        "zip", "odt", "odp", "ods", "pptx", "docx", "xlsx", "epub", "jar", "apk", "gz", "tgz", "bz2", "xz", "txz",
        "zst", "lz", "lz4", "lzma", "7z", "rar", "svgz", "png", "jpg", "jpeg", "gif", "webp", "avif", "heic",
        "heif", "jxl", "bmp", "tif", "tiff", "ico", "mp4", "m4v", "mkv", "webm", "mov", "avi", "mpg", "mpeg",
        "mp3", "m4a", "ogg", "opus", "flac", "aac",
    };
    return !compressed.contains(extension);
}

std::unique_ptr<TransferClient> makeTransferSender(const TransferClientOptions& options,
                                                   const std::vector<std::string>& args, std::string& error)
{
    const auto id = randomHex(8);
    if (!id) {
        error = "The kernel gave no random bytes for the session id";
        return nullptr;
    }
    auto sender = std::make_unique<Sender>(options, *id);
    if (!sender->scan(args, error)) {
        return nullptr;
    }
    sender->start();
    return sender;
}

std::unique_ptr<TransferClient> makeTransferReceiver(const TransferClientOptions& options,
                                                     const std::vector<std::string>& args, std::string& error)
{
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
