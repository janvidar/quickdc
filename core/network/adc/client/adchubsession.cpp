/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <string.h>
#include <time.h>

#include "quickdc.h"
#include "api/core.h"
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


ADC::HubSession::HubSession(QuickDC::Hub* ui, QuickDC::HubListener* listener_, Samurai::IO::Net::URL* url, bool secure_)
	: QuickDC::HubSession(ui)
	, QuickDC::Connection(url, secure_ ? "ADCS hub" : "ADC hub")
	, address(0)
	, proxy(0)
	, listener(listener_)
	, infoTimer(0)
	, passwordToken(0)
	, sid(0)
	, cid(0)
	, pid(0)
	, hubname(0)
	, hubdesc(0)
	, hubversion(0)
	, secure(secure_)
	, expectedKeyprint(0)
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
		expectedKeyprint = strdup(keyprint.c_str());

	cid = (char*) malloc(40);
	pid = (char*) malloc(40);

	proxy = new ADC::CommandProxy(socket, this);
	users = new QuickDC::UserManager(this);

	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;

	/* Borrowed from the core, and shared with every other hub session; see
	   Core::session. Not ours to delete, and its group is not ours to leave set. */
	session = QuickDC::Core::getInstance()->session;

	address = strdup(url->toString().c_str());
	config->setGroup("User");
	session->setGroup(address);

	/*
	 * Load settings from session file, or defaults if no session is found.
	 * This will incidentally create such a session.
	 */
	nickname = strdup(session->getString("Nickname", config->getString("Nickname", PRODUCT)));
	description = strdup(session->getString("Description", config->getString("Description", PRODUCT " user")));
	email = strdup(session->getString("E-mail", config->getString("E-mail", "")));
	password = strdup(session->getString("Password", config->getString("Password", "")));

	/* A hub will never accept an empty nick, and older versions could store one
	 * in the session file. Fall back to the configured nick in that case. */
	if (!*nickname)
	{
		free(nickname);
		const char* fallback = config->getString("Nickname", PRODUCT);
		nickname = strdup(*fallback ? fallback : PRODUCT);
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

	while (extensions.size())
	{
		char* feature = *extensions.begin();
		extensions.erase(extensions.begin());
		free(feature);
	}


	free(hubname);
	free(passwordToken);

	free(nickname);
	free(password);
	free(email);
	free(description);
	free(cid);
	free(pid);
	free(sid);
	free(address);
	free(hubversion);
	free(expectedKeyprint);
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
		trusted = expectedKeyprint;
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
	return address;
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
	hub->onDisconnected();
	delete infoTimer; infoTimer = 0;
}


void ADC::HubSession::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	userState = QuickDC::HubSession::StateNone;
	listener->EventNetStatus(QuickDC::HubListener::NetDisconnected);
	manager->remove(this);
	hub->onDisconnected();
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
	switch (error)
	{
		case Samurai::IO::Net::SocketError::ConnectionTimeout:
			listener->EventNetStatus(QuickDC::HubListener::NetErrorTimeout);
			break;
		case Samurai::IO::Net::SocketError::ConnectionRefused:
			listener->EventNetStatus(QuickDC::HubListener::NetErrorConnectionRefused);
			break;
		case Samurai::IO::Net::SocketError::HostNotFound:
			listener->EventNetStatus(QuickDC::HubListener::NetErrorHostNotFound);
			break;
		default:
			printf("Network error: %d: %s\n", (int) error, msg);
			listener->EventNetStatus(QuickDC::HubListener::NetErrorIO);
	}
	hub->onDisconnected();
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
	if (expectedKeyprint && !QuickDC::Keyprint::isValid(expectedKeyprint))
	{
		QERR("The hub address carries a keyprint we cannot use: '%s'", expectedKeyprint);
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
	if (sid && strcmp(data->getID(), sid) == 0)
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
		ADC::Command* cmd = new ADC::Command(FOURCC('D','R','E','S'), sid, user->getID());
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
		(int) result->getSize(),
		result->getUser() ? result->getUser()->getNick() : "");

	/*
	 * Match on the TTH rather than the name: the whole point of queueing by
	 * hash is that the same file can be called different things elsewhere.
	 */
	if (queue && hubUrl && result->getTTH() && result->getUser())
	{
		QuickDC::QueueFile* qfile = queue->getFileByTTH(result->getTTH());
		const char* cid = (const char*) result->getUser()->getClientID();

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
	return users->getUser(sid);
}


QuickDC::UserManager* ADC::HubSession::getUserManager()
{
	return users;
}


/* API wise */
void ADC::HubSession::sendPrivateChatMessage(const QuickDC::User* user, const char* msg)
{
#ifdef DATADUMP
	QDBG("Sending message to '%s', msg='%s'", user->getNick(), msg);
#endif
	ADC::Command* cmd = new ADC::Command(FOURCC('E','M','S','G'), sid, user->getID());
	cmd->addArgument(msg);
	cmd->addArgument("PM", sid);
	send(cmd);
}


