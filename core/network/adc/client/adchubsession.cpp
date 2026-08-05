/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <string.h>
#include <time.h>

#include "quickdc.h"
#include "api/core.h"
#include "network/portforward.h"
#include "network/reachability.h"
#include "api/server.h"
#include "config/preferences.h"
#include "network/connectionmanager.h"
#include "network/hubmanager.h"
#include "network/hubsession.h"
#include "network/keyprint.h"
#include "network/securitymanager.h"
#include "network/usermanager.h"
#include "network/adc/adccommon.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/adccommands.h"
#include "network/adc/cid.h"
#include "network/adc/client/adcclientsession.h"
#include "network/adc/client/adchubsession.h"
#include "network/adc/parser.h"
#include "share/sharefile.h"
#include "share/sharemanager.h"
#include "share/searchrequest.h"
#include "share/searchresult.h"
#include "network/downloadqueue.h"
#include "network/leafhashes.h"
#include "network/filesource.h"
#include "network/user.h"
#include <string>
#include <string_view>
#include <samurai/crypto/digest/tiger.h>
#include <samurai/io/buffer.h>
#include <samurai/io/file.h>
#include <samurai/io/net/datagram.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/tlsfactory.h>
#include <samurai/io/net/url.h>
#include <samurai/util/base32.h>
#include <span>


/*
 * One field: severity digit followed by a two digit code. Sending the code on
 * its own leaves a peer reading the severity off the first digit of the code.
 */
static std::string adc_status_code(int severity, int code)
{
	char buf[8];
	snprintf(buf, sizeof(buf), "%d%02d", severity, code);
	return std::string(buf);
}


/*
 * Whether a peer dialling us can be answered with TLS. probe.cpp completes a
 * server side handshake before fingerprinting the stream, so the same port
 * serves both protocols and the only thing that can be missing is a
 * certificate to present. Without one the handshake fails, which the peer can
 * only read as a reset - so naming ADCS then would be worse than not offering
 * it at all.
 */
static bool canAcceptTls()
{
	Samurai::IO::File* cert = Samurai::IO::Net::TlsFactory::getCertificate();
	Samurai::IO::File* pkey = Samurai::IO::Net::TlsFactory::getPrivateKey();

	return cert && cert->exists() && pkey && pkey->exists();
}

/*
 * Whether a peer transfer has to be encrypted.
 *
 * On by default. A peer transfer carries the file and, on the NMDC side of this
 * client, nothing about it is protected at all; on the ADC side the protection
 * was previously decided by a single string in a message the hub relayed, so a
 * hub - or anyone who could forge a CTM through it - chose whether the transfer
 * that followed was encrypted. Requiring it takes that choice away from them.
 *
 * The cost is real and is the reason this is a setting rather than a constant: a
 * peer that does not offer ADCS cannot be transferred with at all, and on a
 * plain ADC hub keyprints cannot be trusted anyway, so there the requirement
 * buys confidentiality against an observer but not authentication of the peer.
 */
static bool requireEncryptedTransfers()
{
	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Network");
	return config->getBool("Require encrypted transfers", true);
}

/*
 * Whether to keep claiming that a peer told to connect here will get through.
 *
 * Disproven is the only thing that withdraws it. Nothing here can prove the claim
 * before somebody acts on it, so a client that waited for proof would announce
 * passive on every start and have no way to leave that state - the proof arrives
 * only once a peer has already tried.
 *
 * Overridable both ways round for the same reason the evidence is bounded: the
 * reports come from whoever is on the hub.
 */
static bool reachable(QuickDC::Preferences* config)
{
	config->setGroup("Network");

	if (config->getBool("Force active mode", false)) return true;

	const QuickDC::Reachability* known = QuickDC::Core::getInstance()->reachability;
	if (!known) return true;

	return known->tcp() != QuickDC::Reachability::State::Unreachable;
}

/*
 * The address to publish, active or not.
 *
 * For an active client peerEndpoint() has already ranked what is known - what a hub
 * observed, what a gateway reported, what was configured. For a passive one it
 * answers with nothing, because it is describing somewhere to connect and there is
 * nowhere; the address is still ours to publish, so the same ranking is applied
 * here by hand.
 *
 * "0.0.0.0" is the last resort and is not a blank: ADC reads it as a request for
 * the hub to fill in the address it sees, which is how a client behind a NAT that
 * nothing has told it about learns where it is.
 */
std::string ADC::HubSession::announcedAddress(const QuickDC::PeerEndpoint& endpoint) const
{
	if (endpoint.active && !endpoint.address.empty()) return endpoint.address;

	if (!observedAddress.empty()) return observedAddress;

	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Network");
	const char* configured = config->getString("Address", "0.0.0.0");

	return (configured && *configured) ? configured : "0.0.0.0";
}

/*
 * Refuse a peer connection that would not be encrypted, and say so where a user
 * can see it: a transfer that never starts is otherwise indistinguishable from a
 * peer that is simply not answering.
 */
void ADC::HubSession::refuseUnencrypted(const char* from, const char* token,
                                        const char* proto, const char* why)
{
	QERR("Refusing an unencrypted peer connection: %s", why);

	if (listener)
	{
		const std::string msg = std::string("Refused an unencrypted transfer: ") + why;
		listener->EventSystemError(msg.c_str());
	}

	sendPeerError(from, adc_status_code(ADC_STATUS_SEVERITY_FATAL,
			ADC_CODE_PROTOCOL_UNSUPPORTED).c_str(),
		"Encryption is required", token, proto);
}


ADC::HubSession::HubSession(QuickDC::Hub* ui, QuickDC::HubListener* listener_, Samurai::IO::Net::URL* url, bool secure_)
	: QuickDC::HubSession(ui)
	, QuickDC::Connection(url, secure_ ? "ADCS hub" : "ADC hub")
	, proxy(0)
	, listener(listener_)
	, infoTimer(0)
	, secure(secure_)
	, keyprintVerified(false)
{
	access = QuickDC::HubSession::AccessNormal;
	userState = QuickDC::HubSession::StateNone;

	/*
	 * KEYP: a keyprint in the hub address is the one that means something. It
	 * reached us with the address rather than over the connection it protects,
	 * so it is not something an attacker in the middle can have written.
	 */
	std::string keyprint;
	if (QuickDC::Keyprint::fromQuery(url->getQuery(), keyprint))
		expectedKeyprint = keyprint;

	proxy = new ADC::CommandProxy(socket, this);
	users = new QuickDC::UserManager(this);

	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;

	/* Borrowed from the core, and shared with every other hub session; see
	   Core::session. Not ours to delete, and its group is not ours to leave set. */
	session = QuickDC::Core::getInstance()->session;

	address = url->toString();
	config->setGroup("User");
	session->setGroup(address);

	/*
	 * Load settings from session file, or defaults if no session is found.
	 * This will incidentally create such a session.
	 */
	nickname = session->getString("Nickname", config->getString("Nickname", PRODUCT));
	description = session->getString("Description", config->getString("Description", PRODUCT " user"));
	email = session->getString("E-mail", config->getString("E-mail", ""));
	password = session->getString("Password", config->getString("Password", ""));

	/* A hub will never accept an empty nick, and older versions could store one
	 * in the session file. Fall back to the configured nick in that case. */
	if (nickname.empty())
	{
		const char* fallback = config->getString("Nickname", PRODUCT);
		nickname = *fallback ? fallback : PRODUCT;
		session->setString("Nickname", nickname);
	}

	session->setTime("Last Connect", time(0));

	session->write();
}


ADC::HubSession::~HubSession()
{
	// save session data.
	/* The group is shared, and every other hub session moves it, so it is
	   selected here rather than relied on from the constructor. */
	session->setGroup(address);
	session->setString("Nickname", nickname);
	session->setString("Password", password);
	session->write();

	QDBG("~HubSession()");

	delete users;
	delete infoTimer;
	delete proxy;
}


void ADC::HubSession::EventHostLookup(const Samurai::IO::Net::Socket*)
{
	listener->EventNetStatus(QuickDC::HubListener::NetLookup);
}


void ADC::HubSession::EventHostFound(const Samurai::IO::Net::Socket*)
{
	/* ignore */
}


void ADC::HubSession::EventConnecting(const Samurai::IO::Net::Socket*)
{
	listener->EventNetStatus(QuickDC::HubListener::NetConnecting);
}


void ADC::HubSession::send(ADC::Command* cmd, bool more)
{
	proxy->send(cmd);

	if (!more)
	{
		proxy->write();
	}

	resetIdleTimer();
}


