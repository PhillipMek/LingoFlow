#include "NDI/NdiTimedText.h"

#include <string>

namespace liveai {
namespace ndi {
namespace {

void appendXmlEscaped(std::string& out, std::string_view text)
{
    // Text content of an XML 1.0 element: the five predefined entities are the
    // complete set needed here (attribute quoting does not apply), and the
    // characters XML 1.0 forbids outright become spaces - a document a receiver
    // cannot parse is worse than a document with a space in it.
    for (const unsigned char c : text)
    {
        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default:
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
                    out += ' ';
                else if (c == 0x7F)
                    out += ' ';
                else
                    out += static_cast<char>(c);
        }
    }
}

} // namespace

std::string buildTimedTextDocument(const SubtitleFrame& frame)
{
    std::string out;
    out.reserve(frame.text.size() + 200);

    // One root, no XML prolog - the two shape rules the NDI metadata
    // documentation states (docs/ndi-protocol.md, cited 2026-10-04). No xml:lang:
    // the frame carries no language field, and guessing one onto the wire would
    // be exactly the kind of invented protocol fact this product refuses. The
    // xml:id carries our pipeline's own monotonic sequence - a standard W3C
    // attribute used for our trace identity, not a made-up field.
    out += "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><div><p xml:id=\"seq-";

    out += std::to_string(frame.sequence < 0 ? 0 : frame.sequence);
    out += "\">";

    appendXmlEscaped(out, frame.text);

    out += "</p></div></body></tt>";
    return out;
}

} // namespace ndi
} // namespace liveai
