/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdio.h>
#include <string.h>

#include "quickdc.h"
#include "api/core.h"
#include "api/hub.h"
#include "network/hubmanager.h"
#include "network/dc/commands.h"
#include "network/dc/hubcommands.h"
#include "network/dc/dccommandproxy.h"
#include "network/dc/tag.h"
#include "network/user.h"
#include "network/usermanager.h"
#include "share/sharemanager.h"
#include "share/searchrequest.h"
#include "share/filetypes.h"
#include "network/dc/lock.h"
#include "network/dc/textescape.h"

/**
 * Parse the $ConnectToMe command
 *
 * Syntax: 
 * Syntax: $ConnectToMe <RemoteNick> <SenderIp>:<SenderPort> 
 *         $ConnectToMe <SenderNick> <RemoteNick> <SenderIp>:<SenderPort> (alternative, who use this?)
 */
bool DC::ConnectToMe::invoke(const char* data, size_t) {
	char* address = 0;
	char* remotenick = 0;
	char* sendernick = 0;
	
	// isolate senderip:port from last space
	const char* req = strrchr(data, ' ');
	if (!req) return false;
	
	// detect address:port.
	const char* port_split = strchr(&req[1], ':');
	if (!port_split) return false;
	
	// detect if one or two nicks are present
	const char* usplit = strchr(&data[13], ' ');
	if (usplit != req) {
		sendernick = strndup(&data[13], &usplit[0]-&data[13]);
		remotenick = strndup(&usplit[1], &req[0]-&usplit[1]);
	} else {
		remotenick = strndup(&data[13], &usplit[0]-&data[13]);
	}
	
	address = strndup(&req[1], &port_split[0]-&req[1]);
	uint16_t port = (uint16_t) atoi(&port_split[1]);
	
	session->onActiveConnect(address, port, sendernick);
	
	free(address);
	free(remotenick);
	free(sendernick);
	
	return true;
}

/**
 * Parse the $RevConnectToMe command
 * Syntax: $RevConnectToMe {from} {to}
 * Note: {from} is the remote user, {to} is our nick.
 *
 * Smallest possible string:
 * "$RevConnectToMe a b"
 */
bool DC::RevConnectToMe::invoke(const char* data, size_t length) {
	if (length < 19) return false;
	const char* split = strrchr(data, ' ');
	if (!split) return false;
	char* user = strndup(&data[16], &split[0]-&data[16]);
	session->onPassiveConnect(user);
	free(user);
	return true;
}


/**
 * Parse the $ForceMove command
 * Syntax: $ForceMove {host[:port]}
 * Smallest possible string:
 * "$ForceMove x:1" 13 bytes.
 * Default port is 411 in DC.
 */
bool DC::ForceMove::invoke(const char* data, size_t length) {
	if (length < 13) return false;

	uint16_t port = 411;
	char* addr = 0;
	const char* split = strchr(data, ':');
	if (!split) {
		addr = strdup(&data[11]);
	} else {
		port = (uint16_t) atoi(&split[1]);
		addr = strndup(&data[11], &split[0]-&data[11]);
	}
	
	session->onRedirect(addr, port);
	free(addr);
	return true;
}

/**
 * Handle the $GetPass command. There are no parameters for this command,
 * thus there isn't much to do here.
 */
bool DC::GetPass::invoke(const char*, size_t) {
	session->onRequestPasword();
	return true;
}

/**
 * Handle the $BadPass command. There are no parameters for this command,
 * thus there isn't much to do here.
 */
bool DC::BadPass::invoke(const char*, size_t) {
	session->onBadPassword();
	return true;
}

/**
 * Parse the $Hello command.
 * Syntax: $Hello {string}
 * Smallest possible string: "$Hello a" 8 bytes.
 *
 * There was some message once that the first hello
 * a client receives is the username for that session,
 * but I am not sure whether this is implemented anywhere.
 */
