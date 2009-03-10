/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include <stdio.h>
#include <vector>

#include "network/hubmanager.h"
#include "network/dc/hubsession.h"	
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
	for (std::vector<Hub*>::iterator it = hubs.begin(); *it; it++)
	{
		if (*it == hub) {
			hubs.erase(it);
			break;
		}
	}
}


QuickDC::Hub* QuickDC::HubManager::lookupUser(const char* id) {
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (hub->session && hub->session->isLoggedIn() && hub->session->getUser(id)) return hub;
	}
	return 0;
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

