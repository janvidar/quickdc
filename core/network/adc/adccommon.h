/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADC_COMMON_H
#define HAVE_QUICKDC_ADC_COMMON_H

#include "quickdc.h"

typedef uint32_t sid_t;
typedef uint32_t bbs_message_t;

#define ADC_COMPLIANCE                  "ADC/1.0"
#define ADC_BASE                        "BASE" /* the base set of commands */
#define ADC_BASE_COMPAT                 "BAS0" /* deprectated: old ADC version */
#define ADC_HASH_TIGER                  "TIGR" /* tiger hash support */
#define ADC_EXT_PING                    "PING" /* hub: pinger support */
#define ADC_EXT_ADCS                    "ADCS" /* tls/ssl encrypted channel extension */
#define ADC_EXT_BBS                     "BBS0" /* hub: bulletin board system */
#define ADC_EXT_UCMD                    "UCMD" /* hub: user command extension */
#define ADC_EXT_BZIP                    "BZIP" /* p2p: bzip compressed files.xml */
#define ADC_EXT_ZLIG                    "ZLIG" /* p2p: zlib transfer */
#define ADC_EXT_ZLIF                    "ZLIF" /* full compressed connection (zon, zoff) */

#define ADC_INDEX_FILE                  "files.xml"
#define ADC_INDEX_FILE_BZ2              "files.xml.bz2"

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


#endif // HAVE_QUICKDC_ADC_COMMON_H

