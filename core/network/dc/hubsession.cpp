/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdio.h>
#include <string.h>
#include <string_view>
#include <time.h>

#include "api/core.h"
#include "network/portforward.h"
#include "api/server.h"
#include "api/hub.h"
#include "config/preferences.h"
#include "network/connectionmanager.h"
#include "network/hubmanager.h"
#include "network/hubsession.h"
#include "network/securitymanager.h"
#include <samurai/io/net/url.h>
#include "network/usermanager.h"
#include "network/dc/clientsession.h"
#include "network/dc/commands.h"
#include "network/dc/dccommandproxy.h"
#include "network/dc/dcconnection.h"
#include "network/dc/hubsession.h"
#include "network/dc/hubcommands.h"
#include "network/dc/lock.h"
#include "network/leafhashes.h"
#include "network/downloadqueue.h"
#include "network/adc/parser.h"
#include "network/adc/cid.h"
#include "share/searchrequest.h"
#include "share/sharemanager.h"
#include <samurai/io/buffer.h>
#include <samurai/io/net/datagram.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/socketaddress.h>
#include "share/sharemanager.h"
#include "share/sharefile.h"
#include "share/searchrequest.h"
#include "share/searchresult.h"
#include "network/downloadqueue.h"
#include "network/filesource.h"
#include "network/user.h"
#include "config/settings.h"
#include <samurai/timer.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/datagram.h>
#include <samurai/io/buffer.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/inetaddress.h>

DC::HubSession::HubSession(QuickDC::Hub* ui, QuickDC::HubListener* listener_, Samurai::IO::Net::URL* url)
	: QuickDC::HubSession(ui)
	, QuickDC::Connection(ui->getCore(), url, "NMDC hub")
	, flags(0x1)
	, extendedProtocol(false)
	, state(None)
	, session(0)
	, proxy(0)
	, listener(listener_)
	, infoTimer(0)
{
	proxy = new DC::CommandProxy(socket, this);
	users = new QuickDC::UserManager(this);

	QuickDC::Preferences* config = getCore().config;

	/* Borrowed from the core, and shared with every other hub session; see
	   Core::session. Not ours to delete, and its group is not ours to leave set. */
	session = getCore().session;

	/*
	 * The canonical "{scheme}://{host}:{port}" rather than the address as it was
	 * typed: this names the group every setting for this hub is filed under, and one
	 * hub reached two ways is still one hub. See the same line in the ADC session.
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
		nickname = QuickDC::Settings::read(*config, QuickDC::Settings::Nickname);
		if (nickname.empty()) nickname = PRODUCT;
	}

	session->setTime("Last Connect", time(0));
	
	session->write();
}


DC::HubSession::~HubSession()
{
	/*
	 * Only to disk: nothing of the session's own identity is written here, or a
	 * name or password it merely used would outrank the setting for this hub.
	 */
	session->write();

	delete users;
	delete proxy;
	delete infoTimer;
}

void DC::HubSession::send(DC::Command* cmd, bool more)
{
	proxy->send(cmd);
	
	if (!more)
		proxy->write();

	resetIdleTimer();
}

void DC::HubSession::sendPrivateChatMessage(const QuickDC::User* user, const char* msg,
                                           bool richText)
{
	/* NMDC has no room to say a line is markup, so a caller must not claim it is. */
	(void) richText;

	if (!msg || !*msg) return;
	if (!user) return;

	const QuickDC::User* me = users->getUser(nickname.c_str());

	DC::Command* cmd = new DC::SendTo(me->getNick(), user->getNick(), msg);
	send(cmd);
	
	// Since a NMDC hub does not reflect priv messages back to us, we need
	// to do it here manually.
	const uint64_t now = static_cast<uint64_t>(time(0));

	if (recordChat(me, 0, msg, user->getNick(), false, false, now))
		listener->EventPrivateChat(me, user, msg, user->getNick(), false, false, now);
}

