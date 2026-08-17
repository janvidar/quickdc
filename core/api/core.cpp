/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include <samurai/io/net/bandwidth.h>
#include <samurai/io/net/tlsfactory.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/file.h>
#include <samurai/timer.h>
#include <samurai/timestamp.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "api/core.h"
#include "api/server.h"
#include "chat/archive.h"
#include "config/preferences.h"
#include "network/connectionmanager.h"
#include "network/downloadqueue.h"
#include "network/hubmanager.h"
#include "network/dropcheck.h"
#include "network/portforward.h"
#include "network/reachability.h"
#include "network/searchtokens.h"
#include "network/securitymanager.h"
#include "network/transfermanager.h"
#include "network/adc/server/hub.h"

#include "share/sharemanager.h"
#include "hash/hashmanager.h"

namespace {

/*
 * Whether a port mapping was asked for because a measurement said passive.
 *
 * The whole of the state the check-map-recheck loop needs, and it needs some: a
 * gateway grants a mapping and reports success without knowing whether anything
 * upstream honours it, so the only evidence that the mapping helped is a second
 * measurement taken through it. That means the two listeners below have to agree
 * on which pass they are in - one asks for the mapping, the other measures again
 * when it appears - and on whose mapping it is, since a forward this did not
 * create is not this one's to take away.
 *
 * A file static beside the listeners for the same reason they are: only one Core
 * may exist, and this has to outlive the objects that point at them.
 */
bool mapping_asked_for = false;

/*
 * What the core does with the outcome, which is to record it.
 *
 * Nothing here decides anything: whether the client is reachable is answered by
 * asking Core::portforward, so a front end reads the same state whether it
 * started the forwarding itself or left it to the configuration. A file static
 * because only one Core may exist, and it has to outlive the PortForward that
 * points at it.
 */
class CorePortForwardListener : public QuickDC::PortForwardListener
{
	public:
		/**
		 * The gateway granted a mapping, so the hubs are told and, where the
		 * mapping was asked for because a measurement said passive, the
		 * measurement is taken again through it.
		 *
		 * That second measurement is the whole point of the loop: the gateway
		 * reports success without knowing whether anything upstream honours it,
		 * so only asking a server outside to reach the port settles whether a
		 * second NAT above the gateway lets a packet through.
		 */
		void EventPortForwarded(uint16_t external,
			const Samurai::IO::Net::InetAddress& address) override
		{
			QDBG("The peer port is reachable at %s:%d",
				address.toString().c_str(), static_cast<int>(external));
			(void) external;
			(void) address;
			announce();

			if (mapping_asked_for) QuickDC::startDropCheck();
		}

		/**
		 * No mapping is coming, so the hubs are told the client stays passive and
		 * the loop is abandoned rather than left waiting for one.
		 *
		 * Left marked as having asked for a mapping, the next passive verdict
		 * would read as the pass that releases one - of which there is none, and
		 * the measurement that produced it would be the one being answered.
		 */
		void EventPortForwardFailure(const char* reason) override
		{
			/*
			 * Not an error: a network with no gateway, or a router with the
			 * feature switched off, reaches here and the client is simply passive.
			 */
			QDBG("The peer port could not be forwarded, staying passive: %s", reason);
			(void) reason;
			announce();

			mapping_asked_for = false;
		}

