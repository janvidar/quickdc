/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "remote.h"
#include "json.h"
#include "sockets.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>

namespace Json = QuickDC::Cli::Json;
using QuickDC::Cli::acceptClosing;
using QuickDC::Cli::closeSocket;
using QuickDC::Cli::lastSocketError;
using QuickDC::Cli::socketError;
using QuickDC::Cli::streamSocket;
using QuickDC::Cli::waitFor;
using Clock = std::chrono::steady_clock;

namespace {

const int EXIT_PASSED = 0;
const int EXIT_FAILED = 1;

struct Address {
	bool local = false;
	std::string path;
	std::string host;
	std::string port;
};

bool parseEndpoint(const std::string& endpoint, Address& out, std::string& why)
{
	if (endpoint.find('/') != std::string::npos)
	{
		out.local = true;
		out.path = endpoint;
		if (out.path.size() >= sizeof(sockaddr_un::sun_path))
		{
			why = "'" + endpoint + "' is too long for a socket path";
			return false;
		}
		return true;
	}

	const size_t colon = endpoint.rfind(':');
	if (colon == std::string::npos)
	{
		out.host = "127.0.0.1";
		out.port = endpoint;
	}
	else
	{
		out.host = endpoint.substr(0, colon);
		out.port = endpoint.substr(colon + 1);
		if (out.host.size() > 1 && out.host.front() == '[' && out.host.back() == ']')
			out.host = out.host.substr(1, out.host.size() - 2);
	}

	if (out.host.empty() || out.port.empty() || out.port.find_first_not_of("0123456789") != std::string::npos)
	{
		why = "'" + endpoint + "' is not PORT, HOST:PORT or a socket path";
		return false;
	}
	return true;
}

/* Each address 'address' names, or an empty list with 'why' set. */
std::vector<std::pair<sockaddr_storage, socklen_t>> resolveAddress(const Address& address, bool passive,
                                                                   std::string& why)
{
	std::vector<std::pair<sockaddr_storage, socklen_t>> out;

	if (address.local)
	{
		sockaddr_storage storage {};
		auto* un = reinterpret_cast<sockaddr_un*>(&storage);
		un->sun_family = AF_UNIX;
		std::memcpy(un->sun_path, address.path.c_str(), address.path.size() + 1);
		out.emplace_back(storage, static_cast<socklen_t>(sizeof(sockaddr_un)));
		return out;
	}

	QuickDC::Cli::socketsReady();
	addrinfo hints {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = passive ? AI_PASSIVE : 0;

	addrinfo* found = nullptr;
	const int error = getaddrinfo(address.host.c_str(), address.port.c_str(), &hints, &found);
	if (error != 0)
	{
		why = address.host + ": " + gai_strerror(error);
		return out;
	}

	for (const addrinfo* at = found; at; at = at->ai_next)
	{
		sockaddr_storage storage {};
		std::memcpy(&storage, at->ai_addr, at->ai_addrlen);
		out.emplace_back(storage, at->ai_addrlen);
	}
	freeaddrinfo(found);
	return out;
}

std::string describeBound(int fd, const Address& address)
{
	if (address.local) return address.path;

	sockaddr_storage storage {};
	socklen_t size = sizeof(storage);
	if (getsockname(QuickDC::Cli::native(fd), reinterpret_cast<sockaddr*>(&storage), &size) != 0) return std::string();

	char host[INET6_ADDRSTRLEN] = {};
	char port[8] = {};
	if (getnameinfo(reinterpret_cast<sockaddr*>(&storage), size, host, sizeof(host), port, sizeof(port),
	                NI_NUMERICHOST | NI_NUMERICSERV) != 0)
		return std::string();

	return storage.ss_family == AF_INET6 ? "[" + std::string(host) + "]:" + port : std::string(host) + ":" + port;
}

/** Lines of JSON over one stream socket, without blocking on it. */
class Connection {
	public:
		explicit Connection(int fd) : fd(fd)
		{
			QuickDC::Cli::setNonBlocking(fd);
		}

		~Connection()
		{
			closeSocket(fd);
		}

		Connection(const Connection&) = delete;
		Connection& operator=(const Connection&) = delete;

		void send(const Json::Value& message)
		{
			send(message.serialized());
		}

		void send(const std::string& line)
		{
			outgoing += line;
			outgoing += '\n';
			flush();
		}

		void flush()
		{
			while (!outgoing.empty() && !ended)
			{
				const ssize_t sent = QuickDC::Cli::sendSome(fd, outgoing.data(), outgoing.size());
				const int error = sent < 0 ? socketError() : 0;
				if (sent > 0) outgoing.erase(0, static_cast<size_t>(sent));
				else if (sent < 0 && QuickDC::Cli::interrupted(error)) continue;
				else if (sent < 0 && QuickDC::Cli::wouldBlock(error)) return;
				else ended = true;
			}
		}

		/** Read whatever is there; false once the other end is gone. */
		bool read()
		{
			while (!ended)
			{
				char chunk[65536];
				const ssize_t got = QuickDC::Cli::receiveSome(fd, chunk, sizeof(chunk));
				const int error = got < 0 ? socketError() : 0;
				if (got > 0) incoming.append(chunk, static_cast<size_t>(got));
				else if (got < 0 && QuickDC::Cli::interrupted(error)) continue;
				else if (got < 0 && QuickDC::Cli::wouldBlock(error)) break;
				else ended = true;
			}
			return !ended;
		}

		/** The next whole line read, without its end. */
		std::optional<std::string> line()
		{
			const size_t end = incoming.find('\n');
			if (end == std::string::npos) return std::nullopt;
			std::string out = incoming.substr(0, end);
			incoming.erase(0, end + 1);
			return out;
		}

		/** Send everything queued, for as long as 'deadline' allows. */
		void drain(Clock::time_point deadline)
		{
			flush();
			while (!outgoing.empty() && !ended && Clock::now() < deadline)
			{
				waitFor(fd, POLLOUT, 50);
				flush();
			}
		}

		short events() const { return static_cast<short>(POLLIN | (outgoing.empty() ? 0 : POLLOUT)); }

		const int fd;
		bool ended = false;

	private:
		std::string incoming;
		std::string outgoing;
};

int milliseconds(Clock::time_point deadline)
{
	const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
	return static_cast<int>(std::clamp<long long>(left, 0, 1000));
}

bool isLoopback(const sockaddr_storage& storage)
{
	if (storage.ss_family == AF_INET)
		return (ntohl(reinterpret_cast<const sockaddr_in&>(storage).sin_addr.s_addr) >> 24) == 127;
	if (storage.ss_family == AF_INET6)
		return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6&>(storage).sin6_addr);
	return false;
}

std::string agentKey()
{
	const char* key = std::getenv("QDC_AGENT_KEY");
	return key ? key : "";
}

bool sameKey(const std::string& a, const std::string& b)
{
	unsigned char differs = a.size() != b.size();
	for (size_t n = 0; n < a.size() && n < b.size(); ++n) differs |= static_cast<unsigned char>(a[n] ^ b[n]);
	return differs == 0;
}

std::string text(const Json::Value& message, const char* key)
{
	const Json::Value* found = message.find(key);
	return found ? found->text() : std::string();
}

Json::Value message(std::initializer_list<std::pair<const char*, Json::Value>> members)
{
	Json::Object out;
	for (const auto& [key, value] : members) out.push_back({key, value});
	return Json::Value(std::move(out));
}

const char* outcomeName(QuickDC::Cli::Outcome outcome)
{
	switch (outcome)
	{
		case QuickDC::Cli::Outcome::Next:   return "next";
		case QuickDC::Cli::Outcome::Failed: return "failed";
		case QuickDC::Cli::Outcome::Quit:   return "quit";
	}
	return "failed";
}

class AgentRelay : public QuickDC::Cli::Relay {
	public:
		explicit AgentRelay(Connection& connection) : connection(connection) { }