void DC::HubSession::sendChatMessage(const char* msg, bool richText)
{
	(void) richText;

	if (!msg || !*msg) return;

	const QuickDC::User* me = users->getUser(nickname.c_str());
	DC::Command* cmd = new DC::SendMessage(me->getNick(), msg);
	send(cmd);
}

void DC::HubSession::sendAdminUserKick(const QuickDC::User* user)
{
	if (!user) return;
	const QuickDC::User* me = users->getUser(nickname.c_str());
	if (me && me->isOp() && user)
	{
		DC::Command* cmd = new DC::SendKick(user->getNick());
		send(cmd);
	}
	else
	{
		QERR("ERROR: Unable to kick user '%s'. Check privileges and username?", user->getNick());
	}
}

void DC::HubSession::sendAdminUserRedirect(const QuickDC::User* user, const char* address, const char* reason)
{
	if (!user || !address || !*address || !reason || !*reason) return;

	const QuickDC::User* me = users->getUser(nickname.c_str());

	if (me && me->isOp() && user)
	{
		DC::Command* cmd = new DC::SendRedirect(user->getNick(), address, reason);
		send(cmd);
	}
	else
	{
		QERR("ERROR: Unable to redirect user '%s'. Check privileges and username?", user->getNick());
	}
}



void DC::HubSession::addSupport(const char* feature)
{
	if (feature) extensions.insert(feature);
}

bool DC::HubSession::supports(const char* feature)
{
	return (extendedProtocol && feature && extensions.count(feature) != 0);
}

void DC::HubSession::onLock(const char* lock_, const char* /*pk_*/) {
	/*
	 * $Lock is the first thing a hub says, so this runs against an unverified
	 * peer. calculateKey() has nothing to derive a key from below three
	 * characters, and a hub we cannot answer is one there is no login on.
	 */
	if (!lock_ || strlen(lock_) < 3)
	{
		QERR("Hub sent a lock too short to answer; dropping the connection");
		socket->disconnect();
		return;
	}

	lock = lock_;
	extendedProtocol = lock.starts_with("EXTENDEDPROTOCOL");

	/* A hub may send $Lock more than once, and each one derives a new key. */
	key = Lock::calculateKey(lock);
	if (key.empty())
	{
		QERR("Unable to derive a key from the hub's lock; dropping the connection");
		socket->disconnect();
		return;
	}

	if (extendedProtocol)
	{
		DC::Command* cmd = new DC::SendSupports();
		send(cmd, true);
	}

	{
		DC::Command* cmd = new DC::SendKey(key.c_str(), key.size());
		send(cmd, true);
	}

	{
		DC::Command* cmd = new DC::SendValidateNick(nickname.c_str());
		send(cmd, false);
	}
	
	state = Lock;
	
}

void DC::HubSession::onHubName(const char* hubname_)
{
	hubname = hubname_;
	listener->EventHubName(hubname.c_str());
}

void DC::HubSession::onHubTopic(const char* topic)
{
	hubtopic = topic;
	listener->EventHubDescription(hubtopic.c_str());
}

void DC::HubSession::onPublicChat(const char* from, const char* message)
{
	/* NMDC carries no timestamp, so arrival is the best that can be said. */
	const uint64_t now = static_cast<uint64_t>(time(0));

	const QuickDC::User* u = users->getUser(from);
	if (u)
	{
		if (recordChat(u, 0, message, 0, false, false, now))
			listener->EventChat(u, message, false, false, now);
	}
	else
	{
		if (recordChat(0, from, message, 0, false, false, now))
			listener->EventHubMessage(from, message, false, false, now);
	}
}

void DC::HubSession::onActionChat(const char* from, const char* message)
{
	/* NMDC carries no timestamp, so arrival is the best that can be said. */
	const uint64_t now = static_cast<uint64_t>(time(0));

	const QuickDC::User* u = users->getUser(from);
		
	if (u)
	{
		if (recordChat(u, 0, message, 0, true, false, now))
			listener->EventChat(u, message, true, false, now);
	}
	else
	{
		if (recordChat(0, from, message, 0, true, false, now))
			listener->EventHubMessage(from, message, false, false, now);
	}
}

