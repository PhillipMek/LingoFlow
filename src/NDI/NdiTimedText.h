#pragma once
//
// NdiTimedText - the XML payload of our NDI subtitle metadata (SPEC 38
// Mode A).
//
// The format question was researched, not invented (docs/ndi-protocol.md, facts
// dated 2026-10-04): the current official NDI documentation defines timed-text
// metadata as XML conforming to one of the standard caption formats (TTML1,
// SDP-US, IMSC1, SMPTE-TT, EBU-TT, CFF-TT), with one root element and no XML
// prolog, and says receivers should ignore elements/attributes they do not
// implement. SPEC 38 chooses the exact format "according to the target
// receiver"; TTML1 (W3C, http://www.w3.org/ns/ttml) is the one implementable
// from public standards without inventing anything, so it is our choice until
// the venue receiver says otherwise - and the REQUIRED checkpoint of this task
// is precisely that conversation with a real receiver.
//
// What "partial/final" means on the wire (contract + SPEC 38 note): every
// publish sends the WHOLE current caption document (the snapshot
// semantics - each document replaces what the receiver shows, which is exactly
// "a receiver may replace the in-progress line"). TTML1 has no live-vs-final
// marker and we do not invent one: `final` therefore governs persistence in OUR
// pipeline, not a wire difference. If the venue receiver needs a distinct final
// convention, the checkpoint records that and the receiver-specific mapping is
// built from evidence, not guessed.

#include <string>

#include "NDI/INdiOutput.h"

namespace liveai {
namespace ndi {

/// Builds the metadata document for one subtitle frame: a minimal, well-formed
/// TTML1 <tt><body><div><p> carrying the snapshot text and the pipeline
/// sequence as the element's xml:id (a standard attribute used for our own
/// trace identity - not an invented protocol field).
///
/// Escaping is total for XML text content (&, <, >), and characters that XML
/// 1.0 forbids outright (C0 controls except tab/newline, and lone 0x7F) are
/// replaced with spaces, because a receiver must never be handed a document it
/// cannot parse. The function is pure and deterministic: every rule above has a
/// test.
std::string buildTimedTextDocument(const SubtitleFrame& frame);

} // namespace ndi
} // namespace liveai
