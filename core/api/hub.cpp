/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <atomic>
#include <cctype>
#include <chrono>
#include <stdio.h>
#include <string.h>

#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketbase.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/url.h>
#include <samurai/util/random.h>
#include <samurai/stdc.h>
#include "network/user.h"
#include "network/dc/dccommandparser.h"
#include "network/dc/dccommandproxy.h"
#include "network/dc/dcconnection.h"
#include "network/dc/hubcommands.h"
#include "network/dc/hubsession.h"
#include "network/adc/adccommandparser.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/client/adchubsession.h"
#include "network/connectionmanager.h"
#include "network/hubmanager.h"
#include "api/hub.h"
#include "api/core.h"
#include "config/settings.h"
#include "config/preferences.h"
#include "share/searchresult.h"

QuickDC::Hub::Hub(Core& core_, HubListener* listener_, Samurai::IO::Net::URL* url_)
	: core(core_)
	, manager(core_.hubs)
	, session(0)
	, listener(listener_)
	, requestedDisconnect(false)
	, attempts(0)
	, url(new Samurai::IO::Net::URL(url_))
	, canonicalUrl(0)
	, timer(0)
{
	/*
	 * Build the canonical form once. URL::toString() keeps whatever path the
	 * user typed, and a FileSource stores only "{scheme}://{host}:{port}", so
	 * the two would not compare equal without this.
	 */
	std::string canonical = url->getScheme();
	canonical += "://";
	canonical += url->getHost().toString();
	canonical += ":";
	canonical += std::to_string(url->getPort());
	canonicalUrl = strdup(canonical.c_str());

	if (manager) manager->add(this);
}

QuickDC::Hub::~Hub()
{
	/*
	 * The session first, and this order is not a preference.
	 *
	 * connect() is the only thing that makes one and nothing else takes it over,
	 * so this owns it - and leaving it behind left more than memory: a session
	 * holds a socket registered with the monitor, whose callbacks name this object
	 * and the listener it was given. Both are gone by then, so the next turn of
	 * the event loop reported a disconnect into freed memory. It also holds the
	 * url deleted below.
	 */
	delete session;
	session = 0;

	delete timer;
	timer = 0;

	delete url;
	free(canonicalUrl);

	if (manager) manager->remove(this);
}

void QuickDC::Hub::connect()
{
	redirected.clear();
	open();
}

void QuickDC::Hub::open()
{
	if (session)
	{
	        requestedDisconnect = true;
	        session->disconnect();
	}

	/* Taken over only now: the session just let go of may still be reading the old one. */
	if (!redirectTarget.empty())
	{
		*url = Samurai::IO::Net::URL(redirectTarget);
		free(canonicalUrl);
		canonicalUrl = strdup(redirectTarget.c_str());
		redirectTarget.clear();
		attempts = 0;
	}

	std::string scheme = url->getScheme();
	
	/**
	 * TODO: Create a factory class that will select the apropriate one.
	 */
	if (scheme == "dchub")
	{
		session = new DC::HubSession(this, listener, url);
	}
	else if (scheme == "adcs")
	{
		session = new ADC::HubSession(this, listener, url, true);
	}
	else if (scheme == "adc")
	{
		session = new ADC::HubSession(this, listener, url, false);
	}
	else
	{
		QERR("Protocol not handled. URI: '%s'", url->toString().c_str());
		return;
	}

	requestedDisconnect = false;
	delete timer;
	timer = 0;
	
	if (session)
	{
		session->connect();
	}
}

void QuickDC::Hub::disconnect()
{
	requestedDisconnect = true;
	redirectTarget.clear();

	/* Any attempt still pending is not wanted either. */
	delete timer;
	timer = 0;

	if (session)
	{
		session->disconnect();
	}
}

void QuickDC::Hub::sendPassword(const char* password)
{
	/* Deliberately not guarded on isLoggedIn(): being asked for a password is
	   precisely the state of not being logged in yet. */
	if (session)
		session->sendPassword(password);
	else
		listener->EventSystemError("sendPassword(): No session");
}

void QuickDC::Hub::sendChatMessage(const char *message, bool richText)
{
	if (session && session->isLoggedIn())
	{
		session->sendChatMessage(message, richText);
	}
	else
	{
		listener->EventSystemError("sendChatMessage(): No session");
	}
}

void QuickDC::Hub::sendPrivateChatMessage(const QuickDC::User* user, const char* message,
                                         bool richText) {
	if (!session || !session->isLoggedIn())
	{
		listener->EventSystemError("sendPrivateChatMessage(): No session");
		return;
	}

	if (!user)
	{
		listener->EventSystemError("sendPrivateChatMessage(): No such user");
		return;
	}

	if (!message)
	{
		listener->EventSystemError("sendPrivateChatMessage(): Message is null");
		return;
	}

	if (!strlen(message))
	{
		listener->EventSystemError("sendPrivateChatMessage(): Message is empty");
		return;
	}

	session->sendPrivateChatMessage(user, message, richText);
}