void ADC::HubSession::EventConnected(const Samurai::IO::Net::Socket*)
{
	if (!secure)
	{
		manager->add(this);
		userState = QuickDC::HubSession::StateProtocol;
		ADC::Command* cmd = new ADC::Command(FOURCC('H','S','U','P'));
		ADC::addSupportFlags(*cmd, false);
		send(cmd);

		listener->EventNetStatus(QuickDC::HubListener::NetConnected);
	}
	else
	{
		listener->EventNetStatus(QuickDC::HubListener::NetConnected);
		resetIdleTimer();

		/*
		 * ADCS authenticates a hub by keyprint, not by PKI: hubs are named by
		 * address and sign their own certificates, so a trust store check would
		 * refuse nearly all of them during the handshake - before there is a
		 * certificate for verifyKeyprint() to look at. The keyprint is the whole
		 * of the authentication here.
		 */
		socket->TLSsetAllowUntrusted(true);
		socket->TLSInitialize(false);
		socket->TLSsendHandshake();
	}
}


/**
 * KEYP: compare the certificate the hub just presented against the keyprint we
 * expect it to have. The handshake is finished but nothing has been written
 * yet, which is the one point where dropping the connection costs nothing.
 *
 * A keyprint that came with the hub address arrived out of band, so matching it
 * really does authenticate the hub. A stored one only says the hub is showing
 * the same certificate as last time; that catches a substitution, but says
 * nothing at all about the first time we came here.
 */
bool ADC::HubSession::verifyKeyprint()
{
	std::string actual;
	bool presented = QuickDC::Keyprint::ofPeer(socket.get(), actual);

	std::string trusted;
	const char* source = 0;

	/*
	 * A keyprint is filed under the canonical "{scheme}://{host}:{port}", while
	 * everything else about this session lives under the address as it was typed.
	 * Each access below selects the group it wants; nothing is restored, because
	 * the store is shared with every other hub session and the group that was
	 * current on entry is not necessarily ours to put back.
	 */
	if (expectedKeyprint)
	{
		trusted = *expectedKeyprint;
		source = "the hub address";
	}
	else
	{
		session->setGroup(keyprintGroup());
		if (session->haveSetting("Keyprint"))
		{
			trusted = session->getString("Keyprint");
			if (!trusted.empty()) source = "the last session with this hub";
		}
	}

	if (source && !presented)
	{
		QERR("The hub presented no certificate to check against the keyprint from %s", source);
		reportKeyprintFailure("The hub presented no certificate to check");
		return false;
	}

	if (source && !QuickDC::Keyprint::equals(trusted.c_str(), actual.c_str()))
	{
		/*
		 * The certificate we were actually given is deliberately not reported:
		 * it is the mismatch that matters, and repeating an attacker's keyprint
		 * back at the user only invites them to trust it.
		 */
		QERR("The hub's certificate does not match the keyprint from %s", source);
		reportKeyprintFailure("The hub's certificate has changed since the last connection. "
			"If the hub did not change its certificate, this is a "
			"man-in-the-middle attack. Remove the stored keyprint to accept the new one.");
		return false;
	}

	if (source)
	{
		QDBG("The hub's certificate matches the keyprint from %s", source);
		keyprintVerified = true;
	}

	/*
	 * Either it matched or there was nothing to match it against. Keep it
	 * whichever it was, so that a substitution shows up next time even if the
	 * address is given without a keyprint then.
	 */
	if (presented)
	{
		session->setGroup(keyprintGroup());
		session->setString("Keyprint", actual);
		session->write();
	}


	/*
	 * Say so where the user can see it, not only in the log: this is the one
	 * connection to this hub that nothing could vouch for, and every later one
	 * is checked against what was just stored.
	 */
	if (!source)
	{
		if (presented)
			listener->EventSystemError("Accepting and remembering this hub's certificate on first use");
		else
			listener->EventSystemError("This hub presented no certificate; the connection is unauthenticated");
	}

	return true;
}


/**
 * Where a remembered keyprint lives. The session group is named after the
 * address as it was typed, which for these purposes is too specific: the same
 * hub with and without a 'kp' parameter is still the same certificate, so the
 * canonical "{scheme}://{host}:{port}" is what it is filed under.
 */
const char* ADC::HubSession::keyprintGroup() const
{
	if (hub && hub->getUrl()) return hub->getUrl();
	return address.c_str();
}


void ADC::HubSession::reportKeyprintFailure(const char* msg)
{
	std::string text("Crypto error: ");
	text += msg;

	listener->EventSystemError(text.c_str());
	listener->EventNetStatus(QuickDC::HubListener::NetTlsError);

	/*
	 * Never come back on our own. This may well be a real attack that has just
	 * been foiled, and reconnecting in a loop is how such an attack eventually
	 * gets through.
	 */
	reconnect_time = -1;
	userState = QuickDC::HubSession::StateNone;
	socket->disconnect();
}


void ADC::HubSession::EventTLSConnected(const Samurai::IO::Net::Socket*)
{
	if (!verifyKeyprint()) return;

	listener->EventNetStatus(QuickDC::HubListener::NetTlsConnected);
	if (secure)
	{
		manager->add(this);
		userState = QuickDC::HubSession::StateProtocol;
		ADC::Command* cmd = new ADC::Command(FOURCC('H','S','U','P'));
		ADC::addSupportFlags(*cmd, false);
		send(cmd);
		listener->EventNetStatus(QuickDC::HubListener::NetConnected);
	} else {
		// Renegotiated to secure mode after initial connection.
	}
}

void ADC::HubSession::EventTLSDisconnected(const Samurai::IO::Net::Socket*)
{
	listener->EventNetStatus(QuickDC::HubListener::NetTlsDisconnected);
}



void ADC::HubSession::EventTimeout(const Samurai::IO::Net::Socket*)
{
	manager->remove(this);
	userState = QuickDC::HubSession::StateNone;
	listener->EventNetStatus(QuickDC::HubListener::NetErrorTimeout);
	hub->onDisconnected(QuickDC::HubListener::NetErrorTimeout);
	delete infoTimer; infoTimer = 0;
}


void ADC::HubSession::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	userState = QuickDC::HubSession::StateNone;
	listener->EventNetStatus(QuickDC::HubListener::NetDisconnected);
	manager->remove(this);
	hub->onDisconnected(QuickDC::HubListener::NetDisconnected);
	delete infoTimer; infoTimer = 0;
}


void ADC::HubSession::EventDataAvailable(const Samurai::IO::Net::Socket*)
{
	proxy->read();
	resetIdleTimer();
}


void ADC::HubSession::EventCanWrite(const Samurai::IO::Net::Socket*)
{
	proxy->write();
	resetIdleTimer();
}


void ADC::HubSession::EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError error, const char* msg)
{
	manager->remove(this);
	userState = QuickDC::HubSession::StateNone;
	QuickDC::HubListener::StatusNetwork why = QuickDC::HubListener::NetErrorIO;

	switch (error)
	{
		case Samurai::IO::Net::SocketError::ConnectionTimeout:
			why = QuickDC::HubListener::NetErrorTimeout;
			break;
		case Samurai::IO::Net::SocketError::ConnectionRefused:
			why = QuickDC::HubListener::NetErrorConnectionRefused;
			break;
		case Samurai::IO::Net::SocketError::HostNotFound:
			why = QuickDC::HubListener::NetErrorHostNotFound;
			break;
		case Samurai::IO::Net::SocketError::HostUnreachable:
		case Samurai::IO::Net::SocketError::NetUnreachable:
		case Samurai::IO::Net::SocketError::HostDown:
		case Samurai::IO::Net::SocketError::NetDown:
			/* Apart from the rest, because it is retried differently. */
			why = QuickDC::HubListener::NetErrorHostUnreachable;
			break;
		default:
			QERR("Network error %d on '%s': %s", static_cast<int>(error),
				hub ? hub->getUrl() : "", msg ? msg : "");
	}

	listener->EventNetStatus(why);
	hub->onDisconnected(why);
	delete infoTimer; infoTimer = 0;
}