		void heard(const std::string& line) override
		{
			connection.send(line);
		}

		void failed(const std::string& why) override
		{
			connection.send(message({{"qdc", "failed"}, {"why", why}}));
		}

		void noted(const std::string& text) override
		{
			connection.send(message({{"qdc", "note"}, {"text", text}}));
		}

		void learned(const std::string& name, const std::string& value) override
		{
			connection.send(message({{"qdc", "remember"}, {"name", name}, {"value", value}}));
		}

		void forgot(const std::string& core) override
		{
			connection.send(message({{"qdc", "forget"}, {"core", core}}));
		}

	private:
		Connection& connection;
};

}

namespace {

/* Whether the first line from 'connection', within a few seconds, is {"key":...} with 'key'. */
bool presentsKey(Connection& connection, const std::string& key)
{
	const auto deadline = Clock::now() + std::chrono::seconds(10);
	while (Clock::now() < deadline)
	{
		if (std::optional<std::string> line = connection.line())
		{
			const std::optional<Json::Value> said = Json::parse(*line);
			const Json::Value* given = said ? said->find("key") : nullptr;
			return given && sameKey(given->text(), key);
		}
		waitFor(connection.fd, POLLIN, milliseconds(deadline));
		if (!connection.read() && !connection.line()) return false;
	}
	return false;
}

}

