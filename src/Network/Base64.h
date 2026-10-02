#pragma once
//
// Base64 (RFC 4648 standard alphabet) for the audio append/delta framing of the
// translation protocol (docs/openai-realtime-protocol.md section 7). Lives in
// the Network module because it is wire encoding; nothing above the seam sees
// base64 text.
//
// Deliberately tiny and dependency-free: no JUCE, no Windows, no third-party.
// The audio payloads this encodes are 9600-byte PCM frames a few times per
// second - a straightforward table implementation is plenty on a worker thread.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace liveai {
namespace network {

/// Encodes len bytes as base64 with padding.
std::string base64Encode(const std::uint8_t* data, std::size_t len);

/// Decodes a base64 string. Returns false for any invalid character, bad
/// padding or trailing bits that are not zero; out is untouched in that case.
bool base64Decode(std::string_view text, std::vector<std::uint8_t>& out);

} // namespace network
} // namespace liveai
