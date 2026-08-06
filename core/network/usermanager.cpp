/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "usermanager.h"
#include "user.h"

#include <samurai/io/net/inetaddress.h>

#include <string.h>
#include <string_view>

namespace QuickDC {


UserManager::UserManager(UserEventHandler* eventhandler) : event(eventhandler) {
	shared = 0;
	userIterator = users.end();
}

UserManager::~UserManager() {
	while (!users.empty()) {
		std::map<std::string, User*>::iterator it = users.begin();
		User* user = (*it).second;
		users.erase(it);
		delete user;
	}
}

const User* UserManager::getUser(const char* id) {
	if (!id) return 0;
	const auto it = users.find(id);
	return (it != users.end()) ? it->second : 0;
}

/*
 * Walked directly rather than through first()/next(): those share one iterator
 * with the caller, so looking a user up from inside a walk used to move the
 * walk along with it.
 */
const User* UserManager::getUserByNick(const char* nick) {
	if (!nick) return 0;
	const std::string_view want(nick);
	for (const auto& entry : users) {
		const User* u = entry.second;
		if (want == u->getNick()) return u;
	}
	return 0;
}

const User* UserManager::getUserByClientID(const char* cid) {
	if (!cid || !*cid) return 0;
	const std::string_view want(cid);
	for (const auto& entry : users) {
		const User* u = entry.second;
		if (want == reinterpret_cast<const char*>(u->getClientID())) return u;
	}
	return 0;
}

void UserManager::join(const char* id) {
	/* Every lookup below builds a std::string from this. */
	if (!id) return;

	if (users.count(id))
		return;

	User* u = new User(id);
	users.insert(std::pair<std::string, User*>(std::string(id), u));
	if (event) event->EventUserJoin(u->getNick());
}

void UserManager::quit(const char* id, const char* msg) {
	if (!id) return;

	if (users.count(id)) {
		User* u = users[id];
		users.erase(id);
		shared -= u->getSharedBytes();
		if (event) event->EventUserQuit(u, msg);
		delete u;
	}
}

void UserManager::info(const char* id, User* data, uint64_t oldShare, bool changed) {
	if (!id || !data) return;

	/*
	 * Whether they are an operator is the hub's word rather than their own, and
	 * on NMDC it arrives as a command of its own - so it is applied here, where
	 * both orders of arrival meet, instead of wherever the info was parsed.
	 */
	if (haveOperatorList && data->setOp(operators.contains(id))) changed = true;

	if (!users.count(id)) {
		users.insert(std::pair<std::string, User*>(std::string(id), data));
		if (!data->getNick()) QERR("---------------- PANIC -----------------");
		if (event) event->EventUserJoin(data->getNick());
	} else {
		shared -= oldShare;
	}

	shared += data->getSharedBytes();
	if (event && changed) event->EventUserInfo(data);
}

void UserManager::setOperators(const std::vector<std::string>& nicks) {
	operators.clear();
	operators.insert(nicks.begin(), nicks.end());
	haveOperatorList = true;

	/*
	 * Everybody is told, not only the names in the list: a resend is how the hub
	 * says somebody has been demoted, and there is nothing in it to point at.
	 * Only those whose answer actually moved are reported onwards, so the usual
	 * resend of an unchanged list costs the user list nothing.
	 */
	for (const auto& entry : users) {
		if (entry.second->setOp(operators.contains(entry.first)) && event)
			event->EventUserInfo(entry.second);
	}
}

void UserManager::ip(const char* id, const char* address) {
	if (!id || !address) return;

	const auto found = users.find(id);
	if (found == users.end()) return;

	/*
	 * Refused unless it reads as an address literal. This is whatever the hub
	 * chose to send, and the setters below only cap the length - so anything
	 * else would be stored cut to fifteen characters and shown as an address
	 * from then on. Parsed rather than pattern matched, which also settles the
	 * notations that are read two ways: a leading zero in an octet is octal to
	 * some readers and decimal to others, and is refused here.
	 *
	 * Nothing is resolved. An InetAddress built from a string only starts a
	 * lookup when it is asked to, and a hub naming every user at once would be
	 * a lookup apiece if it did.
	 */
	if (!Samurai::IO::Net::InetAddress(address).isValid()) return;

	/*
	 * One field carries either family, so the colon is what tells them apart:
	 * a dotted quad never holds one and every IPv6 literal does.
	 */
	User* u = found->second;
	const bool changed = strchr(address, ':')
		? u->setAddressIPv6(address)
		: u->setAddressIPv4(address);

	/*
	 * Only a change is worth reporting. A hub answers $UserIP2 with the whole
	 * hub at once and resends it as people arrive, so most of what arrives here
	 * says what was already known.
	 */
	if (changed && event) event->EventUserInfo(u);
}

size_t UserManager::countUsers() const {
	return users.size();
}

/*
 * Counted on demand. The member this used to return was set to zero and never
 * touched again, so every hub reported no operators at all.
 */
size_t UserManager::countOperators() const {
	size_t count = 0;
	for (const auto& entry : users)
		if (entry.second->isOp()) count++;
	return count;
}

uint64_t UserManager::countShared() const {
	return shared;
}


User* UserManager::first() {
	userIterator = users.begin();
	if (userIterator != users.end())
		return (*userIterator).second;
	return 0;
}

/* Built-in iterators */
User* UserManager::next() {
	if (userIterator == users.end()) return 0;

	userIterator++;
	if (userIterator != users.end())
		return (*userIterator).second;
	return 0;
}

/* Built-in iterators */
void UserManager::cleanup() {
	if (event) event->EventUsersCleanup();
	while (!users.empty()) {
		std::map<std::string, User*>::iterator it = users.begin();
		User* user = (*it).second;
		users.erase(it);
		delete user;
	}
}


}
