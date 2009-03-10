/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdio.h>
#include <string>
#include <stdlib.h>
#include "network/dc/clientcommands.h"

#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

/**
 * $MyNick something
 */
bool DC::ClientMyNick::ClientMyNick::invoke(const char* data, size_t length) {
	if (length < 8) return false;
	const char* nick = &data[8];
	session->onNick(nick);
	return true;
}


/**
 * $Lock somelock Pk=data
 * Pk is never used, but could probably be used to get client version data.
 */
bool DC::ClientLock::ClientLock::invoke(const char* data, size_t length) {
	if (length < 12) return false;
	const char* split = strstr(data, " Pk="); // FIXME: Make sure we find the last occurence!
	if (!split) return false;
	const char* pk = &split[4];
	char* lock = strndup(&data[6], &split[0]-&data[6]);
	session->onLock(lock, pk);
	free(lock);
	return true;
}

/**
 * $Key data -- remote user sent a key, let's validate it before we continue.
 */
bool DC::ClientKey::ClientKey::invoke(const char* data, size_t length) {
	if (length < 5) return false;
	const char* key = &data[5];
	session->onKey(key);
	return true;
}

/**
 * This is the direction the other party is trying to negotiate.
 * $Direction (Upload|Download) (10000-32767)
 */
bool DC::ClientDirection::ClientDirection::invoke(const char* data, size_t length) {
	if (length < 19) return false;
	int priority;
	bool upload;
	const char* split = strrchr(data, ' ');
	
	if (strncmp(&data[11], "Upload", 6) == 0) upload = true;
	else if (strncmp(&data[11], "Download", 8) == 0) upload = false;
	else return false;
	
	priority = quickdc_atoi(&split[1]);
	if (priority == 0) return false;
	
	session->onDirection( (upload) ? DC::ClientSession::DirUpload : DC::ClientSession::DirDownload, priority);
	return true;
}

/**
 * $Error somestring
 */
bool DC::ClientError::ClientError::invoke(const char* data, size_t length) {
	if (length < 7) return false;
	const char* message = &data[7];
	session->onError(message);
	return true;
}

/**
 * $FileLength size
 */
bool DC::ClientFileLength::ClientFileLength::invoke(const char* data, size_t length) {
	if (length < 12) return false;
	uint64_t size = quickdc_atoll(&data[12]);
	session->onFileSize(size);
	return true;
}

/**
 * $MaxedOut (no params) -- other party does not have any free slots available
 */
bool DC::ClientMaxedOut::ClientMaxedOut::invoke(const char*, size_t) {
	session->onMaxedOut();
	return true;
}

/**
 * $GetListLen (no params) -- requested the length of MyList.DcLst
 * Note: this is a compatibility function for NMDC only, never used by QuickDC
 */
bool DC::ClientGetListLen::ClientGetListLen::invoke(const char*, size_t) {
	session->onGetListLen();
	return true;
}

/**
 * $Get filename$(offset+1)
 * Or when CHUNK is enabled: 
 * $Get filename$(offset+1)$length
 *
 * FIXME: Does this mean that if CHUNK is available we are guaranteed to see the enhanced $Get?
 */
bool DC::ClientGet::ClientGet::invoke(const char* data, size_t length) {
	if (length < 8) return false;
	
	const char* split = strrchr(data, '$');
	if (!split) return false;
	
	const char* split2 = strrchr(&split[0], '$');
	
	if (session->supports("CHUNK") && split2) {
		char* filename = strndup(&data[5], &split2[0]-&data[5]);
		char* off = strndup(&split2[1], &split[0]-&split2[1]);
	
		uint64_t offset = quickdc_atoll(off) - 1;
		uint64_t length = quickdc_atoll(&split[1]);
		
		session->onGet(filename, offset, length);
		
		free(filename);
		free(off);
	} else {
		char* filename = strndup(&data[5], &split[0]-&data[5]);
		uint64_t offset = quickdc_atoll(&split[1]) - 1;
		session->onGet(filename, offset, -1);
		free(filename);
	}
	
	return true;
}

/**
 * $Supports -- list of supported extensions
 */
bool DC::ClientSupports::ClientSupports::invoke(const char* data, size_t length) {
	if (length < 9) return false;
	
	const char* start = &data[10];
	const char* split;
	while ((split = strchr(&start[0], ' ')) != 0) {
		char* feature = strndup(&start[0], &split[0]-&start[0]);
		session->addSupport(feature);
		free(feature);
		start = &split[1];
	}

	return true;
}

/**
 * $Send (no params) -- start transmitting file
 */
bool DC::ClientSend::ClientSend::invoke(const char*, size_t) {
	session->onSend();
	return true;
}

/**
 * $Failed message -- used when a file does not exist, as a response to $Get
 */