	private:
		/*
		 * Whether this client can be connected to, and on what port, is part of
		 * what a hub was told at login - so a hub joined before the answer changed
		 * is still handing every other client the old one. Both outcomes announce,
		 * because losing a forwarded port matters as much as gaining one: a hub
		 * left saying we are active would have peers waiting on a connection that
		 * can no longer arrive.
		 */
		static void announce()
		{
			const QuickDC::Core* core = QuickDC::Core::getInstance();
			if (!core || !core->hubs) return;

			/* Throttled with everything else that changes what a hub was told,
			   so a network that comes and goes does not update every hub each
			   time it does. */
			core->hubs->requestClientInfoUpdate();
		}
};

CorePortForwardListener portforward_listener;

/*
 * What a measured verdict does, which for now is what a forwarded port does:
 * ask the hubs to say again what they know about this client. The verdict is
 * kept on the check itself, so anything showing it reads it there rather than
 * being handed a copy that goes stale the moment the next run finishes.
 */
class CoreDropCheckListener : public QuickDC::DropCheckListener
{
	public:
		/**
		 * Record what was measured, and carry the loop on from it.
		 *
		 * The verdict goes into the reachability record, which is what the INF is
		 * built from: a measurement outranks anything inferred from what peers
		 * report and is outranked in turn by a connection that actually arrived,
		 * see Reachability::tcp() for the order and why it is that way round.
		 *
		 * A passive answer is the only one worth acting on, so it is the only one
		 * that considers a mapping; any other ends the loop, since there is
		 * nothing left to try and nothing to undo.
		 */
		void EventDropCheckFinished(const QuickDC::DropVerdict& verdict) override
		{
			QDBG("A DROP server reports this client %s on port %d",
			     QuickDC::toString(verdict.mode), static_cast<int>(verdict.port));

			QuickDC::Core* core = QuickDC::Core::getInstance();
			if (!core) return;

			QuickDC::applyDropMeasurement();

			if (verdict.mode == QuickDC::DropMode::Passive) considerMapping(core, verdict);

			if (verdict.mode != QuickDC::DropMode::Passive) mapping_asked_for = false;

			if (core->hubs) core->hubs->requestClientInfoUpdate();
		}

		/**
		 * Nothing was measured, so nothing is announced and the loop is abandoned.
		 *
		 * A check that did not run says nothing about this client, and
		 * re-publishing an unchanged INF would be noise. Nor is a measurement
		 * that could not be taken a reason to keep a mapping asked for on the
		 * strength of one that could - but it is no evidence the mapping is
		 * useless either, so what the gateway granted is left alone and only the
		 * loop is given up.
		 */
		void EventDropCheckFailed(const char* reason) override
		{
			QDBG("The reachability check did not complete: %s", reason);
			(void) reason;

			mapping_asked_for = false;
		}