int QuickDC::Cli::listenOn(const std::string& endpoint, std::string& bound, std::string& why)
{
	Address address;
	if (!parseEndpoint(endpoint, address, why)) return -1;

	const auto candidates = resolveAddress(address, true, why);
	if (candidates.empty()) return -1;

	for (const auto& [storage, size] : candidates)
	{
		const int fd = streamSocket(storage.ss_family);
		if (fd == -1)
		{
			why = lastSocketError();
			continue;
		}

		if (!address.local && !isLoopback(storage) && agentKey().empty())
		{
			why = endpoint + " is reachable from other hosts; set QDC_AGENT_KEY on the agent and its controller";
			closeSocket(fd);
			return -1;
		}

		std::error_code ignored;
		if (!address.local) QuickDC::Cli::reuseAddress(fd);
		else std::filesystem::remove(address.path, ignored);

		/* The socket file is the owner's alone; Windows has no such mode to give it. */
#ifndef _WIN32
		const mode_t mask = address.local ? umask(0177) : 0;
#endif
		const bool isBound = ::bind(QuickDC::Cli::native(fd), reinterpret_cast<const sockaddr*>(&storage), size) == 0;
#ifndef _WIN32
		if (address.local) umask(mask);
#endif

		if (isBound && ::listen(QuickDC::Cli::native(fd), 4) == 0)
		{
			bound = describeBound(fd, address);
			return fd;
		}

		why = endpoint + ": " + lastSocketError();
		closeSocket(fd);
	}
	return -1;
}

