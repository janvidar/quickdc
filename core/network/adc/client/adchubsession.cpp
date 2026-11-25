/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <string.h>
#include <time.h>

#include "quickdc.h"
#include "api/networkmode.h"
#include "api/core.h"
#include "api/dropverdict.h"
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
#include "network/adc/adcbbs.h"
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
#include "network/transfermanager.h"
#include "network/user.h"
#include "config/settings.h"
#include <optional>
#include <string>
#include <string_view>
#include <samurai/crypto/digest/tiger.h>
#include <samurai/io/buffer.h>
#include <samurai/io/file.h>
#include <samurai/io/net/datagram.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/tlscontext.h>
#include <samurai/io/net/url.h>
#include <samurai/util/base32.h>
#include <span>




/*
 * Whether a peer dialling us can be answered with TLS. probe.cpp completes a
 * server side handshake before fingerprinting the stream, so the same port
 * serves both protocols and the only thing that can be missing is a
 * certificate to present. Without one the handshake fails, which the peer can
 * only read as a reset - so naming ADCS then would be worse than not offering
 * it at all.
 */
static bool canAcceptTls(QuickDC::Core& core)
{
	const Samurai::IO::Net::TlsContext& keys = core.loop().tls();
	Samurai::IO::File* cert = keys.getCertificate();
	Samurai::IO::File* pkey = keys.getPrivateKey();

	return cert && cert->exists() && pkey && pkey->exists();
}

/*
 * Whether a peer transfer has to be encrypted on a hub that is not itself.
 *
 * Off unless asked for, and only consulted for a hub reached over plain ADC - see
 * mustEncryptPeers(), where an ADCS hub decides this for itself and does not ask.
 * What is left here is the case where the requirement buys the least: a plain hub
 * relays the KP field a peer connection is held to, so it can rewrite it, and
 * encryption there hides the transfer from an observer without establishing who is
 * at the other end of it.
 *
 * Against that stands what the requirement costs, which is every peer that does not
 * offer ADCS at all - on a plain hub that is a transfer refused rather than a
 * transfer downgraded, since ADC/1.0 is what those clients have. Whoever would
 * rather have neither can say so here.
 */
static bool requireEncryptedTransfers(QuickDC::Core& core)
{
	QuickDC::Preferences* config = core.config;
	return QuickDC::Settings::read(*config, QuickDC::Settings::RequireEncryption);
}

/*
 * Which encrypted peer protocol a user's INF says they will speak, or nothing at
 * all if it names neither spelling.
 *
 * The flag decides the protocol string rather than the other way round, because a
 * peer recognises the one it announced and no other; see ADC_TLS_PROTO. ADCS is
 * preferred where a peer announces both, that being the spelling ADC-EXT settled
 * on, and the draft is what the rest of the network answers to.
 */
static const char* peerTlsProtocol(const QuickDC::User* user)
{
	if (!user) return 0;
	if (user->supports(ADC_EXT_ADCS)) return ADC_TLS_PROTO;
	if (user->supports(ADC_EXT_ADCS_DRAFT)) return ADC_TLS_PROTO_DRAFT;
	return 0;
}

/* Both spellings of the traversal flag, and for the same reason: a client that
   announces only the draft can still take part. */
static bool peerCanTraverse(const QuickDC::User* user)
{
	return user && (user->supports(ADC_EXT_NATT) || user->supports(ADC_EXT_NATT_DRAFT));
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
static bool reachable(QuickDC::Core& core, QuickDC::Preferences* config, bool ipv6)
{

	if (QuickDC::networkModeKeysInForce(*config).forceActive) return true;

	const QuickDC::Reachability* known =
		core.reachabilityFor(ipv6);
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

	QuickDC::Preferences* config = getCore().config;
	const std::string configured = QuickDC::Settings::read(*config, QuickDC::Settings::Address);

	return configured.empty() ? std::string("0.0.0.0") : configured;
}

/*
 * Refuse a peer connection that would not be encrypted, and say so where a user
 * can see it: a transfer that never starts is otherwise indistinguishable from a
 * peer that is simply not answering.
 *
 * Named, and with the protocol the peer asked for, because the refusal repeats:
 * every request from a client that cannot do ADCS produces one, and a line that
 * says only that something was refused leaves a user with no way to tell one
 * peer trying over and over from half the hub each trying once. The nick is what
 * the user list shows, so it is the identifier that can be acted on; the session
 * identifier stands in when the user has already gone.
 */
void ADC::HubSession::refuseUnencrypted(const char* from, const char* token,
                                        const char* proto, const char* why)
{
	const QuickDC::User* user = users ? users->getUser(from) : 0;
	const char* nick = user ? user->getNick() : 0;
	const std::string who = (nick && *nick) ? nick
	                      : (from && *from ? std::string("session ") + from
	                                       : std::string("an unnamed peer"));

	/* An absent protocol field is a case of its own, and one worth being able to
	   see: it is not the same as a peer that named plain ADC. */
	const std::string named = (proto && *proto) ? std::string("'") + proto + "'"
	                                            : std::string("no protocol");

	QERR("Refusing an unencrypted peer connection with '%s' (%s): %s",
		who.c_str(), named.c_str(), why);

	if (listener)
	{
		const std::string msg = "Refused an unencrypted transfer with '" + who
			+ "' (" + named + "): " + why;
		listener->EventSystemError(msg.c_str());
	}

	sendPeerError(from, adc_status_code(ADC_STATUS_SEVERITY_FATAL,
			ADC_CODE_PROTOCOL_UNSUPPORTED).c_str(),
		"Encryption is required", token, proto);
}


ADC::HubSession::HubSession(QuickDC::Hub* ui, QuickDC::HubListener* listener_, Samurai::IO::Net::URL* url, bool secure_)
	: QuickDC::HubSession(ui)
	, QuickDC::Connection(ui->getCore(), url, secure_ ? "ADCS hub" : "ADC hub")
	, proxy(0)
	, listener(listener_)
	, infoTimer(0)
	, keepAlive(QUICKDC_ADC_KEEPALIVE)
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

	QuickDC::Preferences* config = getCore().config;

	/*
	 * Read once rather than per timer: a connection tracking firewall forgets a
	 * connection nothing has crossed for a few minutes, and it forgets it for
	 * the whole of this session or none of it.
	 *
	 * Zero turns the newline off, for anyone who would rather find out the hard
	 * way. Anything else is taken as given, including a value low enough to be
	 * rude to the hub - it is the user's own configuration file.
	 */
	keepAlive = static_cast<time_t>(
		QuickDC::Settings::read(*config, QuickDC::Settings::KeepAliveInterval));

	if (keepAlive < 0) keepAlive = QUICKDC_ADC_KEEPALIVE;

	/* Borrowed from the core, and shared with every other hub session; see
	   Core::session. Not ours to delete, and its group is not ours to leave set. */
	session = getCore().session;

	/*
	 * The canonical "{scheme}://{host}:{port}" rather than the address as it was
	 * typed, because this names the group everything about this hub is filed under.
	 *
	 * A hub reached as "adcs://host:1511" and the same hub reached as
	 * "adcs://host:1511/?kp=..." are one hub with one nickname and one password, and
	 * keying on the typed form gave them a section each - so a keyprint pasted into
	 * the address once was enough to lose the identity that had been set up for it.
	 * The remembered certificate was already filed canonically for exactly this
	 * reason; the rest of the settings were not.
	 */
	address = (hub && hub->getUrl()) ? hub->getUrl() : url->toString();
	session->setGroup(address);

	/*
	 * Load settings from session file, or defaults if no session is found.
	 * This will incidentally create such a session.
	 */
	nickname = QuickDC::Settings::readForHub(*session, address, *config, QuickDC::Settings::Nickname);
	description = QuickDC::Settings::readForHub(*session, address, *config, QuickDC::Settings::Description);
	email = QuickDC::Settings::readForHub(*session, address, *config, QuickDC::Settings::Email);
	password = QuickDC::Settings::readForHub(*session, address, *config, QuickDC::Settings::Password);

	/* A hub will never accept an empty nick, and older versions could store one
	 * in the session file. Fall back to the configured nick in that case. */
	if (nickname.empty())
	{
		const std::string fallback = QuickDC::Settings::read(*config, QuickDC::Settings::Nickname);
		nickname = fallback.empty() ? std::string(PRODUCT) : fallback;
	}

	session->setTime("Last Connect", time(0));

	session->write();
}


