/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdio.h>
#include <stdlib.h>
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
#include "share/searchresult.h"
#include "share/filetypes.h"
#include <string>
#include <string_view>
#include <vector>
#include "network/dc/lock.h"
#include "network/dc/textescape.h"

namespace {

/**
 * A bounded view of the line a handler was given.
 *
 * Everything here decodes a line a hub sent, and a hub has authenticated
 * nothing at the point it can send one, so the length the handler is handed is
 * the only thing that says where that line stops: a fixed offset into it is
 * only there if the hub chose to send that much. Reader keeps the two
 * together - a prefix is skipped only when it is really present, and a field is
 * taken only when the separator that ends it is - so a handler built on it
 * cannot read beyond the line at all, whatever arrives.
 */
class Reader
{
	public:
		Reader(const char* data, size_t length)
			: text(data ? std::string_view(data, length) : std::string_view()) { }

		explicit Reader(std::string_view view) : text(view) { }

		bool empty() const { return text.empty(); }

		/** Whatever has not been taken yet. */
		std::string_view rest() const { return text; }

		/** Consume 'prefix', or leave the line untouched and say it is absent. */
		bool skip(std::string_view prefix)
		{
			if (text.size() < prefix.size()) return false;
			if (text.compare(0, prefix.size(), prefix) != 0) return false;
			text.remove_prefix(prefix.size());
			return true;
		}

		/** Take everything up to the first 'sep', and the separator with it. */
		bool field(std::string_view sep, std::string_view& out)
		{
			const size_t at = text.find(sep);
			if (at == std::string_view::npos) return false;
			out = text.substr(0, at);
			text.remove_prefix(at + sep.size());
			return true;
		}

		bool field(char sep, std::string_view& out)
		{
			return field(std::string_view(&sep, 1), out);
		}

		/** The same, but splitting at the last 'sep' rather than the first. */
		bool lastField(char sep, std::string_view& out)
		{
			const size_t at = text.rfind(sep);
			if (at == std::string_view::npos) return false;
			out = text.substr(0, at);
			text.remove_prefix(at + 1);
			return true;
		}

	private:
		std::string_view text;
};

/**
 * TextEscape::unescape() rewrites the string it is given in place, so it needs
 * a buffer of its own rather than a view into the line. It only ever shortens,
 * hence the resize afterwards.
 */
std::string unescaped(std::string_view text)
{
	std::string copy(text);
	copy.resize(strlen(DC::TextEscape::unescape(&copy[0])));
	return copy;
}

}

/**
 * Parse the $ConnectToMe command
 *
 * Syntax: 
 * Syntax: $ConnectToMe <RemoteNick> <SenderIp>:<SenderPort> 
 *         $ConnectToMe <SenderNick> <RemoteNick> <SenderIp>:<SenderPort> (alternative, who use this?)
 */
