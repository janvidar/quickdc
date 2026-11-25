/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADC_COMMON_H
#define HAVE_QUICKDC_ADC_COMMON_H

#include <cstdio>
#include <string>
#include <string_view>

#include "quickdc.h"

typedef uint32_t sid_t;

#define ADC_COMPLIANCE                  "ADC/1.0"
/*
 * What the CTM protocol field says for an encrypted peer connection.
 *
 * ADCS/1.0 is what ADC-EXT specifies. ADCS/0.10 is the number the extension
 * carried while it was a draft, and the DC++ family still names that one - so both
 * are understood when read, and which of them is sent follows what the peer said it
 * speaks rather than what this client would rather use. A peer only recognises the
 * spelling it announced: answering a client that knows the draft alone with
 * ADCS/1.0 names a protocol it has never heard of, and it refuses its own transfer.
 * See adc_is_tls_protocol() and ADC::peerTlsProtocol().
 */
#define ADC_TLS_PROTO                   "ADCS/1.0"
#define ADC_TLS_PROTO_DRAFT             "ADCS/0.10"
#define ADC_BASE                        "BASE" /* the base set of commands */
#define ADC_BASE_COMPAT                 "BAS0" /* deprectated: old ADC version */
#define ADC_HASH_TIGER                  "TIGR" /* tiger hash support */
#define ADC_EXT_PING                    "PING" /* hub: pinger support */
#define ADC_EXT_ADCS                    "ADCS" /* tls/ssl encrypted channel extension */
#define ADC_EXT_UCMD                    "UCMD" /* hub: user command extension */
#define ADC_EXT_BZIP                    "BZIP" /* p2p: bzip compressed files.xml */
#define ADC_EXT_ZLIG                    "ZLIG" /* p2p: zlib transfer */
#define ADC_EXT_ZLIF                    "ZLIF" /* full compressed connection (zon, zoff) */
#define ADC_EXT_PFSR                    "PFSR" /* partial file sharing (PSR) */
#define ADC_EXT_NATT                    "NATT" /* p2p: NAT traversal for two passive clients */

/*
 * The same two extensions as their drafts named them in SU, and the names most of
 * the network still reads.
 *
 * A fourcc in SU is matched whole, so ADCS and ADC0 are two unrelated words to
 * every client on the hub: one that only knows ADC0 - which is the DC++ family, and
 * so most of what is out there - sees a client announcing ADCS alone as one with no
 * encryption at all, and arranges a plaintext transfer or none. Announcing both
 * costs five bytes in an INF and is the difference between being offered an
 * encrypted connection and not.
 *
 * Read the same way round: either flag is taken as the extension being supported,
 * and which one the peer announced decides what is named back at it.
 */
#define ADC_EXT_ADCS_DRAFT              "ADC0" /* p2p: ADCS, as the draft named it */
#define ADC_EXT_NATT_DRAFT              "NAT0" /* p2p: NATT, as the draft named it */

/*
 * What a client says it can be reached by, in INF's SU.
 *
 * TCP4 is the one every other client acts on: it says a peer told to connect will
 * get through. UDP4 says the same about search results. Neither is verified by
 * anything in base ADC, which is what ClaimCheck is about.
 */
#define ADC_EXT_TCP4                    "TCP4"
#define ADC_EXT_UDP4                    "UDP4"

/*
 * And the same two claims about the IPv6 endpoint, which is a different endpoint
 * with a different answer: a home connection routinely carries native IPv6 while
 * every IPv4 attempt dies in the carrier's NAT, so a client can be worth dialling
 * on one family and not on the other. Claimed and withdrawn independently, from
 * the evidence gathered for that family alone.
 */
#define ADC_EXT_TCP6                    "TCP6"
#define ADC_EXT_UDP6                    "UDP6"

/*
 * Rich text: markdown chat, the RT flag on MSG.
 *
 * One fourcc in two places. In SUP a client offers it with "ADRTF0" and may only send the
 * RT flag if the hub answers with the same, so a hub decides whether markdown - and the
 * media a message can point at - is carried at all. In INF's SU it tells the other
 * clients that this one will render what it is sent.
 */
#define ADC_EXT_RICHTEXT                "RTF0"

