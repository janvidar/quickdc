/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include <stdio.h>
#include <string.h>
#include <vector>

#include "network/hubmanager.h"
#include "network/dc/hubsession.h"
#include "network/usermanager.h"
#include "api/hub.h"

QuickDC::HubManager::HubManager()
{
	// FIXME: We should add a load()/save() functions here
	//        This can take care of reconnecting to old
	//        hubs when re-starting the application.
}


QuickDC::HubManager::~HubManager()
{
	
	// Shutdown all connections
	while (hubs.size()) {
		QuickDC::Hub* hub = hubs.back();
		hubs.pop_back();
		delete hub;
	}
}


void QuickDC::HubManager::add(QuickDC::Hub* hub) {
	QDBG("Adding hub");
	hubs.push_back(hub);
}


void QuickDC::HubManager::remove(QuickDC::Hub* hub) {
	QDBG("Removing hub");
	/* Compared against end(); dereferencing without that walked off the end. */
	for (std::vector<Hub*>::iterator it = hubs.begin(); it != hubs.end(); it++)
	{
		if (*it == hub) {
			hubs.erase(it);
			break;
		}
	}
}


QuickDC::Hub* QuickDC::HubManager::getHubByUrl(const char* url) {
	if (!url) return 0;
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		const char* hubUrl = hub->getUrl();
		if (hubUrl && !strcmp(hubUrl, url)) return hub;
	}
	return 0;
}


QuickDC::Hub* QuickDC::HubManager::lookupUser(const char* id) {
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (hub->session && hub->session->isLoggedIn() && hub->session->getUser(id)) return hub;
	}
	return 0;
}


QuickDC::Hub* QuickDC::HubManager::lookupUserByClientID(const char* cid) {
	if (!cid) return 0;

	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (!hub->session || !hub->session->isLoggedIn()) continue;

		QuickDC::UserManager* users = hub->session->getUserManager();
		if (users && users->getUserByClientID(cid)) return hub;
	}
	return 0;
}


QuickDC::Hub* QuickDC::HubManager::lookupUserByNick(const char* nick) {
	if (!nick || !*nick) return 0;

	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (!hub->session || !hub->session->isLoggedIn()) continue;

		QuickDC::UserManager* users = hub->session->getUserManager();
		if (users && users->getUserByNick(nick)) return hub;
	}
	return 0;
}


size_t QuickDC::HubManager::searchAllHubs(QuickDC::Share::SearchRequest* request) {
	if (!request) return 0;

	size_t sent = 0;
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];

		/* sendSearchRequest() would report this to the user as an error. */
		if (!hub->session || !hub->session->isLoggedIn()) continue;

		hub->sendSearchRequest(request);
		sent++;
	}
	return sent;
}


size_t QuickDC::HubManager::countHubsNormal() {
	size_t n = 0;
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (hub->session && hub->session->isLoggedIn() && hub->session->access == HubSession::AccessNormal) n++;
	}
	return n;
}


size_t QuickDC::HubManager::countHubsRegistered() {
	size_t n = 0;
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (hub->session && hub->session->isLoggedIn() && hub->session->access == HubSession::AccessRegistered) n++;
	}
	return n;
}


size_t QuickDC::HubManager::countHubsOperator() {
	size_t n = 0;
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (hub->session && hub->session->isLoggedIn() && hub->session->access == HubSession::AccessOperator) n++;
	}
	return n;
}

