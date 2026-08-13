/*
 * BBS0: the bulletin board syntax - board names, hashes, the BBD and BBL fields,
 * and the post document.
 *
 * The document tests are the ones that matter. A post document's tiger tree hash
 * is its name, so a header written or read one byte differently is a different
 * post: what is pinned here is not that a reasonable document parses, but that
 * every document which is not canonical is refused rather than repaired, and that
 * every document which is canonical survives being read and written again byte for
 * byte - unrecognised parameters included, since those are the ones a reader is
 * most likely to drop.
 */

#include <samurai/samurai.h>
#include <string.h>

#include <string>

#include "api/bulletins.h"
#include "network/adc/adcbbs.h"
#include "network/adc/adccommon.h"
#include "network/adc/parser.h"

/* The identities and hashes the specification's own examples use. */
#define BBS_CID_A  "IPJJWEPPPLCA3PF2ZCRRYO4F2ZX2EV2JMW2KC3I"
#define BBS_CID_B  "NNPPWEPPPLCA3PF2ZCRRYO4F2ZX2EV2JMW2KC3I"
#define BBS_TTH_A  "KX3TQ7ZVN5PLQGKD3NDBK6ZTZG5PYQXSNMFYVJH"
#define BBS_TTH_B  "7ZQGKD3NDBK6ZTZG5PYQXSNMFYVJH4TXAVN6PLQ"

/* The two post documents from the specification's examples, byte for byte. */
#define BBS_THREAD_HEADER \
	"IBB0 ID" BBS_CID_A " SJHub\\supgrade\\son\\sSaturday DA1786439650 RT1"

#define BBS_THREAD_BODY \
	"The hub will be **offline from 09:00 to 11:00 UTC** on Saturday.\n" \
	"\n" \
	"Release notes are attached: [notes.pdf](magnet:?xt=urn:tree:tiger:" \
	"VN6PLQ7ZQGKD3NDBK6ZTZG5PYQXSNMFYVJH4TXA&dn=notes.pdf&xl=1048576)\n"

#define BBS_REPLY_HEADER \
	"IBB0 ID" BBS_CID_B " PA" BBS_TTH_A \
	" SJRe:\\sHub\\supgrade\\son\\sSaturday DA1786442550"

#define BBS_REPLY_BODY "Will the file list survive the upgrade?\n"

static bool bbs_parses(const std::string& document, QuickDC::Post& post)
{
	return ADC::parsePost(document.data(), document.size(), post);
}

static bool bbs_refuses(const std::string& document)
{
	/* A refusal leaves the caller's post alone, which is what lets a reader keep
	   whatever it already held for a hash it re-fetched. */
	QuickDC::Post post;
	post.subject = "untouched";

	if (ADC::parsePost(document.data(), document.size(), post)) return false;
	return post.subject == "untouched";
}

/** A document whose header is canonical, so that a test can spoil one thing in it. */
static std::string bbs_document(const std::string& header, const std::string& body)
{
	return header + "\n" + body;
}

/** A canonical header of exactly 'total' bytes, its terminating newline counted. */
static std::string bbs_header_of_size(size_t total)
{
	std::string header = "IBB0 ID" BBS_CID_A " XX";
	if (total < header.size() + 2) return std::string();

	header.append(total - header.size() - 1, 'A');
	header += '\n';
	return header;
}

static bool bbs_read_board(const char* line, QuickDC::Board& board)
{
	ADC::Command command;
	if (!ADC::Parser::parse(line, strlen(line), command)) return false;
	return ADC::readBoard(&command, board);
}

static bool bbs_read_entry(const char* line, QuickDC::BoardEntry& entry)
{
	ADC::Command command;
	if (!ADC::Parser::parse(line, strlen(line), command)) return false;
	return ADC::readEntry(&command, entry);
}

/* ------------------------------------------------------------------------- *
 * Board names
 * ------------------------------------------------------------------------- */

EXO_TEST(bbs_boardname_accepted, {
	return ADC::isValidBoardName("general")
		&& ADC::isValidBoardName("announcements")
		&& ADC::isValidBoardName("dev.adc")
		&& ADC::isValidBoardName("a")
		&& ADC::isValidBoardName("0")
		&& ADC::isValidBoardName("A-b_c.1")
		&& ADC::isValidBoardName("_");
});

EXO_TEST(bbs_boardname_dotdot_is_legal, {
	/*
	 * The character set excludes the separators and permits '.', so these are all
	 * board names and none of them is a path component. This is the test that goes
	 * red if somebody decides a board name is safe to build a filename out of.
	 */
	return ADC::isValidBoardName("..")
		&& ADC::isValidBoardName(".")
		&& ADC::isValidBoardName("...")
		&& ADC::isValidBoardName("..--..");
});

