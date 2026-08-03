/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/hublist.h"
#include "xml/reader.h"

#include <samurai/io/buffer.h>
#include <samurai/io/compression.h>
#include <samurai/stdc.h>

#include <chrono>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

Samurai::IO::Net::HTTP::Client::Options hublistOptions()
{
	Samurai::IO::Net::HTTP::Client::Options options;

	/* One deadline over the whole fetch, and longer than the client's own ten
	   seconds: a hublist is a megabyte or so from a server that is shared by
	   every client that ever heard of it. */
	options.timeout = std::chrono::seconds(60);

	/*
	 * The client's own body limit is one megabyte, which is what a UPnP reply
	 * needs and less than a real hublist. Raised deliberately, and to the same
	 * number the chunked framing is allowed, so a server that answers with
	 * Transfer-Encoding: chunked is bounded the same way as one that answers
	 * with a Content-Length.
	 */
	options.limits.maxBody = QUICKDC_HUBLIST_MAX_DOWNLOAD;
	options.limits.maxChunkSize = QUICKDC_HUBLIST_MAX_DOWNLOAD;

	/*
	 * The client does not follow redirects by default, which is right for a
	 * device on the local network and wrong for a named server on the internet.
	 * A hublist address that has not moved at least once is the exception.
	 */
	options.maxRedirects = QUICKDC_HUBLIST_MAX_REDIRECTS;

	/*
	 * allowUntrustedCertificate is deliberately left off. A hublist is served by
	 * a public host with a certificate a trust store knows, so there is nothing
	 * here that a fingerprint or an out-of-band secret authenticates instead -
	 * and with it on, https would only keep out an observer who was not trying.
	 */

	options.userAgent = PRODUCT "/" VERSION;
	return options;
}

/* What a bzip2 stream opens with: the magic, then the block size as a digit. */
bool isBZip2(std::string_view data)
{
	return data.size() >= 4
		&& data[0] == 'B' && data[1] == 'Z' && data[2] == 'h'
		&& data[3] >= '1' && data[3] <= '9';
}

/*
 * Unpack 'packed' into 'out', which is empty on entry.
 *
 * The plaintext is streamed through a fixed window into the one buffer the XML
 * reader is later handed, so it is never held twice - and what a bomb makes
 * enormous is exactly the plaintext.
 */
bool inflate(std::string_view packed, Samurai::IO::Buffer& out, std::string& reason)
{
	Samurai::IO::BZip2Decompressor codec;

	char window[65536];
	std::span<const char> in(packed.data(), packed.size());
	uint64_t produced = 0;

	while (true)
	{
		const Samurai::IO::Codec::Progress p = codec.step(in, window);

		if (p.status == Samurai::IO::Codec::Status::Error)
		{
			reason = "the hublist is not valid bzip2 data";
			return false;
		}

		produced += p.produced;
		if (produced > QUICKDC_HUBLIST_MAX_PLAIN)
		{
			reason = "the hublist unpacks to more than the "
				+ std::to_string(QUICKDC_HUBLIST_MAX_PLAIN)
				+ " bytes we are willing to read";
			return false;
		}

		if (p.produced)
		{
			out.append(window, p.produced);

			/*
			 * Buffer::append drops what it cannot make room for and says so
			 * only to the log. Nothing has taken anything out of 'out', so its
			 * size is what has been appended: a short one is a truncated
			 * hublist, and that is the one thing this must not hand on.
			 */
			if (out.size() != static_cast<size_t>(produced))
			{
				reason = "the hublist could not be held in memory";
				return false;
			}
		}

		in = in.subspan(p.consumed);

		if (p.status == Samurai::IO::Codec::Status::StreamEnd) return true;

		/* Nothing moved, and there is nothing left to feed it. A flush that
		   needs several calls consumes nothing but does produce, so this is the
		   stream stopping short of its end rather than a flush in progress. */
		if (!p.consumed && !p.produced)
		{
			reason = "the hublist is a truncated bzip2 stream";
			return false;
		}
	}
}

/*
 * A hublist is flat: <Hublist> holds <Hubs>, which holds one self-closing <Hub>
 * per hub. <Hubs> is not insisted on, because generators differ over whether
 * anything sits between it and the root, but the root is: a document that never
 * says it is a hublist is one of the other things a URL can serve, and mining a
 * <Hub> out of that would report hubs nobody published.
 */
class HubListHandler final : public XML::SAX::ContentHandler
{
	public:
		explicit HubListHandler(std::vector<QuickDC::HubListEntry>& hubs_)
			: hubs(hubs_) { }

		const std::string& getProblem() const { return problem; }
		bool sawRoot() const { return root; }
		size_t getSkipped() const { return skipped; }

		void startDocument() override { }

		/*
		 * A document that ends with elements still open is a file that stopped
		 * before it finished, which is what a hublist truncated in transit looks
		 * like: the tags it does have parse, and the hubs after the cut are
		 * simply absent. The reader is content with it - it only insists that
		 * every '<' produced a tag - so the count has to be kept here.
		 */
		void endDocument() override
		{
			if (open) onProblem("it does not close every element it opens");
		}

