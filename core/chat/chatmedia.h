/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CHAT_CHATMEDIA_H
#define HAVE_QUICKDC_CHAT_CHATMEDIA_H

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace QuickDC {

class Preferences;

/** A file a message names by its hash, as a magnet URI. */
struct ChatAttachment {
	/** Tiger tree root, base32; empty where the URI named none of the right shape. */
	std::string tth;
	/** From 'dn', reduced to a name: a sender does not choose where a file lands. */
	std::string name;
	/** From 'xl'; 0 where the sender left it out. */
	uint64_t size = 0;
	/** Written as a picture to show, '![...](...)', rather than a file to offer. */
	bool inlineMedia = false;
};

/** Only xt, dn and xl are read, and only a tiger root of the right shape is taken. */
ChatAttachment readMagnet(std::string_view uri);

/**
 * Every magnet a markdown message links to, in order: inline links, images and
 * autolinks, outside code spans and code blocks.
 */
std::vector<ChatAttachment> attachmentsIn(std::string_view markdown);

/** Why a picture in a message was not fetched by itself. */
enum class ChatMediaSkip {
	None,
	/** Fetching is off, on this hub or in general. */
	Off,
	/** The message gave no size, so nobody can say what it would cost. */
	Unsized,
	/** Larger than the limit. */
	TooLarge
};

/** Whether a picture of 'size' is fetched unasked; 'limit' 0 is none. */
ChatMediaSkip chatMediaVerdict(uint64_t size, bool fetching, uint64_t limit);

/** Whether pictures on the hub at 'hubUrl' are fetched unasked: its own choice, or the general one. */
bool chatMediaFetchedOn(const Preferences& config, const Preferences& session, const std::string& hubUrl);

/** The name a pasted picture is kept under, for when it was pasted. */
std::string pastedName(std::time_t when, std::string_view suffix);

/**
 * Remove the pasted pictures in 'folder' last written more than 'days' before
 * 'now'; 0 keeps them all. Only what pastedName() names. Returns how many went.
 */
int sweepPasted(const std::string& folder, int days, std::time_t now);

}

#endif // HAVE_QUICKDC_CHAT_CHATMEDIA_H
