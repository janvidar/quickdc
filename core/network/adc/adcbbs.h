/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCBBS_H
#define HAVE_QUICKDC_ADCBBS_H

/**
 * Reading and writing the BBS0 bulletin board syntax.
 *
 * Free functions with no state and no I/O, so that the client, the hub and the
 * tests all decide the same way what a board name is and what a post document
 * says. The hub validates a submission with the same isValidTTH() a client
 * validates an entry with, which is the point of the file.
 *
 * The document rules are the expensive ones to get wrong. A post document's
 * tiger tree hash is its name, so a document that is written or read one byte
 * differently is a different post, and a reader that repairs one has produced a
 * file that no longer hashes to the name it asked for.
 */

#include "quickdc.h"

#include "api/bulletins.h"
#include "network/adc/parser.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ADC {

/**
 * The FOURCC at the start of a post document.
 *
 * The header is a whole ADC message rather than a private syntax resembling one,
 * so that it is read with the parser that is already here. 'I' carries no SID,
 * and the trailing '0' versions the document format as it versions the feature -
 * a later revision of the format is a later FOURCC, not a flag inside this one.
 */
inline constexpr std::string_view POST_DOCUMENT_FOURCC = "IBB0";

/**
 * The most a post document's header may occupy, its terminating newline
 * included.
 *
 * The header has to be bounded before the file has been read, so this is both
 * the largest header accepted and the distance a reader will look for the
 * newline that ends it. A document with no newline in this many bytes is refused
 * rather than read further.
 */
inline constexpr size_t MAX_POST_HEADER = 8192;

/** A board name is at most this many characters, and at least one. */
inline constexpr size_t MAX_BOARD_NAME_LENGTH = 64;

/**
 * The bounds isValidCID() holds a CID to.
 *
 * Base32 carries five bits a character, so 26 characters is a 128 bit hash and
 * 128 characters a 640 bit one. No ADC session hash is outside that, and the
 * only one in use - TIGR, which a hub announcing BBS0 must announce - sits at 39
 * in the middle of it.
 */
inline constexpr size_t MIN_CID_LENGTH = 26;
inline constexpr size_t MAX_CID_LENGTH = 128;

/**
 * Whether 'name' is a board name: one or more of A-Z, a-z, 0-9, '.', '-' and
 * '_', and at most MAX_BOARD_NAME_LENGTH of them. Case sensitive.
 *
 * A board name is never a filename and never a path component, and this is where
 * that is written down rather than left to be discovered: the character set
 * excludes the separators but permits '.', so ".." is a legal board name and so
 * is "...". A name that passes this is safe to compare, to send and to display,
 * and is not safe to build a path out of - a client that caches posts on disk
 * names the files after the TTH.
 *
 * Dots separate hierarchy by convention, as in "dev.adc", and the convention
 * carries no meaning beyond grouping boards for display.
 */
bool isValidBoardName(std::string_view name);

/**
 * Whether 'tth' is a tiger tree root: exactly QUICKDC_TTH_LENGTH characters of
 * unpadded base32, which is uppercase A-Z and the digits 2-7.
 *
 * Base32 has no padding at this length and no lower case, so a lower case root
 * and a root containing '0', '1', '8' or '9' are both refused rather than
 * folded: two spellings of one hash would be two names for one post.
 */
bool isValidTTH(std::string_view tth);

/**
 * Whether 'cid' could be a client ID: unpadded base32, of a length between
 * MIN_CID_LENGTH and MAX_CID_LENGTH.
 *
 * Not the same check as isValidTTH(), and assuming it is would be the natural
 * mistake, since every CID anyone has seen is 39 characters. A CID's length
 * follows the session hash the hub negotiated and is not fixed by the protocol,
 * so pinning it at 39 - which ADC::CID::verify() does, correctly, for this
 * client's own identity - would refuse every ID in every document written on a
 * hub that negotiated anything else, and a document cannot be repaired: those
 * posts would be permanently unreadable. The bound is here so that the field is
 * still bounded before it reaches a cache key or a display.
 */