/*
 * Bulletin boards: durable threaded posts on a hub, the BBD, BBL and BBP commands.
 *
 * One fourcc in one place, which is the contrast worth writing down beside the one
 * above. Rich text is offered in SUP and announced again in INF's SU, because a
 * client sends rich text to other clients and they have to know it is coming. A
 * bulletin is only ever sent to a hub and only ever received from one, so there is
 * nothing for another client to be told: SUP is the whole of the negotiation, and
 * the absence of an SU flag here is deliberate rather than an omission.
 *
 * The hub's answer is authoritative for the connection and is not cached across
 * one, since a hub may announce this later than its first ISUP and may withdraw it
 * with RMBBS0. A hub must not announce it without also announcing TIGR: every
 * identifier in the extension is a tiger tree root, and a client that cannot hash
 * one cannot fetch a post.
 */
#define ADC_EXT_BULLETIN                "BBS0"

#define ADC_INDEX_FILE                  "files.xml"
#define ADC_INDEX_FILE_BZ2              "files.xml.bz2"

/** Whether a CTM protocol field names an encrypted peer connection. */
inline bool adc_is_tls_protocol(std::string_view protocol)
{
	return protocol == ADC_TLS_PROTO || protocol == ADC_TLS_PROTO_DRAFT;
}


/*
 * One status field: severity digit followed by a two digit code.
 *
 * Sending the code on its own leaves a peer reading the severity off the first
 * digit of the code - 42 read as severity 4, which is not a severity at all.
 */
inline std::string adc_status_code(int severity, int code)
{
	char buf[8];
	snprintf(buf, sizeof(buf), "%d%02d", severity, code);
	return std::string(buf);
}


#define ADC_STATUS_SEVERITY_INFO        0
#define ADC_STATUS_SEVERITY_ERROR       1 /* recoverable error */
#define ADC_STATUS_SEVERITY_FATAL       2 /* fatal error, also leads to disconnect */

#define ADC_STATUS_DOMAIN_GENERIC       0
#define ADC_STATUS_DOMAIN_HUB           1
#define ADC_STATUS_DOMAIN_ACCESS        2
#define ADC_STATUS_DOMAIN_SECURITY      3
#define ADC_STATUS_DOMAIN_PROTOCOL      4
#define ADC_STATUS_DOMAIN_TRANSFER      5

#define ADC_CODE_HUB_ERROR              10
#define ADC_CODE_HUB_FULL               11
#define ADC_CODE_HUB_DISABLED           12

#define ADC_CODE_ACCESS_ERROR           20
#define ADC_CODE_ACCESS_NICK_ERROR      21
#define ADC_CODE_ACCESS_NICK_USED       22
#define ADC_CODE_ACCESS_PASSWORD        23
#define ADC_CODE_ACCESS_CID_USED        24
#define ADC_CODE_ACCESS_DENIED          25
#define ADC_CODE_ACCESS_REGISTERED      26
#define ADC_CODE_ACCESS_PID_ERROR       27

#define ADC_CODE_SECURITY_GENERIC       30
#define ADC_CODE_SECURITY_BAN_PERM      31
#define ADC_CODE_SECURITY_BAN_TEMP      32 /* Specify: 'TL' */

#define ADC_CODE_PROTOCOL_ERROR         40
#define ADC_CODE_PROTOCOL_UNSUPPORTED   41 /* Specify: 'TO' (token), 'PR' (protocol). Context CTM/RCM */
#define ADC_CODE_PROTOCOL_CONNECT_ERROR 42 /* Specify: 'TO' (token), 'PR' (protocol). Context CTM/RCM */
#define ADC_CODE_PROTOCOL_FLAG_REQUIRED 43 /* Specify: 'FL' (flag). Context: INF */
#define ADC_CODE_PROTOCOL_STATE_INVALID 44 /* Specify: 'FC' (fourcc). */
#define ADC_CODE_PROTOCOL_FEATURE_ERROR 45 /* Specify: 'FC' (fourcc). Context: All feature casts */
#define ADC_CODE_PROTOCOL_IP_INF_ERROR  46 /* Specify: 'IP' (ip address). Context: INF */