EXO_TEST(bbs_boardname_length, {
	const std::string longest(64, 'a');
	const std::string toolong(65, 'a');

	return ADC::isValidBoardName(longest)
		&& !ADC::isValidBoardName(toolong)
		&& !ADC::isValidBoardName("");
});

EXO_TEST(bbs_boardname_refuses_separators_and_space, {
	return !ADC::isValidBoardName("a/b")
		&& !ADC::isValidBoardName("a\\b")
		&& !ADC::isValidBoardName("a b")
		&& !ADC::isValidBoardName("a\nb")
		&& !ADC::isValidBoardName("a:b")
		&& !ADC::isValidBoardName("~/general")
		&& !ADC::isValidBoardName("g\xc3\xa9n\xc3\xa9ral");
});

/* ------------------------------------------------------------------------- *
 * Hashes and identities
 * ------------------------------------------------------------------------- */

EXO_TEST(bbs_tth_accepted, {
	return ADC::isValidTTH(BBS_TTH_A) && ADC::isValidTTH(BBS_TTH_B);
});

EXO_TEST(bbs_tth_length_is_fixed, {
	const std::string root(BBS_TTH_A);

	return !ADC::isValidTTH(root.substr(0, 38))
		&& !ADC::isValidTTH(root + "A")
		&& !ADC::isValidTTH("");
});

EXO_TEST(bbs_tth_alphabet, {
	/* Base32 has no '0', '1', '8' or '9' and no lower case: a root that looks
	   like it holds a digit holds one of 2-7, and two spellings of one hash would
	   be two names for one post. */
	std::string zero(BBS_TTH_A);
	std::string one(BBS_TTH_A);
	std::string eight(BBS_TTH_A);
	std::string nine(BBS_TTH_A);

	zero[0] = '0';
	one[10] = '1';
	eight[38] = '8';
	nine[20] = '9';

	std::string lower(BBS_TTH_A);
	for (size_t n = 0; n < lower.size(); n++)
		if (lower[n] >= 'A' && lower[n] <= 'Z') lower[n] = static_cast<char>(lower[n] - 'A' + 'a');

	return !ADC::isValidTTH(zero)
		&& !ADC::isValidTTH(one)
		&& !ADC::isValidTTH(eight)
		&& !ADC::isValidTTH(nine)
		&& !ADC::isValidTTH(lower);
});

EXO_TEST(bbs_cid_length_is_not_fixed, {
	/*
	 * A CID's length follows the session hash and is not fixed at 39, so this is
	 * deliberately not the TTH check. A hub that negotiated a 256 bit session hash
	 * hands out 52 character CIDs, and refusing those would make every post
	 * written there permanently unreadable.
	 */
	const std::string tiger(BBS_CID_A);
	const std::string sha256(52, 'A');
	const std::string shortest(26, 'A');

	return ADC::isValidCID(tiger)
		&& ADC::isValidCID(sha256)
		&& ADC::isValidCID(shortest)
		&& ADC::isValidCID(std::string(128, 'A'))
		&& !ADC::isValidTTH(sha256);
});

EXO_TEST(bbs_cid_is_still_bounded, {
	return !ADC::isValidCID("")
		&& !ADC::isValidCID(std::string(25, 'A'))
		&& !ADC::isValidCID(std::string(129, 'A'))
		&& !ADC::isValidCID("ipjjweppplca3pf2zcrryo4f2zx2ev2jmw2kc3i")
		&& !ADC::isValidCID("IPJJWEPPPLCA3PF2ZCRRYO4F2ZX2EV2JMW2KC31");
});

/* ------------------------------------------------------------------------- *
 * The post document: what must be accepted
 * ------------------------------------------------------------------------- */

EXO_TEST(bbs_post_spec_thread_example, {
	const std::string document = bbs_document(BBS_THREAD_HEADER, BBS_THREAD_BODY);

	QuickDC::Post post;
	if (!bbs_parses(document, post)) return false;

	if (post.cid != BBS_CID_A) return false;
	if (!post.parent.empty()) return false;                  /* no PA: it starts a thread */
	if (post.subject != "Hub upgrade on Saturday") return false;
	if (!post.date || *post.date != 1786439650) return false;
	if (!post.isRichText()) return false;
	if (!post.unrecognised.empty()) return false;
	if (post.body != BBS_THREAD_BODY) return false;

	/* And the header it came from, byte for byte. */
	return ADC::writePostHeader(post) == BBS_THREAD_HEADER;
});