	private:
		/**
		 * Ask the gateway to forward the port, or conclude that doing so did not
		 * help.
		 *
		 * Two passes, and which one this is depends on whether a mapping has
		 * already been asked for. The first asks; the second is reached only
		 * because the answer is still passive with the mapping in place, which
		 * means something above the gateway is dropping the packets - so the
		 * forward is released. Leaving one open to a port that could not be
		 * reached through it has no upside, and it is a hole in somebody's
		 * network that nothing is using.
		 *
		 * A forward this did not ask for is never released. It may be there
		 * because the configuration asked for it at start-up, or because a front
		 * end asked, and neither is this loop's to undo.
		 *
		 * One is only asked for where forwarding was asked for at all, which is
		 * where [Network] UseUPnP is already on. A search tells every host on the
		 * local segment what this machine runs, the same disclosure that setting
		 * gates - and a measurement saying passive is not consent to start
		 * announcing this machine there.
		 */
		static void considerMapping(QuickDC::Core* core,
		                            const QuickDC::DropVerdict& verdict)
		{
			if (!core->portforward || !core->config) return;

			if (mapping_asked_for)
			{
				if (core->portforward->isForwarded())
				{
					QDBG("The mapping did not help; releasing it");
					core->portforward->stop();
				}

				mapping_asked_for = false;
				return;
			}

			core->config->setGroup("Network");
			if (!core->config->getBool("UseUPnP", false)) return;

			if (core->portforward->isForwarded()) return;
			if (core->portforward->isBusy()) return;

			const uint16_t port = verdict.port
				? verdict.port
				: static_cast<uint16_t>(core->config->getNumber("Port", 1411));

			QDBG("Passive on port %d; asking the gateway to forward it",
			     static_cast<int>(port));

			mapping_asked_for = true;
			core->portforward->start(port);
		}
};

CoreDropCheckListener dropcheck_listener;

/**
 * The hash cache to use for a core whose files were named rather than assumed.
 *
 * Beside the configuration it was given, since that is the directory such a core was
 * pointed at, and the hash manager rotates its cache through a sibling of whatever it
 * is told - so one name settles the lot. A configuration with no directory in it is
 * answered with a bare name, which lands wherever the process is running, and that is
 * still not the account's own cache.
 */
std::string hashCacheBeside(const std::string& config_file)
{
	const size_t slash = config_file.rfind('/');

	if (slash == std::string::npos) return "hashdb.cache";

	return config_file.substr(0, slash + 1) + "hashdb.cache";
}

/*
 * Make sure there is a certificate to present, generating one if there is not.
 *
 * A peer transfer has to be encrypted unless the user turns that off, and a peer
 * dialling us can only be answered with TLS if we hold a certificate - so on a
 * client that has never had one, requiring encryption would refuse every inbound
 * transfer, and nothing in this client ever created one. Nobody can sign a
 * certificate for a peer on a home connection, so it is self-signed; in ADC the
 * peer's identity is its keyprint rather than a name a certificate asserts, so
 * that costs nothing that was on offer.
 *
 * An existing pair is left exactly as it is. Replacing it would change the
 * keyprint that hubs and peers were given, and a certificate that is present but
 * unreadable is for a person to look at, not for this to overwrite.
 */
void ensureCertificate(const char* key_path, const char* cert_path)
{
	Samurai::IO::File key(key_path);
	Samurai::IO::File cert(cert_path);

	if (key.exists() && cert.exists()) return;

	/*
	 * One of the two, which is a case to report rather than to repair: a key on
	 * its own is useless without the certificate that goes with it, but it may
	 * still be the key whose keyprint peers were given, and generating a new pair
	 * over it would change that silently. Left alone, and said out loud - because
	 * the visible effect is only that encrypted transfers stop working.
	 */
	if (key.exists() || cert.exists())
	{
		QERR("Found '%s' without its counterpart, so no certificate can be "
			"presented - move it aside to have a new pair generated",
			key.exists() ? key_path : cert_path);
		return;
	}

	/*
	 * The name is a fixed label rather than the nick or an address: a nick is
	 * UTF-8 and may hold spaces, which a common name cannot, and the address
	 * this client is reached at is not known here and changes. Nothing verifies
	 * it either way.
	 */
	if (!Samurai::IO::Net::TlsFactory::generateSelfSignedCertificate(
			key_path, cert_path, QUICKDC_CERTIFICATE_NAME))
	{
		/*
		 * Not fatal, but worth saying: without a certificate this client cannot
		 * answer a peer with TLS, so with encryption required it cannot be
		 * transferred with at all.
		 */
		QERR("No certificate could be generated - inbound encrypted transfers "
			"will not be possible");
		return;
	}

	QDBG("Generated a self-signed certificate in '%s'", cert_path);
}

}


