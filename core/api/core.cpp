/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include <samurai/io/net/tlsfactory.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/file.h>
#include <samurai/messagehandler.h>
#include <samurai/timer.h>
#include <samurai/timestamp.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "api/core.h"
#include "api/server.h"
#include "api/service.h"
#include "chat/archive.h"
#include "config/preferences.h"
#include "config/migrations.h"
#include "config/settings.h"
#include "network/connectionmanager.h"
#include "network/downloadqueue.h"
#include "network/hubmanager.h"
#include "network/dropcheck.h"
#include "api/networkmode.h"
#include "network/localaddresses.h"
#include "network/mappingpolicy.h"
#include "network/portforward.h"
#include "network/reachability.h"
#include "network/searchtokens.h"
#include "network/securitymanager.h"
#include "network/transfermanager.h"
#include "network/adc/cid.h"
#include "network/adc/server/hub.h"

#include "share/sharemanager.h"
#include "hash/hashmanager.h"

namespace {

/* The core's gateway, as the mapping policy sees it. */
class GatewayOf final : public QuickDC::MappingPolicy::Gateway {
	public:
		explicit GatewayOf(QuickDC::PortForward& forward) : forward(forward) { }

		bool forwarded() const override { return forward.isForwarded(); }
		bool busy() const override { return forward.isBusy(); }
		void start(uint16_t port) override { forward.start(port); }
		void stop() override { forward.stop(); }

	private:
		QuickDC::PortForward& forward;
};

/*
 * What the check-map-recheck loop keeps between events, one per core.
 */
struct AutomationState {
	QuickDC::Core* core;

	/** Whether to hold a gateway mapping; see network/mappingpolicy.h. */
	QuickDC::MappingPolicy mapping {};

	/*
	 * Whether a measurement is owed once the gateway has been dealt with.
	 *
	 * A mapping takes a search, a description fetch and two calls to settle, and a
	 * verdict reached before it existed answers for a different network: it would have
	 * the client record passive and announce it for the life of a measurement, through
	 * a port that works. So a run that would race the gateway is owed rather than
	 * started, and whichever way the gateway answers releases it - both outcomes are
	 * reported, exactly once, including a search that nothing answered.
	 *
	 */
	bool measurement_owed = false;

	bool measureIfStale(bool ipv6);
	void measureAgainIfOwed();

	/** Ask the gateway for the mapping the plan wants, or let go of one it does not. */
	void applyMapping();