EXO_TEST(bbs_post_spec_reply_example, {
	const std::string document = bbs_document(BBS_REPLY_HEADER, BBS_REPLY_BODY);

	QuickDC::Post post;
	if (!bbs_parses(document, post)) return false;

	if (post.cid != BBS_CID_B) return false;
	if (post.parent != BBS_TTH_A) return false;
	if (post.subject != "Re: Hub upgrade on Saturday") return false;
	if (!post.date || *post.date != 1786442550) return false;
	if (post.richTextValue) return false;                    /* no RT: plain text */
	if (post.isRichText()) return false;
	if (post.body != BBS_REPLY_BODY) return false;

	return ADC::writePostHeader(post) == BBS_REPLY_HEADER;
});

EXO_TEST(bbs_post_roundtrip, {
	QuickDC::Post out;
	out.cid = BBS_CID_A;
	out.parent = BBS_TTH_B;
	out.subject = "A subject with spaces, a \\ and a tab\t";
	out.date = 1786439650;
	out.richTextValue = 1;
	out.body = "one\ntwo\n";

	const std::string document = bbs_document(ADC::writePostHeader(out), out.body);

	QuickDC::Post back;
	if (!bbs_parses(document, back)) return false;

	return back.cid == out.cid
		&& back.parent == out.parent
		&& back.subject == out.subject
		&& back.date == out.date
		&& back.richTextValue == out.richTextValue
		&& back.body == out.body
		&& ADC::writePostHeader(back) == ADC::writePostHeader(out);
});

EXO_TEST(bbs_post_roundtrip_keeps_unrecognised_in_order, {
	/*
	 * The one a reader is most likely to get wrong. Canonical form puts parameters
	 * this version does not know last, in the order they were written, and
	 * canonicality is checked by writing the header out again - so a reader that
	 * dropped them, sorted them or moved them would refuse every document that
	 * carries any, and there would be no way to extend the format at all.
	 */
	const char* header = "IBB0 ID" BBS_CID_A " SJhello DA5 RT1 ZZlast XXfirst\\sof\\stwo";
	const std::string document = bbs_document(header, "body\n");

	QuickDC::Post post;
	if (!bbs_parses(document, post)) return false;

	if (post.unrecognised.size() != 2) return false;
	if (post.unrecognised[0].name != "ZZ" || post.unrecognised[0].value != "last") return false;
	if (post.unrecognised[1].name != "XX") return false;
	if (post.unrecognised[1].value != "first of two") return false;

	return ADC::writePostHeader(post) == header;
});

EXO_TEST(bbs_post_writes_unrecognised_last, {
	QuickDC::Post post;
	post.cid = BBS_CID_A;
	post.subject = "x";
	post.unrecognised.push_back({ "ZZ", "1" });
	post.unrecognised.push_back({ "AA", "2" });

	/* Last, and in the order they were given rather than sorted. */
	return ADC::writePostHeader(post) == "IBB0 ID" BBS_CID_A " SJx ZZ1 AA2";
});

EXO_TEST(bbs_post_sg_and_ky_are_ignored_and_kept, {
	/*
	 * Reserved for a future authorship extension. Ignored by name, and kept like
	 * any other unrecognised parameter, because a document carrying them still has
	 * to round-trip - a reader that dropped them would refuse every signed post
	 * once such an extension exists.
	 */
	const char* header = "IBB0 ID" BBS_CID_A " SJsigned SGZZZZ KYAAAA";
	const std::string document = bbs_document(header, "");

	QuickDC::Post post;
	if (!bbs_parses(document, post)) return false;

	if (post.unrecognised.size() != 2) return false;
	if (post.unrecognised[0].name != "SG" || post.unrecognised[1].name != "KY") return false;

	return ADC::writePostHeader(post) == header;
});

EXO_TEST(bbs_post_no_parent_and_parent, {
	const std::string thread = bbs_document("IBB0 ID" BBS_CID_A " SJnew", "");
	const std::string reply = bbs_document("IBB0 ID" BBS_CID_A " PA" BBS_TTH_A " SJre", "");

	QuickDC::Post first;
	QuickDC::Post second;
	if (!bbs_parses(thread, first) || !bbs_parses(reply, second)) return false;

	return first.parent.empty() && second.parent == BBS_TTH_A;
});

EXO_TEST(bbs_post_body_is_untouched, {
	/*
	 * Only the header is ADC and the first newline is where ADC stops. Everything
	 * after it is inside the hash exactly as it lies, so newlines, carriage
	 * returns, a NUL and bytes that are no text at all all survive.
	 */
	const std::string header = "IBB0 ID" BBS_CID_A " SJbinary";
	std::string body("first\r\nsecond\n\n");
	body.push_back('\0');
	body += "\xff\xfe trailing bytes and no newline at the end";

	const std::string document = bbs_document(header, body);

	QuickDC::Post post;
	if (!bbs_parses(document, post)) return false;

	return post.body == body && post.body.size() == body.size();
});