QuickDC::Hub::DisconnectRefusal QuickDC::Hub::disconnectUser(const char* user, const std::string& message,
                                                             const std::string& redirect, int64_t banSeconds)
{
	if (!session || !session->isLoggedIn()) return DisconnectRefusal::NoSession;

	const QuickDC::User* u = user ? session->getUser(user) : 0;
	if (!u) return DisconnectRefusal::NoSuchUser;

	const QuickDC::User* me = session->getLocalUser();
	if (!me || !me->isOp()) return DisconnectRefusal::NotOperator;

	if (!redirect.empty())
		session->sendAdminUserRedirect(u, redirect.c_str(), message.empty() ? "Redirected" : message.c_str());
	else if (banSeconds != 0 && !session->supportsBans())
		return DisconnectRefusal::Unsupported;
	else
		session->sendAdminUserKick(u, message.c_str(), banSeconds);

	return DisconnectRefusal::None;
}

bool QuickDC::Hub::connectUser(const char* user, const char* token)
{
	if (!session || !session->isLoggedIn())
	{
		listener->EventSystemError("connectUser(): No session");
		return false;
	}

	const QuickDC::User* u = session->getUser(user);
	if (!u)
	{
		listener->EventSystemError("connectUser(): No such user");
		return false;
	}

	if (token)
	{
		session->sendConnectionRequest(u, token);
		return true;
	}

	/*
	 * Nothing in the download queue is behind this one - it is a manual
	 * connect - so there is no job to correlate and any unique token will do.
	 */
	static std::atomic<size_t> counter{0};
	char generated[32];
	snprintf(generated, sizeof(generated), "MAN%lu%lu",
		static_cast<unsigned long>(Samurai::TimeStamp().getInternalData()),
		static_cast<unsigned long>(++counter));

	session->sendConnectionRequest(u, generated);
	return true;
}

bool QuickDC::Hub::connectUserByClientID(const char* cid, const char* token)
{
	if (!session || !session->isLoggedIn())
	{
		QDBG("connectUserByClientID(): not logged in");
		return false;
	}

	if (!cid || !*cid || !token) return false;

	QuickDC::UserManager* users = session->getUserManager();
	if (!users) return false;

	const QuickDC::User* u = sourceUser(cid);
	if (!u)
	{
		/*
		 * Named with the hub it was looked for on and with who is there without
		 * a client ID, because those are the two ways this fails while the user
		 * is plainly on the hub: a source recorded against the wrong hub, or a
		 * record that never picked up the client ID to be found by.
		 */
		QDBG("connectUserByClientID(): '%s' is not on '%s' (token=%s); %s",
			cid, getUrl() ? getUrl() : "?", token,
			users->describeMissingClientIDs(8).c_str());
		return false;
	}

	session->sendConnectionRequest(u, token);
	return true;
}

const char* QuickDC::Hub::getUrl() const
{
	return canonicalUrl;
}

bool QuickDC::Hub::isSecure() const
{
	return session && session->isSecure();
}

bool QuickDC::Hub::supportsRichText() const
{
	return session && session->supportsRichText();
}

bool QuickDC::Hub::supportsBulletins() const
{
	return session && session->supportsBulletins();
}

/*
 * Each of the five refuses on its own account when there is no session, rather
 * than through a shared guard, so that the answer is the same false a hub without
 * boards gives - one caller, one question, one answer. Nothing is said to the
 * listener: see the note in the header.
 */
bool QuickDC::Hub::subscribeBoard(const char* board, uint64_t timestamp)
{
	return session && session->subscribeBoard(board, timestamp);
}

bool QuickDC::Hub::unsubscribeBoard(const char* board)
{
	return session && session->unsubscribeBoard(board);
}

bool QuickDC::Hub::requestEntry(const char* board, const char* tth)
{
	return session && session->requestEntry(board, tth);
}

bool QuickDC::Hub::submitPost(const char* tth, uint64_t size, const char* board,
                              const char* subject, const char* parent)
{
	return session && session->submitPost(tth, size, board, subject, parent);
}

bool QuickDC::Hub::withdrawPost(const char* tth, const char* board)
{
	return session && session->withdrawPost(tth, board);
}

const QuickDC::User* QuickDC::Hub::getLocalUser() const
{
	return (session) ? session->getLocalUser() : 0;
}

