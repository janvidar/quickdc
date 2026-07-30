/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "api/core.h"
#include "api/server.h"
#include "api/hub.h"
#include "config/preferences.h"
#include "network/connectionmanager.h"
#include "network/hubmanager.h"
#include "network/hubsession.h"
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
#include <samurai/timer.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/datagram.h>
#include <samurai/io/buffer.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/inetaddress.h>

DC::HubSession::HubSession(QuickDC::Hub* ui, QuickDC::HubListener* listener_, Samurai::IO::Net::URL* url)
	: QuickDC::HubSession(ui)
	, QuickDC::Connection(url, "NMDC hub")
	, nickname(0)
	, password(0)
	, description(0)
	, email(0)
	, hubname(0)
	, lock(0)
	, key(0)
	, flags(0x1)
	, extendedProtocol(false)
	, state(None)
	, session(0)
	, address(0)
	, proxy(0)
	, listener(listener_)
	, infoTimer(0)
{
	proxy = new DC::CommandProxy(socket, this);
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


DC::HubSession::~HubSession()
{
	/* erase() invalidates the iterator, so it cannot be the one the loop then
	   increments - take the first element each time round instead. */
	while (!extensions.empty())
	{
		std::set<char*>::iterator it = extensions.begin();
		char* feature = *it;
		extensions.erase(it);
		free(feature);
	}

	// save session data.
	/* The group is shared, and every other hub session moves it, so it is
	   selected here rather than relied on from the constructor. */
	session->setGroup(address);
	session->setString("Nickname", nickname);
	session->setString("Password", password);
	session->write();
	
	
	
	free(address);
	free(password);
	free(email);
	free(description);
	free(hubname);
	free(nickname);
	free(lock);
	free(key);
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

void DC::HubSession::sendPrivateChatMessage(const QuickDC::User* user, const char* msg)
{
	if (!msg || !strlen(msg)) return;
	if (!user) return;
	
	const QuickDC::User* me = users->getUser(nickname);

	DC::Command* cmd = new DC::SendTo(me->getNick(), user->getNick(), msg);
	send(cmd);
	
	// Since a NMDC hub does not reflect priv messages back to us, we need
	// to do it here manually.
	listener->EventPrivateChat(me, user, msg, user->getNick(), false);
}

void DC::HubSession::sendChatMessage(const char* msg)
{
	if (!msg || !strlen(msg)) return;

	const QuickDC::User* me = users->getUser(nickname);
	DC::Command* cmd = new DC::SendMessage(me->getNick(), msg);
	send(cmd);
}

void DC::HubSession::sendAdminUserKick(const QuickDC::User* user)
{
	if (!user) return;
	const QuickDC::User* me = users->getUser(nickname);
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
	if (!user || !address || !strlen(address) || !reason || !strlen(reason)) return;
	
	const QuickDC::User* me = users->getUser(nickname);
	
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
	extensions.insert(strdup(feature));
}

bool DC::HubSession::supports(const char* feature)
{
	return (extendedProtocol && extensions.count((char*) feature));
}

void DC::HubSession::onLock(const char* lock_, const char* /*pk_*/) {
	/*
	 * $Lock is the first thing a hub says, so this runs against an unverified
	 * peer. calculateKey() answers 0 for anything under three characters, and
	 * the key is handed to strlen() further down.
	 */
	if (!lock_ || strlen(lock_) < 3)
	{
		QERR("Hub sent a lock too short to answer; dropping the connection");
		socket->disconnect();
		return;
	}

	if (lock) free(lock);
	lock = strdup(lock_);
	extendedProtocol = (strncmp(lock, "EXTENDEDPROTOCOL", 16) == 0);

	key = Lock::calculateKey(lock, strlen(lock));
	if (!key)
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
		DC::Command* cmd = new DC::SendKey(key, strlen(key));
		send(cmd, true);
	}
	
	{
		DC::Command* cmd = new DC::SendValidateNick(nickname);
		send(cmd, false);
	}
	
	state = Lock;
	
}

void DC::HubSession::onHubName(const char* hubname_)
{
	if (hubname) free(hubname);
	hubname = strdup(hubname_);
	listener->EventHubName(hubname);
}

void DC::HubSession::onPublicChat(const char* from, const char* message)
{
	const QuickDC::User* u = users->getUser(from);
	if (u)
	{
		listener->EventChat(u, message);
	}
	else
	{
		listener->EventHubMessage(from, message);
	}
}

void DC::HubSession::onActionChat(const char* from, const char* message)
{
	const QuickDC::User* u = users->getUser(from);
		
	if (u)
	{
		listener->EventChat(u, message, true);
	}
	else
	{
		listener->EventHubMessage(from, message);
	}
}

void DC::HubSession::onPrivateChat(const char* to, const char* from, const char* message)
{
	const QuickDC::User* u = users->getUser(from);
	const QuickDC::User* u2 = users->getUser(to);
	if (u)
	{
		listener->EventPrivateChat(u, u2, message, u->getNick(), false);
	}
	else
	{
		listener->EventHubMessage(from, message);
	}
}

void DC::HubSession::onRedirect(const char* host, uint16_t port)
{
	listener->EventHubRedirect(host, port);
}

void DC::HubSession::onBadPassword()
{
	free(password);
	password = strdup("");

	/*
	 * Cleared on disk as well as in memory, and now rather than when the
	 * session ends. The hub has said this password is wrong, so keeping it
	 * only means every later run offers it again and fails the same way - and
	 * the destructor would write the cleared one out regardless.
	 */
	session->setGroup(address);
	session->setString("Password", password);
	session->write();

	/* The hub still wants a password, which is the interface's problem. */
	listener->EventHubAutenticate();
}

void DC::HubSession::onLoggedIn()
{
	state = Idle;

	/* As ADC does. Without it a client cannot tell a hub it has merely reached
	   from one it is actually logged in to. */
	listener->EventHubStatus(QuickDC::HubListener::HubLoggedIn);
}

void DC::HubSession::onActiveConnect(const char* address, uint16_t port, const char* user)
{
	const QuickDC::User* u = (user) ? users->getUser(user) : 0;
	if (listener->EventClientConnect(address, port, u))
	{
		QDBG("Connecting to %s:%d...", address, port);
		auto socket = Samurai::IO::Net::Socket::create(nullptr, address, port);
		DC::ClientSession* connection = new DC::ClientSession(socket, this);
		socket->setEventHandler(connection);
		socket->connect();
	}
}

/**
 * FIXME: Add nick to expected connection list, timeout 45 seconds.
 */
void DC::HubSession::onPassiveConnect(const char* user)
{
	const QuickDC::User* u = users->getUser(user);
	if (u)
	{
		if (listener->EventClientConnect(u))
		{
			QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
			config->setGroup("Network");
			if (config->getBool("Active mode") && QuickDC::Core::getInstance()->server)
			{
				const char* addr = config->getString("Address");
				uint16_t port = (uint16_t) config->getNumber("Port");
				
				DC::Command* cmd = new DC::SendConnectToMe(user, addr, port);
				send(cmd);
			}
		}
	}
}

void DC::HubSession::sendConnectionRequest(const QuickDC::User* u, const char*)
{
	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	DC::Command* cmd = 0;
	config->setGroup("Network");
	if (config->getBool("Active mode") && QuickDC::Core::getInstance()->server)
	{
		QDBG("Requesting active connection to user: %s", u->getNick());
		config->setGroup("Network");
		
		const char* addr = config->getString("Address");
		uint16_t port = (uint16_t) config->getNumber("Port");
		
		cmd = new DC::SendConnectToMe(u->getID(), addr, port);
	}
	else
	{
		QDBG("Requesting passive connection to user: %s", u->getNick());
		cmd = new DC::SendRevConnectToMe(nickname, u->getID());
	}
	send(cmd);
}


void DC::HubSession::onRequestMyInfo(const char* nick_)
{
	if (strcmp(nick_, nickname) != 0)
	{
		// FIXME: change nickname to whatever the hub suggests for us
		//        this requires change in other layers also.
		free(nickname);
		nickname = strdup(nick_);
	}
	
	state = Nick;
	
	send(new DC::SendVersion(), true);
	send(new DC::SendGetNickList(), true);
	sendMyInfo();
}

void DC::HubSession::sendMyInfo()
{
	DC::Command* cmd = new DC::SendMyInfo(getNick(), getDescription(), getEmail(), QuickDC::Core::getInstance()->shares->getSharedSize());
	send(cmd);
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
	if (!password || !*password)
		listener->EventHubAutenticate();

	send(new DC::SendMyPass(getPassword()));
}

void DC::HubSession::sendPassword(const char* password_)
{
	if (state != Password || !password_) return;

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

	send(new DC::SendMyPass(getPassword()));
}

void DC::HubSession::onBadNick()
{
	free(nickname);
	QDBG("ERROR: Bad nickname!");
	nickname = 0;
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
	QuickDC::Share::Manager* shares = QuickDC::Core::getInstance()->shares;
	if (!shares) return;

	shares->search(request);

	/* A hash we do not share may still be one we are downloading. */
	if (request->isTTH() && !shares->getFileByTTH(request->getTTH()))
		sendPartialStatus(request);
}

void DC::HubSession::sendPartialStatus(QuickDC::Share::SearchRequest* request)
{
	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	if (!queue || !request || !request->getTTH() || !core->server) return;

	/* A PSR is an ADC message, which a passive searcher's hub will not relay,
	   so only an active one can be answered. */
	const char* replyTo = request->getToken();
	if (!replyTo || !strchr(replyTo, ':')) return;

	QuickDC::QueueFile* file = queue->getFileByTTH(request->getTTH());
	if (!file) return;

	QuickDC::LeafHashes* leaves = file->getLeaves();
	if (!leaves || !leaves->isLoaded()) return;

	uint64_t block = leaves->getLeafSize();
	size_t runs = file->getChunks().countBlockRuns(block);
	if (!runs) return;

	std::string indexes = file->getChunks().toIndexString(block);
	if (indexes.empty()) return;

	std::string host(replyTo);
	std::string port;
	size_t cut = host.find_last_of(':');
	port = host.substr(cut + 1);
	host = host.substr(0, cut);

	std::string hubIpPort;
	if (hub && hub->getUrl())
	{
		hubIpPort = hub->getUrl();
		size_t scheme = hubIpPort.find("://");
		if (scheme != std::string::npos) hubIpPort = hubIpPort.substr(scheme + 3);
	}

	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	int myPort = config->getBool("Active mode") ? config->getNumber("Port", 0) : 0;

	/* NI is what identifies us on an NMDC hub, where we have no client ID. */
	ADC::Command cmd(FOURCC('U','P','S','R'), ADC::CID::getInstance()->getCID());
	cmd.addArgument("NI", nickname);
	cmd.addArgument("HI", hubIpPort.c_str());
	cmd.addArgument("U4", std::to_string(myPort));
	cmd.addArgument("TR", request->getTTH());
	cmd.addArgument("PC", std::to_string((int) runs));
	cmd.addArgument("PI", indexes.c_str());

	Samurai::IO::Buffer buffer;
	cmd.write(&buffer);
	Samurai::IO::Net::DatagramPacket packet(&buffer);
	Samurai::IO::Net::InetSocketAddress peer(host.c_str(), Samurai::Util::Convert::to_int32(port.c_str()),
		Samurai::IO::Net::InetAddress::Version::IPv4);
	packet.setAddress(&peer);
	core->server->send(&packet);

	QDBG("Offered %d run(s) of '%s' to %s", (int) runs, request->getTTH(), replyTo);
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

	QuickDC::Core* core = QuickDC::Core::getInstance();
	QuickDC::QueueManager* queue = (core) ? core->queue : 0;
	const char* hubUrl = (hub) ? hub->getUrl() : 0;

	QDBG("Search result: '%s' (%d bytes) from '%s'",
		result->getFile() ? result->getFile() : "",
		(int) result->getSize(),
		result->getUser() ? result->getUser()->getNick() : "");

	/*
	 * Match on the TTH, not the name: the point of queueing by hash is that
	 * the same file is called something else on someone else's disk. A reply
	 * without a TTH is still shown, it just cannot become a source.
	 */
	if (queue && hubUrl && result->getTTH() && result->getUser())
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
	return users->getUser(nickname);
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
	hub->onDisconnected();
}

void DC::HubSession::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	/* As the ADC session does. Without this the session goes on reporting
	   itself logged in to a hub it is no longer connected to, and the hub
	   manager searches on it and counts it. */
	state = None;

	manager->remove(this);
	listener->EventNetStatus(QuickDC::HubListener::NetDisconnected);
	hub->onDisconnected();
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
			listener->EventNetStatus(QuickDC::HubListener::NetErrorIO);
	}
	
	hub->onDisconnected();
	delete infoTimer; infoTimer = 0;
}

bool DC::HubSession::isLoggedIn() const
{
	return state == Idle;
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
	
	
	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Network");
	int numslots = config->getNumber("Slots", 3);
	
	Samurai::IO::Buffer buffer;

	buffer.append("$SR ");
	buffer.append(getNick());
	buffer.append(' ');
	
	if (1) { /* FILENAME */
		buffer.append(filename);
		buffer.append((char) 0x05);
		buffer.append(file->getSize());
	} else { /* DIRECTORY */
		buffer.append(filename);
	}
	
	buffer.append(' ');
	buffer.append(0); /* free slots */
	buffer.append('/');
	buffer.append(numslots);
	buffer.append((char) 0x05);
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
		buffer.append((char) 0x05);
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
		if (pos && QuickDC::Core::getInstance()->server) {
			pos[0] = '\0';
			Samurai::IO::Net::DatagramPacket packet(&buffer);
			Samurai::IO::Net::InetSocketAddress peerAddr(addr, Samurai::Util::Convert::to_int32(&pos[1]), Samurai::IO::Net::InetAddress::Version::IPv4);
			packet.setAddress(&peerAddr);
			QuickDC::Core::getInstance()->server->send(&packet);
		}
		free(addr);
	}
#endif // 0
}