EXO_TEST(bbs_post_empty_body, {
	/* "IBB0 ...\n" and nothing else is a post with no body, not a truncated file. */
	QuickDC::Post post;
	if (!bbs_parses(bbs_document("IBB0 ID" BBS_CID_A " SJempty", ""), post)) return false;

	return post.body.empty() && post.subject == "empty";
});

EXO_TEST(bbs_post_reserved_rt_value_is_plain_text, {
	/*
	 * Every RT value but 1 is reserved, and a reader must treat the body as plain
	 * text rather than refuse the post - so the value is kept as it was written and
	 * comes back the same way. This is why RT is a value here and not a flag: a
	 * bool could not reproduce "RT2" and every such post would be refused.
	 */
	const char* header = "IBB0 ID" BBS_CID_A " SJlater DA7 RT2";

	QuickDC::Post post;
	if (!bbs_parses(bbs_document(header, "body"), post)) return false;

	if (!post.richTextValue || *post.richTextValue != 2) return false;
	if (post.isRichText()) return false;

	return ADC::writePostHeader(post) == header;
});

EXO_TEST(bbs_post_rt_zero_is_kept, {
	/* RT0 is a value and absence is not, so the two are not the same header. */
	const char* header = "IBB0 ID" BBS_CID_A " SJx RT0";

	QuickDC::Post post;
	if (!bbs_parses(bbs_document(header, ""), post)) return false;

	return post.richTextValue && *post.richTextValue == 0
		&& !post.isRichText()
		&& ADC::writePostHeader(post) == header;
});

EXO_TEST(bbs_post_date_is_64_bit, {
	/*
	 * A date beyond what 32 bits hold, which is the year 2106 and not far off. The
	 * conversion this used to invite - to_int32 - turns it into something small
	 * enough to look ordinary.
	 */
	const char* header = "IBB0 ID" BBS_CID_A " SJfuture DA99999999999";

	QuickDC::Post post;
	if (!bbs_parses(bbs_document(header, ""), post)) return false;

	return post.date && *post.date == 99999999999ULL
		&& ADC::writePostHeader(post) == header;
});

EXO_TEST(bbs_post_accepts_multibyte_utf8, {
	/* Two and four byte sequences in a subject, which is ordinary text. */
	const char* header = "IBB0 ID" BBS_CID_A " SJH\xc3\xbc" "bben\\s\xf0\x9f\x93\xa2";

	QuickDC::Post post;
	if (!bbs_parses(bbs_document(header, ""), post)) return false;

	return post.subject == "H\xc3\xbc" "bben \xf0\x9f\x93\xa2"
		&& ADC::writePostHeader(post) == header;
});

EXO_TEST(bbs_post_header_of_exactly_8192_bytes, {
	/* The largest header there is, its terminating newline counted. */
	const std::string document = bbs_header_of_size(8192);
	if (document.size() != 8192) return false;

	QuickDC::Post post;
	return bbs_parses(document, post) && post.unrecognised.size() == 1;
});

/* ------------------------------------------------------------------------- *
 * The post document: what must be refused
 *
 * Every one of these is refused rather than repaired. A repaired document is a
 * different file: it no longer hashes to the name it was fetched under, so the
 * reader concludes the post is corrupt - and a reader that served the repair on
 * would be handing the next reader a post under a name that is not its own.
 * ------------------------------------------------------------------------- */

EXO_TEST(bbs_post_reject_header_of_8193_bytes, {
	const std::string document = bbs_header_of_size(8193);
	return document.size() == 8193 && bbs_refuses(document);
});

EXO_TEST(bbs_post_reject_no_newline_in_8192_bytes, {
	/* The header has to be bounded before the file has been read, so a document
	   with no newline in the window is refused rather than read further. */
	std::string document = "IBB0 ID" BBS_CID_A " XX";
	document.append(9000, 'A');

	return bbs_refuses(document);
});

EXO_TEST(bbs_post_reject_carriage_return, {
	/*
	 * A CR before the terminating newline is not part of the grammar. It has to be
	 * looked for: it survives unescaping and being written again untouched, so the
	 * round trip alone would accept a header carrying one.
	 */
	return bbs_refuses("IBB0 ID" BBS_CID_A " SJhello\r\nbody")
		&& bbs_refuses("IBB0 ID" BBS_CID_A "\r SJhello\nbody");
});

EXO_TEST(bbs_post_reject_wrong_fourcc, {
	return bbs_refuses(bbs_document("IBB1 ID" BBS_CID_A " SJx", ""))
		&& bbs_refuses(bbs_document("HBB0 ID" BBS_CID_A " SJx", ""))
		&& bbs_refuses(bbs_document("IBB0X ID" BBS_CID_A " SJx", ""))
		&& bbs_refuses(bbs_document("IBB", ""))
		&& bbs_refuses(bbs_document("", ""));
});