void DC::HubSession::onPrivateChat(const char* to, const char* from, const char* message)
{
	/* NMDC carries no timestamp, so arrival is the best that can be said. */
	const uint64_t now = static_cast<uint64_t>(time(0));

	const QuickDC::User* u = users->getUser(from);
	const QuickDC::User* u2 = users->getUser(to);
	if (u)
	{
		/* No client id on NMDC, so the nickname is what lasts. */
		if (recordChat(u, 0, message, u->getNick(), false, false, now))
			listener->EventPrivateChat(u, u2, message, u->getNick(), false, false, now);
	}
	else
	{
		if (recordChat(0, from, message, 0, false, false, now))
			listener->EventHubMessage(from, message, false, false, now);
	}
}

void DC::HubSession::onRedirect(const char* host, uint16_t port)
{
	listener->EventHubRedirect(host, port);
}

void DC::HubSession::onBadPassword()
{
	password.clear();

	/*
	 * Cleared on disk as well as in memory, and now rather than when the
	 * session ends. The hub has said this password is wrong, so keeping it
	 * only means every later run offers it again and fails the same way.
	 */
	session->remove(address, QuickDC::Settings::Password.key);
	session->write();

	/* The hub still wants a password, which is the interface's problem. */
	listener->EventHubAutenticate();
}

void DC::HubSession::onLoggedIn()
{
	state = Idle;
	markEstablished();

	/* As ADC does. Without it a client cannot tell a hub it has merely reached
	   from one it is actually logged in to. */
	listener->EventHubStatus(QuickDC::HubListener::HubLoggedIn);

	/* One more hub we are on, which the others were told a count of. */
	getCore().hubs->requestClientInfoUpdate();
}

/**
 * A peer connection is only served once we have said we expect it. NMDC has no
 * token to name the request by, so the nick is what the clearance records and
 * what DC::ClientSession matches the peer's $MyNick against.
 */
void DC::HubSession::expectPeer(const char* nick)
{
	if (!nick || !*nick) return;

	QuickDC::SecurityManager* security = getCore().securityManager;
	if (!security) return;

	QDBG("Expecting a peer connection from '%s'", nick);
	security->grant(new QuickDC::Clearance(
			QuickDC::Clearance::AuthorizedUser,
			QuickDC::Clearance::NMDC,
			this,
			nick,
			0));
}

void DC::HubSession::onActiveConnect(const char* address, uint16_t port, const char* user)
{
	const QuickDC::User* u = (user) ? users->getUser(user) : 0;
	if (listener->EventClientConnect(address, port, u))
	{
		QDBG("Connecting to %s:%d...", address, port);

		/*
		 * We are dialling out because this peer asked us to, so the connection
		 * we are about to make is one we expect - the nick it will present is
		 * the one that asked.
		 */
		if (user) expectPeer(user);

		auto socket = Samurai::IO::Net::Socket::create(nullptr, address, port);
		DC::ClientSession* connection = new DC::ClientSession(getCore(), socket, this);
		socket->setEventHandler(connection);
		socket->connect();
	}
}

/**
 * FIXME: Time the expected connection out after 45 seconds.
 */
void DC::HubSession::onPassiveConnect(const char* user)
{
	const QuickDC::User* u = users->getUser(user);
	if (u)
	{
		if (listener->EventClientConnect(u))
		{
			const QuickDC::Core* core = &getCore();
			QuickDC::Preferences* config = core->config;
		
			const QuickDC::PeerEndpoint endpoint = QuickDC::peerEndpoint(
				QuickDC::Settings::read(*config, QuickDC::Settings::ActiveMode),
		core->server != 0 && core->server->listening(false),
				QuickDC::Settings::read(*config, QuickDC::Settings::Address),
				/* NMDC has no field for a hub to report the address it sees. */
				std::string(),
				static_cast<uint16_t>(QuickDC::Settings::read(*config, QuickDC::Settings::Port)),
				core->portforward);

			if (endpoint.active)
			{
				/* Telling them where to reach us is what makes the inbound
				   connection an expected one. */
				expectPeer(user);

				DC::Command* cmd = new DC::SendConnectToMe(
					user, endpoint.address.c_str(), endpoint.port);
				send(cmd);
			}
		}
	}
}

