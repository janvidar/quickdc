/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/adc/adcbbs.h"

#include "network/downloadqueue.h"

#include <samurai/stdc.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

/**
 * The base32 alphabet, as RFC 4648 writes it and as ADC uses it: upper case, no
 * padding, and no '0', '1', '8' or '9' - which is why a root that looks like a
 * number is not one.
 */
constexpr std::string_view BASE32_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

constexpr std::string_view BOARD_NAME_ALPHABET =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789.-_";

bool isBase32(std::string_view text)
{
	return text.find_first_not_of(BASE32_ALPHABET) == std::string_view::npos;
}

bool isAllDigits(std::string_view text)
{
	if (text.empty()) return false;
	for (const char c : text)
		if (c < '0' || c > '9') return false;
	return true;
}

/**
 * Whether 'name' is an ADC parameter name: an upper case letter, then an upper
 * case letter or a digit.
 *
 * Held to the grammar rather than accepted as any two bytes, because the header
 * is nothing but a FOURCC and named parameters - a token that is not one of those
 * is not a parameter this reader failed to recognise, it is a header that is not
 * an ADC message.
 */
bool isParameterName(std::string_view name)
{
	if (name.size() != 2) return false;
	if (name[0] < 'A' || name[0] > 'Z') return false;
	if (name[1] >= 'A' && name[1] <= 'Z') return true;
	return name[1] >= '0' && name[1] <= '9';
}

/**
 * Strict UTF-8 well-formedness, and what it deliberately does not check.
 *
 * ADC requires its text to be UTF-8 in normalization form C, and BBS0 requires it
 * of a post header. This is the encoding half: an overlong form, a surrogate, a
 * codepoint above U+10FFFF and a truncated or stray sequence are all refused,
 * which is what stops one sequence of bytes being read as another.
 *
 * Normalization form C is NOT verified, and saying that plainly is better than a
 * comment claiming a check that is not here. There is no Unicode normalisation
 * anywhere in this tree, nor in the C++ library, so the check would have to be a
 * table of canonical combining classes and composition exclusions that nothing
 * else needs, maintained against a Unicode version. Guessing at it is worse than
 * leaving it out, because a document cannot be repaired: a header wrongly called
 * denormalised is a post this client can never read, permanently, while a header
 * that is well formed UTF-8 and not in NFC costs a subject line that compares
 * unequal to a normalised copy of itself. What it leaves open is that two
 * different sequences of bytes can look alike on screen, which is the confusable
 * text whatever displays a subject already has to deal with.
 */
bool isWellFormedUtf8(std::string_view text)
{
	size_t at = 0;
	while (at < text.size())
	{
		const unsigned char lead = static_cast<unsigned char>(text[at]);
		size_t length = 0;

		/* The bounds on the first continuation byte, which is where an overlong
		   form, a surrogate and anything past U+10FFFF are ruled out. */
		unsigned char low = 0x80;
		unsigned char high = 0xbf;

		if (lead < 0x80)
		{
			at++;
			continue;
		}
		else if (lead >= 0xc2 && lead <= 0xdf) length = 2;         /* c0 and c1 are overlong */
		else if (lead == 0xe0) { length = 3; low = 0xa0; }         /* e0 80..9f is overlong */
		else if (lead >= 0xe1 && lead <= 0xec) length = 3;
		else if (lead == 0xed) { length = 3; high = 0x9f; }        /* ed a0.. is a surrogate */
		else if (lead >= 0xee && lead <= 0xef) length = 3;
		else if (lead == 0xf0) { length = 4; low = 0x90; }         /* f0 80..8f is overlong */
		else if (lead >= 0xf1 && lead <= 0xf3) length = 4;
		else if (lead == 0xf4) { length = 4; high = 0x8f; }        /* f4 90.. is past U+10FFFF */
		else return false;                                         /* 80..c1 and f5..ff lead nothing */

		if (text.size() - at < length) return false;

		const unsigned char first = static_cast<unsigned char>(text[at + 1]);
		if (first < low || first > high) return false;

		for (size_t n = 2; n < length; n++)
		{
			const unsigned char c = static_cast<unsigned char>(text[at + n]);
			if (c < 0x80 || c > 0xbf) return false;
		}

		at += length;
	}
	return true;
}

/**
 * Whether every backslash in 'text' begins one of the three escapes ADC defines.
 *
 * The ADC grammar reserves every escape but \s, \n and \\, and a message
 * containing another one is rejected - so this is checked on the raw bytes,
 * before they are handed to the parser, rather than left to fall out of anything
 * else. ADC::Parser::unescape() cannot report one: its default arm copies the
 * escaped character through, so "\D" quietly becomes "D" and parse() returns
 * true. Re-serialising would in fact catch that particular case, since an unknown
 * escape always loses its backslash and the header then fails to reproduce
 * itself - but that is an accident of how unescape() happens to behave, and the
 * rule is worth enforcing where it can be read.
 *
 * A backslash as the last byte of the header is refused here too. It is an escape
 * of the newline that ends the header, which is not an escape ADC has.
 */
