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
#include <samurai/crypto/digest/tigertree.h>
#include <samurai/io/buffer.h>
#include <samurai/io/net/datagram.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/url.h>
#include <samurai/util/base32.h>


ADC::HubSession::HubSession(QuickDC::Hub* ui, QuickDC::HubListener* listener_, Samurai::IO::Net::URL* url, bool secure_)
	: QuickDC::HubSession(ui)
	, QuickDC::Connection(url, secure_ ? "ADCS hub" : "ADC hub")
	, address(0)
	, proxy(0)
	, listener(listener_)
	, infoTimer(0)
	, sid(0)
	, cid(0)
	, pid(0)
	, hubname(0)
	, hubdesc(0)
	, hubversion(0)
	, secure(secure_)
{
	access = QuickDC::HubSession::AccessNormal;
	userState = QuickDC::HubSession::StateNone;
	
	cid = (char*) malloc(40);
	pid = (char*) malloc(40);

	proxy = new ADC::CommandProxy(socket, this);
	users = new QuickDC::UserManager(this);

	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	session = new QuickDC::Preferences("~/.quickdc/session.conf");
	
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
	session->setTime("Last Connect", time(0));
	
	session->write();
}


ADC::HubSession::~HubSession()
{
	// save session data.
	session->setString("Nickname", nickname);
	session->setString("Password", password);
	session->write();
	delete session;
	
	QDBG("~HubSession()");
	
	while (extensions.size())
	{
		char* feature = *extensions.begin();
		extensions.erase(extensions.begin());
		free(feature);
	}

	
	free(hubname);
	
	free(nickname);
	free(password);
	free(email);
	free(description);
	free(cid);
	free(pid);
	free(sid);
	free(address);
	free(hubversion);
	delete users;
	delete infoTimer;
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
#ifdef SSL_SUPPORT
		listener->EventNetStatus(QuickDC::HubListener::NetConnected);
		resetIdleTimer();
		socket->TLSInitialize(false);
		socket->TLSsendHandshake();
#else
		QERR("TLS not supported.");
#endif
	}
}