void DC::HubSession::sendConnectionRequest(const QuickDC::User* u, const char*)
{
	const QuickDC::Core* core = &getCore();
	QuickDC::Preferences* config = core->config;
	DC::Command* cmd = 0;

	/* Either branch below ends with this peer connecting to us or us to them. */
	expectPeer(u->getID());

	const QuickDC::PeerEndpoint endpoint = QuickDC::peerEndpoint(
		QuickDC::Settings::read(*config, QuickDC::Settings::ActiveMode),
		core->server != 0 && core->server->listening(false),
		QuickDC::Settings::read(*config, QuickDC::Settings::Address),
		/* NMDC has no field for a hub to report the address it sees. */
		std::string(),
		static_cast<uint16_t>(QuickDC::Settings::read(*config, QuickDC::Settings::Port)),
		core->portforward);

	if (endpoint.active)
	{
		QDBG("Requesting active connection to user: %s", u->getNick());
		cmd = new DC::SendConnectToMe(
			u->getID(), endpoint.address.c_str(), endpoint.port);
	}
	else
	{
		QDBG("Requesting passive connection to user: %s", u->getNick());
		cmd = new DC::SendRevConnectToMe(nickname.c_str(), u->getID());
	}
	send(cmd);
}


void DC::HubSession::onRequestMyInfo(const char* nick_)
{
	if (nickname != nick_)
	{
		// FIXME: change nickname to whatever the hub suggests for us
		//        this requires change in other layers also.
		nickname = nick_;
	}
	
	state = Nick;
	
	send(new DC::SendVersion(), true);
	send(new DC::SendGetNickList(), true);
	sendMyInfo();
}

void DC::HubSession::sendMyInfo()
{
	DC::Command* cmd = new DC::SendMyInfo(getCore(), getNick(), getDescription(), getEmail(), getCore().shares->getSharedSize());
	send(cmd);
}


/*
 * Whether this session may send the password, and say so if it may not.
 *
 * NMDC puts the password on the wire as typed. There is no hashed exchange in the
 * protocol and no TLS path for it anywhere in this tree, so anyone between here
 * and the hub reads it, and so does the hub itself - which for a password reused
 * elsewhere is the part that outlasts the session. Nothing about that is
 * apparent from the client's side, where an NMDC hub asking to log in looks like
 * an ADC hub asking to log in, which is what this makes visible.
 *
 * Permitted unless the user says otherwise, because there is nothing to fall back
 * to: refusing by default would end every registered NMDC login without offering
 * a way to keep one. That default is the product decision this leaves open, not a
 * claim that sending it is safe.
 */
bool DC::HubSession::maySendPassword()
{
	if (password.empty()) return true;

	QuickDC::Preferences* config = getCore().config;

	if (!QuickDC::Settings::read(*config, QuickDC::Settings::AllowPlaintextPasswords))
	{
		QERR("Refusing to send a password to '%s' in the clear", address.c_str());
		if (listener)
			listener->EventSystemError("This hub cannot encrypt, and sending "
				"passwords in the clear is turned off - not logging in");
		return false;
	}

	QERR("Sending a password to '%s' in the clear; NMDC cannot do otherwise",
		address.c_str());
	if (listener)
		listener->EventSystemError("Sending your password to this hub in the "
			"clear: NMDC has no way to encrypt it, so anyone on the path can "
			"read it");

	return true;
}

void DC::HubSession::onRequestPasword()
{
	state = Password;

	/*
	 * An empty password is not an answer and the hub will refuse it, but it is
	 * still sent: waiting for the interface to supply one would turn a login
	 * that fails immediately into one that hangs. The refusal comes back as
	 * $BadPass, which leaves the session here in the password state, so a
	 * password supplied then is sent as a second attempt.
	 */
	if (password.empty())
		listener->EventHubAutenticate();

	if (!maySendPassword()) return;

	send(new DC::SendMyPass(getPassword()));
}