QuickDC::Core::Core()
{
	QDBG_INIT;

	startup = time(0);

	references = 0;
	
	Samurai::IO::File::mkdir("~/.quickdc",         0700);
	Samurai::IO::File::mkdir("~/.quickdc/hub",     0700);
	Samurai::IO::File::mkdir("~/.quickdc/hub/bbs", 0700);
	Samurai::IO::File::mkdir("~/.quickdc/lists",   0700);
	Samurai::IO::File::mkdir("~/.quickdc/ssl",     0700);
	/* The default download destination; [Downloads]/Directory overrides it. */
	Samurai::IO::File::mkdir(QUICKDC_DOWNLOAD_DIR, 0700);
	
	config = new Preferences("~/.quickdc/quickdc.conf");
	session = new Preferences("~/.quickdc/session.conf");
	quickdc_core = this;
	
	monitor = Samurai::IO::Net::SocketMonitor::getInstance();
	bandwidthManager = new Samurai::IO::Net::BandwidthManager();
	messageHandler = Samurai::MessageHandler::getInstance();
	securityManager = new QuickDC::SecurityManager();
	reachability = new QuickDC::Reachability();
	searchTokens = new QuickDC::SearchTokens();

	ensureCertificate(QUICKDC_PRIVATE_KEY_FILE, QUICKDC_CERTIFICATE_FILE);
	Samurai::IO::Net::TlsFactory::setKeys(QUICKDC_PRIVATE_KEY_FILE, QUICKDC_CERTIFICATE_FILE);

	hash = new QuickDC::Hash::Manager();
	shares = new QuickDC::Share::Manager(config, hash);
	server = new DCServer();
	connections = new ConnectionManager();
	hubs = new HubManager();
	transfers = new TransferManager();
	/*
	 * Borrowed, not made. Samurai::Timer registers itself with
	 * TimerManager::getInstance(), which is a function-local static, so a manager
	 * constructed here would be a second one that nothing ever registers with -
	 * and every timer, the hub reconnect among them, would sit in the singleton
	 * unprocessed while run() polled an empty list.
	 */
	timers = Samurai::TimerManager::getInstance();
	localHub = new ADC::LocalHub::Hub();
	queue = new QuickDC::QueueManager();
	chat = new QuickDC::ChatArchive(QUICKDC_CHAT_DIR);

	dropcheck = 0;
	portforward = new PortForward(&portforward_listener);

	/*
	 * Off unless asked for. A search tells every host on the local segment that
	 * this one is here and what it runs, which is a disclosure a user should
	 * choose - and a client that cannot be connected to still works, it is only
	 * passive.
	 */
	config->setGroup("Network");
	if (config->getBool("UseUPnP", false))
		portforward->start(config->getNumber("Port", 1411));

	if (std::string(config->getString("Reachability server", "")).length())
	{
		dropcheck = new DropCheck(&dropcheck_listener);
		startDropCheck();
	}

	shares->initialize();
}


QuickDC::Core::Core(const std::string& config_file, const std::string& session_file,
                    const std::string& queue_file, const std::string& chat_dir)
{
	startup = time(0);
	references = 0;

	config = new Preferences(config_file);
	session = new Preferences(session_file);

	connections = new ConnectionManager();
	hubs = new HubManager();

	/*
	 * Beside the files this core was named with, and never at the usual place.
	 *
	 * A core built this way is one somebody pointed at a directory of their own - a
	 * test, or a second instance - and the whole point of naming its files is that it
	 * touches nothing of the account's. The cache is the one file the hash manager
	 * writes without being asked, and it rotates through a sibling of whatever it is
	 * given, so naming it here is the whole of what keeps a run out of ~/.quickdc.
	 *
	 * Made rather than left out: without one, nothing here can hash, so attaching a
	 * file fails with "there is no hash manager" - and a caller waiting to be told the
	 * hash of what it just wrote waits for ever. That is a real path through this core,
	 * and it is what this is made for rather than left null.
	 */
	hash = new QuickDC::Hash::Manager(hashCacheBeside(config_file).c_str());
	shares = new QuickDC::Share::Manager(config, hash);

	/* Peer sessions reach for this without checking whether there is one. */
	securityManager = new QuickDC::SecurityManager();
	reachability = new QuickDC::Reachability();
	searchTokens = new QuickDC::SearchTokens();

	/* Only when asked for: it reads and writes the file it is given. */
	queue = queue_file.empty() ? 0 : new QuickDC::QueueManager(queue_file.c_str());

	/*
	 * Only where it was told to. This constructor exists so a caller can have a
	 * core that stays out of the user's own files, and writing their
	 * conversations into the real directory would be the very thing it avoids.
	 */
	chat = chat_dir.empty() ? 0 : new QuickDC::ChatArchive(chat_dir);

	/* Consulted before an upload starts, to count the ones in flight. */
	transfers = new TransferManager();

	server = 0;
	/* Nothing to forward to without a listening socket. */
	dropcheck = 0;
	portforward = 0;
	localHub = 0;
	bandwidthManager = 0;

	/*
	 * Borrowed singletons, not ours; the destructor must not release them.
	 *
	 * The message handler is not optional, whatever this constructor leaves out.
	 * Deletion of a dropped connection is deferred through it - a session is
	 * retired from inside its own socket's callback, and cannot be freed there -
	 * so a core that never drains it never frees a peer session or the socket it
	 * holds. Every connection made through such a core leaked itself and a
	 * descriptor, and nothing said so.
	 *
	 * Timers are drained by SocketMonitor::wait() rather than from here, so a
	 * core with none of its own still has working ones.
	 */
	monitor = Samurai::IO::Net::SocketMonitor::getInstance();
	messageHandler = Samurai::MessageHandler::getInstance();
	timers = Samurai::TimerManager::getInstance();

	quickdc_core = this;
}