bool isValidCID(std::string_view cid);

/**
 * Read a board descriptor out of a BBD.
 *
 * Refused rather than repaired when anything required is missing or malformed:
 * BD, PE, MS, TS and OT are all required of a descriptor that describes a board,
 * and one that cannot be trusted to say what a board is would otherwise be
 * displayed as a board with a limit of zero that nobody may post to.
 *
 * A descriptor carrying RM1 says a board has ceased to exist and is read the way
 * readEntry() reads a tombstone: BD and the flag are the whole of it, since the
 * limits and timestamps of a board that is gone describe nothing.
 *
 * @return false when nothing was read, leaving 'board' untouched.
 */
bool readBoard(Command* command, QuickDC::Board& board);

/**
 * Read an index entry out of a BBL.
 *
 * A tombstone carries TR, BD, TS and RM1 and should carry nothing else, so the
 * fields that describe a post - SI, ID and TH - are required of an entry that is
 * one and not of the record that a post is gone. Requiring them of both would
 * refuse every withdrawal on every board.
 *
 * @return false when nothing was read, leaving 'entry' untouched.
 */
bool readEntry(Command* command, QuickDC::BoardEntry& entry);

/**
 * The canonical header line of a post document, without its terminating newline.
 *
 * The newline is the caller's to append, and it is inside the hash: a document
 * assembled without it is a different post from the one every other client will
 * read, and the mistake shows up as a hash nobody can find rather than as a
 * parse error.
 *
 * Canonical because the hash covers every byte, so formatting is identity: one
 * space between the FOURCC and each parameter and between parameters, no
 * parameter twice, none with an empty value, the order ID PA SJ DA RT with
 * unrecognised parameters last in the order they arrived, and the shortest
 * escaping ADC admits with nothing escaped that does not require it.
 *
 * It validates nothing, and that is deliberate: it is also the yardstick
 * parsePost() measures a header against, so it must emit exactly what it is
 * given and never differ from the bytes it is being compared with. Two
 * consequences for a composer. A Post with an empty cid produces a header with
 * no ID, which parsePost() refuses. A subject carrying a newline is escaped
 * faithfully as "\n", and parsePost() refuses that too - strip newlines from a
 * subject before composing, because a post that no reader will accept is worse
 * than one with a flattened subject line.
 */
std::string writePostHeader(const QuickDC::Post& post);

/**
 * Parse a post document: its header line into 'out', and every byte after the
 * first newline into out.body.
 *
 * Refused, and never repaired, when the document has no newline in its first
 * MAX_POST_HEADER bytes, when its header exceeds that including the newline,
 * when a carriage return appears before that newline, when the FOURCC is not
 * exactly IBB0, when the header is not well formed UTF-8, when it contains an
 * escape sequence the ADC grammar does not define, or when it is not canonical.
 *
 * Repairing is not an option available here, however tempting it looks: the
 * document was asked for by hash, so a reader that repairs one is holding bytes
 * that no longer hash to the name it requested, and it will conclude the file is
 * corrupt and discard it - or, worse, serve the repair on to the next reader
 * under the original name.
 *
 * Parameters this version does not recognise are kept in out.unrecognised and
 * ignored. SG and KY are ignored by name, being reserved for a future authorship
 * extension, and kept like any other unrecognised parameter so that a document
 * carrying them still round-trips.
 *
 * ID is the one field required of a document: nothing else in the header is worth
 * refusing a post over. A subject is required of a post that starts a thread and
 * that is a rule for whoever composes one - a document already written and hashed
 * without one is a post that can be read and displayed, and refusing it here would
 * make it unreadable for ever rather than untidy.
 *
 * The header's escapes and its UTF-8 encoding are checked; normalization form C is
 * not, and isWellFormedUtf8() in the implementation says why at length.
 *
 * @return false when the document is refused, leaving 'out' untouched.
 */
bool parsePost(const char* data, size_t len, QuickDC::Post& out);

}

#endif // HAVE_QUICKDC_ADCBBS_H