bool DC::Hello::invoke(const char* data, size_t length)
{
	if (length < 8) return false;
	const char* nick = &data[7];
	
	if (strcmp(nick, session->getNick()) == 0)
	{
		session->onRequestMyInfo(nick);
	}
	else
	{
		session->users->join(nick);
	}
	return true;
}

/**
 * $HubIsFull command/error. No parameters.
 * NOTE: only some hubs actually use this, most just prints a
 * message and disconnects.
 */
bool DC::HubIsFull::invoke(const char*, size_t) {
	
	return false;
}

/**
 * Parse the $HubName command
 * Syntax: $HubName {string}
 * Smallest possible string:
 * "$HubName a" 10 bytes.
 */
bool DC::HubName::invoke(const char* data, size_t length) {
	if (length < 10) return false;
	const char* name = &data[9];
	session->onHubName(name);
	return true;
}

/**
 * Parse the $Lock command
 *
 * Syntax: $Lock {data} Pk={data}
 * Each datablock must be at least one byte and not a space. (Should 
 * not at least).
 *
 * Shortest lock: "$Lock a Pk=b", 12 bytes
 */
bool DC::LockParser::invoke(const char* data, size_t length) {
	if (length < 12) return false;
	const char* split = strstr(data, " Pk=");
	if (!split) return false;
	const char* pk = &split[4];
	char* lock = strndup(&data[6], &split[0]-&data[6]);
	session->onLock(lock, pk);
	free(lock);
	return true;
}

/**
 * Handle the $LogedIn command (yes, it's a protocol typo).
 * There are no parameters for this command, thus there isn't much to do here.
 */
bool DC::LoggedIn::invoke(const char*, size_t) {
	session->onLoggedIn();
	return true;
}

/**
 * Parse the $MyINFO command
 * Syntax:
 * $MyINFO $ALL nickname description$ $speed[flags]$email$sharedsizeinbyte$
 * 
 * flags - a special byte with these values possibly or'ed together:
 * 0x01 - alway set
 * 0x02 - user is away
 * 0x04 - user is a server (uptime 2h)
 * 0x08 - user is fast (sending more than 100KB/s)
 * 
 * Variations:
 * $MyINFO $ALL nickname description$(A|P|\s)$(speed)$(mail)$(sharesize)$
 * The AP - part tells whether or not the user is in passive or active mode.
 * This is a PtokaX hub extension.
 *
 * TODO: Add active/passive mode checks here.
 *       Add flags parser here.
 */