bool DC::ClientFailed::ClientFailed::invoke(const char* data, size_t length) {
	if (length < 8) return false;
	const char* message = &data[8];
	session->onError(message);
	return true;
}

/**
 * $GetZBlock [offset] [length] [filename]
 *
 * Quirks: If length is -1, this means until EOF.
 * Must answer: "$Sending [bytes]|[compressed data]"
 *          or: "$Failed [reason]|
 */
bool DC::ClientExtGetZBlock::ClientExtGetZBlock::invoke(const char* data, size_t length) {
	if (length < 15) return false;
	const char* split1 = strchr(&data[11], ' ');
	if (!split1) return false;
	
	const char* split2 = strchr(&split1[1], ' ');
	if (!split2) return false;
	
	char* offset = strndup(&data[11], &split1[0]-&data[11]);
	char* len = strndup(&split1[1], &split2[0]-&split1[1]);
	const char* filename = &split2[1];

	// FIXME: handle these
	session->onGetZBlock(filename, 0, -1);

	free(offset);
	free(len);

	return true;
}

/**
 * $ADCGET
 * $ADCGET {type} filename offset length [ZL1]
 *
 * type must be one of:
 * - file
 * - list
 * - tth1
 *
 * Notes:
 * - The ZL1 is optional and might not be set at all.
 * - The filename might contain spaces, so we need to parse backwards.
 *
 * Examples:
 * - "$ADCGET list / 0 -1 ZL1"
 * - "$ADCGET list / 0 -1"
 * - "$ADCGET file /files.xml 0 -1"
 * - "$ADCGET tth1 TTH/PPUROLR2WSYTGPLCM3KV4V6LJC36SCTFQJFDJKA 0 -1"
 */
bool DC::ClientExtADCGET::ClientExtADCGET::invoke(const char* data_, size_t length) {
	if (length < 18) return false;

	uint32_t type = FOURCC(data_[8], data_[9], data_[10], data_[11]);

	switch (type) {
		case FOURCC('f','i','l','e'):
		case FOURCC('l','i','s','t'):
		case FOURCC('t','t','h','1'):
			break;
		default:
			return false;
	}

	char* data = strdup(data_);
	char* split = 0;
	bool zl1 = false;
	uint64_t offset = 0;
	int64_t len = 0;

	// Check for ZL1 (zlib transfer)
	split = strrchr(data, ' ');
	if (!split || strlen(split) < 2) { free(data); return false; }
	if (strncmp(split, " ZL1", 4) == 0) {
		zl1 = true;
		split[0] = '\0';
		split = strrchr(data, ' ');
	}

	// Extract chunk length
	if (!split)split = strrchr(data, ' ');
	len = quickdc_atoll(&split[1]);
	split[0] = '\0';
	split = strrchr(data, ' ');

	// Extract the offset
	if (!split) { free(data); return false; }
	length = quickdc_atoull(&split[1]);
	split[0] = '\0';

	// Extract the filename;	
	char* filename = strdup(&data[13]);


	QDBG("ADCGET: filename='%s', offset='%u', length='%l', type=%d\n", filename, offset, len, type);
	session->onADCGet(filename, type, offset, len, zl1);
	
	free(data);
	return true;	
}

/**
 * Syntax: exactly like $ADCGET.
 */
bool DC::ClientExtADCSND::ClientExtADCSND::invoke(const char* data_, size_t length) {
	if (length < 18) return false;

	uint32_t type = FOURCC(data_[8], data_[9], data_[10], data_[11]);

	switch (type) {
		case FOURCC('f','i','l','e'):
		case FOURCC('l','i','s','t'):
		case FOURCC('t','t','h','1'):
			break;
		default:
			return false;
	}

	char* data = strdup(data_);
	char* split = 0;
	bool zl1 = false;
	uint64_t offset = 0;
	int64_t len = 0;

	// Check for ZL1 (zlib transfer)
	split = strrchr(data, ' ');
	if (!split || strlen(split) < 2) { free(data); return false; }
	if (strncmp(split, " ZL1", 4) == 0) {
		zl1 = true;
		split[0] = '\0';
		split = strrchr(data, ' ');
	}

	// Extract chunk length
	if (!split)split = strrchr(data, ' ');
	len = quickdc_atoll(&split[1]);
	split[0] = '\0';
	split = strrchr(data, ' ');

	// Extract the offset
	if (!split) { free(data); return false; }
	length = quickdc_atoull(&split[1]);
	split[0] = '\0';

	// Extract the filename;	
	char* filename = strdup(&data[13]);


	QDBG("ADCSND: filename='%s', offset='%u', length='%l', type=%d\n", filename, offset, len, type);
	session->onADCSend(filename, type, offset, len, zl1);
	
	free(data);
	return true;	
}

// eof