	/** A verdict of passive: either the mapping did not help, or one is worth asking for. */
	void considerMapping(const QuickDC::DropVerdict& verdict);
};
/**
 * Measure 'ipv6', unless there is nothing to measure or nothing to learn.
 *
 * Nothing to measure is a family with no peer port: a run against it would come
 * back blocked and be perfectly right, having tested a port nothing listens on.
 * Nothing to learn is a measurement still young enough to be believed - the same
 * age the record weighs one by, so what is believed and what is worth asking
 * again cannot disagree. Nothing to act on is a passive client, which is not
 * offering the port whichever way the answer came back.
 *
 * @return whether a run was started, which is how the caller knows the family it
 *         asked about is now busy rather than simply not worth asking about.
 */
bool AutomationState::measureIfStale(bool ipv6)
{
	if (!core->server || !core->config) return false;

	if (!core->server->listening(ipv6)) return false;

	/*
	 * Nothing to act on is a client that has declared itself passive. Inbound
	 * reachability is measured by having a stranger's server dial the peer port,
	 * which costs that third party a request and this client an open window, for an
	 * answer that changes nothing: a passive client is not offering the port
	 * whatever the answer turns out to be.
	 *
	 * Peeked rather than read, because the ordinary accessor stores its default when
	 * the key is absent - and this is reached from a timer, so it would mark the
	 * file dirty every few minutes over a question it only asked.
	 */
	if (!QuickDC::Settings::read(*core->config, QuickDC::Settings::ActiveMode)) return false;

	if (QuickDC::dropMeasurementIsFresh(QuickDC::lastDropVerdict(*core, ipv6), time(0)))
		return false;

	/*
	 * Nothing to learn either from a family this host has no route out on: the
	 * check could only fail, and it would be asked again on every refresh, since a
	 * check that never completes is never fresh. Asked here rather than once at
	 * start-up, so an address that appears later is measured at the next refresh.
	 */
	if (ipv6 && !QuickDC::hostHasGlobalIPv6()) return false;

	return QuickDC::startDropCheck(*core, ipv6);
}

/**
 * Take the measurement a mapping made necessary, once one can be started.
 *
 * Cleared only when a run actually started, because one check has one session and
 * one armed window: a request refused because the other family is going is not the
 * run this owes, and forgetting it there would leave the verdict that predates the
 * mapping standing until the refresh timer noticed. Every path that ends a run
 * calls this, so the owed run is taken at the first moment there is room for it.
 */
void AutomationState::measureAgainIfOwed()
{
	if (!measurement_owed) return;

	if (QuickDC::startDropCheck(*core, false)) measurement_owed = false;
}

/*
 * How old the measurement in hand may be before another is taken.
 *
 * The age at which the record stops believing one, and deliberately not a period
 * chosen here: anything longer leaves a window where nothing is believed and
 * nothing is being asked, and anything shorter spends a request on an answer that
 * is still good. Kept in step by being the same constant rather than by matching
 * it.
 */
const time_t DROP_REFRESH_AGE = QUICKDC_REACHABILITY_MEASUREMENT_AGE;

/*
 * How often that is looked at.
 *
 * Shorter than the age because a measurement taken for some other reason moves
 * when the one in hand expires, and a timer running at exactly the age would
 * discover that a third of an hour after it happened. Looking more often than
 * measuring is what keeps the two apart: a look costs a comparison, and only a
 * measurement that has actually aged out costs a request.
 */
const time_t DROP_REFRESH_TICK = DROP_REFRESH_AGE / 3;

/*
 * What the core does with the outcome, which is to record it.
 *
 * Nothing here decides anything: whether the client is reachable is answered by
 * asking Core::portforward, so a front end reads the same state whether it
 * started the forwarding itself or left it to the configuration.
 */
class CorePortForwardListener : public QuickDC::PortForwardListener
{
	public:
		explicit CorePortForwardListener(AutomationState& owner) : owner(owner) { }

		/**
		 * The gateway granted a mapping, so the hubs are told and the
		 * measurement is taken again through it.
		 *
		 * That second measurement is the whole point of the loop: the gateway
		 * reports success without knowing whether anything upstream honours it,
		 * so only asking a server outside to reach the port settles whether a
		 * second NAT above the gateway lets a packet through.
		 *
		 * Whoever asked for the mapping, and not only where this loop did. A
		 * forward that arrived because the configuration asked for it at start-up
		 * or because a front end did changes the same thing about the network,
		 * and anything measured before it answers for the network as it was: left
		 * alone, the client would sit announcing passive through a port that had
		 * begun to work and say so only when the verdict aged out.
		 */
		void EventPortForwarded(uint16_t external,
			const Samurai::IO::Net::InetAddress& address) override
		{
			QDBG("The peer port is reachable at %s:%d",
				address.toString().c_str(), static_cast<int>(external));
			(void) external;
			(void) address;
			announce();

			owner.mapping.granted();
			owner.measurement_owed = true;
			owner.measureAgainIfOwed();
		}