bool DC::MyINFO::invoke(const char* data_, size_t) {
	char* data = strdup(&data_[13]);
	char* start = &data[0];
	char* split = 0;

	const char* nickname = 0;
	char* description = 0;
	const char* email = 0;
	const char* sharesize = 0;
	DC::Tag* tag = 0;
	char* speed = 0;
	uint8_t flags = 0x00;
	uint64_t shared = 0ULL;

	// find and extract nickname
	split = strchr(start, ' ');
	if (!split) { free(data); return false; }
	nickname = start;
	split[0] = '\0';
	start = &split[1];

	// find description
	split = strchr(start, '$');
	if (!split && split != data) { free(data); return false; }
	description = start;
	split[0] = '\0';
	if (split[2] != '$') { free(data); return false; };
#if 0
	if (split[1] == 'A');      /* PtokaX: Active mode user */
	else if (split[1] == 'P'); /* PtokaX: Passive mode user */
#endif //0
	start = &split[3];
	
	// find and extract speed
	split = strchr(start, '$');
	if (!split) { free(data); return false; }
	speed = start;
	split[0] = '\0';
	start = &split[1];

	// find and extract email
	split = strchr(start, '$');
	if (!split) { free(data); return false; }
	email = start;
	split[0] = '\0';
	start = &split[1];

	// find and extract share size
	split = strchr(start, '$');
	if (!split) { free(data); return false; }
	sharesize = start;
	split[0] = '\0';
	shared = quickdc_atoll(sharesize);
	
	// extract flags from speed field.
	if (strlen(speed) > 1) {
		size_t pos = strlen(speed)-1;
		flags = (uint8_t) speed[pos];
		speed[pos] = '\0';
	} else flags = 0x01;
	
	// extract tag from description string
	split = strrchr(description, '<');
	if (split) {
		char* split2 = strrchr(split, '>');
		if (split2) {
			tag = new Tag(split);
			split[0] = '\0';
		}
	}

	if (!(flags | 0x01)) flags |= 0x01;

	QuickDC::User* u = (QuickDC::User*) session->users->getUser(nickname);
	bool changed = false;
	uint64_t oldSize = 0;

	if (!u) {
		u = new QuickDC::User(nickname, nickname, description, email, shared);
		changed = true;
	}

	oldSize = u->getSharedBytes();
	if (u->setDescription(description)) changed = true;
	if (u->setEmail(email)) changed = true;
	if (u->setSharedBytes(shared)) changed = true;
	if (tag && tag->isOK()) {
		if (u->setUserAgent(tag->getUserAgent())) changed = true;
		if (u->setNumSlots(tag->getSlots())) changed = true;
		int hub_cnt_op = 0; int hub_cnt_reg = 0; int hub_cnt_user = 0;
		tag->getHubCount(hub_cnt_op, hub_cnt_reg, hub_cnt_user);
		if (u->setHubsOperator(hub_cnt_op)) changed = true;
		if (u->setHubsRegistered(hub_cnt_reg)) changed = true;
		if (u->setHubsRegular(hub_cnt_user)) changed = true;
		// TODO: Add active/passive mode.
	}
	session->users->info(nickname, u, oldSize, changed);
	
	if (!session->isLoggedIn()) session->onLoggedIn();
	
	
	free(data);
	delete tag;
	return true;
}

/**
 * Handle the $NickList command.
 * Syntax: $NickList nickA$$nickB$$
 *
 * If the server supports the NoGetINFO extension, then
 * only the users without info is returned in the list,
 * and these users are usually bots.
 */
bool DC::NickList::invoke(const char* data, size_t length) {
	if (length < 11) return true;
	bool info = !session->supports("NoGetINFO");
	char* nicklist = strdup(&data[10]);
	
	char* start = &nicklist[0];
	char* end;
	while ((end = strstr(&start[0], "$$"))) {
		end[0] = '\0';
		const char* nick = &start[0];
		if (info /* && strcmp(nick, session->getNick()) != 0 */)
		{
			session->send(new DC::SendGetInfo(nick, session->getNick()));
		}
		start = &end[2];
	}
	if (info) proxy->write();

	free(nicklist);
	
	return true;
}

/**
 * Handle the $OpList command.
 * Syntax: $OpList nickA$$nickB$$...$$
 */
bool DC::OpList::invoke(const char* data, size_t length) {
	if (length < 9) return true;
	char* oplist = strdup(&data[8]);
	char* start = &oplist[0];
	char* end;
	while ((end = strstr(&start[0], "$$"))) {
		end[0] = '\0';
		const char* op = &start[0];
		session->users->op(op);
		start = &end[2];
	}
	free(oplist);
	return true;
}

/**
 * Parse the public chat command.
 * Note: the public chat command is a special case command
 * since it does not begin with a $.
 * 
 * Syntax:
 *
 * <{nickname}> {message}
 *
 * Smallest valid message: 4 bytes
 */
bool DC::PublicChat::invoke(const char* data, size_t length) {
	if (length < 4) return false;
	
	const char* split = strstr(data, "> ");
	if (!split) return false;

	char* nick = strndup(&data[1], &split[0]-&data[1]);
	char* msg  = strdup(&split[2]);

	msg = DC::TextEscape::unescape(msg);
	session->onPublicChat(nick, msg);

	free(nick);
	free(msg);

	return true;
}