ADC::HubSession::~HubSession()
{
	/*
	 * Only to disk: nothing of the session's own identity is written here, or a
	 * name or password it merely used would outrank the setting for this hub.
	 */
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

		/*
		 * Added here rather than in addSupportFlags(), which the hub in this tree
		 * introduces itself through the same call: offering to read a board and
		 * offering to host one are different claims that happen to share a fourcc,
		 * exactly as with the NATT and PING a hub adds after the same call. A hub
		 * announcing BBS0 says it keeps an index and will serve it; a client says
		 * only that it implements the post document and will verify what it
		 * displays, which is the whole of what this promises.
		 */
		cmd->addArgument("AD", ADC_EXT_BULLETIN);
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
	 * Each access below selects the group it wants; nothing is restored, because the
	 * store is shared with every other hub session and the group that was current on
	 * entry is not necessarily ours to put back.
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
			trusted = session->peekString(keyprintGroup(), "Keyprint", "");
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
 * Where a remembered keyprint lives, which is where everything else about this hub
 * lives: 'address' is the canonical form, so there is one group and not two.
 *
 * Kept as a name of its own rather than folded into its callers, because what it
 * answers is a question about the certificate store and the reason the group has to
 * be the canonical one was first written down here.
 */
const char* ADC::HubSession::keyprintGroup() const
{
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

		/* And on this path too, for the reason given at the other one: an ADCS hub
		   is offered the same set as a plain one, encryption having nothing to do
		   with whether a client reads bulletins. */
		cmd->addArgument("AD", ADC_EXT_BULLETIN);
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

	/*
	 * Shareable from the start, because NAT traversal dials a peer from this
	 * connection's own port (see dialFromHubPort()), and a port can only be bound a
	 * second time where every socket on it agreed. Nothing to share through a
	 * proxy, where the port is the proxy's.
	 */
	if (!socket->isProxied()) socket->setReuseLocalPort(0);

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
			markEstablished();
			listener->EventHubStatus(QuickDC::HubListener::HubLoggedIn);
			/* One more hub we are on, which the others were told a count of. */
			getCore().hubs->requestClientInfoUpdate();
		}
	}
	listener->EventUserUpdate(data);
}


void ADC::HubSession::resetIdleTimer()
{
	delete infoTimer;
	infoTimer = 0;

	if (!keepAlive) return;

	/*
	 * Started again on every read and every write, so it only ever expires on a
	 * connection nothing has crossed - and it repeats, so a hub that says
	 * nothing back still gets one of these every interval.
	 */
	infoTimer = new Samurai::Timer(getCore().loop(), this, keepAlive, false);
}


void ADC::HubSession::EventTimeout(Samurai::Timer* /* timer */)
{
	/*
	 * A command with no name at all, which goes out as the single newline ADC
	 * reserves for exactly this. It asks the hub for nothing and tells it
	 * nothing; the point is the packet, not the content.
	 *
	 * Sent because a connection nobody has used is one a firewall between here
	 * and the hub will quietly forget, and the first thing sent afterwards is
	 * answered with a reset rather than delivered. Better to find that out on a
	 * newline than on something somebody typed.
	 */
	ADC::Command* empty = new ADC::Command();
	send(empty);
}