void DC::HubSession::sendPassword(const char* password_)
{
	if (state != Password || !password_) return;

	password = password_;

	/*
	 * Written out now rather than left to the destructor, so a password the
	 * user just supplied survives a crash or a kill.
	 */
	QuickDC::Settings::writeForHub(*session, address, *getCore().config, QuickDC::Settings::Password, password);
	session->write();

	if (!maySendPassword()) return;

	send(new DC::SendMyPass(getPassword()));
}

/*
 * $ValidateDenide: the hub will not have the nickname we asked for. It arrives
 * before anything is authenticated and a hub may send it at any time.
 *
 * The nickname is kept rather than cleared. Every other reader of it - the
 * destructor writing the session file, the user lookups, $MyNick on a peer
 * connection - needs a name to work with, and there is no better one to be had
 * until the interface supplies one.
 */
void DC::HubSession::onBadNick()
{
	QERR("The hub refused the nickname '%s'", nickname.c_str());
	state = None;
	listener->EventHubStatus(QuickDC::HubListener::HubErrorNickTaken);
}

void DC::HubSession::sendSearchRequest(QuickDC::Share::SearchRequest* request)
{
	(void) request;
}

void DC::HubSession::onHubFull()
{
	
}

void DC::HubSession::onNickList()
{
	
}

void DC::HubSession::onOpList()
{

}

void DC::HubSession::onSearch(QuickDC::Share::SearchRequest* request)
{
	/*
	 * Our own search, handed back to us; see ADC::HubSession::onSearch(). Only a
	 * passive search says who sent it, so that is the half this can catch.
	 */
	if (request && request->getUser() && request->getUser() == getLocalUser()) return;

	QuickDC::Share::Manager* shares = getCore().shares;
	if (!shares) return;

	shares->search(request);

	/* A hash we do not share may still be one we are downloading. */
	if (request->isTTH() && !shares->getFileByTTH(request->getTTH()))
		sendPartialStatus(request);
}

void DC::HubSession::sendPartialStatus(QuickDC::Share::SearchRequest* request)
{
	QuickDC::Core* core = &getCore();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	if (!queue || !request || !request->getTTH() || !core->server) return;

	/* A PSR is an ADC message, which a passive searcher's hub will not relay,
	   so only an active one can be answered - which is what a token in
	   "host:port" form says the searcher is. */
	const char* replyTo = request->getToken();
	if (!replyTo) return;

	const std::string_view where(replyTo);
	const size_t cut = where.find_last_of(':');
	if (cut == std::string_view::npos) return;

	QuickDC::QueueFile* file = queue->getFileByTTH(request->getTTH());
	if (!file) return;

	QuickDC::LeafHashes* leaves = file->getLeaves();
	if (!leaves || !leaves->isLoaded()) return;

	uint64_t block = leaves->getLeafSize();
	size_t runs = file->getChunks().countBlockRuns(block);
	if (!runs) return;

	std::string indexes = file->getChunks().toIndexString(block);
	if (indexes.empty()) return;

	const std::string host(where.substr(0, cut));
	const std::string port(where.substr(cut + 1));

	std::string hubIpPort;
	if (hub && hub->getUrl())
	{
		hubIpPort = hub->getUrl();
		size_t scheme = hubIpPort.find("://");
		if (scheme != std::string::npos) hubIpPort = hubIpPort.substr(scheme + 3);
	}

	QuickDC::Preferences* config = core->config;
	int myPort = QuickDC::Settings::read(*config, QuickDC::Settings::ActiveMode) ? QuickDC::Settings::read(*config, QuickDC::Settings::Port) : 0;

	/* NI is what identifies us on an NMDC hub, where we have no client ID. */
	ADC::Command cmd(FOURCC('U','P','S','R'), getCore().identity->getCID());
	cmd.addArgument("NI", nickname);
	cmd.addArgument("HI", hubIpPort.c_str());
	cmd.addArgument("U4", std::to_string(myPort));
	cmd.addArgument("TR", request->getTTH());
	cmd.addArgument("PC", std::to_string(static_cast<int>(runs)));
	cmd.addArgument("PI", indexes.c_str());

	Samurai::IO::Buffer buffer;
	cmd.write(&buffer);
	Samurai::IO::Net::DatagramPacket packet(&buffer);
	Samurai::IO::Net::InetSocketAddress peer(host.c_str(), Samurai::Util::Convert::to_int32(port.c_str()),
		Samurai::IO::Net::InetAddress::Version::IPv4);
	packet.setAddress(&peer);
	core->server->send(&packet);

	QDBG("Offered %d run(s) of '%s' to %s", static_cast<int>(runs), request->getTTH(), replyTo);
}