bool DC::ConnectToMe::invoke(const char* data, size_t length) {
	Reader line(data, length);
	if (!line.skip("$ConnectToMe ")) return false;

	// isolate senderip:port from last space
	std::string_view nicks;
	if (!line.lastField(' ', nicks)) return false;

	// detect address:port.
	Reader endpoint(line.rest());
	std::string_view host;
	if (!endpoint.field(':', host)) return false;
	if (host.empty() || endpoint.empty()) return false;

	/* One nick is ours, and only the hub knows it. Two means the sender named
	   itself first, and that is the nick the connection can be expected from. */
	std::string sendernick;
	Reader who(nicks);
	std::string_view first;
	if (who.field(' ', first)) sendernick.assign(first);

	const std::string address(host);
	const std::string port(endpoint.rest());

	session->onActiveConnect(address.c_str(), (uint16_t) atoi(port.c_str()),
		sendernick.empty() ? 0 : sendernick.c_str());

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
	Reader line(data, length);
	if (!line.skip("$RevConnectToMe ")) return false;

	std::string_view from;
	if (!line.lastField(' ', from)) return false;
	if (from.empty() || line.empty()) return false;

	const std::string user(from);
	session->onPassiveConnect(user.c_str());
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
	Reader line(data, length);
	if (!line.skip("$ForceMove ")) return false;

	uint16_t port = 411;
	std::string_view host;
	if (line.field(':', host)) {
		const std::string number(line.rest());
		port = (uint16_t) atoi(number.c_str());
	} else {
		host = line.rest();
	}

	if (host.empty()) return false;

	const std::string addr(host);
	session->onRedirect(addr.c_str(), port);
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
	Reader line(data, length);
	if (!line.skip("$Hello ")) return false;
	if (line.empty()) return false;

	const std::string nick(line.rest());

	if (nick == session->getNick())
	{
		session->onRequestMyInfo(nick.c_str());
	}
	else
	{
		session->users->join(nick.c_str());
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
	Reader line(data, length);
	if (!line.skip("$HubName ")) return false;
	if (line.empty()) return false;

	const std::string name(line.rest());
	session->onHubName(name.c_str());
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
	Reader line(data, length);
	if (!line.skip("$Lock ")) return false;

	std::string_view lockdata;
	if (!line.field(" Pk=", lockdata)) return false;
	if (lockdata.empty() || line.empty()) return false;

	const std::string lock(lockdata);
	const std::string pk(line.rest());
	session->onLock(lock.c_str(), pk.c_str());
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
bool DC::MyINFO::invoke(const char* data, size_t length) {
	Reader line(data, length);
	if (!line.skip("$MyINFO $ALL ")) return false;

	// find and extract nickname
	std::string_view nick;
	if (!line.field(' ', nick)) return false;
	if (nick.empty()) return false;

	// find description
	std::string_view descdata;
	if (!line.field('$', descdata)) return false;

	/* Exactly one byte between the description and the speed: the mode flag a
	   PtokaX hub puts there. A hub with nothing to say still sends the '$' on
	   either side of it, so the field is there but empty. */
	std::string_view mode;
	if (!line.field('$', mode) || mode.size() != 1) return false;
#if 0
	if (mode[0] == 'A');      /* PtokaX: Active mode user */
	else if (mode[0] == 'P'); /* PtokaX: Passive mode user */
#endif //0

	/* find and extract speed. Its last byte is the status flag described above;
	   nothing reads either of them yet. */
	std::string_view speed;
	if (!line.field('$', speed)) return false;

	// find and extract email
	std::string_view emaildata;
	if (!line.field('$', emaildata)) return false;

	// find and extract share size
	std::string_view sharesize;
	if (!line.field('$', sharesize)) return false;

	const std::string nickname(nick);
	const std::string email(emaildata);
	const std::string shares(sharesize);
	std::string description(descdata);
	DC::Tag* tag = 0;

	const uint64_t shared = Samurai::Util::Convert::to_int64(shares.c_str());

	// extract tag from description string
	const size_t open = description.rfind('<');
	if (open != std::string::npos && description.find('>', open) != std::string::npos) {
		tag = new Tag(description.substr(open).c_str());
		description.erase(open);
	}

	QuickDC::User* u = (QuickDC::User*) session->users->getUser(nickname.c_str());
	bool changed = false;
	uint64_t oldSize = 0;

	if (!u) {
		u = new QuickDC::User(nickname.c_str(), nickname.c_str(),
			description.c_str(), email.c_str(), shared);
		changed = true;
	}

	oldSize = u->getSharedBytes();
	if (u->setDescription(description.c_str())) changed = true;
	if (u->setEmail(email.c_str())) changed = true;
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
	session->users->info(nickname.c_str(), u, oldSize, changed);

	if (!session->isLoggedIn()) session->onLoggedIn();

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
	Reader line(data, length);
	if (!line.skip("$NickList ")) return true;

	bool info = !session->supports("NoGetINFO");

	std::string_view nick;
	while (line.field("$$", nick)) {
		if (nick.empty()) continue;
		if (info /* && nick != session->getNick() */)
		{
			const std::string name(nick);
			session->send(new DC::SendGetInfo(name.c_str(), session->getNick()));
		}
	}
	if (info) proxy->write();

	return true;
}

/**
 * Handle the $OpList command.
 * Syntax: $OpList nickA$$nickB$$...$$
 */
bool DC::OpList::invoke(const char* data, size_t length) {
	Reader line(data, length);
	if (!line.skip("$OpList ")) return true;

	std::string_view nick;
	while (line.field("$$", nick)) {
		if (nick.empty()) continue;
		const std::string op(nick);
		session->users->op(op.c_str());
	}
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
	Reader line(data, length);
	if (!line.skip("<")) return false;

	std::string_view nick;
	if (!line.field("> ", nick)) return false;
	if (nick.empty()) return false;

	const std::string from(nick);
	const std::string msg = unescaped(line.rest());

	session->onPublicChat(from.c_str(), msg.c_str());

	return true;
}


/**
 * Parse the public chat action command.
 * Format: "* nickname message"
 * 
 * Smallest valid message: 5 bytes
 */
bool DC::ActionChat::invoke(const char* data, size_t length) {
	Reader line(data, length);
	if (!line.skip("* ")) return false;

	std::string_view nick;
	if (!line.field(' ', nick)) return false;
	if (nick.empty()) return false;

	const std::string from(nick);
	const std::string msg = unescaped(line.rest());

	session->onActionChat(from.c_str(), msg.c_str());
	return true;
}

/**
 * Parse the $Quit command.
 * Syntax: $Quit {nick}
 * Smallest possible string: "$Quit a" 7 bytes.
 */
bool DC::Quit::invoke(const char* data, size_t length) {
	Reader line(data, length);
	if (!line.skip("$Quit ")) return false;
	if (line.empty()) return false;

	const std::string nick(line.rest());
	session->users->quit(nick.c_str());
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
	Reader line(data, length);
	if (!line.skip("$Search ")) return false;

	std::string_view addr;
	if (!line.field(' ', addr)) return false;

	/* "a.b.c.d:e" is the shortest address and "Hub:a" the shortest nick, so
	   nothing shorter than either can be replied to. */
	if (addr.size() < 5) return false;

	enum QuickDC::Share::SearchRequest::SizePolicy s_policy;

	/* check that pattern is valid: each of the three flags is a single
	   character, and the size between them is whatever the sender wrote. */
	std::string_view sizeflag;
	std::string_view limitflag;
	std::string_view sizedata;
	std::string_view typeflag;
	if (!line.field('?', sizeflag) || sizeflag.size() != 1) return false;
	if (!line.field('?', limitflag) || limitflag.size() != 1) return false;
	if (!line.field('?', sizedata)) return false;
	if (!line.field('?', typeflag) || typeflag.size() != 1) return false;

	// figure out search type
	if ((sizeflag[0] == 'F' && limitflag[0] == 'F') || (sizeflag[0] == 'F' && limitflag[0] == 'T')) {
		s_policy = QuickDC::Share::SearchRequest::SizeAny; // any size
	} else if (sizeflag[0] == 'T' && limitflag[0] == 'T') {
		s_policy = QuickDC::Share::SearchRequest::SizeMax; // size is max
	} else if (sizeflag[0] == 'T' && limitflag[0] == 'F') {
		s_policy = QuickDC::Share::SearchRequest::SizeMin; // size is min
	} else {
		s_policy = QuickDC::Share::SearchRequest::SizeExact; // size is exact
	}

	int type = 0;
	if (typeflag[0] > '0' && typeflag[0] <= '9') {
		type = (typeflag[0] - 48);
	}

	QuickDC::Share::SearchRequest* request = new QuickDC::Share::SearchRequest(session);

	if (type > 0 && type < 8) {
		request->setType(QuickDC::Share::SearchRequest::SearchFiles);
		for (int n = 0; n < KNOWN_EXTENSIONS; n++)
			if (known_ext[n].ftype == type)
				request->addExtension(known_ext[n].ext);
	} else if (type == 8) {
		request->setType(QuickDC::Share::SearchRequest::SearchDirectories);
	}

	const std::string size_str(sizedata);
	uint64_t size = Samurai::Util::Convert::to_uint64(size_str.c_str());
	request->setSize(size);

	if (size == 0) s_policy = QuickDC::Share::SearchRequest::SizeAny; // any size

	/* After the adjustment above, or a search naming no size would still be
	   asked for with whatever policy the line happened to carry. */
	request->setSizePolicy(s_policy);

	const std::string replyaddr(addr);
	const std::string query(line.rest());

	if (type != 9) {
		/* '$' separated terms. An empty one selects the whole share rather than
		   narrowing it, so it is no term at all. */
		Reader terms(query);
		for (;;) {
			std::string_view term;
			const bool more = terms.field('$', term);
			if (!more) term = terms.rest();

			if (!term.empty()) {
				std::string text = unescaped(term);
				request->addInclusion(text.c_str());
			}
			if (!more) break;
		}
	} else {
		if (Samurai::Util::istarts_with(query, "TTH:") && query.size() > 40)
			request->setTTH(&query.c_str()[4]);
	}

	const QuickDC::User* user = 0;
	if (std::string_view(replyaddr).starts_with("Hub:")) {
		user = session->getUser(&replyaddr.c_str()[4]);
		if (user) request->setUser(user);
	} else {
		request->setToken(replyaddr.c_str());
	}

	session->onSearch(request);

	delete request;

	return true;
}

/**
 * Does this look like the "{hub name} ({host}:{port})" trailer?
 *
 * Requiring a colon inside the parentheses is what keeps it apart from a
 * nickname that merely happens to contain brackets, which matters because the
 * trailer and an optional trailing nick are otherwise indistinguishable.
 */
/**
 * Strict unsigned decode: every character has to be a digit.
 *
 * Convert::to_int32()/Convert::to_uint64() are no use for a field that might
 * contain anything else. They count every non-digit anywhere in the string as
 * leading padding to be skipped, then read from there to the end without
 * validating, so "2/4" comes back as -6 rather than 2.
 */
static bool dc_sr_number(const std::string& text, uint64_t& out)
{
	/* 20 digits is the most that can fit in a uint64_t. */
	if (text.empty() || text.size() > 20) return false;

	out = 0;
	for (size_t n = 0; n < text.size(); n++)
	{
		if (text[n] < '0' || text[n] > '9') return false;
		out = out * 10 + (uint64_t) (text[n] - '0');
	}
	return true;
}

static bool dc_sr_is_hub_info(const std::string& field)
{
	if (field.empty() || field[field.size()-1] != ')') return false;

	size_t open = field.rfind('(');
	if (open == std::string::npos) return false;

	return (field.find(':', open) != std::string::npos);
}

/**
 * See hubcommands.h for the syntax this decodes.
 * Smallest plausible string: "$SR n f 0/1\x05h (1:1)", 21 bytes.
 */
bool DC::parseSearchResult(const char* data, size_t length,
	std::string& nick, std::string& path, uint64_t& size,
	size_t& freeslots, std::string& tth)
{
	size = 0;
	freeslots = 0;
	nick.clear();
	path.clear();
	tth.clear();

	if (length < 21) return false;

	Reader line(data, length);
	if (!line.skip("$SR ")) return false;

	std::string_view sender;
	if (!line.field(' ', sender) || sender.empty()) return false;

	nick.assign(sender);
	std::string rest(line.rest());

	/* Split on the 0x05 separators. */
	std::vector<std::string> fields;
	size_t at = 0;
	for (;;)
	{
		size_t next = rest.find((char) 0x05, at);
		if (next == std::string::npos)
		{
			fields.push_back(rest.substr(at));
			break;
		}
		fields.push_back(rest.substr(at, next - at));
		at = next + 1;
	}

	/* The last field that looks like the trailer is the trailer. */
	size_t hubIndex = fields.size();
	for (size_t n = fields.size(); n > 0; n--)
	{
		if (dc_sr_is_hub_info(fields[n-1])) { hubIndex = n-1; break; }
	}
	if (hubIndex == fields.size() || hubIndex < 1 || hubIndex > 2) return false;

	/* "{hub name} (...)" or "TTH:{root} (...)" */
	std::string info = fields[hubIndex].substr(0, fields[hubIndex].rfind('('));
	while (info.size() && info[info.size()-1] == ' ') info.erase(info.size()-1);

	if (info.size() > 4 && Samurai::Util::istarts_with(info, "TTH:"))
	{
		std::string candidate = info.substr(4);
		if (candidate.size() == 39) tth = candidate;
	}

	std::string slots;

	if (hubIndex == 2)
	{
		/* A file: fields are {path} and "{size} {free}/{total}". */
		path = fields[0];

		size_t split = fields[1].find(' ');
		if (split == std::string::npos) return false;

		if (!dc_sr_number(fields[1].substr(0, split), size)) return false;
		slots = fields[1].substr(split + 1);
	}
	else
	{
		/* A directory: "{path} {free}/{total}" in a single field. */
		size_t split = fields[0].rfind(' ');
		if (split == std::string::npos) return false;

		path = fields[0].substr(0, split);
		slots = fields[0].substr(split + 1);
	}

	if (path.empty()) return false;

	/* "{free}/{total}" */
	size_t slash = slots.find('/');
	if (slash == std::string::npos) return false;

	uint64_t free_slots = 0;
	if (!dc_sr_number(slots.substr(0, slash), free_slots)) return false;

	uint64_t total_slots = 0;
	if (!dc_sr_number(slots.substr(slash + 1), total_slots)) return false;

	freeslots = (size_t) free_slots;
	return true;
}

bool DC::SearchResultParser::invoke(const char* data, size_t length)
{
	std::string nick;
	std::string path;
	std::string tth;
	uint64_t size = 0;
	size_t freeslots = 0;

	if (!DC::parseSearchResult(data, length, nick, path, size, freeslots, tth))
		return false;

	/*
	 * The path keeps its '\' separators: that is the name the peer knows the
	 * file by, and so it is what a source has to record.
	 */
	QuickDC::Share::SearchResult* result = new QuickDC::Share::SearchResult(
		path.c_str(), size, freeslots, tth.empty() ? 0 : tth.c_str(), 0, 0);

	/* The user manager owns the user; the result only borrows it. */
	result->setUser(const_cast<QuickDC::User*>(session->getUser(nick.c_str())));

	/* Takes ownership of 'result'. */
	session->onSearchResult(result);
	return true;
}

/**
 * Parse the $Supports command
 * Syntax: $Supports {string} [string2] ...
 * Smallest possible string:
 * "$Supports", 9 bytes.
 */
bool DC::Supports::invoke(const char* data, size_t length) {
	Reader line(data, length);
	if (!line.skip("$Supports ")) return false;

	for (;;) {
		std::string_view feature;
		const bool more = line.field(' ', feature);

		/* The last feature has no space after it, and a hub that leaves a
		   trailing one has nothing after that. */
		if (!more) feature = line.rest();

		if (!feature.empty()) {
			const std::string name(feature);
			session->addSupport(name.c_str());
		}
		if (!more) break;
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
	Reader line(data, length);
	if (!line.skip("$To: ")) return false;

	// split 'to' and 'from'
	std::string_view todata;
	if (!line.field(" From: ", todata)) return false;

	std::string_view fromdata;
	if (!line.field(" $", fromdata)) return false;

	if (todata.empty() || fromdata.empty()) return false;

	std::string_view message = line.rest();

	// Workaround for faulty bots and hubs
	if (!message.empty() && message[0] == '<') {
		Reader body(message.substr(1));
		std::string_view repeated;
		if (body.field("> ", repeated)) message = body.rest();
	}

	const std::string to(todata);
	const std::string from(fromdata);
	const std::string msg = unescaped(message);

	session->onPrivateChat(to.c_str(), from.c_str(), msg.c_str());

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
	Reader line(data, length);
	if (!line.skip("$UserIP ")) return false;

	/* One "{nick} {ip}", or a list of them: the same loop reads either, since
	   the single form is a list of one that does not end with a separator. */
	for (;;) {
		std::string_view entry;
		const bool more = line.field("$$", entry);
		if (!more) entry = line.rest();

		Reader pair(entry);
		std::string_view nick;
		if (pair.lastField(' ', nick) && !nick.empty() && !pair.empty()) {
			const std::string user(nick);
			const std::string ip(pair.rest());
			session->users->ip(user.c_str(), ip.c_str());
		}

		if (!more) break;
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