void ADC::HubSession::EventSearchReply(QuickDC::Share::SearchReply* reply)
{
	QuickDC::Share::SearchRequest* req = reply->getRequest();
	const QuickDC::User* user = reply->getRequest()->getUser();
	QuickDC::Share::File* file = reply->getFile();

	const std::optional<size_t> freeSlots = getCore().transfers && getCore().config
		? getCore().transfers->freeUploadSlots(QuickDC::Settings::read(*getCore().config, QuickDC::Settings::Slots))
		: std::nullopt;

	if (user->getAddressIPv4() && user->getPortIPv4() && getCore().server)
	{
		/* Send UDP packet */

		ADC::Command cmd(FOURCC('U','R','E','S'), getCore().identity->getCID());
		cmd.addArgument("FN", file->getPublicName());
		cmd.addArgument("SI", std::to_string(file->getSize()));
		cmd.addArgument("TR", file->getTTH());
		cmd.addArgument("TD", "0");
		cmd.addArgument("TO", req->getToken());
		if (freeSlots) cmd.addArgument("SL", std::to_string(*freeSlots));

		Samurai::IO::Buffer buffer;
		cmd.write(&buffer);
		Samurai::IO::Net::DatagramPacket packet(&buffer);
		Samurai::IO::Net::InetSocketAddress peerAddr(user->getAddressIPv4(), user->getPortIPv4(), Samurai::IO::Net::InetAddress::Version::IPv4);
		packet.setAddress(&peerAddr);
		getCore().server->send(&packet);

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
		if (freeSlots) cmd->addArgument("SL", std::to_string(*freeSlots));

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

	QuickDC::Core* core = &getCore();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	const char* hubUrl = (hub) ? hub->getUrl() : 0;

	QDBG("Search result: '%s' (%d bytes) from '%s'",
		result->getFile() ? result->getFile() : "",
		static_cast<int>(result->getSize()),
		result->getUser() ? result->getUser()->getNick() : "");

	/*
	 * Match on the TTH rather than the name: the whole point of queueing by
	 * hash is that the same file can be called different things elsewhere.
	 *
	 * Never ourselves: onSearch() sees to it that we do not answer our own
	 * searches, and this is the other half of that rule.
	 */
	if (queue && hubUrl && result->getTTH() && result->getUser()
		&& result->getUser() != getLocalUser())
	{
		QuickDC::QueueFile* qfile = queue->getFileByTTH(result->getTTH());
		const char* cid = reinterpret_cast<const char*>(result->getUser()->getClientID());

		if (qfile && cid && *cid)
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


void ADC::HubSession::onQuit(const char* id, const char* message, const char* redirect, const char* timeLeft)
{
	const bool self = id && !sid.empty() && sid == id;
	if (!self)
	{
		users->quit(id, message);
		return;
	}

	/* Somebody decided this; coming straight back would be arguing with them. */
	reconnect_time = -1;

	if (redirect && *redirect)
	{
		const Samurai::IO::Net::URL to(redirect);
		listener->EventHubStatus(QuickDC::HubListener::HubRedirected);
		if (hub) hub->onRedirect(redirect, to.isValid() ? to.getPort() : 0);
	}
	else if (timeLeft && *timeLeft)
	{
		const bool forGood = Samurai::Util::Convert::to_int64(timeLeft) < 0;
		listener->EventHubStatus(forGood ? QuickDC::HubListener::HubBannedPermanently
		                                 : QuickDC::HubListener::HubBannedTemporarily);
	}
	else
	{
		listener->EventHubStatus(QuickDC::HubListener::HubKicked);
	}

	users->quit(id, message);
}


void ADC::HubSession::sendAdminUserKick(const QuickDC::User* user, const char* message, int64_t banSeconds)
{
	ADC::Command* cmd = new ADC::Command(FOURCC('H','D','S','C'));
	cmd->addArgument(user->getID());
	if (message && *message) cmd->addArgument("MS", message);
	if (banSeconds != 0) cmd->addArgument("TL", std::to_string(banSeconds < 0 ? -1 : banSeconds));
	send(cmd);
}


void ADC::HubSession::sendAdminUserRedirect(const QuickDC::User* user, const char* address, const char* reason)
{
	ADC::Command* cmd = new ADC::Command(FOURCC('H','D','S','C'));
	cmd->addArgument(user->getID());
	if (reason && *reason) cmd->addArgument("MS", reason);
	if (address && *address) cmd->addArgument("RD", address);
	send(cmd);
}


void ADC::HubSession::forgetAnnouncedInfo()
{
	announcedInfo.clear();
}


/**
 * Describe this client to the hub, as the fields an INF is built from.
 *
 * Advertising an endpoint here is also what makes a measurement usable, so one
 * taken before there was an endpoint is offered again once there is: the
 * reachability record ignores a verdict until something has been advertised,
 * since a verdict is about an endpoint, and the check made at start-up routinely
 * finishes before the first login - so without offering it again, the one answer
 * that was actually measured would be the one thrown away.
 */
void ADC::HubSession::collectClientInfo(std::vector<ADC::InfField>& fields)
{
	QuickDC::Preferences* config = getCore().config;

	fields.emplace_back("NI", nickname);
	fields.emplace_back("SL", std::to_string(QuickDC::Settings::read(*config, QuickDC::Settings::Slots)));
	fields.emplace_back("SS", std::to_string(getCore().shares->getSharedSize()));
	fields.emplace_back("SF", std::to_string(getCore().shares->getFiles()));
	fields.emplace_back("DE", description);
	fields.emplace_back("EM", email);
	fields.emplace_back("VE", PRODUCT " " VERSION);
	fields.emplace_back("ID", getCore().identity->getCID());
	fields.emplace_back("PD", getCore().identity->getPID());

	/*
	 * How many hubs this client is on, counting this one - which the manager is
	 * asked to leave out, because it counts the hubs we are logged in to and
	 * this INF is what logging in consists of. Counted here instead, so the
	 * number does not depend on which side of the login it is asked from.
	 *
	 * Until the hub answers, what kind of user we will be there is a guess: one
	 * we hold a password for will make us registered, anything else a normal
	 * user. Guessing beats leaving it out, and costs nothing when it is right -
	 * the update that login schedules then finds the counts unchanged and sends
	 * nothing, where it used to exist for no reason but to say one greater than
	 * what had gone out a moment earlier. Being made an operator is the rare
	 * case, and still costs the one correction it always did.
	 */
	QuickDC::HubManager* hubs = getCore().hubs;
	size_t normal = hubs->countHubsNormal(this);
	size_t registered = hubs->countHubsRegistered(this);
	size_t op = hubs->countHubsOperator(this);

	switch (isLoggedIn() ? access
	                     : (password.empty() ? QuickDC::HubSession::AccessNormal
	                                         : QuickDC::HubSession::AccessRegistered))
	{
		case QuickDC::HubSession::AccessOperator:   op++; break;
		case QuickDC::HubSession::AccessRegistered: registered++; break;
		default:                                    normal++; break;
	}

	fields.emplace_back("HN", std::to_string(normal));
	fields.emplace_back("HR", std::to_string(registered));
	fields.emplace_back("HO", std::to_string(op));

	/*
	 * KEYP: publish what our own certificate hashes to, so a peer dialling us
	 * over ADCS can tell that it reached us and not someone in between. No
	 * certificate, nothing to say.
	 */
	std::string keyprint;
	if (!QuickDC::Keyprint::ofOwnCertificate(getCore().loop().tls(), keyprint)) keyprint.clear();
	fields.emplace_back("KP", keyprint);


	/*
	 * Where a peer should be told to reach us, which is the forwarded port when
	 * there is one rather than the port that was configured: a gateway may have
	 * granted a different one, and it is the granted one anything can reach.
	 */
	QuickDC::Core* core = &getCore();
	const QuickDC::PeerEndpoint endpoint = QuickDC::peerEndpoint(
		QuickDC::networkModeKeysInForce(*config).activeMode,
		core->server != 0 && core->server->listening(false),
		QuickDC::Settings::read(*config, QuickDC::Settings::Address),
		observedAddress,
		core->peerPort(),
		core->portforward);

	QDBG("Active mode %d (server=%p)\n", endpoint.active ? 1 : 0, core->server);

	/*
	 * The IPv6 endpoint, which no gateway is involved in: the address is the one
	 * a server outside reported for that family, since there is nothing
	 * configured to fall back on and no wildcard a hub would fill in.
	 */
	const QuickDC::PeerEndpoint endpoint6 = QuickDC::peerEndpoint6(
		QuickDC::networkModeKeysInForce(*config).activeMode,
		core->server != 0 && core->server->listening(true),
		QuickDC::lastDropVerdict(*core, true).publicAddress,
		core->peerPort());

	/* Evidence belongs to the endpoint it was gathered for; announcing a different
	   one discards what was known about the old. */
	if (core->reachability && endpoint.active)
		core->reachability->advertising(endpoint.address, endpoint.port);

	if (core->reachability6 && endpoint6.active)
		core->reachability6->advertising(endpoint6.address, endpoint6.port);

	if ((core->reachability && endpoint.active)
	    || (core->reachability6 && endpoint6.active))
		QuickDC::applyDropMeasurement(*core);

	/*
	 * What we can do, as the other clients see it. Built up rather than written out
	 * twice: the only difference between the two cases is whether anyone can connect
	 * to us, and a feature added to one list and forgotten in the other is a bug that
	 * only shows in passive mode.
	 */
	std::string supports;

	/* The separator belongs between two flags rather than in front of each, since
	   any one of them may be the first this client has to claim. */
	const auto claim = [&supports](const char* flag)
	{
		if (!supports.empty()) supports += ",";
		supports += flag;
	};

	/*
	 * Encryption, in both spellings, and only where a peer dialling us could
	 * actually be answered: this is the flag other clients read to decide whether to
	 * ask for ADCS at all, so claiming it without a certificate to present sends
	 * them at a handshake that cannot complete and costs them the transfer. A
	 * certificate is generated at startup, so the claim is the ordinary case.
	 *
	 * ADCS first because it is the one ADC-EXT specifies; ADC0 because the DC++
	 * family looks for that one and for nothing else.
	 */
	if (canAcceptTls(getCore()))
	{
		claim(ADC_EXT_ADCS);
		claim(ADC_EXT_ADCS_DRAFT);
	}

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
	fields.emplace_back("I4", announcedAddress(endpoint));

	/* Nothing to say for U4 when nobody can connect here, and saying nothing is
	   also what withdraws a port announced while we still could. */
	fields.emplace_back("U4", endpoint.active ? std::to_string(endpoint.port) : std::string());

	if (endpoint.active)
	{
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
		if (reachable(getCore(), config, false)) claim(ADC_EXT_TCP4);

		/*
		 * UDP4 is not withdrawn, because nothing can disprove it: a peer that
		 * cannot reach this port has no datagram it could send to say so. Claimed
		 * whenever the port is open, as before.
		 */
		claim(ADC_EXT_UDP4);
	}

	/*
	 * And the same for IPv6, claimed from that family's own evidence.
	 *
	 * Sent only where there is an endpoint: unlike I4 there is no wildcard that
	 * asks a hub to fill the address in, so a client that does not know its own
	 * IPv6 address says nothing rather than naming one no peer could reach. An
	 * empty field is what withdraws a claim made while we did know.
	 */
	fields.emplace_back("I6", endpoint6.active ? endpoint6.address : std::string());
	fields.emplace_back("U6", endpoint6.active ? std::to_string(endpoint6.port)
	                                           : std::string());

	if (endpoint6.active)
	{
		if (reachable(getCore(), config, true)) claim(ADC_EXT_TCP6);
		claim(ADC_EXT_UDP6);
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
	 *
	 * In both spellings, as with encryption: NATT is what ADC-EXT names, NAT0 is
	 * what the clients that implement it look for, and a peer that reads neither
	 * flag falls back to telling us that we are both passive.
	 */
	if (canTraverse())
	{
		claim(ADC_EXT_NATT);
		claim(ADC_EXT_NATT_DRAFT);
	}

	claim(ADC_EXT_RICHTEXT);

	fields.emplace_back("SU", supports);
}


void ADC::HubSession::sendClientInfo()
{
	/* Nothing can be said before the hub has named us. A broadcast with no
	   source is not written at all, and taking it as said would leave every
	   field in it recorded as told and never sent again. */
	if (!localSid()) return;

	std::vector<ADC::InfField> fields;
	collectClientInfo(fields);

	std::vector<const ADC::InfField*> changed;

	for (const ADC::InfField& field : fields)
	{
		const std::string* told = 0;
		for (const ADC::InfField& seen : announcedInfo)
			if (seen.first == field.first) { told = &seen.second; break; }

		/*
		 * A field this hub has not been told is worth sending when there is
		 * something to say for it, and left out otherwise - an unset e-mail
		 * belongs out of the INF altogether rather than in it and blank.
		 *
		 * One this hub has been told is sent when it has changed, and that
		 * includes changing to nothing: the empty field is how ADC says to drop
		 * one, so a client that has stopped being reachable withdraws the port
		 * it published rather than leaving peers dialling it.
		 *
		 * Which leaves 'NI', 'ID' and 'PD' out of every INF after the first
		 * without being named here, since none of them ever changes - and 'PD'
		 * is one the specification allows only in the first.
		 */
		if (told ? *told == field.second : field.second.empty()) continue;

		changed.push_back(&field);
	}

	/*
	 * Nothing has changed, so there is nothing to say. Sending the INF anyway
	 * would be a line naming this client and no fields, which a hub relays to
	 * everyone on it for no purpose - and does so on every hub count, port
	 * forwarding result and reachability verdict, which is often.
	 */
	if (changed.empty()) return;

	ADC::Command* cmd = new ADC::Command(FOURCC('B','I','N','F'), localSid());
	for (const ADC::InfField* field : changed)
		cmd->addArgument(field->first.c_str(), field->second);

	announcedInfo = std::move(fields);
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


bool ADC::HubSession::mustEncryptPeers() const
{
	return secure || requireEncryptedTransfers(getCore());
}


const char* ADC::HubSession::peerConnectProtocol(const QuickDC::User* user, bool listening) const
{
	const char* tls = peerTlsProtocol(user);

	/*
	 * A peer that announced neither flag is still asked for encryption where this
	 * hub demands it, in the spelling the specification gives. SU is a claim a
	 * client has to remember to make, and treating silence as "cannot" would turn
	 * every under-announcing peer into a transfer refused outright rather than one
	 * attempt that may yet be answered.
	 */
	if (!tls && mustEncryptPeers()) tls = ADC_TLS_PROTO;

	if (!tls) return ADC_COMPLIANCE;

	/*
	 * Naming encryption on the end that will be dialled promises a handshake we
	 * have to complete, and without a certificate there is nothing to answer with.
	 * Where the requirement stands that is the end of it; where it does not, plain
	 * ADC is still a transfer.
	 */
	if (listening && !canAcceptTls(getCore()))
		return mustEncryptPeers() ? 0 : ADC_COMPLIANCE;

	return tls;
}


void ADC::HubSession::sendConnectionRequest(const QuickDC::User* user, const char* token)
{
	if (!user) return;
	if (!token) return;

	QuickDC::Preferences* config = getCore().config;

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
	const QuickDC::Core* core = &getCore();
	const QuickDC::PeerEndpoint endpoint = QuickDC::peerEndpoint(
		QuickDC::networkModeKeysInForce(*config).activeMode,
		core->server != 0 && core->server->listening(false),
		QuickDC::Settings::read(*config, QuickDC::Settings::Address),
		observedAddress,
		core->peerPort(),
		core->portforward);

	/*
	 * Which protocol to name depends on what the peer said it speaks, and on who is
	 * going to dial whom:
	 *
	 *  - CTM tells the peer to connect to us, so we have to be able to accept
	 *    whatever we name - see canAcceptTls().
	 *
	 *  - RCM asks the peer to send us a CTM, so we dial out, and outbound TLS is
	 *    handled by ADC::ClientSession, which needs no certificate of our own.
	 */
	const char* protocol = peerConnectProtocol(user, endpoint.active);

	if (!protocol)
	{
		/*
		 * Said out loud rather than logged, because the whole of it from a user's
		 * side is a download that never starts - and named, for the same reason
		 * refuseUnencrypted() names its peer.
		 */
		const char* nick = user->getNick();
		QERR("Cannot ask '%s' for a connection: this hub requires encryption and "
			"there is no certificate to accept it with", nick ? nick : "");

		if (listener)
		{
			const std::string msg = std::string("Cannot arrange an encrypted "
				"transfer with '") + (nick ? nick : "an unnamed peer")
				+ "': no certificate to accept one with";
			listener->EventSystemError(msg.c_str());
		}
		return;
	}

	if (endpoint.active)
	{
		QDBG("Requesting active connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','C','T','M'), localSid(), user->getID());
		cmd->addArgument(protocol);
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
		getCore().securityManager->grant(clearance);

	}
	else
	{
		QDBG("Requesting passive connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','R','C','M'), localSid(), user->getID());

		cmd->addArgument(protocol);
		cmd->addArgument(token);

		/* Add to securitymanager */
		QuickDC::Clearance* clearance = new QuickDC::Clearance(
				QuickDC::Clearance::AuthorizedUser,
				QuickDC::Clearance::ADC,
				this,
				reinterpret_cast<const char*>(user->getClientID()),
				token);
		getCore().securityManager->grant(clearance);
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

	/* A SID is the start of a session, and a hub holds what it was told for no
	   longer than one - so whatever a previous connection said is not something
	   this one can send the difference from. */
	forgetAnnouncedInfo();

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
			markEstablished();

			/* This is the transition, so this is where it gets reported.
			   EventUserInfo() also tries to, and cannot: it is reached through
			   users->info() below, by which time the state it tests has already
			   been moved. */
			listener->EventHubStatus(QuickDC::HubListener::HubLoggedIn);
			/* One more hub we are on, which the others were told a count of. */
			getCore().hubs->requestClientInfoUpdate();
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

	if (session->peekString(keyprintGroup(), QUICKDC_ADC_SALT_SETTING, "") == salt)
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
	 * user just supplied survives a crash or a kill.
	 */
	QuickDC::Settings::writeForHub(*session, address, *getCore().config, QuickDC::Settings::Password, password);
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


/*
 * The most features this session will remember a hub claiming.
 *
 * A hub's SUP is not the single exchange a peer's CSUP is: a hub may revise what
 * it offers for as long as the session lasts, so nothing in the protocol says
 * how many names may arrive, and every one of them is held for the life of the
 * connection. Without a ceiling, a hub can grow a table in this client as fast
 * as it cares to write four letter words - seven bytes each on the wire, a good
 * deal more to keep - and it is the client that runs out. ADC has nothing like
 * this many extensions to name.
 */
static constexpr size_t MAX_SUPPORT_FLAGS = 64;

void ADC::HubSession::addSupport(const char* feature)
{
	if (!feature) return;

	/*
	 * Only while there is a session for the claim to belong to. What a hub
	 * supports is current rather than remembered - see removeSupport() - so a
	 * flag arriving once the session has ended is part of a conversation that is
	 * over, and taking it would leave this session answering for a hub it is no
	 * longer talking to.
	 *
	 * Every state above that one is accepted, and deliberately so: a hub is
	 * entitled to send another SUP after the login exchange, which is where a
	 * feature is added or withdrawn mid-session.
	 */
	if (userState == QuickDC::HubSession::StateNone) return;
	if (extensions.size() >= MAX_SUPPORT_FLAGS) return;

	extensions.emplace(feature);
}


/*
 * A hub may take back a feature it once offered, by naming it with 'RM' in a
 * later SUP, and what it says now is the whole of the answer.
 *
 * Nothing is assumed from what it said a moment ago, and nothing is kept against
 * the hub between sessions - each connection is a new HubSession, and so a new
 * set. A hub that has lost whatever it needed to serve a feature has no other
 * way of saying so, and a client still asking is one it has to answer with an
 * error every time.
 */
void ADC::HubSession::removeSupport(const char* feature)
{
	if (!feature) return;

	extensions.erase(feature);
}


void ADC::HubSession::onSupports()
{
	if (!listener)
		return;

	for (const auto& feature : extensions)
		listener->EventHubExtension(feature.c_str());

	/*
	 * And the one question that cannot be answered by naming what arrived.
	 *
	 * The loop above reports the whole set after every SUP, so a listener hears a
	 * feature it already knew about again, and it never hears of one being taken
	 * away - there is no shape in that notification for a withdrawal. Bulletin
	 * boards are the first feature where the difference matters: an interface with
	 * a board open has to close it when the hub withdraws BBS0, since every
	 * subscription went with it.
	 *
	 * So availability is re-derived from what the hub currently claims and stated
	 * outright, rather than inferred from what this SUP happened to mention. Stated
	 * every time and not only on a change: a current answer is idempotent, which is
	 * what makes it safe to repeat, and a listener drawing on it compares against
	 * what it last drew - see the Qt bridge, which does exactly that for rich text.
	 */
	listener->EventBoardsAvailable(supportsBulletins());
}


bool ADC::HubSession::supports(const char* feature) const
{
	/* Nothing is not a feature, and contains() would make a std::string of it. */
	return feature && extensions.contains(feature);
}


bool ADC::HubSession::supportsRichText() const
{
	return supports(ADC_EXT_RICHTEXT);
}


bool ADC::HubSession::mayUseBulletins() const
{
	return supportsBulletins() && userState == QuickDC::HubSession::StateNormal;
}


/*
 * BBL, which asks for entries: the whole of a board from a point in time, one
 * entry by its hash, or nothing further.
 *
 * TS is written whatever it is, a resumption from the beginning included. The
 * extension reads an absent TS as zero, so the two say the same thing, and saying
 * it costs three bytes and makes a capture read the way the request was meant.
 *
 * TR and TS may not both appear, and here they cannot: a subscription writes TS
 * and never TR, a request for one entry writes TR and never TS. That is the
 * invariant rather than a check, because the two are separate functions - a
 * single one taking both would have to refuse half of what it was given.
 */
bool ADC::HubSession::subscribeBoard(const char* board, uint64_t timestamp)
{
	if (!mayUseBulletins()) return false;
	if (!board || !ADC::isValidBoardName(board)) return false;

	ADC::Command* cmd = new ADC::Command(FOURCC('H','B','B','L'));
	cmd->addArgument("BD", board);
	cmd->addArgument("TS", std::to_string(timestamp));
	send(cmd);
	return true;
}


bool ADC::HubSession::unsubscribeBoard(const char* board)
{
	if (!mayUseBulletins()) return false;
	if (!board || !ADC::isValidBoardName(board)) return false;

	ADC::Command* cmd = new ADC::Command(FOURCC('H','B','B','L'));
	cmd->addArgument("BD", board);
	cmd->addArgument("RM", "1");
	send(cmd);
	return true;
}


bool ADC::HubSession::requestEntry(const char* board, const char* tth)
{
	if (!mayUseBulletins()) return false;
	if (!board || !ADC::isValidBoardName(board)) return false;
	if (!tth || !ADC::isValidTTH(tth)) return false;

	ADC::Command* cmd = new ADC::Command(FOURCC('H','B','B','L'));
	cmd->addArgument("BD", board);
	cmd->addArgument("TR", tth);
	send(cmd);
	return true;
}


/*
 * BBP, which submits a post the caller has already composed, hashed and made
 * servable.
 *
 * Four fields are the hub's to fill in and are never sent: the author, the
 * nickname it went out under, the thread root and the timestamp. A hub discards
 * them rather than refusing the command, as it discards PD from an INF, so this
 * is not about being accepted - it is that every one of them is a claim this end
 * is not entitled to make. ID and NI would be a client naming somebody else as
 * the author; TS would be a client choosing its own place in a board's order,
 * which is the order every cursor is made of; TH is not the client's to work out,
 * the hub knowing the thread of the post named by PA and a post without one
 * starting a thread whose root is its own hash.
 *
 * The subject is sent to match the document and is refused rather than repaired
 * where it carries a newline. A document whose subject contains one is refused by
 * every reader, so the post being submitted is one nobody could ever display, and
 * flattening it here would leave the index saying something the document does not.
 */
bool ADC::HubSession::submitPost(const char* tth, uint64_t size, const char* board,
                                 const char* subject, const char* parent)
{
	if (!mayUseBulletins()) return false;
	if (!tth || !ADC::isValidTTH(tth)) return false;
	if (!board || !ADC::isValidBoardName(board)) return false;

	/* A post document is a header and the newline that ends it at the very least,
	   so nothing that could be hashed is this long. */
	if (!size) return false;

	if (parent && *parent && !ADC::isValidTTH(parent)) return false;

	const std::string_view text(subject ? subject : "");
	if (text.find_first_of("\r\n") != std::string_view::npos) return false;

	ADC::Command* cmd = new ADC::Command(FOURCC('H','B','B','P'));
	cmd->addArgument("TR", tth);
	cmd->addArgument("SI", std::to_string(size));
	cmd->addArgument("BD", board);
	if (parent && *parent) cmd->addArgument("PA", parent);

	/* An empty subject is left out rather than sent as a field with nothing in
	   it: a post that starts a thread is required to have one and a reply is not,
	   and an empty SJ says a post has a subject which is the empty string. */
	if (!text.empty()) cmd->addArgument("SJ", subject);

	send(cmd);
	return true;
}


/*
 * And withdrawal, which is the same command carrying RM1 and the hash of the post
 * to take out of the index.
 *
 * No size, and no subject: what is being named is an entry the hub already holds,
 * and the fields that describe a post describe this one no longer - the subject of
 * a withdrawn post is usually the reason it was withdrawn.
 */
bool ADC::HubSession::withdrawPost(const char* tth, const char* board)
{
	if (!mayUseBulletins()) return false;
	if (!tth || !ADC::isValidTTH(tth)) return false;
	if (!board || !ADC::isValidBoardName(board)) return false;

	ADC::Command* cmd = new ADC::Command(FOURCC('H','B','B','P'));
	cmd->addArgument("TR", tth);
	cmd->addArgument("BD", board);
	cmd->addArgument("RM", "1");
	send(cmd);
	return true;
}


/*
 * Both of these are dropped before the session reaches the NORMAL state, as the
 * connect requests further down this file are.
 *
 * A hub sends a descriptor on a session's entry to NORMAL, after the INF exchange,
 * and an entry only to a session that subscribed or posted - neither of which can
 * have happened earlier. So nothing legitimate is lost, and what is refused is a
 * hub putting a board's title and description in front of a user it has not yet
 * accepted, at a point where nothing said about that board could be answered.
 */
void ADC::HubSession::onBoardDescription(const QuickDC::Board& board)
{
	if (!listener) return;
	if (userState != QuickDC::HubSession::StateNormal) return;

	listener->EventBoardDescribed(board);
}


void ADC::HubSession::onBoardEntry(const QuickDC::BoardEntry& entry)
{
	if (!listener) return;
	if (userState != QuickDC::HubSession::StateNormal) return;

	listener->EventBoardEntry(entry);
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

	/* As onHubName does above. Kept and reported to nobody, the description was
	   known to the session and to nothing that could draw it. */
	listener->EventHubDescription(hubdesc.c_str());
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

		/*
		 * A refusal of something this client said about a board, told apart from
		 * every other status by 'FC' and passed on whole.
		 *
		 * Taken before the switch below rather than as cases within it, because
		 * most of the codes a board is refused with are ones ADC already had for
		 * something else: a board this session may not post on and a login denied
		 * arrive as the same 25, and only the command named in FC distinguishes
		 * them. The five codes the extension asked for of its own are not in that
		 * switch at all and would come out as an unknown code in the log, which is
		 * where a rate limit and a board that no longer exists would have ended.
		 *
		 * Severity 1 only. Every one of these is recoverable by definition - none
		 * of them is a reason to disconnect a client - so a fatal status that
		 * happens to name BBL is about the connection and belongs with the rest of
		 * them below.
		 *
		 * And from the hub only. A DSTA is another client talking, and no other
		 * client is a party to a bulletin board at any point: this extension is
		 * spoken between a client and its hub, so a refusal from anywhere else is
		 * somebody on the hub inventing an answer to a post they know nothing
		 * about. It is not conditioned on the hub still announcing BBS0, though -
		 * a refusal may be the last thing said before the feature is withdrawn, and
		 * dropping that one would leave a submission pending for ever with nothing
		 * to explain it.
		 */
		if (code[0] == '1' && cmd->getFourcc() == FOURCC('I','S','T','A'))
		{
			/*
			 * Looked for past the two positional arguments, not from the start of
			 * the command. The code and the message come first and a message is
			 * arbitrary text, so a hub whose words happen to begin with a field
			 * name - "BDrop it", say - would otherwise have its sentence read as a
			 * field, and the sentence is the part a remote party chooses freely.
			 */
			const char* context = cmd->getArgument("FC", 2, 2);

			if (context && (!strcmp(context, "BBL") || !strcmp(context, "BBP")))
			{
				const char* board = cmd->getArgument("BD", 2, 2);
				const char* tth = cmd->getArgument("TR", 2, 2);

				/*
				 * Neither is passed on unless it is what it claims to be. A board
				 * name is protocol text that reaches a user interface and is never
				 * a path, and a hash that is not one names no post - see
				 * ADC::isValidBoardName(). A refusal is still worth reporting
				 * without them: the code and the hub's words are the message.
				 */
				if (board && !ADC::isValidBoardName(board)) board = 0;
				if (tth && !ADC::isValidTTH(tth)) tth = 0;

				QERR("The hub refused a bulletin board command: %s ('%s')",
					code, mesg ? mesg : "no reason given");

				listener->EventBoardRefused(Samurai::Util::Convert::to_int32(&code[1]),
					mesg ? mesg : "", board ? board : "", tth ? tth : "");
				return;
			}
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

				/* Not offered again on the next run only to be refused the same way. */
				session->remove(address, QuickDC::Settings::Password.key);
				session->write();
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

				QuickDC::Core* core = &getCore();

				/*
				 * The hub reporting on a connection it made itself, rather than
				 * relaying a peer's. ISTA is the hub speaking, and no 'TO' means
				 * this is not the answer to any transfer we asked for - so the only
				 * connection it can be about is the hub's own attempt at the port
				 * we published.
				 *
				 * Worth more than a peer's report, and treated so: see
				 * Reachability::connectionCheckFailed().
				 */
				const bool fromHub = cmd->getFourcc() == FOURCC('I','S','T','A');
				if (fromHub && !(job && *job))
				{
					QERR("The hub could not connect to the port we published: %s",
						mesg ? mesg : "no reason given");

					if (core->reachability)
					{
						/*
						 * Against both families, because the report names
						 * neither. A hub says its attempt failed and not which
						 * address it tried, so attributing this to one family
						 * would be a guess - and it is the weakest evidence
						 * there is, outranked by anything either endpoint has
						 * of its own, so it only decides a family that has
						 * nothing better to say.
						 */
						core->reachability->connectionCheckFailed();
						if (core->reachability6)
							core->reachability6->connectionCheckFailed();

						/* Said again without the claim, so peers stop being told
						   to connect somewhere nothing answers - and so NATT is
						   no longer suppressed by a claim that was false. */
						core->hubs->requestClientInfoUpdate();
					}
					break;
				}

				const bool ours = job && *job && core->queue
					&& core->queue->lookupRequest(job);

				if (ours && who && *who && core->reachability)
				{
					/* Both, and for the reason the hub's own failed check is; a
					   peer reports that it could not get in, never over what. */
					core->reachability->connectionRefused(who);
					if (core->reachability6) core->reachability6->connectionRefused(who);
				}
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
				getCore().hubs->requestClientInfoUpdate();
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
                                  bool richText, uint64_t timestamp)
{
	const QuickDC::User* u = users->getUser(from);
	/*
	 * Not reported when the transcript already held it. A hub replaying its
	 * history hands back lines this client wrote down the first time round, and
	 * the conversation on screen was filled from that same transcript as it
	 * opened - so passing them on would show everything twice.
	 */
	if (u)
	{
		if (recordChat(u, 0, message, 0, action, richText, timestamp))
			listener->EventChat(u, message, action, richText, timestamp);
	}
	else
	{
		if (recordChat(0, hubname.c_str(), message, 0, action, richText, timestamp))
			listener->EventHubMessage(hubname.c_str(), message, action, richText, timestamp);
	}
}


void ADC::HubSession::onPrivateChat(const char* to, const char* from, const char* message,
                                    const char* context, bool action, bool richText,
                                    uint64_t timestamp) {

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

		/*
		 * Filed against whoever is not us, by the id that outlasts the session
		 * rather than by the one it is routed with - see recordChat().
		 */
		const QuickDC::User* other = mine ? u2 : u;
		const char* cid = reinterpret_cast<const char*>(other->getClientID());
		const char* with = (cid && *cid) ? cid : other->getNick();

		if (recordChat(u, 0, message, with, action, richText, timestamp))
		{
			listener->EventPrivateChat(u, u2, message, mine ? to : from, action,
			                           richText, timestamp);
		}
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
	getCore().securityManager->grant(clearance);

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
	bool peerSecure = adc_is_tls_protocol(protocol);

	/*
	 * A plain connection is refused where this hub requires encryption, which on an
	 * ADCS hub is unconditional. That is also the downgrade this closes: the
	 * protocol field arrives in a message the hub relayed, so accepting a weaker one
	 * than we asked for would let whoever could write that field decide for us.
	 *
	 * On a plain ADC hub it stands or falls with the setting, and by default plain
	 * ADC/1.0 is a peer connection like any other - it is what a client without
	 * ADCS has to offer, and refusing it there refuses the transfer without
	 * protecting anything a plain hub could not already reach.
	 */
	if (!peerSecure && protocol == ADC_COMPLIANCE && mustEncryptPeers())
	{
		refuseUnencrypted(from, token, proto,
			secure ? "this hub was reached over ADCS and the peer answered with plain ADC"
			       : "the peer answered with plain ADC");
		return;
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

	auto csock = Samurai::IO::Net::Socket::create(getCore().loop(), nullptr, ip, port);
	ADC::ClientSession* connection = new ADC::ClientSession(getCore(), csock, this, token, from, peerSecure, peerKeyprint);
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
                                     uint16_t port, const char* token, ADC::PeerDial dial)
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
	const bool peerSecure = adc_is_tls_protocol(protocol);

	/* KEYP is the TLS client's check of the server's certificate; as the
	   server we are shown none to check. */
	const char* peerKeyprint = 0;
	if (peerSecure && user->hasKeyprint() && keyprintVerified && !ADC::peerRole(dial).tlsServer)
		peerKeyprint = user->getKeyprint();

	QDBG("Traversing to '%s' at %s:%d from our own %d", user->getNick(), ip,
		static_cast<int>(port), static_cast<int>(mine));

	auto csock = Samurai::IO::Net::Socket::create(getCore().loop(), nullptr, ip, port);
	if (!csock) return false;

	/* Before connect(), which is the only moment a local port can be chosen. */
	csock->setReuseLocalPort(mine);

	ADC::ClientSession* connection = new ADC::ClientSession(getCore(), csock, this, token,
		user->getID(), peerSecure, peerKeyprint);
	connection->traverse(dial, ip, port, mine);
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
	 * A traversed connection is a peer connection like any other, and the protocol
	 * it names is held to the same requirement - otherwise the one path a passive
	 * pair has left would also be the one way around it.
	 */
	if (!adc_is_tls_protocol(proto ? proto : "") && mustEncryptPeers())
	{
		refuseUnencrypted(from, token, proto,
			"the peer offered to traverse in the clear");
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

	dialFromHubPort(user, proto, port, token, ADC::PeerDial::SentRnt);
}


/* Our own request answered: dial, and say nothing further. */
void ADC::HubSession::onNatReply(const char* from, const char* proto,
                                 uint16_t port, const char* token)
{
	if (userState != QuickDC::HubSession::StateNormal) return;
	if (!sid.empty() && sid == from) return;

	const QuickDC::User* user = users->getUser(from);
	if (!user) return;

	/* The protocol our own DNAT named is the floor, as it is for a CTM answering
	   an RCM: an answer that drops the encryption we asked for is not an answer. */
	if (!adc_is_tls_protocol(proto ? proto : "") && mustEncryptPeers())
	{
		refuseUnencrypted(from, token, proto,
			"the peer answered our traversal request in the clear");
		return;
	}

	dialFromHubPort(user, proto, port, token, ADC::PeerDial::SentNat);
}


void ADC::HubSession::onTraversalFailed(const char* sid, const char* why)
{
	const QuickDC::User* user = (users && sid && *sid) ? users->getUser(sid) : 0;
	const std::string who = (user && user->getNick()) ? user->getNick() : "a peer";
	const std::string msg = "NAT traversal with '" + who + "' failed: " + why;

	QERR("%s", msg.c_str());
	if (listener) listener->EventSystemError(msg.c_str());
}


void ADC::HubSession::onPassiveConnect(const char* from, const char* proto, const char* token)
{
	if (userState != QuickDC::HubSession::StateNormal) return;

	if (!sid.empty() && sid == from) return;
	const QuickDC::User* user = users->getUser(from);
	if (!user) return;

	QDBG("Requested to setup connection to '%s' using '%s' (token=%s)", from, proto, token);

	QuickDC::Preferences* config = getCore().config;

	/*
	 * Answering an RCM means telling the peer to connect to us, so as in
	 * sendConnectionRequest() we may only name a protocol we can accept. This
	 * one was asked for rather than volunteered, though, so it is honoured
	 * whenever we are able to - the hub's own protocol has no say in it.
	 */
	const std::string_view requested(proto ? proto : "");
	bool peerSecure = adc_is_tls_protocol(requested);

	/* A protocol left out means plain ADC, which is what it meant here before
	   ADCS was an option at all. One we do not speak cannot be answered. */
	if (!requested.empty() && !peerSecure && requested != ADC_COMPLIANCE)
	{
		QERR("Peer asked for protocol '%s', which is not supported", proto);
		sendPeerError(from, adc_status_code(ADC_STATUS_SEVERITY_FATAL, ADC_CODE_PROTOCOL_UNSUPPORTED).c_str(),
			"Protocol not supported", token, proto);
		return;
	}

	if (peerSecure && !canAcceptTls(getCore()))
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
	if (!peerSecure && mustEncryptPeers())
	{
		if (!canAcceptTls(getCore()))
		{
			refuseUnencrypted(from, token, proto,
				"encryption is required and there is no certificate to accept it with");
			return;
		}

		refuseUnencrypted(from, token, proto,
			"the peer did not ask for ADCS");
		return;
	}

	/*
	 * The protocol the peer asked for, in the peer's own words.
	 *
	 * A CTM answering an RCM has to name what the RCM named: the peer dials on the
	 * strength of that field and compares it against the protocols it knows, so a
	 * client that asked for ADCS/0.10 and is answered with ADCS/1.0 reads the answer
	 * as a protocol it does not speak and refuses the connection it asked for. Both
	 * spellings mean the same TLS, and this end has no preference to express - it
	 * was invited.
	 */
	const char* protocol = requested.empty() ? ADC_COMPLIANCE : proto;

	/* The endpoint the INF advertised, for the same reason as in
	   sendConnectionRequest(): a gateway may have granted another port. */
	const QuickDC::Core* core = &getCore();
	const QuickDC::PeerEndpoint endpoint = QuickDC::peerEndpoint(
		QuickDC::networkModeKeysInForce(*config).activeMode,
		core->server != 0 && core->server->listening(false),
		QuickDC::Settings::read(*config, QuickDC::Settings::Address),
		observedAddress,
		core->peerPort(),
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
		getCore().securityManager->grant(clearance);

		send(cmd);
	} else if (canTraverse() && peerCanTraverse(user)) {
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
		getCore().securityManager->grant(clearance);

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

	/*
	 * Our own search, handed back to us: a broadcast goes to every session the hub
	 * has, and the sender is one of them. Answering it makes this client a source
	 * for a file it is itself looking for, which ends in it dialling its own port.
	 */
	if (request && request->getUser() && request->getUser() == getLocalUser()) return;

	QuickDC::Share::Manager* shares = getCore().shares;
	if (!shares) return;

	shares->search(request);

	/* A hash we do not share may still be one we are downloading. */
	if (request->isTTH() && !shares->getFileByTTH(request->getTTH()))
		sendPartialStatus(request);
}

void ADC::HubSession::sendPartialStatus(QuickDC::Share::SearchRequest* request)
{
	QuickDC::Core* core = &getCore();
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
	int port = QuickDC::networkModeKeysInForce(*config).activeMode ? core->peerPort() : 0;

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
		ADC::Command cmd(FOURCC('U','P','S','R'), getCore().identity->getCID());
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
	QuickDC::Core* core = &getCore();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	const char* url = (hub) ? hub->getUrl() : 0;

	if (queue && url) queue->onPartialSources(url, cid, tth, indexes);
}