bool QuickDC::Hub::isLocalUser(const char* cid) const
{
	if (!session || !cid || !*cid) return false;

	const QuickDC::User* me = session->getLocalUser();
	if (!me) return false;

	/* The lookup connectUserByClientID() makes, so the two cannot disagree
	   about who a caller meant. */
	return sourceUser(cid) == me;
}

bool QuickDC::Hub::isNmdc() const
{
	return url && url->getScheme() == "dchub";
}

/*
 * A SID names a user only while the user stays, and the next one to get it is
 * somebody else, so on ADC a source is the CID alone.
 */
const QuickDC::User* QuickDC::Hub::sourceUser(const char* id) const
{
	if (!session || !id || !*id) return 0;

	QuickDC::UserManager* users = session->getUserManager();
	const QuickDC::User* named = (users) ? users->getUserByClientID(id) : 0;
	if (!named && isNmdc()) named = session->getUser(id);
	return named;
}

std::string QuickDC::Hub::sourceIdOf(const char* id) const
{
	if (!session || !id || !*id) return std::string();
	if (isNmdc()) return session->getUser(id) ? std::string(id) : std::string();

	const QuickDC::User* named = sourceUser(id);
	if (!named) named = session->getUser(id);
	const char* cid = (named) ? reinterpret_cast<const char*>(named->getClientID()) : 0;
	return (cid && *cid) ? std::string(cid) : std::string();
}

QuickDC::UserManager* QuickDC::Hub::getUserManager() const
{
	return (session) ? session->getUserManager() : 0;
}

void QuickDC::Hub::sendSearchRequest(Share::SearchRequest* request)
{
	if (!session || !session->isLoggedIn())
	{
		listener->EventSystemError("sendSearchRequest(): No session");
		return;
	}
	
	if (!request)
	{
		listener->EventSystemError("sendSearchRequest(): No request");
		return;
	}
	session->sendSearchRequest(request);
}

void QuickDC::Hub::onSearchResult(Share::SearchResult* result)
{
	if (!result) return;

	if (!session)
	{
		delete result;
		return;
	}

	session->onSearchResult(result);
}

void QuickDC::Hub::EventTimeout(Samurai::Timer*)
{
	delete timer; timer = 0;
	open();
}