int QuickDC::Cli::serveController(int listener, const std::vector<std::string>& cores, Runner& runner,
                                  Session& session, const std::function<bool()>& start)
{
	const std::string key = agentKey();
	std::unique_ptr<Connection> accepted;
	while (!accepted)
	{
		const int fd = acceptClosing(listener);
		if (fd == -1)
		{
			const int error = socketError();
			if (QuickDC::Cli::interrupted(error) || QuickDC::Cli::aborted(error)) continue;
			std::fprintf(stderr, "qdc: accept: %s\n", QuickDC::Cli::errorText(error).c_str());
			return EXIT_FAILED;
		}

		accepted = std::make_unique<Connection>(fd);
		if (!presentsKey(*accepted, key))
		{
			std::fprintf(stderr, "qdc: refused a controller without the agent key\n");
			accepted.reset();
		}
	}
	closeSocket(listener);

	Connection& connection = *accepted;
	AgentRelay relay(connection);
	session.setRelay(&relay);

	Json::Array names(cores.begin(), cores.end());
	connection.send(message({{"qdc", "hello"}, {"cores", Json::Value(std::move(names))}}));

	Grammar grammar;
	grammar.cores = cores;

	bool started = false;
	bool stopping = false;

	const auto done = [&connection](Outcome outcome) {
		connection.send(message({{"qdc", "done"}, {"outcome", outcomeName(outcome)}}));
	};

	const auto startOnce = [&]() {
		if (started) return true;
		started = true;
		if (start()) return true;
		relay.failed("the cores did not start; the agent's own output says why");
		return false;
	};

	while (!stopping && !connection.ended)
	{
		waitFor(connection.fd, connection.events(), 50);
		session.pump(Clock::now());
		connection.flush();
		if (!connection.read()) break;

		while (std::optional<std::string> line = connection.line())
		{
			const std::optional<Json::Value> received = Json::parse(*line);
			if (!received || !received->isObject())
			{
				relay.failed("the controller sent what is not a JSON object: " + *line);
				done(Outcome::Failed);
				continue;
			}

			if (received->find("stop"))
			{
				stopping = true;
				break;
			}

			if (const Json::Value* name = received->find("remember"))
			{
				session.adopt(name->text(), text(*received, "value"));
				continue;
			}

			if (received->find("start"))
			{
				done(startOnce() ? Outcome::Next : Outcome::Failed);
				continue;
			}

			const Json::Value* run = received->find("run");
			if (!run)
			{
				relay.failed("the controller sent no command: " + *line);
				done(Outcome::Failed);
				continue;
			}

			Statement statement;
			try
			{
				if (!parseLine(grammar, 0, run->text(), statement))
				{
					done(Outcome::Next);
					continue;
				}
				if (statement.beforeStart && started)
					throw ScriptError("'" + statement.line.verb + "' is taken before the cores start, and they have");
			}
			catch (const ScriptError& e)
			{
				relay.failed(e.what());
				done(Outcome::Failed);
				continue;
			}

			if (!statement.beforeStart && !statement.anytime && !startOnce())
			{
				done(Outcome::Failed);
				continue;
			}

			done(statement.step(session));
		}
	}

	runner.stop();
	session.pump(Clock::now());

	if (stopping) connection.send(message({{"qdc", "stopped"}}));
	connection.drain(Clock::now() + std::chrono::seconds(10));
	session.setRelay(nullptr);
	return EXIT_PASSED;
}

namespace {

/** A qdc process running some of the cores, as the controller sees it. */
struct Agent {
	std::string endpoint;
	std::vector<std::string> cores;
	std::unique_ptr<Connection> connection;
	bool stopped = false;
};

std::string describeAgent(const Agent& agent)
{
	std::string names;
	for (const std::string& core : agent.cores) names += (names.empty() ? "" : ", ") + core;
	return "the agent at " + agent.endpoint + " (core " + names + ")";
}

int connectTo(const std::string& endpoint, std::string& why)
{
	Address address;
	if (!parseEndpoint(endpoint, address, why)) return -1;

	/* An agent in a container that is still coming up refuses for a while. */
	const auto deadline = Clock::now() + std::chrono::seconds(10);
	while (true)
	{
		const auto candidates = resolveAddress(address, false, why);
		for (const auto& [storage, size] : candidates)
		{
			const int fd = streamSocket(storage.ss_family);
			if (fd == -1) continue;
			if (::connect(QuickDC::Cli::native(fd), reinterpret_cast<const sockaddr*>(&storage), size) == 0) return fd;
			why = endpoint + ": " + lastSocketError();
			closeSocket(fd);
		}

		if (Clock::now() >= deadline) return -1;
		waitFor(nullptr, 0, 100);
	}
}

class Controller : public QuickDC::Cli::Feed {
	public:
		explicit Controller(QuickDC::Cli::Session& session) : session(session) { }

		bool connect(const std::vector<std::pair<std::string, std::string>>& wanted)
		{
			for (const auto& [core, endpoint] : wanted)
			{
				Agent*& agent = byEndpoint[endpoint];
				if (!agent)
				{
					std::string why;
					const int fd = connectTo(endpoint, why);
					if (fd == -1)
					{
						std::fprintf(stderr, "qdc: cannot reach the agent for core '%s': %s\n", core.c_str(), why.c_str());
						return false;
					}

					agents.push_back(std::make_unique<Agent>());
					agent = agents.back().get();
					agent->endpoint = endpoint;
					agent->connection = std::make_unique<Connection>(fd);
					agent->connection->send(message({{"key", agentKey()}}));
					if (!hello(*agent)) return false;
				}

				if (std::find(agent->cores.begin(), agent->cores.end(), core) == agent->cores.end())
				{
					std::fprintf(stderr, "qdc: the agent at %s runs no core '%s'\n", endpoint.c_str(), core.c_str());
					return false;
				}
				byCore[core] = agent;
			}
			return true;
		}