EXO_TEST(bbs_post_reject_repeated_parameter, {
	/* Whichever of the two a reader kept, the other one would be lost - and a post
	   that says two things about its own subject says neither. */
	return bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJone SJtwo", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " ID" BBS_CID_B " SJx", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx XXa XXb", ""));
});

EXO_TEST(bbs_post_reject_out_of_order, {
	return bbs_refuses(bbs_document("IBB0 SJfirst ID" BBS_CID_A, ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx PA" BBS_TTH_A, ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " RT1 DA5 SJx", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " XXearly SJx", ""));
});

EXO_TEST(bbs_post_reject_empty_value, {
	return bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJ", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx XX", ""))
		&& bbs_refuses(bbs_document("IBB0 ID", ""));
});

EXO_TEST(bbs_post_reject_extra_space, {
	/* Exactly one space between the FOURCC and each parameter, and between
	   parameters. A second one leaves an empty parameter behind. */
	return bbs_refuses(bbs_document("IBB0  ID" BBS_CID_A " SJx", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A "  SJx", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx ", ""));
});

EXO_TEST(bbs_post_reject_unknown_escape, {
	/*
	 * The ADC grammar reserves every escape but \s, \n and \\. Note what an
	 * unnecessary escape looks like with only three of them defined: there is no
	 * way to escape something that does not need escaping without naming an escape
	 * that does not exist, so "\." and "\S" are that case as well as this one.
	 */
	return bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJa\\.b", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJa\\Sb", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJa\\tb", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJa\\\\\\xb", ""));
});

EXO_TEST(bbs_post_reject_trailing_backslash, {
	/* A backslash as the last byte of the header escapes the newline that ends it,
	   which is not an escape ADC has. */
	return bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJends\\", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJa\\\\\\", ""));
});

EXO_TEST(bbs_post_reject_newline_in_subject, {
	/* Escaped or not, a newline in a subject is a subject that cannot be shown in
	   a list of them, and canonical form admits none. */
	return bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJtwo\\nlines", ""));
});

EXO_TEST(bbs_post_reject_number_that_is_not_shortest, {
	/* "DA01" and a run of digits too long to hold are both headers that would come
	   back written differently, and one that cannot be written again as it arrived
	   is not the document it claims to be. */
	return bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx DA01", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx DA0000000000", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx DA99999999999999999999999", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx DAyesterday", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx DA-5", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " SJx RT1.0", ""));
});

EXO_TEST(bbs_post_reject_missing_or_malformed_id, {
	/* ID is required of every post document, and a post whose author is not even
	   claimed has nothing for a reader to compare against the index entry. */
	return bbs_refuses(bbs_document("IBB0 SJnobody DA5", ""))
		&& bbs_refuses(bbs_document("IBB0", ""))
		&& bbs_refuses(bbs_document("IBB0 IDnot-base32 SJx", ""))
		&& bbs_refuses(bbs_document("IBB0 IDSHORT SJx", ""));
});

EXO_TEST(bbs_post_reject_malformed_parent, {
	/* PA names a document, and a hash that is not one names nothing that can ever
	   be fetched. */
	return bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " PAKX3TQ7 SJx", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " PA" BBS_TTH_A "A SJx", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " PA" BBS_CID_A "0 SJx", ""));
});

EXO_TEST(bbs_post_reject_parameter_that_is_not_one, {
	/* The header is a FOURCC and named parameters, and nothing else: a token that
	   is not a parameter is not one this reader failed to recognise. */
	return bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " sjlowercase", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " 1Xdigit\\sfirst", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " X", ""))
		&& bbs_refuses(bbs_document("IBB0 ID" BBS_CID_A " -Xdash", ""));
});

EXO_TEST(bbs_post_reject_malformed_utf8, {
	/*
	 * The header is UTF-8, and an encoding that is not well formed is where one
	 * sequence of bytes is read as another. Each of these is a different way of
	 * getting that wrong.
	 */
	const std::string prefix = "IBB0 ID" BBS_CID_A " SJ";

	return bbs_refuses(bbs_document(prefix + "\xc0" "\xaf", ""))          /* overlong '/' */
		&& bbs_refuses(bbs_document(prefix + "\xc1" "\xbf", ""))          /* overlong */
		&& bbs_refuses(bbs_document(prefix + "\xe0" "\x80" "\xaf", ""))   /* overlong, three bytes */
		&& bbs_refuses(bbs_document(prefix + "\xf0" "\x80" "\x80" "\xaf", "")) /* overlong, four */
		&& bbs_refuses(bbs_document(prefix + "\xed" "\xa0" "\x80", ""))   /* a lone surrogate */
		&& bbs_refuses(bbs_document(prefix + "\xed" "\xbf" "\xbf", ""))   /* the other end of it */
		&& bbs_refuses(bbs_document(prefix + "\xf4" "\x90" "\x80" "\x80", "")) /* past U+10FFFF */
		&& bbs_refuses(bbs_document(prefix + "\xf5" "\x80" "\x80" "\x80", "")) /* no such lead */
		&& bbs_refuses(bbs_document(prefix + "\xc3", ""))                 /* truncated */
		&& bbs_refuses(bbs_document(prefix + "\xe2" "\x82", ""))          /* truncated */
		&& bbs_refuses(bbs_document(prefix + "\x80" "a", ""))             /* a stray continuation */
		&& bbs_refuses(bbs_document(prefix + "\xff", ""));
});