		void startElement(const char*, const char* localName, const char*,
		                  const AttributeList& list) override;

		void endElement(const char*, const char*, const char*) override
		{
			/* The reader reports an end tag with nothing open, so this counts
			   down past zero rather than wrapping. */
			if (open) open--;
			else onProblem("it closes an element that was never opened");
		}

		void characters(const char*, size_t, size_t) override { }
		void ignorableWhitespace(const char*, size_t, size_t) override { }

		void warning(const char* str) override { QERR("Hublist XML warning: %s", str); }

		/*
		 * Both of these refuse the list. The reader reports a document it could
		 * not follow through error() and then carries on, so the events after one
		 * describe a document nobody wrote - and there is no telling which hubs
		 * are missing from it.
		 */
		void error(const char* str) override { onProblem(str); }
		void fatalError(const char* str) override { onProblem(str); }

	private:
		void onProblem(const char* why)
		{
			if (problem.empty())
				problem = why && *why ? why : "it is not well formed";
		}

		std::vector<QuickDC::HubListEntry>& hubs;
		std::string problem;
		size_t skipped = 0;
		size_t open = 0;
		bool root = false;
};

void HubListHandler::startElement(const char*, const char* localName, const char*,
                                  const AttributeList& list)
{
	/*
	 * localName is whatever name the parser found, and it is documented as being
	 * free to hand a handler none at all: an absent name becomes the empty one
	 * here rather than being compared through a null pointer.
	 */
	const std::string_view element(localName ? localName : "");

	open++;

	if (Samurai::Util::iequals(element, "Hublist"))
	{
		root = true;
		return;
	}

	if (!root || !problem.empty()) return;
	if (!Samurai::Util::iequals(element, "Hub")) return;

	if (hubs.size() >= QUICKDC_HUBLIST_MAX_HUBS)
	{
		onProblem("it names more hubs than we are willing to keep");
		return;
	}

	QuickDC::HubListEntry entry;

	/* The attribute names are the server's, and no two generators agree on the
	   case of them. */
	for (const auto& [attribute, value] : list)
	{
		if (Samurai::Util::iequals(attribute, "Name"))
			entry.name = value;
		else if (Samurai::Util::iequals(attribute, "Address"))
			entry.address = value;
		else if (Samurai::Util::iequals(attribute, "Description"))
			entry.description = value;
		else if (Samurai::Util::iequals(attribute, "Country"))
			entry.country = value;
		else if (Samurai::Util::iequals(attribute, "Users"))
			entry.users = Samurai::Util::Convert::to_uint64(value);
		else if (Samurai::Util::iequals(attribute, "Shared"))
			entry.shared = Samurai::Util::Convert::to_uint64(value);
		else if (Samurai::Util::iequals(attribute, "Minshare"))
			entry.minshare = Samurai::Util::Convert::to_uint64(value);
		else if (Samurai::Util::iequals(attribute, "Minslots"))
			entry.minslots = Samurai::Util::Convert::to_uint64(value);
		else if (Samurai::Util::iequals(attribute, "Maxhubs"))
			entry.maxhubs = Samurai::Util::Convert::to_uint64(value);
		else if (Samurai::Util::iequals(attribute, "Maxusers"))
			entry.maxusers = Samurai::Util::Convert::to_uint64(value);
	}

	/*
	 * An entry with no address names nowhere to connect to, and no other field
	 * can stand in for it. Such an entry is dropped and the rest of the list is
	 * kept: one unusable row is a generator's bug, where refusing the whole list
	 * over it would leave a user with no hubs at all because of one hub they
	 * were never going to pick. Note the difference from a limit being hit,
	 * which refuses everything - there the missing hubs are unknowable, and here
	 * they are counted and logged.
	 */
	if (entry.address.empty())
	{
		skipped++;
		return;
	}

	hubs.push_back(std::move(entry));
}

}


QuickDC::HubList::HubList(HubListListener* listener_)
	: listener(listener_)
	, client(this, hublistOptions())
{
}


QuickDC::HubList::~HubList()
{
}


bool QuickDC::HubList::isBusy() const
{
	return client.isBusy();
}


void QuickDC::HubList::cancel()
{
	client.abort();
}


void QuickDC::HubList::download(const Samurai::IO::Net::URL& url)
{
	if (client.isBusy())
	{
		fail("a hublist is already being downloaded");
		return;
	}

	QDBG("Downloading the hublist at %s", url.toString().c_str());
	client.get(url);
}