void ADC::HubSession::connect()
{
	reconnect_time = 60;

	/*
	 * A keyprint we cannot use must not be quietly treated as no keyprint at
	 * all. That is precisely the case where the user believes the connection is
	 * authenticated when nothing is checking it.
	 */
	if (expectedKeyprint && !QuickDC::Keyprint::isValid(expectedKeyprint->c_str()))
	{
		QERR("The hub address carries a keyprint we cannot use: '%s'", expectedKeyprint->c_str());
		listener->EventSystemError("The 'kp' in the hub address is not a SHA256 keyprint; not connecting");
		return;
	}

	if (expectedKeyprint && !secure)
	{
		QERR("The hub address carries a keyprint, but names no encrypted protocol");
		listener->EventSystemError("A keyprint can only be checked over ADCS; use an adcs:// address");
		return;
	}

	proxy->connect();
}


void ADC::HubSession::disconnect()
{
	userState = QuickDC::HubSession::StateNone;
	proxy->close();
	delete infoTimer; infoTimer = 0;
}


bool ADC::HubSession::isLoggedIn() const
{
	return userState == QuickDC::HubSession::StateNormal;
}


bool ADC::HubSession::isSecure() const
{
	return socket && socket->isSecure();
}


void ADC::HubSession::EventUserJoin(const char* nick)
{
	listener->EventUserJoin(nick);
}


void ADC::HubSession::EventUserQuit(const QuickDC::User* data, const char* msg)
{
	listener->EventUserLeave(data, msg);
}


void ADC::HubSession::EventUserInfo(const QuickDC::User* data)
{
	if (!sid.empty() && sid == data->getID())
	{
		if (userState != QuickDC::HubSession::StateNormal)
		{
			if (userState == QuickDC::HubSession::StateVerify)
			{
				if (data->isOp())
				{
					access = QuickDC::HubSession::AccessOperator;
				}
				else
				{
					access = QuickDC::HubSession::AccessRegistered;
				}
			}
			else
			{
				access = QuickDC::HubSession::AccessNormal;
			}
			userState = QuickDC::HubSession::StateNormal;
			listener->EventHubStatus(QuickDC::HubListener::HubLoggedIn);
			/* One more hub we are on, which the others were told a count of. */
			QuickDC::Core::getInstance()->hubs->requestClientInfoUpdate();
		}
	}
	listener->EventUserUpdate(data);
}


void ADC::HubSession::resetIdleTimer()
{
	delete infoTimer;
	infoTimer = new Samurai::Timer(this, 300, false);
}


void ADC::HubSession::EventTimeout(Samurai::Timer* /* timer */)
{
	ADC::Command* empty = new ADC::Command();
	send(empty);
}


void ADC::HubSession::EventSearchReply(QuickDC::Share::SearchReply* reply)
{
	QuickDC::Share::SearchRequest* req = reply->getRequest();
	const QuickDC::User* user = reply->getRequest()->getUser();
	QuickDC::Share::File* file = reply->getFile();

	if (user->getAddressIPv4() && user->getPortIPv4() && QuickDC::Core::getInstance()->server)
	{
		/* Send UDP packet */

		ADC::Command cmd(FOURCC('U','R','E','S'), ADC::CID::getInstance()->getCID());
		cmd.addArgument("FN", file->getPublicName());
		cmd.addArgument("SI", std::to_string(file->getSize()));
		cmd.addArgument("TR", file->getTTH());
		cmd.addArgument("TD", "0");
		cmd.addArgument("TO", req->getToken());
		// cmd.addArgument("SL", "3"); // FIXME: implement me

		Samurai::IO::Buffer buffer;
		cmd.write(&buffer);
		Samurai::IO::Net::DatagramPacket packet(&buffer);
		Samurai::IO::Net::InetSocketAddress peerAddr(user->getAddressIPv4(), user->getPortIPv4(), Samurai::IO::Net::InetAddress::Version::IPv4);
		packet.setAddress(&peerAddr);
		QuickDC::Core::getInstance()->server->send(&packet);

	}
	else
	{
		/* Send result via server */
		ADC::Command* cmd = new ADC::Command(FOURCC('D','R','E','S'), localSid(), user->getID());
		cmd->addArgument("FN", file->getPublicName());
		cmd->addArgument("SI", std::to_string(file->getSize()));
		cmd->addArgument("TR", file->getTTH());
		cmd->addArgument("TD", "0");
		cmd->addArgument("TO", req->getToken());
		// cmd.addArgument("SL", "3"); // FIXME: implement me

		send(cmd);
	}
}


/**
 * A search reply came back. Two things want to know: the download queue, for
 * which this may be a new source, and the user interface.
 *
 * Takes ownership of 'result'.
 */
void ADC::HubSession::onSearchResult(QuickDC::Share::SearchResult* result)
{
	if (!result) return;

	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	const char* hubUrl = (hub) ? hub->getUrl() : 0;

	QDBG("Search result: '%s' (%d bytes) from '%s'",
		result->getFile() ? result->getFile() : "",
		static_cast<int>(result->getSize()),
		result->getUser() ? result->getUser()->getNick() : "");

	/*
	 * Match on the TTH rather than the name: the whole point of queueing by
	 * hash is that the same file can be called different things elsewhere.
	 */
	if (queue && hubUrl && result->getTTH() && result->getUser())
	{
		QuickDC::QueueFile* qfile = queue->getFileByTTH(result->getTTH());
		const char* cid = reinterpret_cast<const char*>(result->getUser()->getClientID());

		if (qfile && cid)
		{
			if (!qfile->getSize() && result->getSize())
				qfile->setSize(result->getSize());

			if (!qfile->findSource(hubUrl, cid))
			{
				/* adc://host:port/{cid}/TTH:{root} - see filesource.h */
				std::string name("TTH:");
				name += result->getTTH();

				qfile->addSource(new QuickDC::FileSource(hubUrl, cid, name.c_str()));
				QDBG("A search reply gave us a new source for '%s'", qfile->getLocalFilename());
			}
		}
	}

	if (listener) listener->EventSearchResult(result);

	delete result;
}

bool ADC::HubSession::knowUser(const char* id)
{
	return (users->getUser(id) != 0);
}


const QuickDC::User* ADC::HubSession::getUser(const char* id)
{
	return users->getUser(id);
}


const QuickDC::User* ADC::HubSession::getLocalUser()
{
	return users->getUser(localSid());
}


QuickDC::UserManager* ADC::HubSession::getUserManager()
{
	return users;
}


/* API wise */
void ADC::HubSession::sendPrivateChatMessage(const QuickDC::User* user, const char* msg,
                                            bool richText)
{
#ifdef DATADUMP
	QDBG("Sending message to '%s', msg='%s'", user->getNick(), msg);
#endif
	ADC::Command* cmd = new ADC::Command(FOURCC('E','M','S','G'), localSid(), user->getID());
	cmd->addArgument(msg);
	cmd->addArgument("PM", localSid());

	/* Only flagged as markdown if the hub announced the extension in its SUP;
	   anywhere else the message goes out as plain text, which still reads fine. */
	if (richText && supportsRichText()) cmd->addArgument("RT", "1");
	send(cmd);
}


void ADC::HubSession::sendChatMessage(const char* msg, bool richText)
{
	ADC::Command* cmd = new ADC::Command(FOURCC('B','M','S','G'), localSid());
	cmd->addArgument(msg);

	/*
	 * Says the text is markdown. Left off an unformatted line on purpose, so that
	 * somebody typing *asterisks* is quoted rather than emphasised at the far end.
	 *
	 * Also left off unless the hub announced the extension in its SUP: a hub that
	 * knows nothing of the flag would relay it to clients that cannot read it, so
	 * the message goes out as plain text instead.
	 */
	if (richText && supportsRichText()) cmd->addArgument("RT", "1");

	send(cmd);
}


void ADC::HubSession::sendAdminUserKick(const QuickDC::User* user)
{
	ADC::Command* cmd = new ADC::Command(FOURCC('H','D','S','C'));
	cmd->addArgument(user->getID());
	send(cmd);
}


void ADC::HubSession::sendAdminUserRedirect(const QuickDC::User* user, const char*, const char*)
{
	ADC::Command* cmd = new ADC::Command(FOURCC('H','D','S','C'));
	cmd->addArgument(user->getID());
	send(cmd);
}