bool hasOnlyKnownEscapes(std::string_view text)
{
	for (size_t at = 0; at < text.size(); at++)
	{
		if (text[at] != '\\') continue;
		if (at + 1 >= text.size()) return false;

		const char escaped = text[at + 1];
		if (escaped != 's' && escaped != 'n' && escaped != '\\') return false;
		at++;
	}
	return true;
}

/**
 * ADC::Parser::escape() as a std::string.
 *
 * The primitives are a C buffer API and the two of them have to be measured the
 * same way: escapeLength() counts with strlen(), escape() writes a terminator
 * after what it produced, and the buffer is therefore one byte longer than the
 * length. Handing escape() strlen() as its input size rather than the string's
 * own is what keeps the two in step - a value carrying a NUL is then cut at it
 * instead of overrunning the buffer, and no document parsePost() accepted can
 * carry one, since a NUL in a header is refused there.
 */
std::string escapeValue(const std::string& value)
{
	const char* text = value.c_str();
	const size_t length = ADC::Parser::escapeLength(text);

	std::string escaped(length, '\0');
	ADC::Parser::escape(text, strlen(text), escaped.data(), length);
	return escaped;
}

/** One canonical parameter, or nothing at all where there is no value to write. */
void appendParameter(std::string& header, std::string_view name, const std::string& value)
{
	/* Canonical form emits no parameter with an empty value, and nothing that is
	   not a named parameter. Both are refused when read, so emitting either would
	   be composing a post no reader will accept. */
	if (value.empty() || !isParameterName(name)) return;

	header += ' ';
	header += name;
	header += escapeValue(value);
}

/**
 * A number as a post document writes one.
 *
 * Digits only, and no spelling of a value but the shortest: "DA01" and a run of
 * digits too long to hold are both headers that would come back written
 * differently, and a header that does not survive being written again is not the
 * document it claims to be. The round trip below would refuse either one anyway;
 * checking here is what names the reason.
 */
bool readDocumentNumber(std::string_view text, uint64_t& out)
{
	if (!isAllDigits(text)) return false;

	const uint64_t value = Samurai::Util::Convert::to_uint64(std::string(text));
	if (std::to_string(value) != text) return false;

	out = value;
	return true;
}

/**
 * A number as a command carries one.
 *
 * to_uint64() rather than the EXTRACT_INT idiom of adccommands.cpp, which is
 * to_int32(): every count, size and timestamp here is 64 bit, and a hub that
 * stamps a board far in the future is the case the extension's own security
 * discussion turns on - precisely the value a 32 bit conversion mangles into
 * something small enough to look ordinary. It saturates rather than wraps, so an
 * absurd claim stays absurd.
 *
 * Leading zeros are allowed, unlike in a document: nothing here is hashed, so
 * there is no canonical spelling for a field to fail to match.
 *
 * A field that is not required may be absent, and may not be something other than
 * a number: a hub that says a board holds "many" posts has told us nothing we can
 * act on, and reading it as none would put a number on screen that nobody sent.
 */
bool readWireNumber(ADC::Command* command, const char* name, uint64_t& out, bool required)
{
	if (!command->haveArgument(name, 2)) return !required;

	const char* value = command->getArgument(name, 2);
	if (!value || !isAllDigits(value)) return false;

	out = Samurai::Util::Convert::to_uint64(value);
	return true;
}

/**
 * A string field, held to whatever it claims to be wherever it appears, and
 * required to appear only where the caller says so.
 */
bool readChecked(ADC::Command* command, const char* name, std::string& out,
                 bool required, bool (*valid)(std::string_view))
{
	if (!command->haveArgument(name, 2)) return !required;

	const char* value = command->getArgument(name, 2);
	if (!value || !valid(value)) return false;

	out = value;
	return true;
}

/**
 * Whether a flag is present and set.
 *
 * getArgument() hands back what follows the two character name, so the flag's
 * value is at [0] and not at [1]. Only "1" counts: every other value of RM is
 * reserved, and reading one as a removal would act on a meaning it has not been
 * given yet.
 */
bool isFlagSet(ADC::Command* command, const char* name)
{
	const char* flag = command->getArgument(name, 2);
	return flag && flag[0] == '1' && flag[1] == '\0';
}