bool QuickDC::HubList::parse(std::string_view served, std::vector<HubListEntry>& hubs,
                             std::string& reason)
{
	hubs.clear();
	reason.clear();

	if (served.empty())
	{
		reason = "the server returned nothing";
		return false;
	}

	/* The download limit is the client's to enforce, and is enforced again here
	   because this is also reachable with bytes that never came off a socket. */
	if (served.size() > QUICKDC_HUBLIST_MAX_DOWNLOAD)
	{
		reason = "the hublist is larger than the "
			+ std::to_string(QUICKDC_HUBLIST_MAX_DOWNLOAD)
			+ " bytes we are willing to read";
		return false;
	}

	try
	{
		Samurai::IO::Buffer plain;

		if (isBZip2(served))
		{
			if (!inflate(served, plain, reason))
			{
				hubs.clear();
				return false;
			}
		}
		else
		{
			plain.append(served.data(), served.size());
			if (plain.size() != served.size())
			{
				reason = "the hublist could not be held in memory";
				return false;
			}
		}

		HubListHandler handler(hubs);
		XML::SAX::XMLReader reader(&plain, &handler);
		reader.parse();

		if (!handler.getProblem().empty())
		{
			reason = "the hublist is not readable XML: " + handler.getProblem();
			hubs.clear();
			return false;
		}

		if (!handler.sawRoot())
		{
			reason = "what the server returned is not a hublist";
			hubs.clear();
			return false;
		}

		if (handler.getSkipped())
			QERR("Ignored %lu hublist entries that named no address",
				static_cast<unsigned long>(handler.getSkipped()));
	}
	catch (const std::bad_alloc&)
	{
		/* The strings and the vector throw where Buffer::append drops, and both
		   ways of running out have to end as a refusal rather than as a partial
		   list or as a terminate(). */
		reason = "the hublist could not be held in memory";
		hubs.clear();
		return false;
	}

	return true;
}


void QuickDC::HubList::EventHttpResponse(Samurai::IO::Net::HTTP::Client*,
                                         const Samurai::IO::Net::HTTP::Response& response)
{
	const int code = response.getStatusCode();

	/*
	 * A redirect the client could act on has already been followed, and one it
	 * could not - too many of them, or a Location naming something unusable -
	 * arrives as an error rather than here. So what reaches this is a 3xx with no
	 * Location at all, which is a server that has not said where the list went.
	 * Saying so is the only useful answer: read as a hublist, the body such a
	 * response carries reports a list of no hubs, and that is indistinguishable
	 * from a list which is genuinely empty.
	 */
	if (code >= 300 && code <= 399)
	{
		std::string reason = "the server redirected, which is not followed";

		const std::optional<std::string_view> location =
			response.getHeaders().get("Location");

		if (location && !location->empty())
		{
			reason += " (to ";
			reason += *location;
			reason += ")";
		}

		fail(reason);
		return;
	}

	if (!response.isSuccess())
	{
		std::string reason = "the server answered " + std::to_string(code);
		if (!response.getReasonPhrase().empty())
		{
			reason += " ";
			reason += response.getReasonPhrase();
		}
		fail(reason);
		return;
	}

	std::vector<HubListEntry> hubs;
	std::string reason;

	if (!parse(response.getBody(), hubs, reason))
	{
		fail(reason);
		return;
	}

	QDBG("The hublist names %lu hubs", (unsigned long) hubs.size());
	listener->EventHubList(hubs);
}


void QuickDC::HubList::EventHttpError(Samurai::IO::Net::HTTP::Client*,
                                      Samurai::IO::Net::HTTP::Status status,
                                      const char* msg)
{
	/*
	 * These four are told apart because each asks a different thing of whoever
	 * wanted the list, and none of them is worth a retry. The limits in the first
	 * two are named because they are ours and not the server's: a list that grew
	 * past what we are willing to read, or a chain longer than we are willing to
	 * walk, is a different thing from a server that broke.
	 */
	switch (status)
	{
		case Samurai::IO::Net::HTTP::Status::ResponseTooLarge:
			fail("the hublist is larger than the "
				+ std::to_string(QUICKDC_HUBLIST_MAX_DOWNLOAD)
				+ " bytes we are willing to download");
			return;

		case Samurai::IO::Net::HTTP::Status::TooManyRedirects:
			fail("the hublist server redirected more than "
				+ std::to_string(QUICKDC_HUBLIST_MAX_REDIRECTS)
				+ " times without arriving at a list");
			return;

		case Samurai::IO::Net::HTTP::Status::TlsFailed:
			/*
			 * The server answered, and either what it presented is not who the
			 * URL asked for or the handshake never got that far. Both are worth
			 * looking into rather than retrying, which is what tells them apart
			 * from not having connected at all.
			 *
			 * What the socket said goes to the log rather than into the reason:
			 * it is a TLS or a system error string, and neither is a sentence.
			 */
			QERR("Hublist TLS failure: %s", msg && *msg ? msg : "no detail given");
			fail("the hublist server's certificate could not be verified, "
				"or the TLS handshake did not complete");
			return;

		case Samurai::IO::Net::HTTP::Status::InsecureRedirect:
			fail("the hublist server redirected from https to plain http, "
				"which is not followed");
			return;

		default:
			break;
	}

	std::string reason = "the hublist could not be downloaded: ";
	reason += msg && *msg ? msg : Samurai::IO::Net::HTTP::toString(status);
	fail(reason);
}


void QuickDC::HubList::fail(const std::string& reason)
{
	QERR("Hublist: %s", reason.c_str());
	listener->EventHubListFailure(reason.c_str());
}