void ADC::HubSession::sendClientInfo()
{
	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Network");

	ADC::Command* cmd = new ADC::Command(FOURCC('B','I','N','F'), localSid());
	cmd->addArgument("NI", nickname);
	cmd->addArgument("SL", std::to_string(config->getNumber("Slots", 3)));
	cmd->addArgument("SS", std::to_string(QuickDC::Core::getInstance()->shares->getSharedSize()));
	cmd->addArgument("SF", std::to_string(QuickDC::Core::getInstance()->shares->getFiles()));
	cmd->addArgument("DE", description);
	cmd->addArgument("EM", email);
	cmd->addArgument("VE", PRODUCT " " VERSION);
	cmd->addArgument("ID", ADC::CID::getInstance()->getCID());
	cmd->addArgument("PD", ADC::CID::getInstance()->getPID());
	cmd->addArgument("HN", std::to_string(QuickDC::Core::getInstance()->hubs->countHubsNormal()));
	cmd->addArgument("HR", std::to_string(QuickDC::Core::getInstance()->hubs->countHubsRegistered()));
	cmd->addArgument("HO", std::to_string(QuickDC::Core::getInstance()->hubs->countHubsOperator()));

	/*
	 * KEYP: publish what our own certificate hashes to, so a peer dialling us
	 * over ADCS can tell that it reached us and not someone in between.
	 */
	std::string keyprint;
	if (QuickDC::Keyprint::ofOwnCertificate(keyprint))
		cmd->addArgument("KP", keyprint.c_str());

	config->setGroup("Network");

	/*
	 * Where a peer should be told to reach us, which is the forwarded port when
	 * there is one rather than the port that was configured: a gateway may have
	 * granted a different one, and it is the granted one anything can reach.
	 */
	const QuickDC::Core* core = QuickDC::Core::getInstance();
	const QuickDC::PeerEndpoint endpoint = QuickDC::peerEndpoint(
		config->getBool("Active mode"), core->server != 0,
		config->getString("Address", "0.0.0.0"),
		observedAddress,
		static_cast<uint16_t>(config->getNumber("Port")),
		core->portforward);

	QDBG("Active mode %d (server=%p)\n", endpoint.active ? 1 : 0, core->server);

	/* Evidence belongs to the endpoint it was gathered for; announcing a different
	   one discards what was known about the old. */
	if (core->reachability && endpoint.active)
		core->reachability->advertising(endpoint.address, endpoint.port);

	/*
	 * What we can do, as the other clients see it. Built up rather than written out
	 * twice: the only difference between the two cases is whether anyone can connect
	 * to us, and a feature added to one list and forgotten in the other is a bug that
	 * only shows in passive mode.
	 */
	std::string supports = "ADC0";

	/*
	 * The address goes out whether or not anyone can connect to it.
	 *
	 * It used to be sent only by an active client, which reads as the private thing
	 * to do and is why it was that way - but NAT traversal cannot work without it.
	 * Two passive clients reach each other by dialling at once, and neither can dial
	 * an address the hub never passed on; the specification says as much, requiring
	 * the hub to forward I4 for both of them.
	 *
	 * So this is a deliberate trade rather than an oversight: a passive user's
	 * address becomes visible to the other clients on the hub - the hub itself has
	 * always seen it - in exchange for being able to transfer with the other passive
	 * users at all. Without it a passive pair has no path between them.
	 *
	 * Nothing else changes: no U4, and neither TCP4 nor UDP4, so a peer still reads
	 * us as somebody to be invited rather than somebody to connect to.
	 */
	cmd->addArgument("I4", announcedAddress(endpoint));

	if (endpoint.active)
	{
		cmd->addArgument("U4", std::to_string(endpoint.port));

		/*
		 * TCP4 is a claim other clients act on: it says a peer told to connect
		 * here will get through. Withdrawn once enough peers have reported that
		 * they could not, which is the only way this client finds out - and
		 * withdrawing it matters beyond honesty, because NATT engages only when
		 * neither side claims TCP4, so a false claim suppresses the fallback that
		 * would have worked.
		 *
		 * Withheld only when disproven, never merely unproven: a client that waited
		 * for proof would announce passive on every fresh start and stay that way
		 * until somebody happened to connect, which nothing would prompt.
		 *
		 * The evidence can be overruled. Someone who knows their own network is
		 * entitled to say so, and a peer that lies about failing to connect must
		 * not be the last word.
		 */
		if (reachable(config))
			supports += ",TCP4";

		/*
		 * UDP4 is not withdrawn, because nothing can disprove it: a peer that
		 * cannot reach this port has no datagram it could send to say so. Claimed
		 * whenever the port is open, as before.
		 */
		supports += ",UDP4";
	}

	/*
	 * NATT, whether active or not: it says this client can reach a peer that cannot
	 * be connected to, which is worth announcing either way - a peer only acts on it
	 * when neither side claims TCP4.
	 *
	 * Claimed only when it could work. It rests on dialling from this connection's
	 * own local port, so a connection with no port to read - or one through a proxy,
	 * where the port belongs to the proxy - cannot do it, and saying otherwise would
	 * send a peer to something that never answers.
	 */
	if (canTraverse())
	{
		supports += ",";
		supports += ADC_EXT_NATT;
	}

	supports += ",";
	supports += ADC_EXT_RICHTEXT;

	cmd->addArgument("SU", supports);
	send(cmd);
}


void ADC::HubSession::sendSearchRequest(QuickDC::Share::SearchRequest* request)
{
	if (!request) return;

	ADC::Command* cmd = new ADC::Command(FOURCC('B','S','C','H'), localSid());

	if (request->isTTH())
	{
		cmd->addArgument("TR", request->getTTH());
	}
	else
	{

		for (const std::string& term : request->getInclusions())
			cmd->addArgument("AN", term.c_str());

		for (const std::string& term : request->getExclusions())
			cmd->addArgument("NO", term.c_str());

		for (const std::string& ext : request->getExtensions())
			cmd->addArgument("EX", ext.c_str());

		switch (request->getSizePolicy())
		{
			case QuickDC::Share::SearchRequest::SizeMin:
				cmd->addArgument("GE");
				break;

			case QuickDC::Share::SearchRequest::SizeMax:
				cmd->addArgument("LE");
				break;

			case QuickDC::Share::SearchRequest::SizeExact:
				cmd->addArgument("EQ");
				break;

			case QuickDC::Share::SearchRequest::SizeAny:
				/* do nothing */
				break;
		}

		switch (request->getFileType())
		{
			case QuickDC::Share::SearchRequest::SearchFiles:
				cmd->addArgument("TY", "1");
				break;

			case QuickDC::Share::SearchRequest::SearchDirectories:
				cmd->addArgument("TY", "2");
				break;

			case QuickDC::Share::SearchRequest::SearchAll:
				/* do nothing */
				break;
		}
	}

	if (request->getToken()) cmd->addArgument("TO", request->getToken());
	send(cmd);
}