/** An optional string field, taken as it is. Absent and empty are both empty. */
void readString(ADC::Command* command, const char* name, std::string& out)
{
	const char* value = command->getArgument(name, 2);
	if (value) out = value;
}

}


bool ADC::isValidBoardName(std::string_view name)
{
	if (name.empty() || name.size() > MAX_BOARD_NAME_LENGTH) return false;
	return name.find_first_not_of(BOARD_NAME_ALPHABET) == std::string_view::npos;
}


bool ADC::isValidTTH(std::string_view tth)
{
	if (tth.size() != QUICKDC_TTH_LENGTH) return false;
	return isBase32(tth);
}


bool ADC::isValidCID(std::string_view cid)
{
	if (cid.size() < MIN_CID_LENGTH || cid.size() > MAX_CID_LENGTH) return false;
	return isBase32(cid);
}


bool ADC::readBoard(ADC::Command* command, QuickDC::Board& board)
{
	if (!command) return false;

	QuickDC::Board read;

	if (!readChecked(command, "BD", read.name, true, ADC::isValidBoardName)) return false;

	read.removed = isFlagSet(command, "RM");

	/*
	 * PE, MS, TS and OT are required of a descriptor, and only where it describes a
	 * board. A descriptor saying a board is gone is read the way a tombstone is
	 * read in readEntry(): what identifies the board and the fact that it has
	 * ceased to exist, with the limits and the timestamps of something that no
	 * longer has any beside the point.
	 *
	 * The extension exempts a tombstone by name and says nothing either way about
	 * this, so the leniency is by analogy - and it is the side of the question that
	 * cannot be permanently wrong. A removal read as a malformed command is a dead
	 * board left in the interface with nothing to ever take it out again, while a
	 * truncated descriptor read as a removal is undone by the next descriptor the
	 * hub sends.
	 */
	const bool describesBoard = !read.removed;

	if (!readWireNumber(command, "PE", read.permissions, describesBoard)) return false;
	if (!readWireNumber(command, "MS", read.maxSize, describesBoard)) return false;
	if (!readWireNumber(command, "TS", read.newest, describesBoard)) return false;
	if (!readWireNumber(command, "OT", read.oldest, describesBoard)) return false;

	/* NP is the one number a descriptor of a board may leave out, being a display
	   detail rather than something a subscription depends on. */
	if (!readWireNumber(command, "NP", read.posts, false)) return false;

	readString(command, "NI", read.title);
	readString(command, "DE", read.description);

	board = std::move(read);
	return true;
}


bool ADC::readEntry(ADC::Command* command, QuickDC::BoardEntry& entry)
{
	if (!command) return false;

	QuickDC::BoardEntry read;

	/* What every entry has, whether it describes a post or the end of one. */
	if (!readChecked(command, "TR", read.tth, true, ADC::isValidTTH)) return false;
	if (!readChecked(command, "BD", read.board, true, ADC::isValidBoardName)) return false;
	if (!readWireNumber(command, "TS", read.timestamp, true)) return false;

	read.removed = isFlagSet(command, "RM");

	/*
	 * And what a post has that a tombstone has not. Each is validated wherever it
	 * appears and required only of an entry that describes a post, because a
	 * tombstone carries TR, BD, TS and RM1 and should carry nothing else - the
	 * subject and author of a withdrawn post are usually the reason it was
	 * withdrawn. Requiring them of both would refuse every withdrawal on every
	 * board; a hub that sends them anyway is not refused for it either.
	 */
	const bool describesPost = !read.removed;

	if (!readWireNumber(command, "SI", read.size, describesPost)) return false;
	if (!readChecked(command, "ID", read.cid, describesPost, ADC::isValidCID)) return false;
	if (!readChecked(command, "TH", read.thread, describesPost, ADC::isValidTTH)) return false;
	if (!readChecked(command, "PA", read.parent, false, ADC::isValidTTH)) return false;

	readString(command, "NI", read.nick);
	readString(command, "SJ", read.subject);

	entry = std::move(read);
	return true;
}


std::string ADC::writePostHeader(const QuickDC::Post& post)
{
	std::string header(POST_DOCUMENT_FOURCC);

	appendParameter(header, "ID", post.cid);
	appendParameter(header, "PA", post.parent);
	appendParameter(header, "SJ", post.subject);
	if (post.date) appendParameter(header, "DA", std::to_string(*post.date));
	if (post.richTextValue) appendParameter(header, "RT", std::to_string(*post.richTextValue));

	/* Last, and in the order they arrived, which is the whole of the rule. */
	for (const QuickDC::Post::Parameter& parameter : post.unrecognised)
		appendParameter(header, parameter.name, parameter.value);

	return header;
}


