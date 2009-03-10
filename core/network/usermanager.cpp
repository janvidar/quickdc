/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "usermanager.h"
#include "user.h"

namespace QuickDC {


UserManager::UserManager(UserEventHandler* eventhandler) : event(eventhandler) {
	ops = 0;
	shared = 0;
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
	if (users.count((char*) id) > 0) {
		User* u = users[std::string(id)];
		return u;
	}
	return 0;
}

const User* UserManager::getUserByNick(const char* nick) {
	if (!nick) return 0;
	User* u = first();
	while (u) {
		if (u->getNick() && strcmp(u->getNick(), nick) == 0) return u;
		u = next();
	}
	return 0;
}

void UserManager::join(const char* id) {
	if (users.count((char*) id))
		return;

	User* u = new User(id);
	users.insert(std::pair<std::string, User*>(std::string(id), u));
	if (event) event->EventUserJoin(u->getNick());
}

void UserManager::quit(const char* id, const char* msg) {
	if (users.count((char*) id)) {
		User* u = users[(char*) id];
		users.erase((char*) id);
		shared -= u->getSharedBytes();
		if (event) event->EventUserQuit(u, msg);
		delete u;
	}
}

void UserManager::info(const char* id, User* data, uint64_t oldShare, bool changed) {
	if (!users.count((char*) id)) {
		users.insert(std::pair<std::string, User*>(std::string(id), data));
		if (!data->getNick()) QERR("---------------- PANIC -----------------");
		if (event) event->EventUserJoin(data->getNick());
	} else {
		shared -= oldShare;
	}

	shared += data->getSharedBytes();
	if (event && changed) event->EventUserInfo(data);
}

void UserManager::op(const char* id) {
	if (users.count((char*) id)) {
		User* u = users[std::string(id)];
		// FIXME: u->setOp(true);
		if (event) event->EventUserInfo(u);
	}
}

void UserManager::ip(const char* id, const char* /*ip*/) {
	if (users.count((char*) id)) {
		User* u = users[std::string(id)];
		// FIXME: u->setIP(ip);
		if (event) event->EventUserInfo(u);
	}
}

size_t UserManager::countUsers() const {
	return users.size();
}

size_t UserManager::countOperators() const {
	return ops;
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