/**
 * Parse the public chat action command.
 * Format: "* nickname message"
 * 
 * Smallest valid message: 5 bytes
 */
bool DC::ActionChat::invoke(const char* data, size_t length) {
	if (length < 5) return false;

	const char* split = strchr(&data[2], ' ');
	if (!split) return false;

	char* nick = strndup(&data[2], &split[0]-&data[2]);
	char* msg  = strdup(&split[1]);

	msg = DC::TextEscape::unescape(msg);
	session->onActionChat(nick, msg);

	free(nick);
	free(msg);
	return true;
}

/**
 * Parse the $Quit command.
 * Syntax: $Quit {nick}
 * Smallest possible string: "$Quit a" 7 bytes.
 */
bool DC::Quit::invoke(const char* data, size_t length) {
	if (length < 7) return false;
	session->users->quit(&data[6]);
	return true;
}



/**
 * Parse the $Search command.
 * Example: "$Search 82.13.31.166:1813 F?T?0?9?TTH:2ZROV7YW7RTOKFJDF62WDMM2XGGC55Q3VF7WODI"
 * Crasher: "$Search 212.31.189.14:28573 T?F?49999999?1?rit$sten$r16"
 * IP:  minimal: "a.b.c.d:e" 8 bytes.
 * Name minimal: "Hub:a" 5 bytes
 */
bool DC::Search::invoke(const char* data, size_t length) {
	if (length < 20) return false;
	
	const char* req = strchr(&data[8], ' ');
	if (!req) return false;

	if (&req[0] - &data[8] < 5) return false;
	
	enum QuickDC::Share::SearchRequest::SizePolicy s_policy;
	
	// check that pattern is valid
	if (req[2] != '?' || req[4] != '?') return false;

	// figure out search type
	if ((req[1] == 'F' && req[3] == 'F') || (req[1] == 'F' && req[3] == 'T')) {
		s_policy = QuickDC::Share::SearchRequest::SizeAny; // any size	
	} else if (req[1] == 'T' && req[3] == 'T') {
		s_policy = QuickDC::Share::SearchRequest::SizeMax; // size is max
	} else if (req[1] == 'T' && req[3] == 'F') {
		s_policy = QuickDC::Share::SearchRequest::SizeMin; // size is min
	} else {
		s_policy = QuickDC::Share::SearchRequest::SizeExact; // size is exact
	}

	// split the search type from size given
	const char* typeoffset = strchr(&req[6], '?');
	if (!typeoffset) return false;

	int type = 0;
	if (typeoffset[1] > '0' && typeoffset[1] <= '9') {
		type = (typeoffset[1] - 48);
	}
	
	QuickDC::Share::SearchRequest* request = new QuickDC::Share::SearchRequest(session);
	request->setSizePolicy(s_policy);
	
	if (type > 0 && type < 8) {
		request->setType(QuickDC::Share::SearchRequest::SearchFiles);
		for (int n = 0; n < KNOWN_EXTENSIONS; n++)
			if (known_ext[n].ftype == type)
				request->addExtension(known_ext[n].ext);
	} else if (type == 8) {
		request->setType(QuickDC::Share::SearchRequest::SearchDirectories);
	}
	
	char* size_str = strndup(&req[5], &typeoffset[0] - &req[5]);
	uint64_t size = quickdc_atoull(size_str);
	request->setSize(size);
	
	if (size == 0) s_policy = QuickDC::Share::SearchRequest::SizeAny; // any size
	char* replyaddr = strndup(&data[8], &req[0] - &data[8]);
	char* query = strdup(&typeoffset[3]);
	
	if (type != 9) {
		char* last = query;
		char* jump = strchr(last, '$');
		while (jump) {
			jump[0] = '\0';
			request->addInclusion(DC::TextEscape::unescape(last));
			last = &jump[1];
			jump = strchr(last, '$');
		}
		if (strlen(last))
			request->addInclusion(DC::TextEscape::unescape(last));
	} else {
		if (strncasecmp(query, "TTH:", 4) == 0 && strlen(query) > 40)
			request->setTTH(&query[4]);
	}

	const QuickDC::User* user = 0;
	if (strncmp(replyaddr, "Hub:", 4) == 0) {
		user = session->getUser(&replyaddr[4]);
		if (user) request->setUser(user);
	} else {
		request->setToken(replyaddr);
	}
	
	session->onSearch(request);
	
	// NOTE: Do NOT free query -> managed by SearchRequest.
	free(size_str);
	free(replyaddr);
	free(query);
	delete request;

	return true;
}