		/** No mapping is coming, so the hubs are told the client stays passive. */
		void EventPortForwardFailure(const char* reason) override
		{
			/*
			 * Not an error: a network with no gateway, or a router with the
			 * feature switched off, reaches here and the client is simply passive.
			 */
			QDBG("The peer port could not be forwarded, staying passive: %s", reason);
			(void) reason;
			announce();

			owner.mapping.refused();

			/*
			 * A run held back so that it would not race the gateway is taken now:
			 * nothing was granted, so there is nothing to wait for, and the
			 * network is still worth measuring - a port can be reachable through a
			 * forward somebody made by hand.
			 */
			owner.measureAgainIfOwed();
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
		void announce()
		{
			const QuickDC::Core* core = owner.core;
			if (!core->hubs) return;

			/* Throttled with everything else that changes what a hub was told,
			   so a network that comes and goes does not update every hub each
			   time it does. */
			core->hubs->requestClientInfoUpdate();
		}

		AutomationState& owner;
};

/*
 * What a measured verdict does, which for now is what a forwarded port does:
 * ask the hubs to say again what they know about this client. The verdict is
 * kept on the check itself, so anything showing it reads it there rather than
 * being handed a copy that goes stale the moment the next run finishes.
 */
class CoreDropCheckListener : public QuickDC::DropCheckListener,
                              public Samurai::TimerListener
{
	public:
		explicit CoreDropCheckListener(AutomationState& owner) : owner(owner) { }

		/**
		 * Take the measurement again once the one in hand has aged out.
		 *
		 * The record stops believing a measurement when it is old enough - see
		 * QUICKDC_REACHABILITY_MEASUREMENT_AGE - and without this nothing would
		 * ever take a newer one. The client would keep running, quietly demoted
		 * from what was measured to what can be guessed from what peers report,
		 * having last been told the truth a quarter of an hour ago.
		 *
		 * What is held is read off the check rather than remembered here, so
		 * that a measurement taken for any other reason postpones this one
		 * rather than being followed by a redundant second.
		 *
		 * A verdict that never ran is treated as one that has expired, so a
		 * server that could not be reached at start-up is tried again rather
		 * than leaving the client permanently unmeasured over one failure.
		 */
		void EventTimeout(Samurai::Timer*) override
		{
			QDBG("Looking for a family measured longer than %d seconds ago",
			     static_cast<int>(DROP_REFRESH_AGE));

			if (owner.measureIfStale(false)) return;
			owner.measureIfStale(true);
		}

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
		/* Only the IPv4 run goes through the gateway; IPv6 has no mapping to test. */
		void EventDropCheckStarted(bool ipv6) override
		{
			if (!owner.core->portforward) return;
			GatewayOf gateway(*owner.core->portforward);
			owner.mapping.checkStarted(gateway, ipv6);
		}

		void EventDropCheckFinished(const QuickDC::DropVerdict& verdict) override
		{
			/*
			 * A lookup is said differently. It carries no port and no mode, so the
			 * ordinary line would read "not known on port 0" - which is what a
			 * measurement that failed looks like, and this one answered.
			 */
			if (verdict.addressOnly)
			{
				QDBG("A DROP server sees this client at %s",
				     verdict.publicAddress.c_str());
			}
			else
			{
				QDBG("A DROP server reports this client %s on port %d",
				     QuickDC::toString(verdict.mode),
				     static_cast<int>(verdict.port));
			}

			QuickDC::Core* core = owner.core;

			QuickDC::applyDropMeasurement(*core);

			if (verdict.mode == QuickDC::DropMode::Passive) owner.considerMapping(verdict);

			if (core->hubs) core->hubs->requestClientInfoUpdate();

			/*
			 * One family at a time, because one check has one session and one
			 * armed window. IPv4 having finished is what makes the run for the
			 * other family possible, so it is started from here rather than
			 * asked for alongside - and only forward, so that two families
			 * cannot take turns starting each other.
			 */
			if (!verdict.ipv6) owner.measureIfStale(true);

			owner.measureAgainIfOwed();
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

			/*
			 * The other family is still owed its run: one family's server being
			 * unreachable says nothing about the other, and they are commonly
			 * two different machines. Never the family that just failed, which
			 * has recorded no verdict and would therefore read as owed one
			 * again immediately - that is left to the refresh timer.
			 */
			QuickDC::Core* core = owner.core;
			if (core->dropcheck && !core->dropcheck->lastFamilyWasIPv6())
				owner.measureIfStale(true);

			owner.measureAgainIfOwed();
		}

	private:


		AutomationState& owner;
};

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

/** The listeners the core acts through, and the state they share. */
namespace {

/**
 * What the configuration asks of the network, worked out from it alone.
 *
 * The mode decides the forwarding and the claim; the rest is read as stored.
 * Every read peeks, so working the plan out never writes to the file.
 */
struct NetworkPlan {
	QuickDC::NetworkMode mode = QuickDC::NetworkMode::Advanced;
	QuickDC::NetworkModeKeys keys;
	QuickDC::DCServer::Endpoint endpoint;
	std::string address;

