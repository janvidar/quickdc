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
 * ADCS/1.0 is what ADC-EXT specifies and what this client names. ADCS/0.10 is the
 * number the extension carried while it was a draft; the DC++ family still names
 * that one, so it is understood when read even though it is never sent - refusing
 * it would refuse the transfer. See adc_is_tls_protocol().
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
 * What a client says it can be reached by, in INF's SU.
 *
 * TCP4 is the one every other client acts on: it says a peer told to connect will
 * get through. UDP4 says the same about search results. Neither is verified by
 * anything in base ADC, which is what ClaimCheck is about.
 */
#define ADC_EXT_TCP4                    "TCP4"
#define ADC_EXT_UDP4                    "UDP4"

/*
 * Rich text: markdown chat, the RT flag on MSG.
 *
 * One fourcc in two places. In SUP a client offers it with "ADRTF0" and may only send the
 * RT flag if the hub answers with the same, so a hub decides whether markdown - and the
 * media a message can point at - is carried at all. In INF's SU it tells the other
 * clients that this one will render what it is sent.
 */
#define ADC_EXT_RICHTEXT                "RTF0"

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


#endif // HAVE_QUICKDC_ADC_COMMON_H