EXO_TEST(bbs_post_reject_nul_in_header, {
	/* ADC has no use for one, and every offset in a header is a byte count. */
	std::string header = "IBB0 ID" BBS_CID_A " SJa";
	header.push_back('\0');
	header += "b";

	return bbs_refuses(bbs_document(header, "body\n"));
});

EXO_TEST(bbs_post_reject_no_newline_at_all, {
	return bbs_refuses("IBB0 ID" BBS_CID_A " SJno\\sterminator")
		&& bbs_refuses("");
});

/* ------------------------------------------------------------------------- *
 * writePostHeader()
 * ------------------------------------------------------------------------- */

EXO_TEST(bbs_write_omits_empty_values, {
	/* Canonical form emits no parameter with an empty value, so an absent subject
	   is an absent parameter rather than "SJ" with nothing after it. */
	QuickDC::Post post;
	post.cid = BBS_CID_A;

	return ADC::writePostHeader(post) == "IBB0 ID" BBS_CID_A;
});

EXO_TEST(bbs_write_escapes_the_shortest_way, {
	QuickDC::Post post;
	post.cid = BBS_CID_A;
	post.subject = "a b\\c\nd";

	return ADC::writePostHeader(post) == "IBB0 ID" BBS_CID_A " SJa\\sb\\\\c\\nd";
});

EXO_TEST(bbs_write_field_order, {
	QuickDC::Post post;
	post.cid = BBS_CID_A;
	post.parent = BBS_TTH_A;
	post.subject = "s";
	post.date = 1;
	post.richTextValue = 1;

	return ADC::writePostHeader(post)
		== "IBB0 ID" BBS_CID_A " PA" BBS_TTH_A " SJs DA1 RT1";
});

/* ------------------------------------------------------------------------- *
 * The board descriptor
 * ------------------------------------------------------------------------- */

EXO_TEST(bbs_board_spec_example, {
	const char* line = "IBBD BDgeneral NIGeneral\\sdiscussion "
		"DEAnything\\sabout\\sthis\\shub PE7 MS262144 TS1786439662 OT1767225600 NP311";

	QuickDC::Board board;
	if (!bbs_read_board(line, board)) return false;

	return board.name == "general"
		&& board.title == "General discussion"
		&& board.description == "Anything about this hub"
		&& board.permissions == 7
		&& board.maxSize == 262144
		&& board.newest == 1786439662
		&& board.oldest == 1767225600
		&& board.posts == 311
		&& !board.removed;
});

EXO_TEST(bbs_board_permissions, {
	/* PE15 is subscribe, post, reply and withdraw one's own; PE5 is an
	   announcements board - subscribe and reply, but not post. */
	QuickDC::Board general;
	QuickDC::Board announcements;

	if (!bbs_read_board("IBBD BDgeneral PE15 MS1 TS0 OT0", general)) return false;
	if (!bbs_read_board("IBBD BDannouncements PE5 MS1 TS0 OT0", announcements)) return false;

	return general.may(QuickDC::Subscribe)
		&& general.may(QuickDC::StartThread)
		&& general.may(QuickDC::Reply)
		&& general.may(QuickDC::WithdrawOwn)
		&& !general.may(QuickDC::WithdrawAny)
		&& announcements.may(QuickDC::Subscribe)
		&& announcements.may(QuickDC::Reply)
		&& !announcements.may(QuickDC::StartThread)
		&& !announcements.may(QuickDC::WithdrawOwn);
});

EXO_TEST(bbs_board_removed_flag, {
	/*
	 * getArgument() hands back what follows the two character name, so the flag is
	 * at [0]. Reading it at [1] is how a flag never takes effect. Every value of RM
	 * but "1" is reserved and means nothing yet.
	 */
	QuickDC::Board gone;
	QuickDC::Board here;
	QuickDC::Board reserved;

	if (!bbs_read_board("IBBD BDgeneral PE1 MS1 TS0 OT0 RM1", gone)) return false;
	if (!bbs_read_board("IBBD BDgeneral PE1 MS1 TS0 OT0", here)) return false;
	if (!bbs_read_board("IBBD BDgeneral PE1 MS1 TS0 OT0 RM2", reserved)) return false;

	return gone.removed && !here.removed && !reserved.removed;
});

