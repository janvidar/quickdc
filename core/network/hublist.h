/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HUBLIST_H
#define HAVE_QUICKDC_HUBLIST_H

#include <samurai/io/net/http/client.h>

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

/**
 * The largest hublist we will take off the wire, and the largest it may unpack
 * to.
 *
 * Both are needed, for the reasons a file list needs both. The first bounds what
 * one download can make us read; the second is what stops a bzip2 bomb, which
 * reaches about a million to one on input crafted for it - so sixteen bytes of
 * such a thing would otherwise reach the second number on their own. The public
 * lists carry a couple of thousand hubs, which is a few hundred kilobytes packed
 * and one or two megabytes as XML, so neither limit is within sight of a real
 * list.
 */
constexpr size_t QUICKDC_HUBLIST_MAX_DOWNLOAD = 4*1024*1024;
constexpr uint64_t QUICKDC_HUBLIST_MAX_PLAIN = 16*1024*1024;

/**
 * How many hubs one list may name.
 *
 * The XML is let go of as soon as it has been read, and this is what is kept: an
 * entry is a handful of strings, so the limit above bounds the parse and this one
 * bounds what the parse leaves behind.
 */
constexpr size_t QUICKDC_HUBLIST_MAX_HUBS = 65536;

/**
 * How many redirects to follow before giving up.
 *
 * Hublist URLs redirect constantly - a list moves host, or gains a CDN in front
 * of it - so not following any would fail on most of the addresses in
 * circulation. Five is the conventional bound, and the client will not follow an
 * https response to a plain http target whatever this says.
 */
constexpr unsigned QUICKDC_HUBLIST_MAX_REDIRECTS = 5;

namespace QuickDC {

/**
 * One hub, as a hublist describes it.
 *
 * Every field but the address is decoration: the address is what a hub is, and
 * an entry without one names nothing. The numbers are what the list claims and
 * are not checked against anything - only the hub itself can say how many users
 * it has.
 */
struct HubListEntry
{
	std::string name;
	std::string address;
	std::string description;
	std::string country;
	uint64_t users = 0;
	uint64_t shared = 0;
	uint64_t minshare = 0;
	uint64_t minslots = 0;
	uint64_t maxhubs = 0;
	uint64_t maxusers = 0;
};

/** Implement this to receive the outcome of a hublist download. */
class HubListListener
{
	public:
		virtual ~HubListListener() { }

		/**
		 * The hubs the list named. A list that names none is a legitimate
		 * answer and arrives here rather than as a failure.
		 */
		virtual void EventHubList(const std::vector<HubListEntry>& hubs) = 0;

		/**
		 * Nothing usable arrived. 'reason' is a sentence fit to show whoever
		 * asked for the list, and is never null.
		 */
		virtual void EventHubListFailure(const char* reason) = 0;
};

/**
 * Fetch a hublist over HTTP and hand the hubs it names to a listener.
 *
 * The format is XML, commonly served bzip2 compressed as hublist.xml.bz2, and
 * both spellings are read. Which one arrived is decided by the bytes and not by
 * the file name or the content type, because both of those are the server's to
 * choose and neither has to agree with what it sent. The older '|' separated
 * .config format is deliberately not read: nothing has served it for a decade
 * and a half, and its fields are positional with no way to say which is which.
 *
 * https is fetched the same way as http: the HTTP client negotiates TLS,
 * verifies the certificate against the trust store and checks it names the host
 * the URL asked for. A certificate that cannot be verified fails the download -
 * a hublist is a public server with a real certificate, and accepting an
 * unverifiable one would encrypt the conversation against everyone except
 * whoever went to the trouble of intercepting it.
 *
 * Redirects are followed, up to the budget above, because most of the hublist
 * addresses in circulation are older than the servers behind them. The client
 * does the following, so only the list at the end of the chain arrives here.
 *
 * A hublist server is no more trustworthy than a peer, so what it serves is
 * bounded before any of it is believed - see the limits above - and a list that
 * hits a limit, or a document that stops before it closes, is refused rather
 * than cut short. A hublist that is missing hubs looks exactly like one that
 * never had them.
 */
class HubList final : private Samurai::IO::Net::HTTP::RequestEventHandler
{
	public:
		explicit HubList(HubListListener* listener);
		~HubList() override;

		/* Owns an HTTP client that holds a pointer back to this as its handler,
		 * so a copy would leave two objects answering for one request. */
		HubList(const HubList&) = delete;
		HubList& operator=(const HubList&) = delete;

		/**
		 * Ask for the list at 'url'.
		 *
		 * The listener hears back exactly once per call, and may hear back
		 * before this returns: a URL we cannot fetch is refused here rather
		 * than after a round trip.
		 */
		void download(const Samurai::IO::Net::URL& url);

		/** Abandon the download. The listener hears nothing further. */
		void cancel();

		bool isBusy() const;

		/**
		 * Read a hublist that is already in hand, exactly as it was served -
		 * bzip2 or XML.
		 *
		 * @param hubs   filled in on success and left empty otherwise, so
		 *               nothing partial is ever handed back.
		 * @param reason on failure, why, in a form fit to show a user.
		 * @return false if the bytes could not be believed.
		 */
		static bool parse(std::string_view served, std::vector<HubListEntry>& hubs,
		                  std::string& reason);

	private:
		void EventHttpResponse(Samurai::IO::Net::HTTP::Client*,
		                       const Samurai::IO::Net::HTTP::Response&) override;
		void EventHttpError(Samurai::IO::Net::HTTP::Client*,
		                    Samurai::IO::Net::HTTP::Status, const char* msg) override;

		void fail(const std::string& reason);

		HubListListener* listener;
		Samurai::IO::Net::HTTP::Client client;
};

}

#endif // HAVE_QUICKDC_HUBLIST_H