#define ADC_CODE_TRANSFER_ERROR         50
#define ADC_CODE_TRANSFER_FILE_ERROR    51 /* File not available */
#define ADC_CODE_TRANSFER_FILE_PART     52 /* Part of file not available */
#define ADC_CODE_TRANSFER_SLOT_ERROR    53 /* No more slots */

/*
 * Bulletin boards. Requested by the BBS0 draft rather than registered, so a hub on
 * the other side of the connection may know nothing of them - which costs a client
 * an unrecognised code and no more, every one of them being severity 1: none is a
 * reason to disconnect anybody.
 *
 * The 6x range would have been the obvious home and is not available: ADC-EXT has
 * allocated 62 inside it without ever defining a 60, so a category cannot be opened
 * there without claiming numbers around one already spent.
 *
 * Only the conditions a client can act on differently have a code of their own. A
 * board that is gone can be dropped from the interface, a post that is too large can
 * be reported against the limit, a rate limit can be waited out, and a post that is
 * not indexed can stop being asked for. Everything else a hub already has a better
 * word for, and where ADC has one it is used instead of these: ACCESS_DENIED for a
 * missing permission, ACCESS_REGISTERED for a board open to registered users only,
 * PROTOCOL_ERROR for a malformed command - a BBL carrying both TR and TS is one -
 * and PROTOCOL_FLAG_REQUIRED for a field that is missing or invalid.
 */
#define ADC_CODE_BULLETIN_ERROR         70 /* Generic: an unknown parent, a duplicate post, too many subscriptions. */
#define ADC_CODE_BULLETIN_NO_BOARD      71 /* Specify: 'BD' (the name that was not found). */
#define ADC_CODE_BULLETIN_TOO_LARGE     72 /* Specify: 'MS' (the board's limit). */
#define ADC_CODE_BULLETIN_RATE          75 /* Specify: 'TL' (seconds until the next post is allowed). */
#define ADC_CODE_BULLETIN_NO_ENTRY      76 /* No index entry for the requested post. */

#define ADC_STR_TRANSFER_FILE_ERROR     "File not available"
#define ADC_STR_TRANSFER_FILE_PART      "Requested part of file is not available"
#define ADC_STR_TRANSFER_SLOT_ERROR     "No more slots available"

/*
 * What is said about a connection that failed.
 *
 * Phrases of ours, chosen here, because the alternative is the C library's text
 * for whatever the local errno was: the hub broadcasts a lost connection to
 * every other user in an IQUI, and a client tells the peer that asked for a
 * connection about it in a DSTA. Either one carries the host's operating system
 * and its error conditions to somebody who has no business knowing them. The
 * errno belongs in the log, which stays here.
 */
#define ADC_STR_CONNECTION_LOST         "Connection lost"
#define ADC_STR_CONNECTION_FAILED       "Connection failed"
#define ADC_STR_CONNECTION_UNEXPECTED   "Connection not expected"

/* How long a peer connection has from coming up to both sides having named themselves. */
#define QUICKDC_PEER_HANDSHAKE_SECONDS  30

/*
 * How long a NAT traversal dial has to become a connection. Both ends dial at
 * once, so a SYN either gets through within the first few retransmissions or the
 * NATs in between will never let it: waiting out the kernel's two minutes only
 * delays the failure.
 */
#define QUICKDC_TRAVERSAL_SECONDS       10

namespace ADC {

/*
 * How a peer connection came to be, which decides who is the TLS client and who
 * opens the C-C handshake with CSUP - the same end for both.
 *
 * A NATT connection is dialled by both ends at once and meets as a TCP
 * simultaneous open, so each end's connect() succeeds and neither tells it which
 * end it is. The rule is the one DC++ follows: the end that sent DNAT plays the
 * dialled end, the end that answered with DRNT the dialling one. Both ends know
 * which command they sent, so they agree without saying anything more.
 */
enum class PeerDial { Out, In, SentNat, SentRnt };

struct PeerRole {
	bool tlsServer;
	bool speaksFirst;
};

constexpr PeerRole peerRole(PeerDial dial)
{
	const bool dialler = dial == PeerDial::Out || dial == PeerDial::SentRnt;
	return PeerRole{!dialler, dialler};
}

}


#endif // HAVE_QUICKDC_ADC_COMMON_H