/**
 * A search reply came back. Two things want to know: the download queue, for
 * which this may be a new source, and the user interface.
 *
 * Takes ownership of 'result'.
 */
void DC::HubSession::onSearchResult(QuickDC::Share::SearchResult* result)
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
	 * Match on the TTH, not the name: the point of queueing by hash is that
	 * the same file is called something else on someone else's disk. A reply
	 * without a TTH is still shown, it just cannot become a source. And never
	 * ourselves; see ADC::HubSession::onSearchResult().
	 */
	if (queue && hubUrl && result->getTTH() && result->getUser()
		&& result->getUser() != getLocalUser())
	{
		QuickDC::QueueFile* qfile = queue->getFileByTTH(result->getTTH());

		/* NMDC has no CID, so a source is keyed on the nick. */
		const char* nick = result->getUser()->getNick();

		if (qfile && nick && *nick)
		{
			if (!qfile->getSize() && result->getSize())
				qfile->setSize(result->getSize());

			if (!qfile->findSource(hubUrl, nick))
			{
				/* dchub://host:port/{nick}/{path} - see filesource.h */
				qfile->addSource(new QuickDC::FileSource(hubUrl, nick, result->getFile()));
				QDBG("A search reply gave us a new source for '%s'", qfile->getLocalFilename());
			}
		}
	}

	if (listener) listener->EventSearchResult(result);

	delete result;
}

void DC::HubSession::EventUserJoin(const char* nick)
{
	listener->EventUserJoin(nick);
}

void DC::HubSession::EventUserQuit(const QuickDC::User* data, const char* msg)
{
	listener->EventUserLeave(data, msg);
}

void DC::HubSession::EventUserInfo(const QuickDC::User* data)
{
	listener->EventUserUpdate(data);
}

void DC::HubSession::resetIdleTimer()
{
	delete infoTimer;
	infoTimer = new Samurai::Timer(this, 300, false);
}

void DC::HubSession::EventTimeout(Samurai::Timer*)
{
	// FIXME: Check wether or not we should send an updated
	//        $MyINFO instead
	// FIXME: Reset the timer whenever we send something.

	send(new DC::SendEmptyCommand());
}

bool DC::HubSession::knowUser(const char* username)
{
	return (users->getUser(username) != 0);
}

const QuickDC::User* DC::HubSession::getUser(const char* username)
{
	return users->getUser(username);
}

const QuickDC::User* DC::HubSession::getLocalUser()
{
	return users->getUser(nickname.c_str());
}

QuickDC::UserManager* DC::HubSession::getUserManager()
{
	return users;
}

void DC::HubSession::connect()
{
	reconnect_time = 60;
	proxy->connect();
}

void DC::HubSession::disconnect()
{
	proxy->close();
}

void DC::HubSession::EventHostLookup(const Samurai::IO::Net::Socket*)
{
	listener->EventNetStatus(QuickDC::HubListener::NetLookup);
}

void DC::HubSession::EventHostFound(const Samurai::IO::Net::Socket*) { /* ignored */ }

void DC::HubSession::EventConnecting(const Samurai::IO::Net::Socket*)
{
	listener->EventNetStatus(QuickDC::HubListener::NetConnecting);
}

void DC::HubSession::EventConnected(const Samurai::IO::Net::Socket*)
{
	manager->add(this);
	listener->EventNetStatus(QuickDC::HubListener::NetConnected);
	resetIdleTimer();
}