	bool operator==(const NetworkPlan& other) const
	{
		return mode == other.mode
			&& keys.activeMode == other.keys.activeMode
			&& keys.useUpnp == other.keys.useUpnp
			&& keys.forceActive == other.keys.forceActive
			&& keys.reachabilityServer == other.keys.reachabilityServer
			&& endpoint == other.endpoint
			&& address == other.address;
	}
};

NetworkPlan planFrom(const QuickDC::Preferences& config)
{
	NetworkPlan plan;
	plan.mode = QuickDC::storedNetworkMode(config);
	plan.keys = QuickDC::networkModeKeys(plan.mode,
		QuickDC::Settings::read(config, QuickDC::Settings::UseUpnp),
		QuickDC::Settings::read(config, QuickDC::Settings::ForceActiveMode),
		QuickDC::Settings::read(config, QuickDC::Settings::ReachabilityServer));
	plan.endpoint.port = static_cast<uint16_t>(QuickDC::Settings::read(config, QuickDC::Settings::Port));
	plan.endpoint.bind = QuickDC::Settings::read(config, QuickDC::Settings::Bind);

	/* An emptied field, saved: there is no address that means, so it is the default. */
	if (plan.endpoint.bind.empty()) plan.endpoint.bind = QuickDC::Settings::Bind.fallback;
	plan.endpoint.bind6 = QuickDC::Settings::read(config, QuickDC::Settings::Bind6);
	plan.address = QuickDC::Settings::read(config, QuickDC::Settings::Address);
	return plan;
}

}

/**
 * The listeners the core acts through, the state they share, and what keeps the
 * network in line with the configuration as it changes.
 *
 * A change is noted as it is made and acted on once per turn of the loop, so
 * that a settings page writing twenty keys is one reconciliation and not twenty.
 * What is acted on is the difference between the plan as it now stands and the
 * one last applied - never a setting read once at start-up.
 */
class QuickDC::Core::Automation : public AutomationState, private PreferencesListener {
	public:
		explicit Automation(Core* owner)
			: AutomationState{owner}
			, portforward(*this)
			, dropcheck(*this)
			, watched(owner->config ? owner->config->subscribe(this) : Subscription())
			, watchedSession(owner->session ? owner->session->subscribe(this) : Subscription())
		{
		}

		/** Take the configuration as already applied, which it is once the core is built. */
		void settle()
		{
			if (core->config) applied = planFrom(*core->config);
			networkChanged = identityChanged = localHubChanged = false;
		}

		void reconcile();

		CorePortForwardListener portforward;
		CoreDropCheckListener dropcheck;

	private:
		void EventSettingChanged(const Preferences& store, const std::string& group,
		                         const std::string& key, const std::string*) override
		{
			/* The session store files a hub's own identity under its address. */
			if (&store == core->session)
			{
				if (key == "Nickname" || key == "Description" || key == "E-mail")
					identityChanged = true;
				return;
			}

			if (group == "Network") networkChanged = true;
			else if (group == "User") identityChanged = true;
			else if (group == "Local hub") localHubChanged = true;
		}