		std::optional<QuickDC::Cli::Arrival> next(Clock::time_point deadline) override
		{
			while (true)
			{
				for (const auto& agent : agents)
				{
					while (std::optional<std::string> line = agent->connection->line())
					{
						std::optional<QuickDC::Cli::Arrival> arrival = handle(*agent, *line);
						if (arrival) return arrival;
					}
				}

				if (!lost.empty() || answered || Clock::now() >= deadline) return std::nullopt;

				std::vector<QuickDC::Cli::Watched> watched;
				for (const auto& agent : agents)
					watched.push_back({agent->connection->ended ? -1 : agent->connection->fd,
					                   agent->connection->events()});
				waitFor(watched.data(), watched.size(), milliseconds(deadline));

				for (const auto& agent : agents)
				{
					agent->connection->flush();
					if (!agent->connection->read() && !agent->stopped && lost.empty())
						lost = describeAgent(*agent) + " went away";
				}
			}
		}

		void learned(const std::string& name, const std::string& value) override
		{
			for (const auto& agent : agents)
				agent->connection->send(message({{"remember", name}, {"value", value}}));
		}

		std::string broken() const override { return lost; }

		/** Run 'statement' on the agents it is for. */
		QuickDC::Cli::Outcome dispatch(const QuickDC::Cli::Grammar& grammar, const QuickDC::Cli::Statement& statement)
		{
			const QuickDC::Cli::Line& line = statement.line;

			std::string expanded;
			try
			{
				expanded = QuickDC::Cli::expandDefinitions(grammar, line.source);
			}
			catch (const QuickDC::Cli::ScriptError& e)
			{
				return session.fail(line, e.what());
			}

			QuickDC::Cli::Outcome outcome = QuickDC::Cli::Outcome::Next;
			for (Agent* agent : targets(line))
			{
				const QuickDC::Cli::Outcome one = ask(*agent, line, message({{"run", expanded}}));
				if (one != QuickDC::Cli::Outcome::Next) outcome = one;
				if (one == QuickDC::Cli::Outcome::Failed) break;
			}
			return outcome;
		}

		/** Start every agent's cores. */
		QuickDC::Cli::Outcome start(const QuickDC::Cli::Line& line)
		{
			for (const auto& agent : agents)
				if (ask(*agent, line, message({{"start", true}})) != QuickDC::Cli::Outcome::Next)
					return QuickDC::Cli::Outcome::Failed;
			return QuickDC::Cli::Outcome::Next;
		}

		/** Stop every agent, hearing what their cores say on the way out. */
		void stop()
		{
			for (const auto& agent : agents) agent->connection->send(message({{"stop", true}}));

			const auto deadline = Clock::now() + std::chrono::seconds(30);
			while (std::any_of(agents.begin(), agents.end(), [](const auto& agent) { return !agent->stopped; }))
			{
				if (std::optional<QuickDC::Cli::Arrival> arrival = next(deadline))
					session.hear(std::move(*arrival));
				else if (!lost.empty() || Clock::now() >= deadline)
					break;
			}
		}

	private:
		bool hello(Agent& agent)
		{
			const auto deadline = Clock::now() + std::chrono::seconds(30);
			while (Clock::now() < deadline)
			{
				if (std::optional<std::string> line = agent.connection->line())
				{
					const std::optional<Json::Value> said = Json::parse(*line);
					const Json::Value* cores = said ? said->find("cores") : nullptr;
					if (!said || text(*said, "qdc") != "hello" || !cores || !cores->isArray()) break;
					for (const Json::Value& core : cores->elements()) agent.cores.push_back(core.text());
					return true;
				}

				waitFor(agent.connection->fd, POLLIN, milliseconds(deadline));
				if (!agent.connection->read() && !agent.connection->line()) break;
			}

			std::fprintf(stderr, "qdc: %s did not say hello\n", agent.endpoint.c_str());
			return false;
		}