EXO_TEST(bbs_board_is_64_bit, {
	/*
	 * A hub can stamp a board however it likes, and a 32 bit conversion is what
	 * turns an implausible timestamp into an ordinary looking one. All of these
	 * are past what 32 bits hold.
	 */
	const char* line = "IBBD BDbig PE1 MS8589934592 TS4294967296 OT4294967297 NP5000000000";

	QuickDC::Board board;
	if (!bbs_read_board(line, board)) return false;

	return board.maxSize == 8589934592ULL
		&& board.newest == 4294967296ULL
		&& board.oldest == 4294967297ULL
		&& board.posts == 5000000000ULL;
});

EXO_TEST(bbs_board_refuses_what_it_cannot_read, {
	/* Refused rather than repaired: a descriptor read with a limit of zero that
	   nobody may post to would be displayed as a board that is exactly that. */
	QuickDC::Board board;
	board.name = "untouched";

	if (bbs_read_board("IBBD BDgeneral MS1 TS0 OT0", board)) return false;          /* no PE */
	if (bbs_read_board("IBBD BDgeneral PE1 TS0 OT0", board)) return false;          /* no MS */
	if (bbs_read_board("IBBD BDgeneral PE1 MS1 OT0", board)) return false;          /* no TS */
	if (bbs_read_board("IBBD BDgeneral PE1 MS1 TS0", board)) return false;          /* no OT */
	if (bbs_read_board("IBBD PE1 MS1 TS0 OT0", board)) return false;                /* no BD */
	if (bbs_read_board("IBBD BDgen/eral PE1 MS1 TS0 OT0", board)) return false;     /* not a name */
	if (bbs_read_board("IBBD BDgeneral PEnone MS1 TS0 OT0", board)) return false;   /* not a number */
	if (bbs_read_board("IBBD BDgeneral PE1 MS1 TS0 OT0 NPmany", board)) return false;

	return board.name == "untouched";
});

/* ------------------------------------------------------------------------- *
 * Index entries
 * ------------------------------------------------------------------------- */

EXO_TEST(bbs_entry_spec_example, {
	const char* line = "IBBL TR" BBS_TTH_A " SI412 BDgeneral ID" BBS_CID_A
		" NIjanvidar TH" BBS_TTH_A " SJHub\\supgrade\\son\\sSaturday TS1786439662";

	QuickDC::BoardEntry entry;
	if (!bbs_read_entry(line, entry)) return false;

	return entry.tth == BBS_TTH_A
		&& entry.size == 412
		&& entry.board == "general"
		&& entry.cid == BBS_CID_A
		&& entry.nick == "janvidar"
		&& entry.parent.empty()
		&& entry.thread == BBS_TTH_A
		&& entry.subject == "Hub upgrade on Saturday"
		&& entry.timestamp == 1786439662
		&& !entry.removed;
});

EXO_TEST(bbs_entry_reply, {
	const char* line = "IBBL TR" BBS_TTH_B " SI298 BDgeneral ID" BBS_CID_B
		" PA" BBS_TTH_A " TH" BBS_TTH_A " SJRe:\\sHub TS1786442600";

	QuickDC::BoardEntry entry;
	if (!bbs_read_entry(line, entry)) return false;

	return entry.parent == BBS_TTH_A && entry.thread == BBS_TTH_A;
});

EXO_TEST(bbs_entry_tombstone, {
	/*
	 * A tombstone carries TR, BD, TS and RM1 and should carry nothing else - the
	 * subject and author of a withdrawn post are usually the reason it was
	 * withdrawn. Requiring SI, ID and TH of every entry would refuse every
	 * withdrawal on every board, which is the one thing a withdrawal cannot
	 * survive.
	 */
	QuickDC::BoardEntry entry;
	if (!bbs_read_entry("IBBL TR" BBS_TTH_A " BDgeneral TS1786443100 RM1", entry))
		return false;

	return entry.removed
		&& entry.tth == BBS_TTH_A
		&& entry.board == "general"
		&& entry.timestamp == 1786443100
		&& entry.size == 0
		&& entry.cid.empty()
		&& entry.thread.empty()
		&& entry.subject.empty();
});

EXO_TEST(bbs_entry_is_64_bit, {
	const char* line = "IBBL TR" BBS_TTH_A " SI5000000000 BDgeneral ID" BBS_CID_A
		" TH" BBS_TTH_A " TS4294967296";

	QuickDC::BoardEntry entry;
	if (!bbs_read_entry(line, entry)) return false;

	return entry.size == 5000000000ULL && entry.timestamp == 4294967296ULL;
});

