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
inline constexpr Text   Bind                    {"Network", "Bind", "0.0.0.0"};
inline constexpr Text   Bind6                   {"Network", "Bind6", "::"};
/** The address to advertise; "0.0.0.0" has a hub use what it sees. */
inline constexpr Text   Address                 {"Network", "Address", "0.0.0.0"};
inline constexpr Flag   ActiveMode              {"Network", "Active mode", false};
inline constexpr Flag   ForceActiveMode         {"Network", "Force active mode", false};
inline constexpr Flag   UseUpnp                 {"Network", "UseUPnP", false};
inline constexpr Text   ReachabilityServer      {"Network", "Reachability server", ""};
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

/* [Local hub] */
inline constexpr Flag   HubEnabled              {"Local hub", "Enabled", false};
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