		std::vector<Agent*> targets(const QuickDC::Cli::Line& line) const
		{
			if (!line.core.empty()) return {byCore.at(line.core)};

			std::vector<Agent*> out;
			for (const auto& agent : agents) out.push_back(agent.get());
			return out;
		}

		QuickDC::Cli::Outcome ask(Agent& agent, const QuickDC::Cli::Line& line, const Json::Value& request)
		{
			if (!lost.empty()) return session.fail(line, lost);

			current = &line;
			waiting = &agent;
			answered.reset();
			agent.connection->send(request);

			while (!answered && lost.empty())
				if (std::optional<QuickDC::Cli::Arrival> arrival = next(Clock::now() + std::chrono::seconds(1)))
					session.hear(std::move(*arrival));

			current = nullptr;
			waiting = nullptr;

			if (!answered) return session.fail(line, lost);
			const QuickDC::Cli::Outcome outcome = *answered;
			answered.reset();
			return outcome;
		}

		std::optional<QuickDC::Cli::Arrival> handle(Agent& agent, const std::string& line)
		{
			std::optional<Json::Value> said = Json::parse(line);
			if (!said || !said->isObject())
			{
				std::fprintf(stderr, "qdc: %s sent what is not JSON: %s\n", agent.endpoint.c_str(), line.c_str());
				return std::nullopt;
			}

			const std::string kind = text(*said, "qdc");
			if (kind.empty())
			{
				std::string core = text(*said, "core");
				return QuickDC::Cli::Arrival{std::move(core), std::move(*said), line};
			}

			if (kind == "done")
			{
				if (&agent == waiting)
				{
					const std::string outcome = text(*said, "outcome");
					answered = outcome == "next" ? QuickDC::Cli::Outcome::Next
					         : outcome == "quit" ? QuickDC::Cli::Outcome::Quit
					         : QuickDC::Cli::Outcome::Failed;
				}
			}
			else if (kind == "failed")
			{
				const std::string why = text(*said, "why");
				if (current) session.fail(*current, why);
				else std::fprintf(stderr, "qdc: %s: %s\n", agent.endpoint.c_str(), why.c_str());
			}
			else if (kind == "note")
			{
				session.note(text(*said, "text"));
			}
			else if (kind == "remember")
			{
				session.learn(text(*said, "name"), text(*said, "value"));
			}
			else if (kind == "forget")
			{
				session.forget(text(*said, "core"));
			}
			else if (kind == "stopped")
			{
				agent.stopped = true;
			}
			return std::nullopt;
		}

		QuickDC::Cli::Session& session;
		std::vector<std::unique_ptr<Agent>> agents;
		std::map<std::string, Agent*> byEndpoint;
		std::map<std::string, Agent*> byCore;
		std::string lost;

		const QuickDC::Cli::Line* current = nullptr;
		Agent* waiting = nullptr;
		std::optional<QuickDC::Cli::Outcome> answered;
};

}

int QuickDC::Cli::control(const std::vector<std::pair<std::string, std::string>>& agents,
                          const Grammar& grammar, const std::vector<Statement>& statements, Session& session)
{
	Controller controller(session);
	session.setFeed(&controller);

	struct Unfeed {
		Session& session;
		~Unfeed() { session.setFeed(nullptr); }
	} unfeed{session};

	if (!controller.connect(agents)) return EXIT_FAILED;

	const bool quiet = session.options().quiet;
	const bool keepGoing = session.options().keepGoing;

	int status = EXIT_PASSED;
	bool started = false;

	for (const Statement& statement : statements)
	{
		if (!started && !statement.beforeStart && !statement.anytime)
		{
			started = true;
			if (controller.start(statement.line) != Outcome::Next)
			{
				status = EXIT_FAILED;
				break;
			}
		}

		if (!quiet) session.note("> " + statement.line.source);

		const Outcome outcome = statement.script ? statement.step(session) : controller.dispatch(grammar, statement);
		if (outcome == Outcome::Quit) break;
		if (outcome == Outcome::Failed)
		{
			status = EXIT_FAILED;
			if (!keepGoing || !controller.broken().empty()) break;
		}
	}

	controller.stop();
	return status;
}