bool ADC::parsePost(const char* data, size_t len, QuickDC::Post& out)
{
	if (!data) return false;

	/*
	 * The header is bounded before the file has been read, so the newline is
	 * looked for in the first MAX_POST_HEADER bytes and nowhere further. The two
	 * rules meet exactly here: a newline found inside that window is a header of
	 * at most MAX_POST_HEADER bytes with its terminator counted, and a longer one
	 * is a document with no newline in the window and refused for that.
	 */
	const size_t bounded = len < MAX_POST_HEADER ? len : MAX_POST_HEADER;
	const void* terminator = memchr(data, '\n', bounded);
	if (!terminator) return false;

	const size_t length = static_cast<const char*>(terminator) - data;
	const std::string_view header(data, length);

	/*
	 * A carriage return is not part of the grammar, and it has to be looked for:
	 * it survives unescaping and being written again untouched, so the round trip
	 * below would accept a header carrying one. The body is another matter and may
	 * hold any byte at all, this one included.
	 */
	if (header.find('\r') != std::string_view::npos) return false;

	/* ADC has no use for a NUL, and every offset from here on is a byte count. */
	if (header.find('\0') != std::string_view::npos) return false;

	if (header.substr(0, POST_DOCUMENT_FOURCC.size()) != POST_DOCUMENT_FOURCC) return false;
	if (header.size() > POST_DOCUMENT_FOURCC.size() && header[POST_DOCUMENT_FOURCC.size()] != ' ')
		return false;

	if (!isWellFormedUtf8(header)) return false;
	if (!hasOnlyKnownEscapes(header)) return false;

	/*
	 * ADC::Parser::parse() does the splitting and the unescaping, so that a post
	 * header is read by the parser the rest of the protocol is read by. What it
	 * does not do is check the escapes - see hasOnlyKnownEscapes() above - and
	 * what it will not do is carry more than ADC::MAX_COMMAND_ARGUMENTS
	 * parameters, which is a ceiling far above the five fields this format
	 * defines: a header naming more than that is refused here rather than parsed.
	 */
	ADC::Command command;
	if (!ADC::Parser::parse(header.data(), header.size(), command)) return false;

	QuickDC::Post parsed;
	std::vector<std::string_view> seen;

	for (size_t n = 0; n < command.countArguments(); n++)
	{
		const char* argument = command.getArgument(n);
		if (!argument) return false;

		const std::string_view parameter(argument);

		/* A name and a value: two bytes and at least one more. This is where a
		   parameter with an empty value goes, and with it the empty token that a
		   second space between two parameters leaves behind. */
		if (parameter.size() < 3) return false;

		const std::string_view name = parameter.substr(0, 2);
		const std::string_view value = parameter.substr(2);
		if (!isParameterName(name)) return false;

		for (const std::string_view already : seen)
			if (already == name) return false;
		seen.push_back(name);

		if (name == "ID")
		{
			if (!isValidCID(value)) return false;
			parsed.cid = value;
		}
		else if (name == "PA")
		{
			if (!isValidTTH(value)) return false;
			parsed.parent = value;
		}
		else if (name == "SJ")
		{
			/* An escaped newline is a newline once unescaped, and canonical form
			   admits none in a subject. */
			if (value.find('\n') != std::string_view::npos) return false;
			parsed.subject = value;
		}
		else if (name == "DA")
		{
			uint64_t date = 0;
			if (!readDocumentNumber(value, date)) return false;
			parsed.date = date;
		}
		else if (name == "RT")
		{
			uint64_t rich = 0;
			if (!readDocumentNumber(value, rich)) return false;
			parsed.richTextValue = rich;
		}
		else
		{
			/* Kept, in order, and not interpreted - SG and KY included. */
			parsed.unrecognised.push_back({ std::string(name), std::string(value) });
		}
	}

	/* ID is the one field required of every post document, and a post whose
	   author is not even claimed has nothing for a reader to compare against the
	   index entry. */
	if (parsed.cid.empty()) return false;

	/*
	 * Canonicality is checked by writing the header back out and comparing the
	 * bytes, which is the only check that cannot disagree with the composer: the
	 * order of the parameters, the single spaces, the absence of a repeat, the
	 * shortest escaping and the shortest spelling of a number are all one
	 * comparison. A document that fails it is refused and not repaired, because a
	 * repair is a different file: it no longer hashes to the name it was asked
	 * for, so the client that repaired it concludes the post is corrupt, and a
	 * client that served the repair on would be handing the next reader a post
	 * under a name that is not its own.
	 */
	if (writePostHeader(parsed) != header) return false;

	/* The body is everything after the newline, raw, escaped in no way, and may
	   hold any byte its format admits. */
	parsed.body.assign(data + length + 1, len - length - 1);

	out = std::move(parsed);
	return true;
}