QuickDC::Core::~Core()
{
	quickdc_core = 0;

	/*
	 * The connections first, and this order is not a preference.
	 *
	 * A session owns whatever transfer it is running, and ~Transfer unregisters
	 * itself from the transfer manager - so tearing the manager down first left
	 * every session's destructor erasing from a vector that no longer existed. A
	 * session may also hand an unfinished chunk back to the queue on its way out,
	 * so that has to outlive it too.
	 */
	delete connections; connections = 0;

	/* Then the queue, which flushes itself to disk as it goes. */
	delete queue; queue = 0;

	/* After the hubs above, which write a line into it as one arrives. */
	delete chat; chat = 0;

	delete dropcheck; dropcheck = 0;

	/*
	 * Before the socket monitor is left behind, and deliberately without asking
	 * the gateway to release anything: that would mean talking to it while the
	 * event loop is being torn down. The finite lease is what cleans up.
	 */
	delete portforward; portforward = 0;

	delete transfers; transfers = 0;
	delete server; server = 0;
	delete hubs; hubs = 0;
	delete securityManager; securityManager = 0;
	delete reachability; reachability = 0;
	delete searchTokens; searchTokens = 0;
	delete shares; shares = 0;
	/* After the hubs, which borrow it and write through it on the way out. */
	delete session; session = 0;
	delete config; config = 0;
	delete hash; hash = 0;
	delete localHub; localHub = 0;
	delete bandwidthManager;

	/*
	 * timers and messageHandler are not deleted: both are function-local statics
	 * behind getInstance(), like the socket monitor, so freeing them is freeing
	 * something that was never allocated - which aborts the process on exit.
	 */
	timers = 0;
	messageHandler = 0;
	QDBG_FINI;
}


void QuickDC::Core::run()
{
	// Poll sockets. A zero timeout turns this into a spin - kevent/poll returns
	// immediately and we burn a core doing nothing - so always block.
	//
	// The interval is set by what is still polled rather than event driven:
	// collecting finished hash jobs, and timers, which are all whole seconds
	// anyway. Hashing runs on its own thread and console input arrives through a
	// socket the monitor watches, so neither needs a short timeout.
	static const int POLL_INTERVAL_MS = 250;
	monitor->wait(POLL_INTERVAL_MS);

	// Guarded from here on, because the second constructor builds a core that
	// only runs a hub session and leaves everything a session does not need
	// null. Without these it can be constructed but not run, which makes the
	// loop unusable for exactly the case that constructor exists for.

	// Collect whatever the hash worker thread has finished.
	if (hash) hash->process();

	// Announce transfers that have finished.
	//
	// A transfer reports completion from inside its own exec(), and the session
	// that owns it responds by deleting it, so the notification has to be
	// deferred to here rather than made on the spot. TransferManager parks them
	// on a list for exactly that reason, and this drains it.
	if (transfers) transfers->process();

	// Expire dead sources, start queued transfers, and flush the queue now
	// and then. Cheap enough to run on every iteration.
	if (queue) queue->process();

	// Process timers.
	// FIXME: it should be sorted so we don't have to iterate ALL timers to before exit.
	if (timers) timers->process();
	if (messageHandler) messageHandler->process();
}


QuickDC::Core* QuickDC::Core::getInstance()
{
	return quickdc_core;
}


const char* QuickDC::Core::getVersion() const 
{
	return PRODUCT " " VERSION " " SYSTEM "/" CPU
#ifdef BUILD
	 " (build " BUILD ")"
#endif
	 ;
}