void ADC::HubSession::sendChatMessage(const char* msg)
{
	ADC::Command* cmd = new ADC::Command(FOURCC('B','M','S','G'), sid);
	cmd->addArgument(msg);
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

	ADC::Command* cmd = new ADC::Command(FOURCC('B','I','N','F'), sid);
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
	QDBG("Active mode %d (server=%p)\n", config->getBool("Active mode") ? 1 : 0, QuickDC::Core::getInstance()->server);

	if (config->getBool("Active mode") && QuickDC::Core::getInstance()->server)
	{
		cmd->addArgument("I4", config->getString("Address", "0.0.0.0"));
		cmd->addArgument("U4", std::to_string(config->getNumber("Port")));
		cmd->addArgument("SU", "ADC0,TCP4,UDP4");
	}
	else
	{
		cmd->addArgument("SU", "ADC0");
	}
	send(cmd);
}


void ADC::HubSession::sendSearchRequest(QuickDC::Share::SearchRequest* request)
{
	if (!request) return;

	ADC::Command* cmd = new ADC::Command(FOURCC('B','S','C','H'), sid);

	if (request->isTTH())
	{
		cmd->addArgument("TR", request->getTTH());
	}
	else
	{

		for (size_t n = 0; n < request->include.size(); n++)
			cmd->addArgument("AN", request->include[n]);

		for (size_t n = 0; n < request->exclude.size(); n++)
			cmd->addArgument("NO", request->exclude[n]);

		for (size_t n = 0; n < request->extension.size(); n++)
			cmd->addArgument("EX", request->extension[n]);

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

	if (request->token) cmd->addArgument("TO", request->token);
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
	if (config->getBool("Active mode") && QuickDC::Core::getInstance()->server)
	{
		QDBG("Requesting active connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','C','T','M'), sid, user->getID());
		cmd->addArgument((secure && canAcceptTls()) ? ADC_TLS_PROTO : ADC_COMPLIANCE);
		cmd->addArgument(std::to_string(config->getNumber("Port")).c_str());
		cmd->addArgument(token);
		send(cmd);

		/* Add to securitymanager */
		QuickDC::Clearance* clearance = new QuickDC::Clearance(
				QuickDC::Clearance::AuthorizedUser,
				QuickDC::Clearance::ADC,
				this,
				(const char*) user->getClientID(),
				token);
		QuickDC::Core::getInstance()->securityManager->grant(clearance);

	}
	else
	{
		QDBG("Requesting passive connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','R','C','M'), sid, user->getID());

		cmd->addArgument(secure ? ADC_TLS_PROTO : ADC_COMPLIANCE);
		cmd->addArgument(token);

		/* Add to securitymanager */
		QuickDC::Clearance* clearance = new QuickDC::Clearance(
				QuickDC::Clearance::AuthorizedUser,
				QuickDC::Clearance::ADC,
				this,
				(const char*) user->getClientID(),
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

	if (sid) free(sid);
	sid = strdup(sid_);
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
		case QuickDC::HubSession::StateNormal:
			break;
	}
	users->info(id, data, oldShare, changed);
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

	free(passwordToken);
	passwordToken = strdup(token);

	/*
	 * Hashing the token with an empty password produces an answer the hub will
	 * refuse, but it is still sent: waiting for the interface to supply one
	 * would turn a login that fails immediately into one that hangs. The token
	 * is kept, so a password supplied while the session is still here is sent
	 * as a second attempt.
	 */
	if (!password || !*password)
		listener->EventHubAutenticate();

	sendPasswordResponse();
}

void ADC::HubSession::sendPassword(const char* password_)
{
	/* Only meaningful in answer to a request, and only derivable while the
	   token that came with it is still around. */
	if (userState != QuickDC::HubSession::StateVerify || !passwordToken) return;
	if (!password_) return;

	free(password);
	password = strdup(password_);

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
	if (!passwordToken || !password) return;

	char password_encoded[40] = { 0, };

	size_t tok_size = (strlen(passwordToken) * 5) >> 3;
	size_t size = strlen(password) + tok_size;
	uint8_t* tok_decoded = new uint8_t[tok_size ? tok_size : 1];
	uint8_t* str = new uint8_t[size ? size : 1];

	Samurai::Util::base32_decode(passwordToken, std::span<unsigned char>(tok_decoded, tok_size));

	memcpy(&str[0], password, strlen(password));
	memcpy(&str[strlen(password)], tok_decoded, tok_size);

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
	extensions.insert(strdup(feature));
}


bool ADC::HubSession::supports(const char* feature)
{
	char* f = strdup(feature);
	bool found = extensions.count(f);
	free(f);
	return found;
}


void ADC::HubSession::onHubName(char* txt)
{
	if (hubname) free(hubname);
	hubname = strdup(txt);

	/* As the NMDC session does. Without this the name is only ever used as a
	   label on hub messages, and the interface never learns it. */
	listener->EventHubName(hubname);
}


void ADC::HubSession::onHubDesc(char* txt)
{
	if (hubdesc) free(hubdesc);
	hubdesc = strdup(txt);
}


void ADC::HubSession::onHubVersion(char* txt)
{
	if (hubversion) free(hubversion);
	hubversion = strdup(txt);
}


void ADC::HubSession::onStatus(ADC::Command* cmd)
{
	char* code = cmd->getArgument(0);
	char* mesg = cmd->getArgument(1);

	bool fatal = false;

	if (code && strlen(code) == 3)
	{
		// Determine severity.
		switch (code[0]) {
			case '1':
				fatal = false;
				break;
			case '2':
				fatal = true;
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

			case ADC_CODE_PROTOCOL_CONNECT_ERROR: /* Specify: 'TO' (token), 'PR' (protocol). Context CTM/RCM */
				QERR("p2p connect error");
				break;

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


			case ADC_CODE_PROTOCOL_IP_INF_ERROR: /* Specify: 'IP' (ip address). Context: INF */
				/* Should not occur. */
				QERR("IP address is wrong");
				break;


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


void ADC::HubSession::onPublicChat(const char* from, const char* message, bool action)
{
	const QuickDC::User* u = users->getUser(from);
	if (u)
	{
		listener->EventChat(u, message, action);
	}
	else
	{
		listener->EventHubMessage(hubname, message);
	}
}


void ADC::HubSession::onPrivateChat(const char* to, const char* from, const char* message, const char* context, bool action) {

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
		const bool mine = sid && strcmp(from, sid) == 0;

		listener->EventPrivateChat(u, u2, message, mine ? to : from, action);
	}
}


void ADC::HubSession::onActiveConnect(const char* from, const char* proto, uint16_t port, const char* token)
{
	if (userState != QuickDC::HubSession::StateNormal) return;

	if (strcmp(from, sid) == 0) return;
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
			(const char*) user->getClientID(),
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
	bool peerSecure = (proto && !strcmp(proto, ADC_TLS_PROTO));

	if (!peerSecure && (!proto || strcmp(proto, ADC_COMPLIANCE)))
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

	if (peerSecure && *user->getKeyprint())
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


void ADC::HubSession::onPassiveConnect(const char* from, const char* proto, const char* token)
{
	if (userState != QuickDC::HubSession::StateNormal) return;

	if (strcmp(from, sid) == 0) return;
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
	bool peerSecure = (proto && !strcmp(proto, ADC_TLS_PROTO));

	/* A protocol left out means plain ADC, which is what it meant here before
	   ADCS was an option at all. One we do not speak cannot be answered. */
	if (proto && *proto && !peerSecure && strcmp(proto, ADC_COMPLIANCE))
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

	const char* protocol = peerSecure ? ADC_TLS_PROTO : ADC_COMPLIANCE;

	if (config->getBool("Active mode") && QuickDC::Core::getInstance()->server) {
		QDBG("Requesting active connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','C','T','M'), sid, user->getID());
		cmd->addArgument(protocol);
		cmd->addArgument(std::to_string(config->getNumber("Port")).c_str());
		if (token) cmd->addArgument(token);

		/* Add to securitymanager */
		QuickDC::Clearance* clearance = new QuickDC::Clearance(
				QuickDC::Clearance::AuthorizedUser,
				QuickDC::Clearance::ADC,
				this,
				(const char*) user->getClientID(),
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
	ADC::Command* cmd = new ADC::Command(FOURCC('D','S','T','A'), sid, remote_sid);
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
		cmd.addArgument("PC", std::to_string((int) runs));
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
		ADC::Command* cmd = new ADC::Command(FOURCC('D','P','S','R'), sid, user->getID());
		cmd->addArgument("HI", hubIpPort.c_str());
		cmd->addArgument("U4", std::to_string(port));
		cmd->addArgument("TR", request->getTTH());
		cmd->addArgument("PC", std::to_string((int) runs));
		cmd->addArgument("PI", indexes.c_str());
		send(cmd);
	}

	QDBG("Offered %d of %d block(s) of '%s'", (int) blocks,
		(int) ((file->getSize() + block - 1) / block), request->getTTH());
}

void ADC::HubSession::onPartialStatus(const char* cid, const char* tth, const char* indexes)
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	const char* url = (hub) ? hub->getUrl() : 0;

	if (queue && url) queue->onPartialSources(url, cid, tth, indexes);
}