EXO_TEST(bbs_entry_refuses_what_it_cannot_read, {
	QuickDC::BoardEntry entry;
	entry.tth = "untouched";

	const char* good = "IBBL TR" BBS_TTH_A " SI412 BDgeneral ID" BBS_CID_A
		" TH" BBS_TTH_A " TS1";

	/* Every field a post's entry must have, one at a time. */
	if (bbs_read_entry("IBBL SI412 BDgeneral ID" BBS_CID_A " TH" BBS_TTH_A " TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " BDgeneral ID" BBS_CID_A " TH" BBS_TTH_A " TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 ID" BBS_CID_A " TH" BBS_TTH_A " TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 BDgeneral TH" BBS_TTH_A " TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 BDgeneral ID" BBS_CID_A " TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 BDgeneral ID" BBS_CID_A " TH" BBS_TTH_A, entry)) return false;

	/* And every one of them malformed rather than missing. */
	if (bbs_read_entry("IBBL TRSHORT SI412 BDgeneral ID" BBS_CID_A " TH" BBS_TTH_A " TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SImany BDgeneral ID" BBS_CID_A " TH" BBS_TTH_A " TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 BDgen\\seral ID" BBS_CID_A " TH" BBS_TTH_A " TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 BDgeneral IDx TH" BBS_TTH_A " TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 BDgeneral ID" BBS_CID_A " THSHORT TS1", entry)) return false;
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 BDgeneral ID" BBS_CID_A " TH" BBS_TTH_A " TSnow", entry)) return false;

	/* A parent that names nothing that can ever be fetched. */
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 BDgeneral ID" BBS_CID_A " PASHORT TH" BBS_TTH_A " TS1", entry)) return false;

	if (entry.tth != "untouched") return false;

	/* The same entry with nothing wrong with it, so that the above is refusing
	   the field it names and not the shape of the command. */
	QuickDC::BoardEntry ok;
	return bbs_read_entry(good, ok) && ok.tth == BBS_TTH_A;
});

EXO_TEST(bbs_entry_tombstone_still_needs_its_own_fields, {
	QuickDC::BoardEntry entry;

	if (bbs_read_entry("IBBL BDgeneral TS1 RM1", entry)) return false;            /* no TR */
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " TS1 RM1", entry)) return false;      /* no BD */
	if (bbs_read_entry("IBBL TR" BBS_TTH_A " BDgeneral RM1", entry)) return false; /* no TS */

	/* And a tombstone the hub decided to say more in is not refused for it. */
	QuickDC::BoardEntry verbose;
	if (!bbs_read_entry("IBBL TR" BBS_TTH_A " SI412 BDgeneral ID" BBS_CID_A
		" TH" BBS_TTH_A " TS2 RM1", verbose)) return false;

	return verbose.removed && verbose.size == 412;
});

/* ------------------------------------------------------------------------- *
 * Feature negotiation
 * ------------------------------------------------------------------------- */

EXO_TEST(bbs_feature_is_negotiated_in_sup, {
	/* A hub feature and nothing else: offered in SUP, answered in SUP, and never a
	   flag in INF's SU, because no client ever sends a bulletin to another. */
	const char* buf = "ISUP ADBASE ADTIGR AD" ADC_EXT_BULLETIN;

	ADC::Command command;
	if (!ADC::Parser::parse(buf, strlen(buf), command)) return false;

	bool found = false;
	for (size_t n = 0; command.getArgument(n); n++)
		if (strcmp(command.getArgument(n), "AD" ADC_EXT_BULLETIN) == 0) found = true;

	return found && command.isCommandHubInfo() && strcmp(ADC_EXT_BULLETIN, "BBS0") == 0;
});

EXO_TEST(bbs_status_codes, {
	/* Severity 1 for all of them: none is a reason to disconnect a client. */
	return adc_status_code(ADC_STATUS_SEVERITY_ERROR, ADC_CODE_BULLETIN_ERROR) == "170"
		&& adc_status_code(ADC_STATUS_SEVERITY_ERROR, ADC_CODE_BULLETIN_NO_BOARD) == "171"
		&& adc_status_code(ADC_STATUS_SEVERITY_ERROR, ADC_CODE_BULLETIN_TOO_LARGE) == "172"
		&& adc_status_code(ADC_STATUS_SEVERITY_ERROR, ADC_CODE_BULLETIN_RATE) == "175"
		&& adc_status_code(ADC_STATUS_SEVERITY_ERROR, ADC_CODE_BULLETIN_NO_ENTRY) == "176";
});