namespace {

bool isAdcScheme(const std::string& scheme) { return scheme == "adc" || scheme == "adcs"; }

std::string lowered(std::string text)
{
	for (char& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return text;
}

}

QuickDC::RedirectRefusal QuickDC::Hub::judgeRedirect(const std::string& from, const char* address,
                                                     uint16_t port, bool follow,
                                                     const std::vector<std::string>& visited,
                                                     std::string& target)
{
	target.clear();
	if (!follow) return RedirectRefusal::Policy;
	if (!address || !*address) return RedirectRefusal::Invalid;

	const std::string fromScheme = lowered(Samurai::IO::Net::URL(from).getScheme());

	/* NMDC's $ForceMove names a host, and a port only as a number of its own. */
	std::string written = address;
	if (written.find("://") == std::string::npos)
	{
		if (fromScheme != "dchub") return RedirectRefusal::Invalid;
		written = "dchub://" + written;
		if (port && written.find(':', 8) == std::string::npos) written += ":" + std::to_string(port);
	}

	const Samurai::IO::Net::URL to(written);
	const uint16_t toPort = to.isValid() ? to.getEffectivePort() : 0;
	if (!to.isValid() || !toPort || to.getHostname().empty()) return RedirectRefusal::Invalid;

	const std::string toScheme = lowered(to.getScheme());
	if (!isAdcScheme(toScheme) && toScheme != "dchub") return RedirectRefusal::Invalid;
	if (isAdcScheme(toScheme) != isAdcScheme(fromScheme)) return RedirectRefusal::OtherProtocol;
	if (fromScheme == "adcs" && toScheme == "adc") return RedirectRefusal::Downgrade;

	const std::string going = toScheme + "://" + lowered(to.getHost().toString()) + ":" + std::to_string(toPort);
	if (Samurai::Util::iequals(going, from)) return RedirectRefusal::Loop;
	for (const std::string& been : visited)
		if (Samurai::Util::iequals(going, been)) return RedirectRefusal::Loop;
	if (visited.size() > QUICKDC_HUB_MAX_REDIRECTS) return RedirectRefusal::Loop;

	target = going;
	return RedirectRefusal::None;
}

void QuickDC::Hub::onRedirect(const char* address, uint16_t port)
{
	if (redirected.empty() && canonicalUrl) redirected.push_back(canonicalUrl);

	std::string target;
	const RedirectRefusal why = judgeRedirect(canonicalUrl ? canonicalUrl : "", address, port,
	                                          followRedirects, redirected, target);

	if (why == RedirectRefusal::None)
	{
		QDBG("Redirected from '%s' to '%s'", canonicalUrl, target.c_str());
		redirected.push_back(target);
		redirectTarget = target;

		/* Not from inside the session that is being told to go. */
		delete timer;
		timer = new Samurai::Timer(core.loop(), this, std::chrono::milliseconds(0), true);
	}

	listener->EventHubRedirect(address, port, target.c_str(), why);
}

int QuickDC::Hub::retryDelay(HubListener::StatusNetwork why, size_t attempts)
{
	if (!attempts) return -1;

	/*
	 * Nothing reachable: one more attempt, soon. On macOS the first connection to
	 * an address on the local network is spent asking whether this application may
	 * make it at all, and fails - so the second one is the one that works. A route
	 * that is genuinely not there will not appear because we asked twice.
	 */
	if (why == HubListener::NetErrorHostUnreachable)
		return (attempts == 1) ? QUICKDC_HUB_RETRY_UNREACHABLE : -1;

	/* Doubling, to the ceiling: 5, 10, 20, 40 ... seconds. */
	uint64_t wait = QUICKDC_HUB_RETRY_BASE;
	for (size_t n = 1; n < attempts && wait < QUICKDC_HUB_RETRY_MAX; n++)
		wait *= 2;

	if (wait > QUICKDC_HUB_RETRY_MAX) wait = QUICKDC_HUB_RETRY_MAX;

	/*
	 * Somewhere in the upper half of that, rather than exactly on it. A hub that
	 * drops every client at once would otherwise have all of them back at the same
	 * instant, which is how it came to drop them.
	 */
	const int spread = static_cast<int>(wait / 2);
	return static_cast<int>(wait - spread) + Samurai::Util::pseudoRandom(0, spread);
}

bool QuickDC::Hub::connectionHeld(uint64_t seconds)
{
	return seconds >= QUICKDC_HUB_STABLE;
}

void QuickDC::Hub::onDisconnected(HubListener::StatusNetwork why)
{
	if (session && session->getUserManager())
	{
		listener->EventUsersCleanup();
		session->getUserManager()->cleanup();
	}

	/*
	 * A connection that lasted starts the backoff again from the beginning:
	 * whatever the trouble was, it was not that this hub cannot be reached.
	 *
	 * Asked of how long the session was logged in rather than of whether it
	 * still is. Every path that ends a session clears its state before reporting
	 * the disconnection, so isLoggedIn() is false by the time this runs and the
	 * backoff went on doubling however long the connection had held - a client
	 * idle on a hub for hours, cut off by a firewall that had forgotten the
	 * connection, came back only after the five minute ceiling.
	 */
	if (session && connectionHeld(session->establishedFor())) attempts = 0;

	const int reconnect_time = (session) ? session->getReconnectTime() : -1;
	session = 0;

	/*
	 * A disconnect that was asked for is reported to nobody: the listener knows it
	 * asked, and saying there will be no attempt invites it to explain an absence
	 * that is its own doing.
	 */
	if (requestedDisconnect) return;

	/* On its way elsewhere, by the timer onRedirect() armed. */
	if (!redirectTarget.empty()) return;

	if (reconnect_time == -1)
	{
		listener->EventReconnectScheduled(-1);
		return;
	}

	attempts++;

	const int wait = retryDelay(why, attempts);
	if (wait < 0)
	{
		QDBG("Not trying '%s' again", canonicalUrl);
		listener->EventReconnectScheduled(-1);
		return;
	}

	QDBG("Trying '%s' again in %d seconds (attempt %d)",
		canonicalUrl, wait, static_cast<int>(attempts));

	delete timer;
	timer = new Samurai::Timer(core.loop(), this, static_cast<time_t>(wait), true);

	/*
	 * After the timer, so that a listener told the wait can rely on the attempt
	 * being armed - and not before, since a listener may reach back in here.
	 */
	listener->EventReconnectScheduled(wait);
}


void QuickDC::Hub::refreshIdentity()
{
	if (!session || !core.config || !core.session) return;

	const std::string group = getUrl();

	std::string nick = Settings::readForHub(*core.session, group, *core.config, Settings::Nickname);
	if (nick.empty()) nick = Settings::read(*core.config, Settings::Nickname);
	if (nick.empty()) nick = PRODUCT;

	if (session->isLoggedIn() && nick != session->currentNick())
	{
		QDBG("Reconnecting to '%s' as '%s'", canonicalUrl, nick.c_str());
		connect();
		return;
	}

	session->setIdentity(nick,
		Settings::readForHub(*core.session, group, *core.config, Settings::Description),
		Settings::readForHub(*core.session, group, *core.config, Settings::Email));
}

bool QuickDC::Hub::hasSourceUser(const char* cid) const
{
	if (!session || !session->isLoggedIn()) return false;
	return sourceUser(cid) != nullptr;
}