/**
 * Parse the $Supports command
 * Syntax: $Supports {string} [string2] ...
 * Smallest possible string:
 * "$Supports", 9 bytes.
 */
bool DC::Supports::invoke(const char* data, size_t length) {
	if (length < 9) return false;
	std::string support(data, length);
	support = support.substr(10);
	if (support[support.length()-1] != ' ') support += ' ';

	size_t offset = 0;
	while ((offset = support.find(' ')) != std::string::npos) {
		std::string feature = support.substr(0, offset);
		support = support.substr(offset+1);
		session->addSupport(feature.c_str());
	}

	return true;
}

/**
 * Parse the $To command
 * Syntax: $To: {nick} From: {nick} ${string}
 * Smallest possible string:
 * "$To: a From: b $c", 17 bytes
 */
bool DC::To::invoke(const char* data, size_t length) {
	if (length < 17) return false;
	const char* msgdata = 0;

	// split 'to' and 'from'
	const char* split1 = strstr(&data[5], " From: ");
	if (!split1) return false;

	const char* split2 = strstr(&split1[6], " $");
	if (!split2) return false;
	
	char* to = strndup(&data[5], &split1[0]-&data[5]);
	char* from = strndup(&split1[7], &split2[0]-&split1[7]);
	const char* message = &split2[2];

	// Workaround for faulty bots and hubs
	if (message[0] == '<') {
		msgdata = strstr(&message[1], "> ");
		if (msgdata) msgdata = &msgdata[2];
		else msgdata = message;
	} else {
		msgdata = message;
	}

	char* msg = strdup(msgdata);
	msg = DC::TextEscape::unescape(msg);

	session->onPrivateChat(to, from, msg);

	free(msg);
	free(to);
	free(from);

	return true;
}

/**
 * Parse the usercommand command, this is an extension
 * introduced in DC++, and is very tied to the user interface.
 */
bool DC::UserCommand::invoke(const char*, size_t) {
	return true;
}

/**
 * $UserIP command. Can be sent for one user aswell
 * as a list of users separated by '$$'. In the latter 
 * case the string will end with '$$'.
 */
bool DC::UserIP::invoke(const char* data, size_t length) {
	std::string userip(data, length);
	userip = userip.substr(8);
	size_t offset = 0;
	
	// UserIP for one user
	if (userip.rfind("$$") == std::string::npos) {
		offset = userip.rfind(' ');
		std::string nick = userip.substr(0, offset);
		std::string ip = userip.substr(offset+1);
		session->users->ip(nick.c_str(), ip.c_str());
		return true;
	}
	
	// List of multiple user ips
	while ((offset = userip.find("$$")) != std::string::npos) {
		std::string pair = userip.substr(0, offset);
		userip = userip.substr(offset+2);
		offset = pair.rfind(' ');
		std::string nick = pair.substr(0, offset);
		std::string ip = pair.substr(offset+1);
		session->users->ip(nick.c_str(), ip.c_str());
	}
	return true;
}

/**
 * Handle the $ValidateDenide command (protocol typo).
 * There are no parameters for this command, thus there isn't much to do here.
 */
bool DC::ValidateDenied::invoke(const char*, size_t) {
	session->onBadNick();
	return true;
}

