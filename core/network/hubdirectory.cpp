/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/hubdirectory.h"
#include "network/hublist.h"
#include "config/preferences.h"
#include "api/huburl.h"

#include <samurai/crypto/digest/sha.h>
#include <samurai/io/net/url.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

class QuickDC::HubDirectory::Fetch final : public QuickDC::HubListListener {
	public:
		Fetch(HubDirectory& owner, std::string url)
			: owner(owner)
			, url(std::move(url))
			, list(owner.loop, this)
		{
		}

		void start()
		{
			const Samurai::IO::Net::URL parsed(url);
			if (!parsed.isValid())
			{
				EventHubListFailure("it is not an address a hublist can be fetched from");
				return;
			}
			list.download(parsed);
		}

		void EventHubList(const std::vector<QuickDC::HubListEntry>& hubs) override
		{
			owner.fetched(url, hubs);
		}

		void EventHubListFailure(const char* reason) override
		{
			owner.failed(url, reason ? reason : "");
		}

	private:
		HubDirectory& owner;
		std::string url;
		QuickDC::HubList list;
};

namespace {

const char GROUP[] = "Hublists";
const char TOTAL[] = "Total";

std::string listKey(int index)
{
	return "List " + std::to_string(index);
}

std::string trimmed(const std::string& text)
{
	const size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/*
 * What decides that two entries are the same hub: the canonical address down to
 * where the hub is and no further. What follows the authority says how to check it
 * - a keyprint above all, which one list carries and another leaves off - and kept
 * in the key it would file one hub under two rows. Lowered, since neither a host
 * nor a scheme is case sensitive and the lists disagree about both.
 */
std::string mergeKey(const std::string& address)
{
	std::string key = QuickDC::hubSettingsGroup(address);
	std::transform(key.begin(), key.end(), key.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return key;
}

/* Looked for past the authority, so a host with 'kp' in its name is not taken for one. */
bool hasKeyprint(const std::string& address)
{
	const size_t query = address.find('?');
	if (query == std::string::npos) return false;

	std::string rest = address.substr(query);
	std::transform(rest.begin(), rest.end(), rest.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return rest.find("kp=") != std::string::npos;
}

/*
 * The cache is the JSON the interface wrote before the core kept the directory:
 *
 *     {"fetched":1769000000,"hubs":[{"address":"...","name":"...",...}],"url":"..."}
 *
 * Read with a reader for what JSON can say, not only for what that writer said,
 * since the files are already on people's disks.
 */
struct JsonMember;

struct Json {
	enum class Kind { Null, Bool, Number, String, Array, Object };

	Kind kind = Kind::Null;
	std::string text;
	std::vector<Json> items;
	std::vector<JsonMember> members;

	const Json* member(std::string_view key) const;
	std::string string(std::string_view key) const;
	/* Exact for an integer, which a share total in bytes needs; nought for anything negative. */
	uint64_t number(std::string_view key) const;
};

/* Not a std::pair: instantiating one asks whether Json can be default
 * constructed while Json is still incomplete, which clang refuses. */
struct JsonMember {
	std::string name;
	Json value;
};

const Json* Json::member(std::string_view key) const
{
	for (const auto& [name, value] : members)
		if (name == key) return &value;
	return nullptr;
}

std::string Json::string(std::string_view key) const
{
	const Json* found = member(key);
	return found && found->kind == Kind::String ? found->text : std::string();
}

uint64_t Json::number(std::string_view key) const
{
	const Json* found = member(key);
	if (!found || found->kind != Kind::Number || found->text.empty() || found->text[0] == '-')
		return 0;
	if (found->text.find_first_of(".eE") == std::string::npos)
		return std::strtoull(found->text.c_str(), nullptr, 10);
	const double value = std::strtod(found->text.c_str(), nullptr);
	return value > 0 && value < 1.8e19 ? static_cast<uint64_t>(value) : 0;
}

class JsonReader {
	public:
		explicit JsonReader(std::string_view in) : in(in) { }

		std::optional<Json> document()
		{
			Json value;
			if (!parse(value, 0)) return std::nullopt;
			space();
			if (at != in.size()) return std::nullopt;
			return value;
		}

	private:
		static constexpr int DEPTH = 32;

		void space()
		{
			while (at < in.size() && (in[at] == ' ' || in[at] == '\t' || in[at] == '\n' || in[at] == '\r'))
				++at;
		}

		bool literal(std::string_view word)
		{
			if (in.substr(at, word.size()) != word) return false;
			at += word.size();
			return true;
		}

		bool parse(Json& out, int depth)
		{
			if (depth > DEPTH) return false;
			space();
			if (at >= in.size()) return false;

			switch (in[at])
			{
				case '{': return object(out, depth);
				case '[': return array(out, depth);
				case '"': out.kind = Json::Kind::String; return string(out.text);
				case 't': out.kind = Json::Kind::Bool; out.text = "true"; return literal("true");
				case 'f': out.kind = Json::Kind::Bool; out.text = "false"; return literal("false");
				case 'n': out.kind = Json::Kind::Null; return literal("null");
				default: return number(out);
			}
		}

		bool object(Json& out, int depth)
		{
			out.kind = Json::Kind::Object;
			++at;
			space();
			if (at < in.size() && in[at] == '}') { ++at; return true; }

			while (true)
			{
				space();
				std::string key;
				if (at >= in.size() || in[at] != '"' || !string(key)) return false;
				space();
				if (at >= in.size() || in[at++] != ':') return false;

				Json value;
				if (!parse(value, depth + 1)) return false;
				out.members.push_back({std::move(key), std::move(value)});

				space();
				if (at >= in.size()) return false;
				if (in[at] == ',') { ++at; continue; }
				if (in[at] == '}') { ++at; return true; }
				return false;
			}
		}

		bool array(Json& out, int depth)
		{
			out.kind = Json::Kind::Array;
			++at;
			space();
			if (at < in.size() && in[at] == ']') { ++at; return true; }

			while (true)
			{
				Json value;
				if (!parse(value, depth + 1)) return false;
				out.items.push_back(std::move(value));

				space();
				if (at >= in.size()) return false;
				if (in[at] == ',') { ++at; continue; }
				if (in[at] == ']') { ++at; return true; }
				return false;
			}
		}

		bool number(Json& out)
		{
			const size_t from = at;
			while (at < in.size() && (std::isdigit(static_cast<unsigned char>(in[at]))
				|| in[at] == '-' || in[at] == '+' || in[at] == '.' || in[at] == 'e' || in[at] == 'E'))
				++at;
			if (at == from) return false;
			out.kind = Json::Kind::Number;
			out.text = std::string(in.substr(from, at - from));
			return true;
		}

		bool hex4(uint32_t& out)
		{
			if (at + 4 > in.size()) return false;
			out = 0;
			for (int n = 0; n < 4; ++n)
			{
				const char c = in[at++];
				out <<= 4;
				if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
				else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
				else return false;
			}
			return true;
		}

		static void utf8(uint32_t code, std::string& out)
		{
			if (code < 0x80)
			{
				out += static_cast<char>(code);
			}
			else if (code < 0x800)
			{
				out += static_cast<char>(0xC0 | (code >> 6));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
			else if (code < 0x10000)
			{
				out += static_cast<char>(0xE0 | (code >> 12));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xF0 | (code >> 18));
				out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
		}

		bool string(std::string& out)
		{
			++at;
			while (at < in.size())
			{
				const char c = in[at++];
				if (c == '"') return true;
				if (c != '\\')
				{
					out += c;
					continue;
				}

				if (at >= in.size()) return false;
				const char escaped = in[at++];
				switch (escaped)
				{
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u':
					{
						uint32_t code = 0;
						if (!hex4(code)) return false;
						if (code >= 0xD800 && code < 0xDC00 && in.substr(at, 2) == "\\u")
						{
							at += 2;
							uint32_t low = 0;
							if (!hex4(low)) return false;
							if (low >= 0xDC00 && low < 0xE000)
								code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
							else
								code = 0xFFFD;
						}
						else if (code >= 0xD800 && code < 0xE000)
						{
							code = 0xFFFD;
						}
						utf8(code, out);
						break;
					}
					default: return false;
				}
			}
			return false;
		}

		std::string_view in;
		size_t at = 0;
};

void quoted(std::string& out, const std::string& text)
{
	out += '"';
	for (const char c : text)
	{
		switch (c)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
				{
					char escaped[8];
					std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
					out += escaped;
				}
				else
				{
					out += c;
				}
		}
	}
	out += '"';
}

/* Only what the list said: most fields are empty in most entries, and the file is read on every start. */
void encoded(std::string& out, const QuickDC::HubListEntry& hub)
{
	out += "{\"address\":";
	quoted(out, hub.address);

	const auto text = [&out](const char* key, const std::string& value) {
		if (value.empty()) return;
		out += ",\"";
		out += key;
		out += "\":";
		quoted(out, value);
	};
	const auto number = [&out](const char* key, uint64_t value) {
		if (!value) return;
		out += ",\"";
		out += key;
		out += "\":";
		out += std::to_string(value);
	};

	text("country", hub.country);
	text("description", hub.description);
	number("maxhubs", hub.maxhubs);
	number("maxusers", hub.maxusers);
	number("minshare", hub.minshare);
	number("minslots", hub.minslots);
	text("name", hub.name);
	number("shared", hub.shared);
	number("users", hub.users);
	out += '}';
}

}

QuickDC::HubDirectory::HubDirectory(Samurai::EventLoop& loop_, Preferences& config_, std::string cacheDir_,
                                    std::function<void()> sourcesChanged_, std::function<void()> hubsChanged_)
	: loop(loop_)
	, config(config_)
	, cacheDir(std::move(cacheDir_))
	, sourcesChanged(std::move(sourcesChanged_))
	, hubsChanged(std::move(hubsChanged_))
{
	load();
	for (Source& source : list) readCache(source);
	merge();
}

QuickDC::HubDirectory::~HubDirectory() = default;

std::vector<std::string> QuickDC::HubDirectory::defaultSources()
{
	/*
	 * The lists that have been up for years and are what other clients ship, https
	 * where the server offers it. Ordered by how well described the lists are rather
	 * than by size: where two name the same hub, the first one's description is shown.
	 */
	return {
		"https://dchublist.org/hublist.xml.bz2",
		"https://te-home.net/?do=hublist&get=hublist.xml.bz2",
		"https://hublist.eu/hublist.xml.bz2"
	};
}

std::vector<std::string> QuickDC::HubDirectory::retiredSources()
{
	/* dchublist.biz answers 403 to everything, its own front page included. */
	return { "https://dchublist.biz/hublist.xml.bz2" };
}

std::string QuickDC::HubDirectory::cacheFile(const std::string& cacheDir, const std::string& url)
{
	/*
	 * Named by a digest of the address, which carries slashes and query strings and
	 * is not a file name. The address is inside the file too, so a stale file or a
	 * collision is recognised rather than read as another source's list.
	 */
	Samurai::Crypto::Digest::Sha1 sha;
	sha.update(url.data(), url.size());

	static const char HEX[] = "0123456789abcdef";
	std::string name;
	for (const uint8_t byte : sha.digest().bytes())
	{
		name += HEX[byte >> 4];
		name += HEX[byte & 0x0F];
	}
	return cacheDir + "/" + name + ".json";
}

void QuickDC::HubDirectory::load()
{
	/*
	 * Asked before anything is read, so that "no sources configured" and "every
	 * source deliberately removed" - a Total of nought - stay two different things.
	 */
	const bool configured = config.haveSetting(GROUP, TOTAL);

	std::vector<std::string> urls;
	bool rewritten = false;

	if (configured)
	{
		const std::vector<std::string> retired = retiredSources();
		stored = std::clamp(config.peekNumber(GROUP, TOTAL, 0), 0, SOURCE_LIMIT);

		for (int n = 0; n < stored; ++n)
		{
			const std::string url = trimmed(config.peekString(GROUP, listKey(n), ""));
			if (url.empty() || std::find(urls.begin(), urls.end(), url) != urls.end()) continue;

			if (std::find(retired.begin(), retired.end(), url) != retired.end())
			{
				rewritten = true;
				continue;
			}
			urls.push_back(url);
		}
	}
	else
	{
		urls = defaultSources();
		rewritten = true;
	}

	for (const std::string& url : urls)
	{
		Source source;
		source.url = url;
		list.push_back(std::move(source));
	}

	/* Written again when a withdrawn one was dropped, or it is skipped in the file for ever. */
	if (rewritten) store();
}

void QuickDC::HubDirectory::store()
{
	const int count = static_cast<int>(list.size());
	for (int n = 0; n < count; ++n) config.setString(GROUP, listKey(n), list[n].url);
	for (int n = count; n < stored; ++n) config.remove(GROUP, listKey(n));
	config.setNumber(GROUP, TOTAL, count);
	config.write();
	stored = count;
}

void QuickDC::HubDirectory::readCache(Source& source) const
{
	if (cacheDir.empty()) return;

	std::ifstream file(cacheFile(cacheDir, source.url), std::ios::binary);
	if (!file) return;

	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const std::optional<Json> document = JsonReader(text).document();
	if (!document || document->kind != Json::Kind::Object) return;

	if (document->string("url") != source.url) return;

	const uint64_t fetched = document->number("fetched");
	if (!fetched) return;

	const Json* listed = document->member("hubs");
	if (!listed || listed->kind != Json::Kind::Array) return;

	std::vector<HubListEntry> hubs;
	hubs.reserve(std::min(listed->items.size(), PER_SOURCE_LIMIT));

	for (const Json& item : listed->items)
	{
		if (hubs.size() >= PER_SOURCE_LIMIT) break;
		if (item.kind != Json::Kind::Object) continue;

		HubListEntry hub;
		hub.address = item.string("address");
		if (hub.address.empty()) continue;

		hub.name = item.string("name");
		hub.description = item.string("description");
		hub.country = item.string("country");
		hub.users = item.number("users");
		hub.shared = item.number("shared");
		hub.minshare = item.number("minshare");
		hub.minslots = item.number("minslots");
		hub.maxhubs = item.number("maxhubs");
		hub.maxusers = item.number("maxusers");
		hubs.push_back(std::move(hub));
	}

	source.hubs = std::move(hubs);
	source.fetched = fetched;
	source.cached = true;
}

void QuickDC::HubDirectory::writeCache(const Source& source) const
{
	if (cacheDir.empty()) return;

	std::error_code error;
	std::filesystem::create_directories(cacheDir, error);

	std::string out = "{\"fetched\":" + std::to_string(source.fetched) + ",\"hubs\":[";
	for (size_t n = 0; n < source.hubs.size(); ++n)
	{
		if (n) out += ',';
		encoded(out, source.hubs[n]);
	}
	out += "],\"url\":";
	quoted(out, source.url);
	out += '}';

	/* Beside and renamed over, since a file cut short parses as a list missing its tail. */
	const std::string path = cacheFile(cacheDir, source.url);
	const std::string partial = path + ".part";
	{
		std::ofstream file(partial, std::ios::binary | std::ios::trunc);
		file.write(out.data(), static_cast<std::streamsize>(out.size()));
		if (!file.flush())
		{
			file.close();
			std::filesystem::remove(partial, error);
			return;
		}
	}
	std::filesystem::rename(partial, path, error);
	if (error) std::filesystem::remove(partial, error);
}

void QuickDC::HubDirectory::merge()
{
	std::vector<Api::DirectoryHub> hubs;
	std::vector<std::string> from;
	std::unordered_map<std::string, size_t> seen;

	for (const Source& source : list)
	{
		if (source.hubs.empty()) continue;
		const uint32_t index = static_cast<uint32_t>(from.size());
		from.push_back(source.url);

		for (const HubListEntry& hub : source.hubs)
		{
			const auto [known, added] = seen.try_emplace(mergeKey(hub.address), hubs.size());
			if (added)
			{
				if (hubs.size() >= MERGED_LIMIT)
				{
					seen.erase(known);
					break;
				}
				hubs.push_back(Api::DirectoryHub{hub, {index}});
				continue;
			}

			Api::DirectoryHub& kept = hubs[known->second];
			if (kept.listedBy.back() != index) kept.listedBy.push_back(index);

			/*
			 * The first list's description, gaining from the others only what it left
			 * blank. The user count is the exception: a stale list is wrong about it
			 * rather than terse, and the larger figure is the one not made up.
			 */
			HubListEntry& entry = kept.hub;
			if (entry.name.empty()) entry.name = hub.name;
			if (entry.description.empty()) entry.description = hub.description;
			if (entry.country.empty()) entry.country = hub.country;
			entry.users = std::max(entry.users, hub.users);

			/*
			 * An address that says which certificate to expect wins over one that does
			 * not, whichever list came first: without it there is nothing to check a
			 * self-signed certificate against. Between two keyprints the first list
			 * still wins, since nothing here can tell which is current.
			 */
			if (!hasKeyprint(entry.address) && hasKeyprint(hub.address)) entry.address = hub.address;
		}
	}

	mergedSources = std::move(from);
	merged = std::move(hubs);
}

std::vector<QuickDC::Api::HubListSource> QuickDC::HubDirectory::sources() const
{
	std::vector<Api::HubListSource> told;
	told.reserve(list.size());
	for (const Source& source : list)
		told.push_back(Api::HubListSource{source.url, source.fetched, source.hubs.size(),
		                                  source.cached, source.fetching, source.error});
	return told;
}

bool QuickDC::HubDirectory::due(const Source& source, uint64_t now)
{
	if (source.attempted && now < source.attempted + RETRY_MINUTES * 60) return false;

	/* An empty list is worth asking again about: it is what a server mid-update returns. */
	return !source.fetched || source.hubs.empty()
		|| now >= source.fetched + static_cast<uint64_t>(MAX_AGE_HOURS) * 3600;
}

void QuickDC::HubDirectory::fetch(Source& source, uint64_t now)
{
	source.fetching = true;
	source.attempted = now;

	std::unique_ptr<Fetch>& one = fetches[source.url];
	if (!one) one = std::make_unique<Fetch>(*this, source.url);
	one->start();
}

void QuickDC::HubDirectory::refresh(bool force, uint64_t now)
{
	bool started = false;
	for (Source& source : list)
	{
		if (source.fetching || (!force && !due(source, now))) continue;
		started = true;
		fetch(source, now);
	}
	if (started) sourcesChanged();
}

bool QuickDC::HubDirectory::add(const std::string& given, uint64_t now)
{
	const std::string url = trimmed(given);
	if (url.empty() || find(url) || static_cast<int>(list.size()) >= SOURCE_LIMIT) return false;

	Source source;
	source.url = url;
	readCache(source);
	list.push_back(std::move(source));
	store();

	if (!list.back().hubs.empty())
	{
		merge();
		hubsChanged();
	}

	Source& added = list.back();
	if (due(added, now)) fetch(added, now);
	sourcesChanged();
	return true;
}

bool QuickDC::HubDirectory::remove(const std::string& given)
{
	const std::string url = trimmed(given);
	const auto found = std::find_if(list.begin(), list.end(), [&url](const Source& s) { return s.url == url; });
	if (found == list.end()) return false;

	const bool listed = !found->hubs.empty();
	list.erase(found);
	fetches.erase(url);
	store();

	if (!cacheDir.empty())
	{
		std::error_code error;
		std::filesystem::remove(cacheFile(cacheDir, url), error);
	}

	if (listed)
	{
		merge();
		hubsChanged();
	}
	sourcesChanged();
	return true;
}

bool QuickDC::HubDirectory::reset()
{
	const std::vector<std::string> defaults = defaultSources();

	std::vector<std::string> current;
	for (const Source& source : list) current.push_back(source.url);
	if (current == defaults) return false;

	std::vector<Source> kept;
	for (const std::string& url : defaults)
	{
		const auto found = std::find_if(list.begin(), list.end(), [&url](const Source& s) { return s.url == url; });
		if (found != list.end())
		{
			kept.push_back(std::move(*found));
			continue;
		}
		Source source;
		source.url = url;
		readCache(source);
		kept.push_back(std::move(source));
	}

	for (const std::string& url : current)
		if (std::find(defaults.begin(), defaults.end(), url) == defaults.end()) fetches.erase(url);

	list = std::move(kept);
	store();
	merge();
	hubsChanged();
	sourcesChanged();
	return true;
}

QuickDC::HubDirectory::Source* QuickDC::HubDirectory::find(const std::string& url)
{
	for (Source& source : list)
		if (source.url == url) return &source;
	return nullptr;
}

void QuickDC::HubDirectory::fetched(const std::string& url, const std::vector<HubListEntry>& hubs)
{
	Source* source = find(url);
	if (!source) return;

	source->hubs.assign(hubs.begin(), hubs.begin() + static_cast<std::ptrdiff_t>(std::min(hubs.size(), PER_SOURCE_LIMIT)));
	source->fetching = false;
	source->fetched = static_cast<uint64_t>(time(0));
	source->cached = false;
	source->error.clear();

	writeCache(*source);
	merge();
	hubsChanged();
	sourcesChanged();
}

void QuickDC::HubDirectory::failed(const std::string& url, const std::string& reason)
{
	Source* source = find(url);
	if (!source) return;

	/* What it had is kept: a list that could not be fetched today is not a list of no hubs. */
	source->fetching = false;
	source->error = reason;
	sourcesChanged();
}
