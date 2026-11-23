/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The core keeping the network in line with the configuration as it changes.
 *
 * Each case changes a setting the way a settings page does - a write to the
 * store and nothing else - turns the loop once, and looks at what moved. The
 * point being that nobody has to ask: no "apply" call, no "tell the hubs" call,
 * no restart.
 */

#include "quickdc.h"
#include "api/core.h"
#include "api/server.h"
#include "config/preferences.h"
#include "network/hubmanager.h"

#include <samurai/io/file.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socketaddress.h>

#include <unistd.h>

#include <memory>
#include <string>

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_networkplan_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

class Ignoring : public Samurai::IO::Net::ServerSocketEventHandler
{
	public:
		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }
		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket>) override { }
};

/* Whether a socket of our own can be bound to 'port' on the loopback just now. */
bool freeOnLoopback(uint16_t port)
{
	Ignoring nobody;
	Samurai::IO::Net::InetAddress loopback("127.0.0.1");
	Samurai::IO::Net::InetSocketAddress at(loopback, port);
	auto probe = Samurai::IO::Net::ServerSocket::create(&nobody, at);
	return probe && probe->listen();
}

uint16_t sparePort()
{
	Ignoring nobody;
	Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
	auto probe = Samurai::IO::Net::ServerSocket::create(&nobody, any);
	if (!probe || !probe->listen()) return 0;
	return probe->getLocalPort();
}

/* A hub-only core over scratch files, with a peer port of its own on the loopback. */
struct Fixture {
	std::string cfg = scratch("config.conf");
	std::string ses = scratch("session.conf");
	std::unique_ptr<QuickDC::Core> core;
	uint16_t first = 0;

	/* The configuration is on disk before the core starts, so it is already applied. */
	Fixture()
	{
		first = sparePort();
		{
			QuickDC::Preferences written(cfg);
			written.setString("Network", "Bind", "127.0.0.1");
			written.setString("Network", "Bind6", "");
			written.setNumber("Network", "Port", first);
			written.write();
		}

		core = std::make_unique<QuickDC::Core>(cfg, ses);

		QuickDC::DCServer::Endpoint at;
		at.port = first;
		at.bind = "127.0.0.1";
		at.bind6 = "";
		core->server = new QuickDC::DCServer(*core, at);
	}

	~Fixture()
	{
		delete core->server;
		core->server = nullptr;
		core.reset();
		Samurai::IO::File(cfg).remove();
		Samurai::IO::File(ses).remove();
	}
};

}

/* A new port is listened on at the next turn, and the old one let go of. */
EXO_TEST(networkplan_a_changed_port_is_listened_on_without_a_restart,
{
	Fixture f;
	if (!f.first) EXO_SKIP("no spare port");
	EXO_ASSERT(!freeOnLoopback(f.first));

	const uint16_t second = sparePort();
	if (!second || second == f.first) EXO_SKIP("no second spare port");

	f.core->config->setNumber("Network", "Port", second);
	f.core->run();

	EXO_ASSERT_EQ_INT(f.core->server->endpoint().port, second);
	EXO_ASSERT(f.core->server->listening(false));
	EXO_ASSERT(!freeOnLoopback(second));
	EXO_ASSERT(freeOnLoopback(f.first));
	return 1;
});

/* What a hub was told includes the port, so the hubs are told again. */
EXO_TEST(networkplan_a_changed_port_is_told_to_the_hubs,
{
	Fixture f;
	if (!f.first) EXO_SKIP("no spare port");
	EXO_ASSERT(!f.core->hubs->isClientInfoUpdatePending());

	const uint16_t second = sparePort();
	f.core->config->setNumber("Network", "Port", second);
	f.core->run();

	EXO_ASSERT(f.core->hubs->isClientInfoUpdatePending());
	return 1;
});

/* So is a changed nickname, which used to wait for somebody to ask. */
EXO_TEST(networkplan_a_changed_identity_is_told_to_the_hubs,
{
	Fixture f;
	EXO_ASSERT(!f.core->hubs->isClientInfoUpdatePending());

	f.core->config->setString("User", "Nickname", "somebody else");
	f.core->run();

	EXO_ASSERT(f.core->hubs->isClientInfoUpdatePending());
	return 1;
});

/* A setting nothing here depends on moves nothing. */
EXO_TEST(networkplan_an_unrelated_setting_changes_nothing,
{
	Fixture f;
	if (!f.first) EXO_SKIP("no spare port");

	f.core->config->setString("Interface", "Theme", "dark");
	f.core->run();

	EXO_ASSERT(!f.core->hubs->isClientInfoUpdatePending());
	EXO_ASSERT_EQ_INT(f.core->server->endpoint().port, f.first);
	return 1;
});

/* Writing a key back to what it holds is not a change, and does not rebind. */
EXO_TEST(networkplan_the_same_port_written_again_is_left_alone,
{
	Fixture f;
	if (!f.first) EXO_SKIP("no spare port");

	f.core->config->setNumber("Network", "Port", f.first);
	f.core->run();

	EXO_ASSERT(!f.core->hubs->isClientInfoUpdatePending());
	EXO_ASSERT(!freeOnLoopback(f.first));
	return 1;
});

/* A bind address emptied in a form and saved is the default, not an address. */
EXO_TEST(networkplan_an_empty_bind_address_is_the_default,
{
	Fixture f;
	if (!f.first) EXO_SKIP("no spare port");

	f.core->config->setString("Network", "Bind", "");
	f.core->config->setNumber("Network", "Port", sparePort());
	f.core->run();

	EXO_ASSERT(f.core->server->endpoint().bind == "0.0.0.0");
	EXO_ASSERT(f.core->server->listening(false));
	return 1;
});