void DC::HubSession::EventTimeout(const Samurai::IO::Net::Socket*)
{
	manager->remove(this);
	listener->EventNetStatus(QuickDC::HubListener::NetErrorTimeout);
	hub->onDisconnected(QuickDC::HubListener::NetErrorTimeout);
}

void DC::HubSession::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	/* As the ADC session does. Without this the session goes on reporting
	   itself logged in to a hub it is no longer connected to, and the hub
	   manager searches on it and counts it. */
	state = None;

	manager->remove(this);
	listener->EventNetStatus(QuickDC::HubListener::NetDisconnected);
	hub->onDisconnected(QuickDC::HubListener::NetDisconnected);
	delete infoTimer; infoTimer = 0;
}

void DC::HubSession::EventDataAvailable(const Samurai::IO::Net::Socket*)
{
	proxy->read();
}

void DC::HubSession::EventCanWrite(const Samurai::IO::Net::Socket*)
{
	proxy->write();
}

void DC::HubSession::EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError error, const char*)
{
	manager->remove(this);
	
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
			QERR("Network error %d on '%s'", static_cast<int>(error),
				hub ? hub->getUrl() : "");
	}

	listener->EventNetStatus(why);
	hub->onDisconnected(why);
	delete infoTimer; infoTimer = 0;
}

bool DC::HubSession::isLoggedIn() const
{
	return state == Idle;
}

bool DC::HubSession::isSecure() const
{
	return socket && socket->isSecure();
}

void DC::HubSession::EventSearchReply(QuickDC::Share::SearchReply* reply)
{
	(void) reply;
#if 0
	QuickDC::Share::SearchRequest* req = reply->getRequest();
	const QuickDC::User* user = reply->getRequest()->getUser();
	QuickDC::Share::File* file = reply->getFile();
	
	std::string filename = file->getPublicName();
	if (filename[0] == '\\')
		filename = filename.substr(1);
	
	size_t n = filename.find('/');
	while (n != std::string::npos)
	{
		filename.replace(n, 1, 1, '\\');
		n = filename.find('/');
	}
	
	
	QuickDC::Preferences* config = getCore().config;
	int numslots = QuickDC::Settings::read(*config, QuickDC::Settings::Slots);
	
	Samurai::IO::Buffer buffer;

	buffer.append("$SR ");
	buffer.append(getNick());
	buffer.append(' ');
	
	if (1) { /* FILENAME */
		buffer.append(filename);
		buffer.append(static_cast<char>(0x05));
		buffer.append(file->getSize());
	} else { /* DIRECTORY */
		buffer.append(filename);
	}
	
	buffer.append(' ');
	buffer.append(0); /* free slots */
	buffer.append('/');
	buffer.append(numslots);
	buffer.append(static_cast<char>(0x05));
	if (file->getTTH()) {
		buffer.append("TTH:");
		buffer.append(file->getTTH());
	} else {
		buffer.append(hubname);
	}
	buffer.append(" (");
	buffer.append(&address[8]); /* Strip away 'dchub://' */
	buffer.append(')');
	
	if (user) {
		/* Send passive result -- through hub */
		buffer.append(static_cast<char>(0x05));
		buffer.append(user->getID());
		buffer.append("|");
		proxy->write(buffer.pop(buffer.size()));
		proxy->write();
		resetIdleTimer();
	
	} else {
		/* Send active result -- address is stored in token. */
		buffer.append("|");
		char* addr = strdup(req->getToken());
		char* pos = strchr(addr, ':');
		if (pos && getCore().server) {
			pos[0] = '\0';
			Samurai::IO::Net::DatagramPacket packet(&buffer);
			Samurai::IO::Net::InetSocketAddress peerAddr(addr, Samurai::Util::Convert::to_int32(&pos[1]), Samurai::IO::Net::InetAddress::Version::IPv4);
			packet.setAddress(&peerAddr);
			getCore().server->send(&packet);
		}
		free(addr);
	}
#endif // 0
}

