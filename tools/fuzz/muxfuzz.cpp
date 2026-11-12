// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
//
// libFuzzer target for the readers of the attach stream of aglow-mux
// (mux/protocol): what a window reads from the daemon and what the daemon
// reads from a window. A file of its own: mux/ is GPL-3.0-or-later, and the
// other targets are not. The first byte picks the reader. The rest is its
// input.
//
//   0 a host call (Host::decode). One that decodes is written again, and
//     the bytes read back give the same call.
//   1 an event of the stream (Attach::decodeEvent), and its host call
//   2 a frame of input of the stream (Attach::decodeInput)

#include "MuxHost.h"
#include "MuxStream.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

namespace Host = AfterglowMux::Host;
namespace Attach = AfterglowMux::Attach;

void hostCall(std::string_view bytes)
{
    const auto call = Host::decode(bytes);
    if (!call) return;
    const std::string again = Host::encode(*call);
    const auto back = Host::decode(again);
    // What decodes is what the window takes: it writes the same bytes again.
    if (!back || Host::encode(*back) != again) std::abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0) return 0;
    const std::string_view bytes(reinterpret_cast<const char*>(data) + 1, size - 1);
    switch (data[0] % 3) {
    case 0:
        hostCall(bytes);
        break;
    case 1:
        if (const auto event = Attach::decodeEvent(bytes); event && event->type == Attach::EventType::Host) hostCall(event->payload);
        break;
    default:
        (void)Attach::decodeInput(bytes);
        break;
    }
    return 0;
}
