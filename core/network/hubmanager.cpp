/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include <stdio.h>
#include <string.h>
#include <string_view>
#include <vector>

#include "network/hubmanager.h"
#include "network/searchtokens.h"
#include "network/dc/hubsession.h"
#include "network/usermanager.h"
#include "api/core.h"
#include "api/hub.h"

QuickDC::HubManager::HubManager(Core& core_)
	: core(core_)
	, info_timer(0)
	, info_delay(std::chrono::seconds(QUICKDC_HUB_INFO_UPDATE_DELAY_SECONDS))
{
	// FIXME: We should add a load()/save() functions here
	//        This can take care of reconnecting to old
	//        hubs when re-starting the application.
}


QuickDC::HubManager::~HubManager()
{
	/* Before the hubs go: it holds a pointer back to this, and firing during
	   teardown would walk a list being emptied. Whatever it was going to say is
	   moot, since nothing is left to say it to. */
	delete info_timer;
	info_timer = 0;

	
	// Shutdown all connections
	while (hubs.size()) {
		QuickDC::Hub* hub = hubs.back();
		hubs.pop_back();
		hub->forgetManager();
		delete hub;
	}
}


void QuickDC::HubManager::add(QuickDC::Hub* hub) {
	QDBG("Adding hub");
	hubs.push_back(hub);
}


void QuickDC::HubManager::remove(QuickDC::Hub* hub) {
	QDBG("Removing hub");

	/* One fewer hub to count ourselves as being on, which the hubs we are still
	   on were told. Throttled, so a client dropping several at once - or
	   reconnecting in a loop - sends one update rather than one each. */
	requestClientInfoUpdate();
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
		if (hubUrl && std::string_view(hubUrl) == url) return hub;
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


/*
 * Every search goes out naming itself, and what it names is remembered.
 *
 * A datagram's source address is whatever its sender wrote, so a reply can only be
 * told from a stranger by what it answers. The queue draws its own token for a source
 * search, so one may be here already - it is remembered rather than replaced, since it
 * is the one the reply will quote.
 *
 * One token however many hubs it goes to: the same search is being asked in several
 * places, and a reply from any of them answers it.
 */
void QuickDC::HubManager::rememberSearchToken(QuickDC::Share::SearchRequest* request) {
	QuickDC::SearchTokens* tokens = core.searchTokens;
	if (!tokens) return;

	if (const char* existing = request->getToken())
		tokens->remember(existing);
	else
		request->setToken(tokens->issue().c_str());
}


size_t QuickDC::HubManager::searchOneHub(const char* url, QuickDC::Share::SearchRequest* request) {
	if (!request || !url || !*url) return 0;

	QuickDC::Hub* hub = getHubByUrl(url);

	/* sendSearchRequest() would report this to the user as an error. */
	if (!hub || !hub->session || !hub->session->isLoggedIn()) return 0;

	rememberSearchToken(request);
	hub->sendSearchRequest(request);
	return 1;
}


size_t QuickDC::HubManager::searchAllHubs(QuickDC::Share::SearchRequest* request) {
	if (!request) return 0;

	rememberSearchToken(request);

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


void QuickDC::HubManager::requestClientInfoUpdate() {
	/* Already scheduled: that update will carry whatever this call was about, so
	   the window is deliberately not restarted - a client reconnecting in a loop
	   would otherwise push the update back for as long as the loop ran. */
	if (info_timer) return;

	info_timer = new Samurai::Timer(core.loop(), this, info_delay, true);
}


void QuickDC::HubManager::EventTimeout(Samurai::Timer*) {
	/* sendClientInfoToAllHubs() drops the timer itself, which is also what lets a
	   listener reached from there schedule a fresh one rather than be folded into
	   the update being delivered. */
	sendClientInfoToAllHubs();
}


size_t QuickDC::HubManager::sendClientInfoToAllHubs() {
	/* Whatever was scheduled is what is being sent, so it is dropped rather than
	   left to fire again with nothing new to say. */
	delete info_timer;
	info_timer = 0;

	size_t sent = 0;
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];

		/* Nothing to update on a hub that has not been told anything yet: the
		   login sequence sends this itself once it gets that far. */
		if (!hub->session || !hub->session->isLoggedIn()) continue;

		hub->session->sendClientInfo();
		sent++;
	}
	return sent;
}


size_t QuickDC::HubManager::countHubsNormal(const HubSession* except) {
	size_t n = 0;
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (hub->session == except) continue;
		if (hub->session && hub->session->isLoggedIn() && hub->session->access == HubSession::AccessNormal) n++;
	}
	return n;
}


size_t QuickDC::HubManager::countHubsRegistered(const HubSession* except) {
	size_t n = 0;
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (hub->session == except) continue;
		if (hub->session && hub->session->isLoggedIn() && hub->session->access == HubSession::AccessRegistered) n++;
	}
	return n;
}


size_t QuickDC::HubManager::countHubsOperator(const HubSession* except) {
	size_t n = 0;
	for (size_t i = 0; i < hubs.size(); i++)
	{
		QuickDC::Hub* hub = hubs[i];
		if (hub->session == except) continue;
		if (hub->session && hub->session->isLoggedIn() && hub->session->access == HubSession::AccessOperator) n++;
	}
	return n;
}


void QuickDC::HubManager::refreshIdentity() {
	for (size_t i = 0; i < hubs.size(); i++) hubs[i]->refreshIdentity();

	/* Throttled: a description changed on several hubs is one update to each. */
	requestClientInfoUpdate();
}