void ADC::HubSession::EventTLSConnected(const Samurai::IO::Net::Socket*)
{
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


void ADC::HubSession::EventError(const Samurai::IO::Net::Socket*, enum Samurai::IO::Net::SocketError error, const char* msg)
{
	manager->remove(this);
	userState = QuickDC::HubSession::StateNone;
	switch (error)
	{
		case Samurai::IO::Net::ConnectionTimeout:
			listener->EventNetStatus(QuickDC::HubListener::NetErrorTimeout);
			break;
		case Samurai::IO::Net::ConnectionRefused:
			listener->EventNetStatus(QuickDC::HubListener::NetErrorConnectionRefused);
			break;
		case Samurai::IO::Net::HostNotFound:
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
		cmd.addArgument("SI", quickdc_ulltoa(file->getSize()));
		cmd.addArgument("TR", file->getTTH());
		cmd.addArgument("TD", "0");
		cmd.addArgument("TO", req->getToken());
		// cmd.addArgument("SL", "3"); // FIXME: implement me
		
		Samurai::IO::Buffer buffer;
		cmd.write(&buffer);
		Samurai::IO::Net::DatagramPacket packet(&buffer);
		Samurai::IO::Net::InetSocketAddress peerAddr(user->getAddressIPv4(), user->getPortIPv4(), Samurai::IO::Net::InetAddress::IPv4);
		packet.setAddress(&peerAddr);
		QuickDC::Core::getInstance()->server->send(&packet);
	
	}
	else
	{
		/* Send result via server */
		ADC::Command* cmd = new ADC::Command(FOURCC('D','R','E','S'), sid, user->getID());
		cmd->addArgument("FN", file->getPublicName());
		cmd->addArgument("SI", quickdc_ulltoa(file->getSize()));
		cmd->addArgument("TR", file->getTTH());
		cmd->addArgument("TD", "0");
		cmd->addArgument("TO", req->getToken());
		// cmd.addArgument("SL", "3"); // FIXME: implement me
		
		send(cmd);
	}
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
	cmd->addArgument("SL", quickdc_itoa(config->getNumber("Slots", 3), 10));
	cmd->addArgument("SS", quickdc_ulltoa(QuickDC::Core::getInstance()->shares->getSharedSize()));
	cmd->addArgument("SF", quickdc_itoa(QuickDC::Core::getInstance()->shares->getFiles(), 10));
	cmd->addArgument("DE", description);
	cmd->addArgument("EM", email);
	cmd->addArgument("VE", PRODUCT " " VERSION);
	cmd->addArgument("ID", ADC::CID::getInstance()->getCID());
	cmd->addArgument("PD", ADC::CID::getInstance()->getPID());
	cmd->addArgument("HN", quickdc_itoa(QuickDC::Core::getInstance()->hubs->countHubsNormal(), 10));
	cmd->addArgument("HR", quickdc_itoa(QuickDC::Core::getInstance()->hubs->countHubsRegistered(), 10));
	cmd->addArgument("HO", quickdc_itoa(QuickDC::Core::getInstance()->hubs->countHubsOperator(), 10));
	
	config->setGroup("Network");
	QDBG("Active mode %d (server=%p)\n", config->getBool("Active mode") ? 1 : 0, QuickDC::Core::getInstance()->server); 

	if (config->getBool("Active mode") && QuickDC::Core::getInstance()->server)
	{
		cmd->addArgument("I4", config->getString("Address", "0.0.0.0"));
		cmd->addArgument("U4", quickdc_itoa(config->getNumber("Port"), 10));
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
	if  (!user) return;

	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Network");
	const char* protocol = ADC_COMPLIANCE;
	if (config->getBool("Active mode") && QuickDC::Core::getInstance()->server)
	{
		QDBG("Requesting active connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','C','T','M'), sid, user->getID());
		cmd->addArgument(protocol);
		cmd->addArgument(quickdc_itoa(config->getNumber("Port"), 10));
		if (token) cmd->addArgument("TO", token);
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
		cmd->addArgument(protocol);
		if (token) cmd->addArgument("TO", token);

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
	char password_encoded[40] = { 0, };
	
	if (!token) return;

	size_t tok_size = (strlen(token) * 5) >> 3;
	size_t size = strlen(password) + tok_size;
	uint8_t* tok_decoded = new uint8_t[tok_size];
	uint8_t* str = new uint8_t[size];

	base32_decode(token, tok_decoded, tok_size);
	
	memcpy(&str[0], password, strlen(password));
	memcpy(&str[strlen(password)], tok_decoded, tok_size);
	
	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(str, size);
	Samurai::Crypto::Digest::HashValue* value = tiger.digest();
	value->getFormattedString(Samurai::Crypto::Digest::HashValue::FormatBase32, password_encoded, 40);

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
		
		switch (quickdc_atoi(&code[1]))
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
						reconnect_time = quickdc_atoi(arg);
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
		listener->EventPrivateChat(u, u2, message, context, action);
	}
}


void ADC::HubSession::onActiveConnect(const char* from, const char* proto, uint16_t port, const char* token)
{
	if (userState != QuickDC::HubSession::StateNormal) return;

	if (strcmp(from, sid) == 0) return;
	const QuickDC::User* user = users->getUser(from);
	if (!user) return;
	
	const char* ip = user->getAddressIPv4();
	QDBG("Requested to connect to '%s' using '%s' on port %d (token=%s)", from, proto, port, token, ip);

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

	uint16_t port2 = user->getPortIPv4();
	if (port != port2) port = port2;
	
	Samurai::IO::Net::Socket* csock = new Samurai::IO::Net::Socket(0, ip, port);
	ADC::ClientSession* connection = new ADC::ClientSession(csock, this, token, from);
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
	const char* protocol = ADC_COMPLIANCE;
	
	if (config->getBool("Active mode") && QuickDC::Core::getInstance()->server) {
		QDBG("Requesting active connection to user: %s", user->getNick());
		ADC::Command* cmd = new ADC::Command(FOURCC('D','C','T','M'), sid, user->getID());
		cmd->addArgument(protocol);
		cmd->addArgument(quickdc_itoa(config->getNumber("Port"), 10));
		if (token) cmd->addArgument("TO", token);

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

	if (QuickDC::Core::getInstance()->shares)
	{
		QuickDC::Core::getInstance()->shares->search(request);
	}
}