		void measure(const NetworkPlan& plan);

		Subscription watched;
		Subscription watchedSession;
		NetworkPlan applied;
		bool networkChanged = false;
		bool identityChanged = false;
		bool localHubChanged = false;
};

void AutomationState::applyMapping()
{
	if (!core->portforward || !core->config) return;

	const NetworkPlan plan = planFrom(*core->config);
	GatewayOf gateway(*core->portforward);

	/* What is measured before the gateway answers is about the network without it. */
	measurement_owed = mapping.apply(gateway, plan.keys.useUpnp, plan.endpoint.port);
}

void AutomationState::considerMapping(const QuickDC::DropVerdict&)
{
	if (!core->portforward || !core->config) return;

	const NetworkPlan plan = planFrom(*core->config);
	GatewayOf gateway(*core->portforward);
	measurement_owed = mapping.passive(gateway, plan.keys.useUpnp, plan.endpoint.port);
}

/*
 * A passive client is shown the address the world sees it at and never has a
 * stranger dial a port it is not offering; any other is measured. Nothing is
 * asked of anybody where no server is named.
 */
void QuickDC::Core::Automation::measure(const NetworkPlan& plan)
{
	/* The plan names a default server when none is stored; only a stored one was chosen. */
	if (measurement_owed || !core->config) return;
	if (QuickDC::Settings::read(*core->config, QuickDC::Settings::ReachabilityServer).empty()) return;

	if (plan.keys.activeMode) startDropCheck(*core, false);
	else startAddressLookup(*core, false);
}

void QuickDC::Core::Automation::reconcile()
{
	if (!networkChanged && !identityChanged && !localHubChanged) return;
	if (!core->config) return;

	const NetworkPlan plan = planFrom(*core->config);
	bool tellHubs = false;

	/* Before anything is announced, so that what is announced is the new identity. */
	if (identityChanged && core->hubs) core->hubs->refreshIdentity();

	if (networkChanged && !(plan == applied))
	{
		if (core->server && !(plan.endpoint == core->server->endpoint()))
			core->server->listen(plan.endpoint);

		mapping.configurationChanged();
		applyMapping();

		const bool advertisedChanged = plan.mode != applied.mode
			|| plan.keys.activeMode != applied.keys.activeMode
			|| plan.keys.forceActive != applied.keys.forceActive
			|| plan.endpoint != applied.endpoint
			|| plan.address != applied.address;

		if (advertisedChanged)
		{
			tellHubs = true;
			measure(plan);
		}

		applied = plan;
	}

	/* Throttled there, so a burst of changes is one update to every hub. */
	if (tellHubs && core->hubs) core->hubs->requestClientInfoUpdate();

	if (localHubChanged) core->loop().messages().postMessage(MsgConfigUpdated, nullptr, 0, 0);

	networkChanged = identityChanged = localHubChanged = false;
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
	Samurai::IO::File::mkdir(QuickDC::Settings::DownloadDirectory.fallback, 0700);
	
	config = new Preferences("~/.quickdc/quickdc.conf");

	session = new Preferences("~/.quickdc/session.conf");

	/*
	 * Before anything reads either store, so that everything below sees them as
	 * this version expects. Whether this is a first run is answered from the
	 * file's own absence, which nothing below can change.
	 */
	migrateSettings(*config, *session, config->existed() == false);
	identity = new ADC::CID(*config);
	automation = new Automation(this);
	service = 0;
	
	securityManager = new QuickDC::SecurityManager();
	reachability = new QuickDC::Reachability();
	reachability6 = new QuickDC::Reachability();
	searchTokens = new QuickDC::SearchTokens();

	ensureCertificate(QUICKDC_PRIVATE_KEY_FILE, QUICKDC_CERTIFICATE_FILE);
	Samurai::IO::Net::TlsFactory::setKeys(QUICKDC_PRIVATE_KEY_FILE, QUICKDC_CERTIFICATE_FILE);

	hash = new QuickDC::Hash::Manager();
	shares = new QuickDC::Share::Manager(config, hash);
	server = new DCServer(*this, planFrom(*config).endpoint);
	connections = new ConnectionManager(eventLoop);
	hubs = new HubManager(*this);
	transfers = new TransferManager(eventLoop);
	localHub = new ADC::LocalHub::Hub(eventLoop, *config);
	queue = new QuickDC::QueueManager(this);
	chat = new QuickDC::ChatArchive(QUICKDC_CHAT_DIR);

	dropcheck = 0;
	dropRefresh = 0;
	portforward = new PortForward(eventLoop, &automation->portforward);

	/*
	 * Off unless asked for. A search tells every host on the local segment that
	 * this one is here and what it runs, which is a disclosure a user should
	 * choose - and a client that cannot be connected to still works, it is only
	 * passive.
	 */

	/*
	 * What the mode in force asks for, not what the raw key says.
	 *
	 * The two come apart on exactly one file: an installation that predates [Network]
	 * Mode and was passive keeps every value it had, UseUPnP among them, because in
	 * such a file those values are the state being described and rewriting them would
	 * be this client changing behaviour because somebody upgraded. So the key can say
	 * to forward while the mode says nothing is told to connect here, and honouring
	 * the key would leave a hole in somebody's network that nothing uses while the
	 * settings page says in as many words that passive forwards no port.
	 *
	 * The consequence is a behaviour change at the next start for that one
	 * combination - passive with UseUPnP still true stops forwarding - and it is the
	 * right one: the mapping was for peers that are never sent here.
	 *
	 * The stored values are peeked, since the ordinary accessors would write their
	 * defaults back and this read is meant to leave the file as it found it.
	 */
	const NetworkModeKeys wanted = networkModeKeys(storedNetworkMode(*config),
		QuickDC::Settings::read(*config, QuickDC::Settings::UseUpnp),
		QuickDC::Settings::read(*config, QuickDC::Settings::ForceActiveMode),
		QuickDC::Settings::read(*config, QuickDC::Settings::ReachabilityServer));

	/*
	 * Which also settles whether the first measurement waits for the gateway: see
	 * 'measurement_owed'. A mode that forwards nothing leaves nothing to wait for,
	 * and a start() that failed outright has reported before it returns.
	 */
	automation->applyMapping();

	/*
	 * Built whether or not there is a server to ask, because naming one is a
	 * setting rather than a restart: the check holds no resources until it is
	 * started, and it is what a front end attaches to and what the probes hand a
	 * challenge to. Only the measuring is conditional.
	 */
	dropcheck = new DropCheck(eventLoop, &automation->dropcheck);
	dropRefresh = new Samurai::Timer(eventLoop, &automation->dropcheck, DROP_REFRESH_TICK, false);

	/* After the check, which it observes, and before anything starts one. */
	service = new Api::Service(*this);

	/*
	 * The first run against whoever is named, and which run depends on the mode.
	 *
	 * A passive client asks only what address the world sees it at. It has announced
	 * that nothing is to connect here, so having a stranger's server dial the peer
	 * port in order to tell us our own address is the one thing it must not arrange -
	 * and the answer would be about a port nobody will be sent to anyway. The
	 * measuring proper is for the two modes that intend to be reachable.
	 *
	 * Peeked, because the ordinary read would write the default back: this decides
	 * whether to contact a third party at all, and a read that created the key would
	 * be the client answering that question for itself.
	 */
	const bool named =
		!QuickDC::Settings::read(*config, QuickDC::Settings::ReachabilityServer).empty();

	if (named && !automation->measurement_owed)
	{
		if (wanted.activeMode)
			startDropCheck(*this, false);
		else
			startAddressLookup(*this, false);
	}

	shares->initialize();

	automation->settle();
}


QuickDC::Core::Core(const std::string& config_file, const std::string& session_file,
                    const std::string& queue_file, const std::string& chat_dir)
{
	startup = time(0);
	references = 0;

	config = new Preferences(config_file);
	session = new Preferences(session_file);

	connections = new ConnectionManager(eventLoop);
	hubs = new HubManager(*this);

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
	reachability6 = new QuickDC::Reachability();
	searchTokens = new QuickDC::SearchTokens();

	/* Only when asked for: it reads and writes the file it is given. */
	queue = queue_file.empty() ? 0 : new QuickDC::QueueManager(this, queue_file.c_str());

	/*
	 * Only where it was told to. This constructor exists so a caller can have a
	 * core that stays out of the user's own files, and writing their
	 * conversations into the real directory would be the very thing it avoids.
	 */
	chat = chat_dir.empty() ? 0 : new QuickDC::ChatArchive(chat_dir);

	/* Consulted before an upload starts, to count the ones in flight. */
	transfers = new TransferManager(eventLoop);

	server = 0;
	/* Nothing to forward to without a listening socket. */
	dropcheck = 0;
	dropRefresh = 0;
	portforward = 0;
	localHub = 0;

	identity = new ADC::CID(*config);
	automation = new Automation(this);
	automation->settle();
	service = new Api::Service(*this);
}

QuickDC::Core::~Core()
{
	/* First: what it holds reports into the rest. */
	delete service; service = 0;

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

	/* Before the check it starts, which it must not outlive. */
	delete dropRefresh; dropRefresh = 0;

	delete dropcheck; dropcheck = 0;

	/*
	 * Before the socket monitor is left behind, and deliberately without asking
	 * the gateway to release anything: that would mean talking to it while the
	 * event loop is being torn down. The finite lease is what cleans up.
	 */
	delete portforward; portforward = 0;

	delete automation; automation = 0;

	delete transfers; transfers = 0;
	delete server; server = 0;
	delete hubs; hubs = 0;
	delete securityManager; securityManager = 0;
	delete reachability6; reachability6 = 0;
	delete reachability; reachability = 0;
	delete searchTokens; searchTokens = 0;
	delete shares; shares = 0;
	/* After the hubs, which borrow it and write through it on the way out. */
	delete identity; identity = 0;
	delete session; session = 0;
	delete config; config = 0;
	delete hash; hash = 0;
	delete localHub; localHub = 0;
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
	eventLoop.monitor().wait(POLL_INTERVAL_MS);

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

	// Timers the work above made due, and then what was posted - which is also how
	// a dropped connection is freed, since it cannot be deleted from inside its own
	// socket's callback. Not eventLoop.wait(), which would deliver the messages
	// before the work above rather than after it.
	eventLoop.timers().process();
	eventLoop.messages().process();

	/* Last, so that whatever this turn changed is acted on in the same turn. */
	if (automation) automation->reconcile();
}




void QuickDC::Core::runUntilStopped(const std::function<void()>& running)
{
	inbox.open(eventLoop);
	if (running) running();

	while (!stopRequested.load(std::memory_order_acquire))
	{
		run();
		inbox.drain();
	}

	inbox.close();
}

bool QuickDC::Core::submit(Api::Command command)
{
	return post([this, command = std::move(command)] { service->execute(command); });
}

QuickDC::Api::SettingsSnapshot QuickDC::Core::settingsSnapshot() const
{
	return service->settingsSnapshot();
}

QuickDC::Subscription QuickDC::Core::subscribe(Api::EventSink* sink)
{
	return service->subscribe(sink);
}

void QuickDC::Core::requestStop()
{
	stopRequested.store(true, std::memory_order_release);
	inbox.wake();
}

const char* QuickDC::Core::getVersion() const 
{
	return PRODUCT " " VERSION " " SYSTEM "/" CPU
#ifdef BUILD
	 " (build " BUILD ")"
#endif
	 ;
}


