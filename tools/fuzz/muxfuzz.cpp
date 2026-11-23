// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
//
// libFuzzer target for the readers of the attach stream of aglow-mux
// (mux/protocol): what a window reads from the daemon and what the daemon
// reads from a window or a client. A file of its own: mux/ is
// GPL-3.0-or-later, and the other targets are not. The first byte picks the
// reader. The rest is its input.
//
//   0 a host call (Host::decode). One that decodes is written again, and
//     the bytes read back give the same call.
//   1 an event of the stream (Attach::decodeEvent), and its host call
//   2 a frame of input of the stream (Attach::decodeInput)
//   3 a layout, as JSON (layoutFrom). One that reads is written again and
//     reads the same. Then its panes leave it, half at once (reconcile) and
//     the others one by one (remove), and each step is still a layout.
//   4 a launch, as JSON (launchFrom), written again and read the same
//   5 a pane size, as JSON (paneSizeFrom), written again and read the same
//   6 a frame of a relay of the stream (Attach::decodeRelay), written again
//     and read the same
//   7 the settings of a window, as JSON (sessionSettings), written again and
//     read the same
//   8 the options of a new session, as JSON (sessionOptions), written again
//     and read the same
//   9 an environment, as JSON (environmentFrom), written again and read the
//     same
//  10 a session of a list, as JSON (sessionInfo), written again and read the
//     same

#include "MuxHost.h"
#include "MuxLayout.h"
#include "MuxProtocol.h"
#include "MuxStream.h"

#include <AfterglowProto/Json.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace Host = AfterglowMux::Host;
namespace Attach = AfterglowMux::Attach;
namespace Json = AfterglowProto::Json;

void hostCall(std::string_view bytes)
{
    const auto call = Host::decode(bytes);
    if (!call)
        return;
    const std::string again = Host::encode(*call);
    const auto back = Host::decode(again);
    // What decodes is what the window takes: it writes the same bytes again.
    if (!back || Host::encode(*back) != again)
        std::abort();
}

void relayFrame(std::string_view bytes)
{
    const auto frame = Attach::decodeRelay(bytes);
    if (!frame)
        return;
    const std::string again = Attach::encode(*frame);
    const auto back = Attach::decodeRelay(again);
    if (!back || Attach::encode(*back) != again)
        std::abort();
}

void environment(std::string_view bytes)
{
    const auto json = Json::parse(bytes);
    if (!json)
        return;
    const auto read = AfterglowMux::environmentFrom(*json, nullptr);
    if (!read)
        return;
    const std::string text = Json::write(AfterglowMux::environmentJson(*read));
    const auto again = Json::parse(text);
    const auto back = again ? AfterglowMux::environmentFrom(*again, nullptr) : std::nullopt;
    if (!back || Json::write(AfterglowMux::environmentJson(*back)) != text)
        std::abort();
}

/// What a window or the daemon would send again of @p layout reads as it
/// (as its text: text that is not UTF-8 goes out as U+FFFD). A layout
/// without tabs is the end of a session, which nobody sends.
bool stillALayout(const AfterglowMux::Layout& layout)
{
    if (layout.tabs.empty())
        return true;
    const std::string text = Json::write(AfterglowMux::toJson(layout));
    const auto json = Json::parse(text);
    const auto back = json ? AfterglowMux::layoutFrom(*json, nullptr) : std::nullopt;
    return back && Json::write(AfterglowMux::toJson(*back)) == text;
}

void layout(std::string_view bytes)
{
    const auto json = Json::parse(bytes);
    if (!json)
        return;
    auto read = AfterglowMux::layoutFrom(*json, nullptr);
    if (!read)
        return;
    if (!stillALayout(*read))
        std::abort();
    const std::vector<std::uint32_t> panes = read->panes();
    std::vector<std::uint32_t> kept;
    for (std::size_t i = 0; i < panes.size(); i += 2)
        kept.push_back(panes[i]);
    read->reconcile(kept);
    if (read->panes() != kept || !stillALayout(*read))
        std::abort();
    for (const std::uint32_t pane : kept) {
        if (!read->remove(pane) || read->contains(pane) || !stillALayout(*read))
            std::abort();
    }
    if (!read->tabs.empty())
        std::abort();
}

template<typename T, typename From>
void roundTrip(std::string_view bytes, From from)
{
    const auto json = Json::parse(bytes);
    if (!json)
        return;
    const std::optional<T> read = from(*json, nullptr);
    if (!read)
        return;
    const auto back = from(AfterglowMux::toJson(*read), nullptr);
    if (!back || Json::write(AfterglowMux::toJson(*back)) != Json::write(AfterglowMux::toJson(*read)))
        std::abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
        return 0;
    const std::string_view bytes(reinterpret_cast<const char*>(data) + 1, size - 1);
    switch (data[0] % 11) {
    case 0:
        hostCall(bytes);
        break;
    case 1:
        if (const auto event = Attach::decodeEvent(bytes); event && event->type == Attach::EventType::Host)
            hostCall(event->payload);
        break;
    case 2:
        (void)Attach::decodeInput(bytes);
        break;
    case 3:
        layout(bytes);
        break;
    case 4:
        roundTrip<AfterglowMux::Launch>(bytes, AfterglowMux::launchFrom);
        break;
    case 5:
        roundTrip<AfterglowMux::PaneSize>(bytes, AfterglowMux::paneSizeFrom);
        break;
    case 6:
        relayFrame(bytes);
        break;
    case 7:
        roundTrip<AfterglowMux::SessionSettings>(bytes, AfterglowMux::sessionSettings);
        break;
    case 8:
        roundTrip<AfterglowMux::SessionOptions>(bytes, AfterglowMux::sessionOptions);
        break;
    case 9:
        environment(bytes);
        break;
    default:
        roundTrip<AfterglowMux::SessionInfo>(bytes, [](const Json::Value& value, std::string*) {
            return AfterglowMux::sessionInfo(value);
        });
        break;
    }
    return 0;
}
