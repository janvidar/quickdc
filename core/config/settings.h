/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONFIG_SETTINGS_H
#define HAVE_QUICKDC_CONFIG_SETTINGS_H

/*
 * Every setting the core reads, declared once: where it lives, what type it is,
 * and what it is when nothing is stored.
 *
 * Reading one never writes it. A default written back by the read that wanted
 * it is a default every other reader of the same key has to agree with, and
 * they did not - the port was 1411 in one place and 0 in another, so the first
 * reader to run decided what the client listened on. Here there is one.
 */

#include "config/preferences.h"
#include "quickdc.h"

#include <string>

namespace QuickDC {
namespace Settings {

struct Text {
	const char* group;
	const char* key;
	const char* fallback;
};

struct Number {
	const char* group;
	const char* key;
	int fallback;
};

struct Flag {
	const char* group;
	const char* key;
	bool fallback;
};

inline std::string read(const Preferences& from, const Text& setting)
{
	return from.peekString(setting.group, setting.key, setting.fallback);
}

inline int read(const Preferences& from, const Number& setting)
{
	return from.peekNumber(setting.group, setting.key, setting.fallback);
}

inline bool read(const Preferences& from, const Flag& setting)
{
	return from.peekBool(setting.group, setting.key, setting.fallback);
}

/**
 * A setting that one hub may override: read from 'group' (the hub's address) in
 * the session store, and from the configuration where the hub says nothing.
 */
inline std::string readForHub(const Preferences& session, const std::string& group,
                              const Preferences& config, const Text& setting)
{
	return session.peekString(group, setting.key, read(config, setting));
}

/**
 * Keep 'value' as this hub's own, for what readForHub() reads next time - and
 * nothing at all where it is what every hub gets anyway, or empty. A copy of the
 * shared value kept per hub would outrank it for good: a later change to the
 * shared one would never reach this hub, which is what pinned nicknames and
 * passwords before (see config/migrations.cpp).
 */
inline void writeForHub(Preferences& session, const std::string& group,
                        const Preferences& config, const Text& setting, const std::string& value)
{
	if (value.empty() || value == read(config, setting))
		session.remove(group, setting.key);
	else
		session.setString(group, setting.key, value);
}

inline void write(Preferences& to, const Text& setting, const std::string& value)
{
	to.setString(setting.group, setting.key, value);
}

inline void write(Preferences& to, const Number& setting, int value)
{
	to.setNumber(setting.group, setting.key, value);
}

inline void write(Preferences& to, const Flag& setting, bool value)
{
	to.setBool(setting.group, setting.key, value);
}

/* [Network] */
inline constexpr Number Port                    {"Network", "Port", 1411};

/** The peer port asked for; 0 or out of range is the default, since 0 is not a port to offer. */
inline uint16_t peerPort(const Preferences& from)
{
	const int port = read(from, Port);
	return static_cast<uint16_t>(port < 1 || port > 65535 ? Port.fallback : port);
}
inline constexpr Text   Bind                    {"Network", "Bind", "0.0.0.0"};
inline constexpr Text   Bind6                   {"Network", "Bind6", "::"};
/** The address to advertise; "0.0.0.0" has a hub use what it sees. */
inline constexpr Text   Address                 {"Network", "Address", "0.0.0.0"};
inline constexpr Flag   ActiveMode              {"Network", "Active mode", false};
inline constexpr Flag   ForceActiveMode         {"Network", "Force active mode", false};
inline constexpr Flag   UseUpnp                 {"Network", "UseUPnP", false};
inline constexpr Text   ReachabilityServer      {"Network", "Reachability server", ""};
/** How much of getting connected is worked out; see api/networkmode.h. Absent until settled. */
inline constexpr Text   ConnectionMode          {"Network", "Mode", ""};
inline constexpr Number Slots                   {"Network", "Slots", 3};
inline constexpr Number MaxDownloads            {"Network", "Max downloads", 3};
inline constexpr Number ChunkSize               {"Network", "Chunk size", 1024 * 1024};
inline constexpr Number KeepAliveInterval       {"Network", "Keep alive interval", 120};
inline constexpr Flag   AllowPlaintextPasswords {"Network", "Allow plaintext passwords", true};
inline constexpr Flag   RequireEncryption       {"Network", "Require encrypted transfers", false};

/* [User]; the first four may be overridden per hub, see readForHub(). */
inline constexpr Text   Nickname                {"User", "Nickname", PRODUCT};
inline constexpr Text   Description             {"User", "Description", PRODUCT " user"};
inline constexpr Text   Email                   {"User", "E-mail", ""};
inline constexpr Text   Password                {"User", "Password", ""};
inline constexpr Text   PrivateId               {"User", "Private ID", ""};
inline constexpr Text   ClientId                {"User", "Client ID", ""};

/* [Downloads]; read by a user interface to name what it queues, created by the core. */
inline constexpr Text   DownloadDirectory       {"Downloads", "Directory", "~/.quickdc/downloads"};

/* [Chat media]; a hub's own group in the session store may override the first. */
inline constexpr Flag   ChatMediaFetch          {"Chat media", "Fetch automatically", false};
/** Two megabytes: enough for a photograph, small enough to arrive unnoticed. */
inline constexpr Number ChatMediaLimit          {"Chat media", "Maximum size", 2 * 1024 * 1024};
/** About six months; zero keeps them for ever. */
inline constexpr Number ChatMediaKeepDays       {"Chat media", "Keep pasted for days", 180};

/*
 * [Boards]; a hub's own group in the session store may override the first two.
 * Fetching ahead is off because a post is fetched from whoever holds it, which at
 * first is only its author: fetching unasked tells them who read it and when.
 * Offering is for hubs kept as favourites only, where the hub says nothing.
 */
inline constexpr Flag   BoardPrefetch           {"Boards", "Fetch a thread ahead", false};
inline constexpr Flag   BoardServe              {"Boards", "Offer what I have read", true};
inline constexpr Number BoardKeepDays           {"Boards", "Keep posts for days", 90};
inline constexpr Number BoardCache              {"Boards", "Cache size", 256 * 1024 * 1024};

/* [Local hub] */
inline constexpr Flag   HubEnabled             {"Local hub", "Enabled", false};
/* Whether the core logs in to the hub it runs, as a user of its own hub. */
inline constexpr Flag   HubJoin                {"Local hub", "Join", true};
inline constexpr Text   HubName                 {"Local hub", "Name", "A QuickDC hub"};
inline constexpr Text   HubDescription          {"Local hub", "Description", "My QuickDC hub"};
inline constexpr Number HubMaxUsers             {"Local hub", "Max users", 128};
inline constexpr Number HubMaxHistory           {"Local hub", "Max history", 50};
inline constexpr Number HubMaxSendQueue         {"Local hub", "Max send queue bytes", 48 * 1024};
inline constexpr Number HubMaxPerAddress        {"Local hub", "Max connections per address", 8};
inline constexpr Number HubMaxHandshakes        {"Local hub", "Max connections in handshake", 32};
inline constexpr Number HubHandshakeTimeout     {"Local hub", "Handshake timeout", 30};
inline constexpr Number HubIdleTimeout          {"Local hub", "Idle timeout", 900};
inline constexpr Number HubLingerTimeout        {"Local hub", "Linger timeout", 30};
inline constexpr Flag   HubCheckClaims          {"Local hub", "Check reachability claims", true};
inline constexpr Text   HubBulletinDirectory    {"Local hub", "Bulletin directory", "~/.quickdc/hub/bbs"};

/* [Shares], [Attachments]: the counts; the entries are numbered keys beside them. */
inline constexpr Number TotalShares             {"Shares", "Total shares", 0};
inline constexpr Number TotalAttachments        {"Attachments", "Total", 0};

}
}

#endif // HAVE_QUICKDC_CONFIG_SETTINGS_H