void ADC::HubSession::sendConnectionRequest(const QuickDC::User* user, const char* token)
{
	if (!user) return;
	if (!token) return;

	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Network");

	/*
	 * Which protocol to name depends on who is going to dial whom:
	 *
	 *  - CTM tells the peer to connect to us, so we have to be able to accept
	 *    whatever we name - see canAcceptTls().
	 *
	 *  - RCM asks the peer to send us a CTM, so we dial out, and outbound TLS
	 *    is handled by ADC::ClientSession.
	 *
	 * Either way encryption is only volunteered on a hub we reached over ADCS,
	 * which is also the only case where the keyprint that protects the peer
	 * connection could have come from anywhere trustworthy.
	 */
	/*
	 * The same endpoint the INF advertised, rather than the configured port and a
	 * second opinion about whether we are active.
	 *
	 * A CTM naming config's port sent a peer to the port we asked the gateway for,
	 * which is not always the one it granted - PortForward says so when it happens
	 * - so under UPnP a peer was told a port with nothing behind it. The NMDC side
	 * has always asked peerEndpoint(); this is the odd one out.
	 *
	 * And deciding 'active' here from the preference and a socket meant this and
	 * the INF could disagree: a client that told the hub it was passive could still
	 * answer with a CTM.
	 */
	const QuickDC::Core* core = QuickDC::Core::getInstance();
	const QuickDC::PeerEndpoint endpoint = QuickDC::peerEndpoint(
		config->getBool("Active mode"), core->server != 0,
		config->getString("Address", "0.0.0.0"),
		observedAddress,
		static_cast<uint16_t>(config->getNumber("Port")),
		core->portforward);

	if (endpoint.active)
	{
		QDBG("Requesting active connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','C','T','M'), localSid(), user->getID());
		cmd->addArgument((secure && canAcceptTls()) ? ADC_TLS_PROTO : ADC_COMPLIANCE);
		cmd->addArgument(std::to_string(endpoint.port).c_str());
		cmd->addArgument(token);
		send(cmd);

		/* Add to securitymanager */
		QuickDC::Clearance* clearance = new QuickDC::Clearance(
				QuickDC::Clearance::AuthorizedUser,
				QuickDC::Clearance::ADC,
				this,
				reinterpret_cast<const char*>(user->getClientID()),
				token);
		QuickDC::Core::getInstance()->securityManager->grant(clearance);

	}
	else
	{
		QDBG("Requesting passive connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','R','C','M'), localSid(), user->getID());

		cmd->addArgument(secure ? ADC_TLS_PROTO : ADC_COMPLIANCE);
		cmd->addArgument(token);

		/* Add to securitymanager */
		QuickDC::Clearance* clearance = new QuickDC::Clearance(
				QuickDC::Clearance::AuthorizedUser,
				QuickDC::Clearance::ADC,
				this,
				reinterpret_cast<const char*>(user->getClientID()),
				token);
		QuickDC::Core::getInstance()->securityManager->grant(clearance);
		send(cmd);
	}
}


void ADC::HubSession::onSID(char* sid_)
{
	if (userState != QuickDC::HubSession::StateProtocol)
	{
		QERR("Unexpected state transition. Current state=%d. Ignoring.",  userState);
		return;
	}

	sid = sid_;
	sendClientInfo();
	userState = QuickDC::HubSession::StateIdentify;
}


void ADC::HubSession::onUserInfo(const char* id, QuickDC::User* data, uint64_t oldShare, bool changed)
{
	switch (userState) {
		case QuickDC::HubSession::StateNone:
		case QuickDC::HubSession::StateProtocol:
		case QuickDC::HubSession::StateData:
			return;

		case QuickDC::HubSession::StateIdentify:
		case QuickDC::HubSession::StateVerify:
			userState = QuickDC::HubSession::StateNormal;

			/* This is the transition, so this is where it gets reported.
			   EventUserInfo() also tries to, and cannot: it is reached through
			   users->info() below, by which time the state it tests has already
			   been moved. */
			listener->EventHubStatus(QuickDC::HubListener::HubLoggedIn);
			/* One more hub we are on, which the others were told a count of. */
			QuickDC::Core::getInstance()->hubs->requestClientInfoUpdate();
			break;

		case QuickDC::HubSession::StateNormal:
			break;
	}
	users->info(id, data, oldShare, changed);
}


/*
 * Whether the salt a hub offered is one worth answering.
 *
 * The salt is the hub's contribution to the answer, and everything that makes the
 * answer worth more than the password itself rests on it - which is awkward,
 * because a hostile hub is precisely the case that matters. Two things can be
 * checked from here.
 *
 * Its length: a short salt leaves the answer a function of the password alone. A
 * single base32 character decodes to no bytes at all, which makes the answer the
 * same twenty-six characters for ever, replayable by anyone who ever saw one.
 *
 * And whether this hub has used it before. The salt this hub last asked with is
 * remembered against the hub, not against the connection, because the exchange
 * happens once per connection: a hub reusing a salt does it on the next login
 * rather than twice on this one, and the answer whoever was listening captured
 * last time is the answer it would get again.
 */
bool ADC::HubSession::saltIsUsable(const std::string& salt)
{
	const size_t bytes = (salt.size() * 5) >> 3;

	if (bytes < QUICKDC_ADC_SALT_MIN_BYTES)
	{
		QERR("The hub offered a %zu byte salt, and %d is the least that makes an "
			"answer worth sending", bytes, QUICKDC_ADC_SALT_MIN_BYTES);
		if (listener)
			listener->EventSystemError("This hub asked for a password with too "
				"little randomness to protect it - not logging in");
		return false;
	}

	session->setGroup(keyprintGroup());

	if (session->getString(QUICKDC_ADC_SALT_SETTING, "") == salt)
	{
		QERR("The hub offered the salt it last asked with; refusing to answer it twice");
		if (listener)
			listener->EventSystemError("This hub asked for a password with the "
				"same randomness as last time, which would make the answer "
				"replayable - not logging in");
		return false;
	}

	session->setString(QUICKDC_ADC_SALT_SETTING, salt);
	session->write();

	/*
	 * And the answer crosses whatever the session runs over. On a plain hub that
	 * is the clear, where the answer is not the password but is enough to try
	 * every candidate password against offline, at whatever rate the attacker
	 * likes. Said rather than refused: this is the part the user can change, by
	 * using a hub that offers ADCS.
	 */
	if (!secure)
	{
		QERR("Answering a password request over an unencrypted connection to '%s'",
			address.c_str());
		if (listener)
			listener->EventSystemError("This hub is not encrypted, so the answer "
				"to its password request can be captured and attacked offline");
	}

	return true;
}

void ADC::HubSession::onRequestPasword(char* token)
{
	if (userState != QuickDC::HubSession::StateIdentify)
	{
		QERR("Unexpected state transition. Current state=%d. Ignoring.",  userState);
		return;
	}

	userState = QuickDC::HubSession::StateVerify;

	if (!token) return;

	if (!saltIsUsable(token)) return;

	passwordToken = token;

	/*
	 * Hashing the token with an empty password produces an answer the hub will
	 * refuse, but it is still sent: waiting for the interface to supply one
	 * would turn a login that fails immediately into one that hangs. The token
	 * is kept, so a password supplied while the session is still here is sent
	 * as a second attempt.
	 */
	if (password.empty())
		listener->EventHubAutenticate();

	sendPasswordResponse();
}

void ADC::HubSession::sendPassword(const char* password_)
{
	/* Only meaningful in answer to a request, and only derivable while the
	   token that came with it is still around. */
	if (userState != QuickDC::HubSession::StateVerify || passwordToken.empty()) return;
	if (!password_) return;

	password = password_;

	/*
	 * Written out now rather than left to the destructor, so a password the
	 * user just supplied survives a crash or a kill. The group is shared and
	 * every other session moves it, so it is selected here.
	 */
	session->setGroup(address);
	session->setString("Password", password);
	session->write();

	sendPasswordResponse();
}

/* HPAS carries the tiger digest of the password followed by the hub's token. */
void ADC::HubSession::sendPasswordResponse()
{
	/* An empty password still gets an answer; see onRequestPasword(). */
	if (passwordToken.empty()) return;

	char password_encoded[40] = { 0, };

	size_t tok_size = (passwordToken.size() * 5) >> 3;
	size_t size = password.size() + tok_size;
	uint8_t* tok_decoded = new uint8_t[tok_size ? tok_size : 1];
	uint8_t* str = new uint8_t[size ? size : 1];

	Samurai::Util::base32_decode(passwordToken, std::span<unsigned char>(tok_decoded, tok_size));

	memcpy(&str[0], password.data(), password.size());
	memcpy(&str[password.size()], tok_decoded, tok_size);

	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(str, size);
	tiger.digest().getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, password_encoded, 40);

	ADC::Command* cmd = new ADC::Command(FOURCC('H','P','A','S'));
	cmd->addArgument(password_encoded);
	send(cmd);

	delete[] tok_decoded;
	delete[] str;
}


void ADC::HubSession::addSupport(const char* feature)
{
	extensions.emplace(feature);
}


void ADC::HubSession::onSupports()
{
	if (!listener)
		return;

	for (const auto& feature : extensions)
		listener->EventHubExtension(feature.c_str());
}


bool ADC::HubSession::supports(const char* feature) const
{
	return extensions.contains(feature);
}


bool ADC::HubSession::supportsRichText() const
{
	return supports(ADC_EXT_RICHTEXT);
}


void ADC::HubSession::onHubName(char* txt)
{
	hubname = txt;

	/* As the NMDC session does. Without this the name is only ever used as a
	   label on hub messages, and the interface never learns it. */
	listener->EventHubName(hubname.c_str());
}


void ADC::HubSession::onHubDesc(char* txt)
{
	hubdesc = txt;
}


void ADC::HubSession::onHubVersion(char* txt)
{
	hubversion = txt;
}


void ADC::HubSession::onStatus(ADC::Command* cmd)
{
	char* code = cmd->getArgument(0);
	char* mesg = cmd->getArgument(1);

	if (code && strlen(code) == 3)
	{
		/*
		 * Determine severity. It decides only whether there is anything to
		 * report: each code below sets its own reconnect time and listener
		 * event, so recoverable and fatal are already told apart there.
		 */
		switch (code[0]) {
			case '1': // Recoverable
			case '2': // Fatal
				break;
			case '0':
				// Informal messages -- We don't care about those!
				return;
			default:
				QERR("Invalid status code: %s (unknown severity, expected 0-2)", code);
				return;
		}

		switch (Samurai::Util::Convert::to_int32(&code[1]))
		{
			case ADC_STATUS_DOMAIN_GENERIC: // Ignored
				return;

			case ADC_CODE_HUB_ERROR:
				QERR("Generic hub error: '%s'", mesg);
				break;

			case ADC_CODE_HUB_FULL:
				QERR("Hub is full");
				listener->EventHubStatus(QuickDC::HubListener::HubErrorFull);
				reconnect_time = 600; // try again in 10 minutes
				break;

			case ADC_CODE_HUB_DISABLED:
				QERR("Hub is disabled");
				listener->EventHubStatus(QuickDC::HubListener::HubErrorDisabled);
				reconnect_time = 7200; // try again in 2 hours
				break;

			case ADC_CODE_ACCESS_ERROR:
				QERR("Unable to log in. Unspecified reason. Message from server: '%s'", mesg);
				listener->EventHubStatus(QuickDC::HubListener::HubErrorLoginRefused);
				reconnect_time = 1800; // try again in 30 minutes
				break;

			case ADC_CODE_ACCESS_NICK_ERROR:
				QERR("Nick is invalid: '%s'", mesg);
				listener->EventHubStatus(QuickDC::HubListener::HubErrorNickNotAccepted);
				reconnect_time = -1; // don't try again
				break;

			case ADC_CODE_ACCESS_NICK_USED:
				QERR("Nickname is in use");
				listener->EventHubStatus(QuickDC::HubListener::HubErrorNickTaken);
				reconnect_time = 1800; // wait 30 minutes, and try again
				break;

			case ADC_CODE_ACCESS_PASSWORD:
				QERR("Password is invalid");
				listener->EventHubStatus(QuickDC::HubListener::HubErrorWrongPassword);
				reconnect_time = -1;
				break;

			case ADC_CODE_ACCESS_CID_USED:
				QERR("CID is in use");
				listener->EventHubStatus(QuickDC::HubListener::HubErrorCIDTaken);
				reconnect_time = 600; // wait 30 minutes, and try again
				break;

			case ADC_CODE_ACCESS_DENIED: /* Specify: 'FC' */
				QERR("Access denied for given command");
				break;

			case ADC_CODE_ACCESS_REGISTERED:
				QERR("Access restricted to registered users for given command.");
				break;

			case ADC_CODE_ACCESS_PID_ERROR:
				QERR("Invalid PID supplied");
				reconnect_time = -1;
				break;

			case ADC_CODE_SECURITY_GENERIC:
				QERR("Disconnected from hub: '%s'", mesg);
				break;

			case ADC_CODE_SECURITY_BAN_PERM:
				QERR("Permanently banned");
				reconnect_time = -1;
				listener->EventHubStatus(QuickDC::HubListener::HubBannedPermanently);
				break;

			case ADC_CODE_SECURITY_BAN_TEMP: /* Specify: 'TL' */
			{
				QERR("Temporary banned");
				if (cmd->haveArgument("TL", 2, 1))
				{
					char* arg = cmd->getArgument("TL", 2, 1);
					if (arg && strlen(arg)) {
						reconnect_time = Samurai::Util::Convert::to_int32(arg);
						if (reconnect_time == 0)
							reconnect_time = -1;
						else
							reconnect_time++;
					}
				}
				else
				{
					reconnect_time = -1;
				}
				listener->EventHubStatus(QuickDC::HubListener::HubBannedTemporarily);
				break;
			}
			case ADC_CODE_PROTOCOL_ERROR:
				QERR("Protocol error");
				break;

			case ADC_CODE_PROTOCOL_UNSUPPORTED: /* Specify: 'TO' (token), 'PR' (protocol). Context CTM/RCM */
				QERR("p2p connection protocol not supported");
				break;

			/*
			 * A peer we asked to connect is telling us it could not. That is a
			 * port check we did not have to build: a connect request is a
			 * prediction that a connection will arrive, and this is the prediction
			 * failing, reported by the one party in a position to know.
			 *
			 * Counted only for a token this client issued. Without that, anyone on
			 * the hub could quote a token of their own invention and argue us into
			 * being passive; a token from the queue is sixteen bytes of
			 * secureRandom, so it cannot be guessed. The reporter is named so that
			 * one peer repeating itself counts once - see Reachability.
			 */
			case ADC_CODE_PROTOCOL_CONNECT_ERROR: /* Specify: 'TO' (token), 'PR' (protocol). Context CTM/RCM */
			{
				const char* job = cmd->getArgument("TO", 2);
				const char* who = cmd->getSourceID();

				QuickDC::Core* core = QuickDC::Core::getInstance();
				const bool ours = job && *job && core && core->queue
					&& core->queue->lookupRequest(job);

				if (ours && who && *who && core->reachability)
					core->reachability->connectionRefused(who);
				else
					QDBG("A connect error for a job we did not ask for; ignoring it");

				QERR("p2p connect error");
				break;
			}

			case ADC_CODE_PROTOCOL_FLAG_REQUIRED: /* Specify: 'FL' (flag). Context: INF */
				/* Should not occur */
				QERR("INF flag missing");
				break;

			case ADC_CODE_PROTOCOL_STATE_INVALID: /* Specify: 'FC' (fourcc). */
				/* Should not occur. */
				QERR("State invalid");
				break;

			case ADC_CODE_PROTOCOL_FEATURE_ERROR: /* Specify: 'FC' (fourcc). Context: All feature casts */
				/* Should not occur. */
				QERR("Feature invalid/missing");
				reconnect_time = -1;
				break;


			/*
			 * The hub refused the address we claimed and says what it sees
			 * instead. That is the address peers will be told to connect to, so
			 * it is the one to use: taken, and the INF sent again so the hub has
			 * something it will accept.
			 *
			 * Only from the hub, and only for us - a 'D' status is another client
			 * talking, and no other client is in a position to say where we are.
			 */
			case ADC_CODE_PROTOCOL_IP_INF_ERROR: /* Specify: 'IP' (ip address). Context: INF */
			{
				/* ISTA is the hub speaking; DSTA is another client, and no other
				   client is in a position to say where we are. */
				const bool fromHub = cmd->getFourcc() == FOURCC('I','S','T','A');

				const char* seen = cmd->getArgument("IP", 2);
				if (!fromHub || !seen || !*seen)
				{
					QERR("Told our IP address is wrong, with no address to use");
					break;
				}

				if (observedAddress == seen) break;

				QERR("The hub sees us at %s, not at the address we claimed", seen);
				observedAddress = seen;

				/* Coalesced with everything else that changes what a hub was
				   told, so a hub correcting us does not race our own update. */
				QuickDC::Core::getInstance()->hubs->requestClientInfoUpdate();
				break;
			}


			// These can safely be ignored here. Only used for Client-Client transfers
			case ADC_CODE_TRANSFER_ERROR:
			case ADC_CODE_TRANSFER_FILE_ERROR: /* File not available */
			case ADC_CODE_TRANSFER_FILE_PART:  /* Part of file not available */
			case ADC_CODE_TRANSFER_SLOT_ERROR: /* No more slots */
				QERR("Weird p2p error from hub");
				return;

			default:
				QERR("Invalid error code from hub: '%s'. Message=%s", code, mesg);
		}
	}
	else
	{
		QERR("Invalid error code from hub: '%s'. Message=%s", code, mesg);
	}
}


void ADC::HubSession::onPublicChat(const char* from, const char* message, bool action,
                                  bool richText)
{
	const QuickDC::User* u = users->getUser(from);
	if (u)
	{
		listener->EventChat(u, message, action, richText);
	}
	else
	{
		listener->EventHubMessage(hubname.c_str(), message, action, richText);
	}
}


void ADC::HubSession::onPrivateChat(const char* to, const char* from, const char* message, const char* context, bool action, bool richText) {

	const QuickDC::User* u = users->getUser(from);
	const QuickDC::User* u2 = users->getUser(to);
	if (u && u2)
	{
		/*
		 * The conversation is with whichever of the two is not us, which is the
		 * only rule that holds in both directions.
		 *
		 * PM cannot be used for it, though it is what names the message private.
		 * A message is sent as EMSG, which the hub delivers to the target and
		 * echoes back to the sender, and we put our own SID in PM when we send
		 * one - so taking the context from PM files our own messages under a
		 * conversation with ourselves. Clients disagree about whose SID belongs
		 * there in any case.
		 */
		(void) context;
		const bool mine = !sid.empty() && sid == from;

		listener->EventPrivateChat(u, u2, message, mine ? to : from, action, richText);
	}
}


void ADC::HubSession::onActiveConnect(const char* from, const char* proto, uint16_t port, const char* token)
{
	if (userState != QuickDC::HubSession::StateNormal) return;

	if (!sid.empty() && sid == from) return;
	const QuickDC::User* user = users->getUser(from);
	if (!user) return;

	const char* ip = user->getAddressIPv4();
	QDBG("Requested to connect to '%s' at %s using '%s' on port %d (token=%s)", from, ip, proto, port, token);

	if (!ip)
	{
		QERR("No IP to connect to");
		sendPeerError(from, "242", "No IP given in INF", token, proto);
		return;
	}

	/* Add to securitymanager */
	QuickDC::Clearance* clearance = new QuickDC::Clearance(
			QuickDC::Clearance::AuthorizedUser,
			QuickDC::Clearance::ADC,
			this,
			reinterpret_cast<const char*>(user->getClientID()),
			token);
	QuickDC::Core::getInstance()->securityManager->grant(clearance);

	/*
	 * Use the port from the CTM, and only that.
	 *
	 * This used to be overridden with the user's INF 'U4' field, but U4 is the
	 * client-to-hub UDP port that search results are sent to - not a TCP port
	 * for peer connections. The two differ in general, and for ADCS they always
	 * do: a client listens for plain ADC on one port and for TLS on another, so
	 * the override sent our ClientHello to the plaintext port, which reads it as
	 * garbage and hangs up.
	 */
	if (!port)
	{
		QERR("No port given in the connect request");
		sendPeerError(from, adc_status_code(ADC_STATUS_SEVERITY_FATAL, ADC_CODE_PROTOCOL_CONNECT_ERROR).c_str(),
			"No port given", token, proto);
		return;
	}

	/*
	 * The CTM names the protocol the peer is listening with, and it is not
	 * necessarily the hub's: a plain ADC hub may still broker an ADCS peer
	 * connection, and vice versa. Anything else we cannot speak.
	 */
	const std::string_view protocol(proto ? proto : "");
	bool peerSecure = (protocol == ADC_TLS_PROTO);

	/*
	 * A plain connection is refused outright when encryption is required, and
	 * also when we ourselves asked for ADCS - our own request is a floor. The
	 * second case is the downgrade this closes: the protocol field arrives in a
	 * message the hub relayed, so accepting a weaker one than we named let
	 * whoever could write that field decide for us.
	 */
	if (!peerSecure && protocol == ADC_COMPLIANCE)
	{
		if (requireEncryptedTransfers())
		{
			refuseUnencrypted(from, token, proto,
				"the peer answered with plain ADC");
			return;
		}

		if (secure && canAcceptTls())
		{
			refuseUnencrypted(from, token, proto,
				"we asked for ADCS and the peer answered with plain ADC");
			return;
		}
	}

	if (!peerSecure && protocol != ADC_COMPLIANCE)
	{
		QERR("Peer asked for protocol '%s', which is not supported", proto ? proto : "");
		sendPeerError(from, adc_status_code(ADC_STATUS_SEVERITY_FATAL, ADC_CODE_PROTOCOL_UNSUPPORTED).c_str(),
			"Protocol not supported", token, proto);
		return;
	}

	/*
	 * KEYP: the keyprint the hub relayed for this user is only worth checking if
	 * the hub could not have rewritten it. That means our own connection to it
	 * has to be ADCS and matched against a keyprint we already trusted -
	 * otherwise whoever forged this CTM could just as easily forge the KP.
	 */
	const char* peerKeyprint = 0;

	if (peerSecure && user->hasKeyprint())
	{
		if (keyprintVerified)
			peerKeyprint = user->getKeyprint();
		else
			QDBG("Not checking the keyprint of '%s': this hub connection is not itself verified",
				user->getNick());
	}

	auto csock = Samurai::IO::Net::Socket::create(nullptr, ip, port);
	ADC::ClientSession* connection = new ADC::ClientSession(csock, this, token, from, peerSecure, peerKeyprint);
	csock->setEventHandler(connection);
	csock->connect();
}


/*
 * Whether NAT traversal is possible on this connection at all.
 *
 * It needs a local port to dial from that a peer's connection can arrive on, which
 * is this hub connection's own - so no port, no traversal.
 */
bool ADC::HubSession::canTraverse() const
{
	return privateEndpointPort() != 0;
}


uint16_t ADC::HubSession::privateEndpointPort() const
{
	if (!socket) return 0;

	/*
	 * Through a proxy this port reaches the proxy, and the mapping it belongs to is
	 * the proxy's rather than ours - so there is nothing here a peer could dial.
	 * A proxied client is passive and cannot traverse anything.
	 */
	if (socket->isProxied()) return 0;

	return socket->getLocalPort();
}


/*
 * Dial a peer from the very port this hub connection uses.
 *
 * That is the whole of NAT traversal: our NAT already has a mapping for this local
 * port, made when we reached the hub, and a connection the peer starts at the same
 * moment arrives on it. Dialling from a fresh port would make a mapping the peer
 * has never been told about.
 */
bool ADC::HubSession::dialFromHubPort(const QuickDC::User* user, const char* proto,
                                     uint16_t port, const char* token)
{
	const uint16_t mine = privateEndpointPort();
	if (!mine)
	{
		QERR("No local port to traverse from; not dialling '%s'", user->getNick());
		return false;
	}

	const char* ip = user->getAddressIPv4();
	if (!ip || !*ip)
	{
		/* The hub is required to forward I4 for a client that uses this. */
		QERR("The hub did not say where '%s' is; nothing to dial", user->getNick());
		return false;
	}

	const std::string_view protocol(proto ? proto : "");
	const bool peerSecure = (protocol == ADC_TLS_PROTO);

	const char* peerKeyprint = 0;
	if (peerSecure && user->hasKeyprint() && keyprintVerified)
		peerKeyprint = user->getKeyprint();

	QDBG("Traversing to '%s' at %s:%d from our own %d", user->getNick(), ip,
		static_cast<int>(port), static_cast<int>(mine));

	auto csock = Samurai::IO::Net::Socket::create(nullptr, ip, port);
	if (!csock) return false;

	/* Before connect(), which is the only moment a local port can be chosen. */
	csock->setReuseLocalPort(mine);

	ADC::ClientSession* connection = new ADC::ClientSession(csock, this, token,
		user->getID(), peerSecure, peerKeyprint);
	csock->setEventHandler(connection);
	csock->connect();
	return true;
}


/*
 * The peer named the port it will dial from. Dial it, and answer with ours so it
 * can do the same - neither connection succeeds alone.
 */
void ADC::HubSession::onNatConnect(const char* from, const char* proto,
                                   uint16_t port, const char* token)
{
	if (userState != QuickDC::HubSession::StateNormal) return;
	if (!sid.empty() && sid == from) return;

	const QuickDC::User* user = users->getUser(from);
	if (!user) return;

	const uint16_t mine = privateEndpointPort();
	if (!mine)
	{
		sendPeerError(from, adc_status_code(ADC_STATUS_SEVERITY_FATAL,
				ADC_CODE_PROTOCOL_CONNECT_ERROR).c_str(),
			"No port to traverse from", token, proto);
		return;
	}

	/*
	 * Answered before dialling, so the peer starts at the same time we do rather
	 * than after us: the two connections have to cross.
	 */
	ADC::Command* cmd = new ADC::Command(FOURCC('D','R','N','T'), localSid(), user->getID());
	cmd->addArgument(proto ? proto : ADC_COMPLIANCE);
	cmd->addArgument(std::to_string(mine).c_str());
	if (token) cmd->addArgument(token);
	send(cmd);

	dialFromHubPort(user, proto, port, token);
}


/* Our own request answered: dial, and say nothing further. */
void ADC::HubSession::onNatReply(const char* from, const char* proto,
                                 uint16_t port, const char* token)
{
	if (userState != QuickDC::HubSession::StateNormal) return;
	if (!sid.empty() && sid == from) return;

	const QuickDC::User* user = users->getUser(from);
	if (!user) return;

	dialFromHubPort(user, proto, port, token);
}


void ADC::HubSession::onPassiveConnect(const char* from, const char* proto, const char* token)
{
	if (userState != QuickDC::HubSession::StateNormal) return;

	if (!sid.empty() && sid == from) return;
	const QuickDC::User* user = users->getUser(from);
	if (!user) return;

	QDBG("Requested to setup connection to '%s' using '%s' (token=%s)", from, proto, token);

	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Network");

	/*
	 * Answering an RCM means telling the peer to connect to us, so as in
	 * sendConnectionRequest() we may only name a protocol we can accept. This
	 * one was asked for rather than volunteered, though, so it is honoured
	 * whenever we are able to - the hub's own protocol has no say in it.
	 */
	const std::string_view requested(proto ? proto : "");
	bool peerSecure = (requested == ADC_TLS_PROTO);

	/* A protocol left out means plain ADC, which is what it meant here before
	   ADCS was an option at all. One we do not speak cannot be answered. */
	if (!requested.empty() && !peerSecure && requested != ADC_COMPLIANCE)
	{
		QERR("Peer asked for protocol '%s', which is not supported", proto);
		sendPeerError(from, adc_status_code(ADC_STATUS_SEVERITY_FATAL, ADC_CODE_PROTOCOL_UNSUPPORTED).c_str(),
			"Protocol not supported", token, proto);
		return;
	}

	if (peerSecure && !canAcceptTls())
	{
		QERR("Peer asked us to accept an ADCS connection, but we have no certificate to present");
		sendPeerError(from, adc_status_code(ADC_STATUS_SEVERITY_FATAL, ADC_CODE_PROTOCOL_UNSUPPORTED).c_str(),
			"No certificate to accept TLS with", token, proto);
		return;
	}

	/*
	 * We are about to name a protocol in a CTM, and the peer will dial whatever
	 * we name - so this is the last point at which the requirement can be
	 * honoured. Without a certificate we cannot name ADCS at all, which is a
	 * different failure from the peer refusing it and is reported as such.
	 */
	if (!peerSecure && requireEncryptedTransfers())
	{
		if (!canAcceptTls())
		{
			refuseUnencrypted(from, token, proto,
				"encryption is required and there is no certificate to accept it with");
			return;
		}

		refuseUnencrypted(from, token, proto,
			"the peer did not ask for ADCS");
		return;
	}

	const char* protocol = peerSecure ? ADC_TLS_PROTO : ADC_COMPLIANCE;

	/* The endpoint the INF advertised, for the same reason as in
	   sendConnectionRequest(): a gateway may have granted another port. */
	const QuickDC::Core* core = QuickDC::Core::getInstance();
	const QuickDC::PeerEndpoint endpoint = QuickDC::peerEndpoint(
		config->getBool("Active mode"), core->server != 0,
		config->getString("Address", "0.0.0.0"),
		observedAddress,
		static_cast<uint16_t>(config->getNumber("Port")),
		core->portforward);

	if (endpoint.active) {
		QDBG("Requesting active connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','C','T','M'), localSid(), user->getID());
		cmd->addArgument(protocol);
		cmd->addArgument(std::to_string(endpoint.port).c_str());
		if (token) cmd->addArgument(token);

		/* Add to securitymanager */
		QuickDC::Clearance* clearance = new QuickDC::Clearance(
				QuickDC::Clearance::AuthorizedUser,
				QuickDC::Clearance::ADC,
				this,
				reinterpret_cast<const char*>(user->getClientID()),
				token);
		QuickDC::Core::getInstance()->securityManager->grant(clearance);

		send(cmd);
	} else if (canTraverse() && user->supports(ADC_EXT_NATT)) {
		/*
		 * Neither of us can be connected to, which used to end here with nothing
		 * to offer. Both of us can dial, though, and a connection each of us
		 * starts at the same moment crosses in the middle - so name the port ours
		 * will come from and wait for the peer to name its own.
		 */
		QDBG("Traversing to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','N','A','T'), localSid(), user->getID());
		cmd->addArgument(protocol);
		cmd->addArgument(std::to_string(privateEndpointPort()).c_str());
		if (token) cmd->addArgument(token);

		QuickDC::Clearance* clearance = new QuickDC::Clearance(
				QuickDC::Clearance::AuthorizedUser,
				QuickDC::Clearance::ADC,
				this,
				reinterpret_cast<const char*>(user->getClientID()),
				token);
		QuickDC::Core::getInstance()->securityManager->grant(clearance);

		send(cmd);
	} else {
		QDBG("Requesting passive connection to user: %s", user->getNick());
		sendPeerError(from, "242", "Both clients are passive", token, proto);
	}
}


void ADC::HubSession::sendPeerError(const char* remote_sid, const char* code, const char* msg, const char* token, const char* proto)
{
	ADC::Command* cmd = new ADC::Command(FOURCC('D','S','T','A'), localSid(), remote_sid);
	cmd->addArgument(code);
	cmd->addArgument(msg);
	if (token) cmd->addArgument("TO", token);
	if (proto) cmd->addArgument("PR", proto);
	send(cmd);
}


void ADC::HubSession::onSearch(QuickDC::Share::SearchRequest* request)
{
	if (userState != QuickDC::HubSession::StateNormal) return;

	QuickDC::Share::Manager* shares = QuickDC::Core::getInstance()->shares;
	if (!shares) return;

	shares->search(request);

	/* A hash we do not share may still be one we are downloading. */
	if (request->isTTH() && !shares->getFileByTTH(request->getTTH()))
		sendPartialStatus(request);
}

void ADC::HubSession::sendPartialStatus(QuickDC::Share::SearchRequest* request)
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	if (!queue || !request || !request->getTTH()) return;

	QuickDC::QueueFile* file = queue->getFileByTTH(request->getTTH());
	if (!file) return;

	/* Only verified blocks may be offered, so the leaves have to be in. */
	QuickDC::LeafHashes* leaves = file->getLeaves();
	if (!leaves || !leaves->isLoaded()) return;

	uint64_t block = leaves->getLeafSize();
	size_t blocks = file->getChunks().countBlocks(block);
	if (!blocks) return;

	/* PC counts the start/end pairs in PI, not the blocks; a peer that reads it
	   the other way discards the message as malformed. */
	size_t runs = file->getChunks().countBlockRuns(block);

	std::string indexes = file->getChunks().toIndexString(block);
	if (indexes.empty()) return;

	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	int port = config->getBool("Active mode") ? config->getNumber("Port", 0) : 0;

	/* HI names the hub without the scheme, which is how DC++ reports it. */
	std::string hubIpPort;
	if (hub && hub->getUrl())
	{
		hubIpPort = hub->getUrl();
		size_t cut = hubIpPort.find("://");
		if (cut != std::string::npos) hubIpPort = hubIpPort.substr(cut + 3);
	}

	const QuickDC::User* user = request->getUser();

	if (user && user->getAddressIPv4() && user->getPortIPv4() && core->server)
	{
		ADC::Command cmd(FOURCC('U','P','S','R'), ADC::CID::getInstance()->getCID());
		cmd.addArgument("HI", hubIpPort.c_str());
		cmd.addArgument("U4", std::to_string(port));
		cmd.addArgument("TR", request->getTTH());
		cmd.addArgument("PC", std::to_string(static_cast<int>(runs)));
		cmd.addArgument("PI", indexes.c_str());

		Samurai::IO::Buffer buffer;
		cmd.write(&buffer);
		Samurai::IO::Net::DatagramPacket packet(&buffer);
		Samurai::IO::Net::InetSocketAddress peer(user->getAddressIPv4(), user->getPortIPv4(),
			Samurai::IO::Net::InetAddress::Version::IPv4);
		packet.setAddress(&peer);
		core->server->send(&packet);
	}
	else if (user)
	{
		ADC::Command* cmd = new ADC::Command(FOURCC('D','P','S','R'), localSid(), user->getID());
		cmd->addArgument("HI", hubIpPort.c_str());
		cmd->addArgument("U4", std::to_string(port));
		cmd->addArgument("TR", request->getTTH());
		cmd->addArgument("PC", std::to_string(static_cast<int>(runs)));
		cmd->addArgument("PI", indexes.c_str());
		send(cmd);
	}

	QDBG("Offered %d of %d block(s) of '%s'", static_cast<int>(blocks),
		static_cast<int>((file->getSize() + block - 1) / block), request->getTTH());
}

void ADC::HubSession::onPartialStatus(const char* cid, const char* tth, const char* indexes)
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	const char* url = (hub) ? hub->getUrl() : 0;

	if (queue && url) queue->onPartialSources(url, cid, tth, indexes);
}
