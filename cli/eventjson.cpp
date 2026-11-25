/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "eventjson.h"

#include <cstddef>
#include <utility>
#include <variant>

namespace Api = QuickDC::Api;
namespace Ev = QuickDC::Api::Events;

using QuickDC::Cli::Json::Array;
using QuickDC::Cli::Json::Object;
using QuickDC::Cli::Json::Value;

namespace {

/*
 * Every switch below names every enumerator and has no default, so an
 * enumerator added to the protocol is a -Wswitch warning here rather than an
 * event printed with a blank in it.
 */

const char* name(Api::SettingsStore store)
{
	switch (store)
	{
		case Api::SettingsStore::Application: return "application";
		case Api::SettingsStore::Session:     return "session";
	}
	return "?";
}

const char* name(Api::NetStatus status)
{
	using S = Api::NetStatus;
	switch (status)
	{
		case S::Invalid:                return "Invalid";
		case S::Lookup:                 return "Lookup";
		case S::Connecting:             return "Connecting";
		case S::Connected:              return "Connected";
		case S::Disconnected:           return "Disconnected";
		case S::TlsConnected:           return "TlsConnected";
		case S::TlsDisconnected:        return "TlsDisconnected";
		case S::ErrorTimeout:           return "ErrorTimeout";
		case S::ErrorConnectionRefused: return "ErrorConnectionRefused";
		case S::ErrorHostNotFound:      return "ErrorHostNotFound";
		case S::ErrorHostUnreachable:   return "ErrorHostUnreachable";
		case S::ErrorIO:                return "ErrorIO";
		case S::TlsError:               return "TlsError";
	}
	return "?";
}

const char* name(Api::HubStatus status)
{
	using S = Api::HubStatus;
	switch (status)
	{
		case S::None:                 return "None";
		case S::Login:                return "Login";
		case S::Password:             return "Password";
		case S::LoggedIn:             return "LoggedIn";
		case S::ErrorLoginRefused:    return "ErrorLoginRefused";
		case S::ErrorFull:            return "ErrorFull";
		case S::ErrorDisabled:        return "ErrorDisabled";
		case S::ErrorNickNotAccepted: return "ErrorNickNotAccepted";
		case S::ErrorNickTaken:       return "ErrorNickTaken";
		case S::ErrorCidTaken:        return "ErrorCidTaken";
		case S::ErrorWrongPassword:   return "ErrorWrongPassword";
		case S::Kicked:               return "Kicked";
		case S::BannedTemporarily:    return "BannedTemporarily";
		case S::BannedPermanently:    return "BannedPermanently";
		case S::Redirected:           return "Redirected";
	}
	return "?";
}

const char* name(Api::Priority priority)
{
	switch (priority)
	{
		case Api::Priority::Paused: return "Paused";
		case Api::Priority::Low:    return "Low";
		case Api::Priority::Normal: return "Normal";
		case Api::Priority::High:   return "High";
	}
	return "?";
}

const char* name(Api::BoardView view)
{
	switch (view)
	{
		case Api::BoardView::Unknown:  return "Unknown";
		case Api::BoardView::Complete: return "Complete";
		case Api::BoardView::Gap:      return "Gap";
	}
	return "?";
}

const char* name(Ev::PostAdopted::Outcome outcome)
{
	using O = Ev::PostAdopted::Outcome;
	switch (outcome)
	{
		case O::Verified:   return "Verified";
		case O::Disputed:   return "Disputed";
		case O::NoStore:    return "NoStore";
		case O::Unreadable: return "Unreadable";
		case O::TooLarge:   return "TooLarge";
		case O::WrongHash:  return "WrongHash";
		case O::NotAPost:   return "NotAPost";
	}
	return "?";
}

const char* name(Ev::FilelistFailed::Why why)
{
	using W = Ev::FilelistFailed::Why;
	switch (why)
	{
		case W::Refused:           return "Refused";
		case W::OwnListUnreadable: return "OwnListUnreadable";
		case W::NotArrived:        return "NotArrived";
	}
	return "?";
}

const char* name(Ev::AttachmentFailed::Why why)
{
	using W = Ev::AttachmentFailed::Why;
	switch (why)
	{
		case W::NoShares:   return "NoShares";
		case W::Unreadable: return "Unreadable";
		case W::Failed:     return "Failed";
	}
	return "?";
}

const char* name(Ev::ShareFolderRefused::Why why)
{
	using W = Ev::ShareFolderRefused::Why;
	switch (why)
	{
		case W::AlreadyShared: return "AlreadyShared";
		case W::InsideShared:  return "InsideShared";
		case W::NotADirectory: return "NotADirectory";
		case W::NotShared:     return "NotShared";
		case W::NoName:        return "NoName";
	}
	return "?";
}

const char* name(Ev::ReachabilityCheckFailed::Why why)
{
	using W = Ev::ReachabilityCheckFailed::Why;
	switch (why)
	{
		case W::NoConfiguration: return "NoConfiguration";
		case W::NoServer:        return "NoServer";
		case W::CheckFailed:     return "CheckFailed";
	}
	return "?";
}

const char* name(QuickDC::NetworkMode mode)
{
	switch (mode)
	{
		case QuickDC::NetworkMode::Automatic: return "automatic";
		case QuickDC::NetworkMode::Advanced:  return "advanced";
		case QuickDC::NetworkMode::Passive:   return "passive";
	}
	return "?";
}

const char* name(QuickDC::DropMode mode)
{
	switch (mode)
	{
		case QuickDC::DropMode::Unknown: return "Unknown";
		case QuickDC::DropMode::Active:  return "Active";
		case QuickDC::DropMode::Passive: return "Passive";
	}
	return "?";
}

const char* name(QuickDC::DropReach reach)
{
	switch (reach)
	{
		case QuickDC::DropReach::Unknown: return "Unknown";
		case QuickDC::DropReach::Reached: return "Reached";
		case QuickDC::DropReach::Blocked: return "Blocked";
	}
	return "?";
}

template<typename T, typename F>
Array each(const std::vector<T>& items, F convert)
{
	Array out;
	out.reserve(items.size());
	for (const T& item : items) out.push_back(convert(item));
	return out;
}

Array strings(const std::vector<std::string>& items)
{
	return each(items, [](const std::string& s) { return Value(s); });
}

Array numbers(const std::vector<int>& items)
{
	return each(items, [](int n) { return Value(n); });
}

Value user(const Api::User& u)
{
	return Object{
		{"id", u.id},
		{"nick", u.nick},
		{"description", u.description},
		{"email", u.email},
		{"speed", u.speed},
		{"userAgent", u.userAgent},
		{"addressIPv4", u.addressIPv4},
		{"addressIPv6", u.addressIPv6},
		{"keyprint", u.keyprint},
		{"sourceId", u.sourceId},
		{"sharedBytes", u.sharedBytes},
		{"sharedFiles", u.sharedFiles},
		{"slotCount", u.slotCount},
		{"op", u.op},
		{"away", u.away},
	};
}

Value chatLine(const Api::ChatLine& l)
{
	return Object{
		{"hubUrl", l.hubUrl},
		{"from", user(l.from)},
		{"fromHub", l.fromHub},
		{"text", l.text},
		{"context", l.context},
		{"timestamp", l.timestamp},
		{"action", l.action},
		{"richText", l.richText},
		{"privateMessage", l.privateMessage},
	};
}

Value searchHit(const Api::SearchHit& h)
{
	return Object{
		{"hubUrl", h.hubUrl},
		{"token", h.token},
		{"name", h.name},
		{"tth", h.tth},
		{"size", h.size},
		{"freeSlots", h.freeSlots},
		{"user", user(h.user)},
	};
}

Value transfer(const Api::Transfer& t)
{
	return Object{
		{"id", t.id},
		{"filename", t.filename},
		{"transferred", t.transferred},
		{"length", t.length},
		{"bytesPerSecond", t.bytesPerSecond},
		{"ratio", t.ratio},
		{"upload", t.upload},
		{"finished", t.finished},
		{"secure", t.secure},
		{"peerKeyprint", t.peerKeyprint},
		{"outbound", t.outbound},
	};
}

Value shareFolder(const Api::ShareFolder& f)
{
	return Object{{"name", f.name}, {"path", f.path}};
}

Value recentHub(const Api::RecentHub& h)
{
	return Object{{"url", h.url}, {"name", h.name}, {"visited", h.visited}};
}

Value completedDownload(const Api::CompletedDownload& d)
{
	return Object{{"filename", d.filename}, {"tth", d.tth}, {"size", d.size}, {"finished", d.finished}};
}

Value queueItem(const Api::QueueItem& q)
{
	return Object{
		{"filename", q.filename},
		{"tth", q.tth},
		{"size", q.size},
		{"done", q.done},
		{"sources", q.sources},
		{"activeSources", q.activeSources},
		{"priority", name(q.priority)},
		{"complete", q.complete},
	};
}

Value remoteFile(const Api::RemoteFile& f)
{
	return Object{{"name", f.name}, {"size", f.size}, {"tth", f.tth}};
}

Value remoteDirectory(const Api::RemoteDirectory& d)
{
	return Object{
		{"name", d.name},
		{"size", d.size},
		{"fileCount", d.fileCount},
		{"files", each(d.files, remoteFile)},
		{"dirs", each(d.dirs, remoteDirectory)},
	};
}

Value board(const QuickDC::Board& b)
{
	return Object{
		{"name", b.name},
		{"title", b.title},
		{"description", b.description},
		{"permissions", b.permissions},
		{"maxSize", b.maxSize},
		{"newest", b.newest},
		{"oldest", b.oldest},
		{"posts", b.posts},
		{"removed", b.removed},
	};
}

Value boardEntry(const QuickDC::BoardEntry& e)
{
	return Object{
		{"tth", e.tth},
		{"size", e.size},
		{"board", e.board},
		{"cid", e.cid},
		{"nick", e.nick},
		{"parent", e.parent},
		{"thread", e.thread},
		{"subject", e.subject},
		{"timestamp", e.timestamp},
		{"removed", e.removed},
	};
}

Value readState(const Api::BoardReadState& s)
{
	return Object{{"board", s.board}, {"unread", s.unread}, {"read", strings(s.read)}};
}

Value postDocument(const Api::PostDocument& p)
{
	return Object{
		{"tth", p.tth},
		{"cid", p.cid},
		{"parent", p.parent},
		{"subject", p.subject},
		{"body", p.body},
		{"dated", p.dated},
		{"date", p.date},
		{"richText", p.richText},
	};
}

Value hubListEntry(const QuickDC::HubListEntry& h)
{
	return Object{
		{"name", h.name},
		{"address", h.address},
		{"description", h.description},
		{"country", h.country},
		{"users", h.users},
		{"shared", h.shared},
		{"minshare", h.minshare},
		{"minslots", h.minslots},
		{"maxhubs", h.maxhubs},
		{"maxusers", h.maxusers},
	};
}

Value dropVerdict(const QuickDC::DropVerdict& v)
{
	return Object{
		{"ran", v.ran},
		{"ipv6", v.ipv6},
		{"publicAddress", v.publicAddress},
		{"classification", v.classification},
		{"mode", name(v.mode)},
		{"tcp", v.tcp},
		{"udp", v.udp},
		{"tcpReached", name(v.tcpReached)},
		{"udpReached", name(v.udpReached)},
		{"port", v.port},
		{"reason", v.reason},
		{"remedies", strings(v.remedies)},
		{"note", v.note},
		{"observedAt", static_cast<int64_t>(v.observedAt)},
		{"addressOnly", v.addressOnly},
	};
}

/* One overload per event and no template among them: an event left out does not compile. */
struct Fields {
	const QuickDC::Cli::HubNamer& hub;

	using Out = std::pair<const char*, Object>;

	Out operator()(const Ev::HubListFetched& e) const
	{
		return {"HubListFetched", {{"url", e.url}, {"hubs", each(e.hubs, hubListEntry)}}};
	}

	Out operator()(const Ev::HubListFailed& e) const
	{
		return {"HubListFailed", {{"url", e.url}, {"reason", e.reason}}};
	}

	Out operator()(const Ev::ReachabilityCheckStarted& e) const
	{
		return {"ReachabilityCheckStarted", {{"ipv6", e.ipv6}}};
	}

	Out operator()(const Ev::ReachabilityMeasured& e) const
	{
		return {"ReachabilityMeasured", {{"verdict", dropVerdict(e.verdict)}}};
	}

	Out operator()(const Ev::ReachabilityCheckFailed& e) const
	{
		return {"ReachabilityCheckFailed", {{"why", name(e.why)}, {"reason", e.reason}}};
	}

	Out operator()(const Ev::AttachmentReady& e) const
	{
		return {"AttachmentReady", {{"path", e.path}, {"tth", e.tth}, {"size", e.size}}};
	}

	Out operator()(const Ev::AttachmentFailed& e) const
	{
		return {"AttachmentFailed", {{"path", e.path}, {"why", name(e.why)}, {"reason", e.reason}}};
	}

	Out operator()(const Ev::FilelistReady& e) const
	{
		return {"FilelistReady", {
			{"hubUrl", e.hubUrl}, {"sourceId", e.sourceId}, {"root", remoteDirectory(e.root)}}};
	}

	Out operator()(const Ev::FilelistFailed& e) const
	{
		return {"FilelistFailed", {
			{"hubUrl", e.hubUrl}, {"sourceId", e.sourceId}, {"why", name(e.why)}}};
	}

	Out operator()(const Ev::SearchSent& e) const
	{
		return {"SearchSent", {{"token", e.token}, {"hubsAsked", e.hubsAsked}}};
	}

	Out operator()(const Ev::TransferStarted& e) const
	{
		return {"TransferStarted", {{"transfer", transfer(e.transfer)}}};
	}

	Out operator()(const Ev::TransferFinished& e) const
	{
		return {"TransferFinished", {{"transfer", transfer(e.transfer)}}};
	}

	Out operator()(const Ev::TransferStopped& e) const
	{
		return {"TransferStopped", {{"transfer", transfer(e.transfer)}}};
	}

	Out operator()(const Ev::TransferError& e) const
	{
		return {"TransferError", {{"transfer", transfer(e.transfer)}}};
	}

	Out operator()(const Ev::QueueChanged&) const
	{
		return {"QueueChanged", {}};
	}

	Out operator()(const Ev::DownloadCompleted& e) const
	{
		return {"DownloadCompleted", {{"filename", e.filename}, {"size", e.size}, {"tth", e.tth}}};
	}

	Out operator()(const Ev::TransfersSnapshot& e) const
	{
		return {"TransfersSnapshot", {
			{"request", e.request},
			{"active", each(e.active, transfer)},
			{"queued", each(e.queued, queueItem)},
			{"activeDownloads", e.activeDownloads}}};
	}

	Out operator()(const Ev::ShareStatus& e) const
	{
		return {"ShareStatus", {
			{"request", e.request}, {"files", e.files}, {"size", e.size}, {"hashing", e.hashing}}};
	}

	Out operator()(const Ev::ShareFolders& e) const
	{
		return {"ShareFolders", {{"request", e.request}, {"folders", each(e.folders, shareFolder)}}};
	}

	Out operator()(const Ev::ShareFolderRefused& e) const
	{
		return {"ShareFolderRefused", {{"request", e.request}, {"path", e.path}, {"why", name(e.why)}}};
	}

	Out operator()(const Ev::HubOpenFailed& e) const
	{
		return {"HubOpenFailed", {{"hub", hub(e.hub)}}};
	}

	Out operator()(const Ev::HubNetStatus& e) const
	{
		return {"HubNetStatus", {{"hub", hub(e.hub)}, {"status", name(e.status)}, {"secure", e.secure}}};
	}

	Out operator()(const Ev::HubReconnectScheduled& e) const
	{
		return {"HubReconnectScheduled", {{"hub", hub(e.hub)}, {"seconds", e.seconds}}};
	}

	Out operator()(const Ev::HubStatusChanged& e) const
	{
		return {"HubStatusChanged", {{"hub", hub(e.hub)}, {"status", name(e.status)}}};
	}

	Out operator()(const Ev::HubExtensions& e) const
	{
		return {"HubExtensions", {{"hub", hub(e.hub)}, {"richText", e.richText}}};
	}

	Out operator()(const Ev::HubName& e) const
	{
		return {"HubName", {{"hub", hub(e.hub)}, {"name", e.name}}};
	}

	Out operator()(const Ev::HubDescription& e) const
	{
		return {"HubDescription", {{"hub", hub(e.hub)}, {"description", e.description}}};
	}

	Out operator()(const Ev::HubSystemError& e) const
	{
		return {"HubSystemError", {{"hub", hub(e.hub)}, {"message", e.message}}};
	}

	Out operator()(const Ev::HubAuthenticate& e) const
	{
		return {"HubAuthenticate", {{"hub", hub(e.hub)}}};
	}

	Out operator()(const Ev::HubRedirect& e) const
	{
		return {"HubRedirect", {
			{"hub", hub(e.hub)}, {"address", e.address}, {"port", e.port}, {"followed", e.followed}}};
	}

	Out operator()(const Ev::ChatReceived& e) const
	{
		return {"ChatReceived", {{"hub", hub(e.hub)}, {"line", chatLine(e.line)}}};
	}

	Out operator()(const Ev::HistoryRestored& e) const
	{
		return {"HistoryRestored", {
			{"hub", hub(e.hub)}, {"conversation", e.conversation}, {"lines", each(e.lines, chatLine)}}};
	}

	Out operator()(const Ev::UserJoined& e) const
	{
		return {"UserJoined", {{"hub", hub(e.hub)}, {"nick", e.nick}}};
	}

	Out operator()(const Ev::UserLeft& e) const
	{
		return {"UserLeft", {
			{"hub", hub(e.hub)}, {"user", user(e.user)}, {"message", e.message},
			{"disconnected", e.disconnected}}};
	}

	Out operator()(const Ev::UserUpdated& e) const
	{
		return {"UserUpdated", {{"hub", hub(e.hub)}, {"user", user(e.user)}}};
	}

	Out operator()(const Ev::UsersCleared& e) const
	{
		return {"UsersCleared", {{"hub", hub(e.hub)}}};
	}

	Out operator()(const Ev::SearchResult& e) const
	{
		return {"SearchResult", {{"hub", hub(e.hub)}, {"hit", searchHit(e.hit)}}};
	}

	Out operator()(const Ev::BoardsAvailable& e) const
	{
		return {"BoardsAvailable", {{"hub", hub(e.hub)}, {"available", e.available}}};
	}

	Out operator()(const Ev::BoardDescribed& e) const
	{
		return {"BoardDescribed", {{"hub", hub(e.hub)}, {"board", board(e.board)}}};
	}

	Out operator()(const Ev::BoardEntryArrived& e) const
	{
		return {"BoardEntryArrived", {{"hub", hub(e.hub)}, {"entry", boardEntry(e.entry)}}};
	}

	Out operator()(const Ev::BoardRefused& e) const
	{
		return {"BoardRefused", {
			{"hub", hub(e.hub)}, {"code", e.code}, {"message", e.message},
			{"board", e.board}, {"tth", e.tth}}};
	}

	Out operator()(const Ev::BoardEntriesStored& e) const
	{
		return {"BoardEntriesStored", {
			{"store", e.store}, {"request", e.request}, {"kept", numbers(e.kept)},
			{"boards", each(e.boards, readState)}, {"held", strings(e.held)},
			{"failed", strings(e.failed)}, {"offered", strings(e.offered)}}};
	}

	Out operator()(const Ev::BoardStateReported& e) const
	{
		return {"BoardStateReported", {
			{"store", e.store}, {"request", e.request}, {"cursor", e.cursor},
			{"view", name(e.view)}}};
	}

	Out operator()(const Ev::BoardLoaded& e) const
	{
		return {"BoardLoaded", {
			{"store", e.store}, {"request", e.request}, {"entries", each(e.entries, boardEntry)},
			{"unread", e.unread}, {"read", strings(e.read)}, {"held", strings(e.held)},
			{"failed", strings(e.failed)}, {"offered", strings(e.offered)}}};
	}

	Out operator()(const Ev::BoardResumed& e) const
	{
		return {"BoardResumed", {
			{"store", e.store}, {"request", e.request}, {"cursor", e.cursor},
			{"view", name(e.view)}, {"offered", strings(e.offered)}}};
	}

	Out operator()(const Ev::BoardReadChanged& e) const
	{
		return {"BoardReadChanged", {
			{"store", e.store}, {"request", e.request}, {"state", readState(e.state)}}};
	}

	Out operator()(const Ev::PostKnown& e) const
	{
		return {"PostKnown", {
			{"store", e.store}, {"request", e.request}, {"path", e.path}, {"failed", e.failed}}};
	}

	Out operator()(const Ev::PostAdopted& e) const
	{
		return {"PostAdopted", {
			{"store", e.store}, {"request", e.request}, {"outcome", name(e.outcome)},
			{"post", postDocument(e.post)}, {"held", e.held}}};
	}

	Out operator()(const Ev::PostsEvicted& e) const
	{
		return {"PostsEvicted", {{"store", e.store}, {"request", e.request}, {"marked", e.marked}}};
	}

	Out operator()(const Ev::SettingChanged& e) const
	{
		return {"SettingChanged", {
			{"store", name(e.store)}, {"group", e.group}, {"key", e.key},
			{"removed", e.removed}, {"value", e.value}, {"revision", e.revision},
			{"origin", e.origin}}};
	}

	Out operator()(const Ev::SettingsWritten& e) const
	{
		return {"SettingsWritten", {{"store", name(e.store)}, {"origin", e.origin}}};
	}

	Out operator()(const Ev::SettingsSaved& e) const
	{
		return {"SettingsSaved", {{"store", name(e.store)}, {"request", e.request}, {"ok", e.ok}}};
	}

	Out operator()(const Ev::FavouriteHubs& e) const
	{
		return {"FavouriteHubs", {{"request", e.request}, {"urls", strings(e.urls)}}};
	}

	Out operator()(const Ev::RecentHubs& e) const
	{
		return {"RecentHubs", {{"request", e.request}, {"hubs", each(e.hubs, recentHub)}}};
	}

	Out operator()(const Ev::CompletedDownloads& e) const
	{
		return {"CompletedDownloads", {
			{"request", e.request}, {"downloads", each(e.downloads, completedDownload)}}};
	}

	Out operator()(const Ev::ConnectionMode& e) const
	{
		return {"ConnectionMode", {
			{"request", e.request}, {"mode", name(e.mode)}, {"activeMode", e.activeMode},
			{"useUpnp", e.useUpnp}, {"forceActive", e.forceActive},
			{"reachabilityServer", e.reachabilityServer}}};
	}
};

Fields::Out fieldsOf(const Api::Event& event, const QuickDC::Cli::HubNamer& hubName)
{
	return std::visit(Fields{hubName}, event.value);
}

template<size_t... I>
std::vector<std::string> namesOf(std::index_sequence<I...>)
{
	const QuickDC::Cli::HubNamer none = [](Api::HubHandle h) { return Value(h); };
	return {fieldsOf(Api::Event(std::variant_alternative_t<I, Api::Event::Variant>{}), none).first...};
}

}

QuickDC::Cli::Json::Object QuickDC::Cli::describe(const std::string& core, const Api::Event& event,
                                const HubNamer& hubName)
{
	Fields::Out described = fieldsOf(event, hubName);

	Object out;
	out.reserve(described.second.size() + 2);
	out.push_back({"core", core});
	out.push_back({"event", described.first});
	for (Json::Member& member : described.second) out.push_back(std::move(member));
	return out;
}

const char* QuickDC::Cli::eventName(const Api::Event& event)
{
	const HubNamer none = [](Api::HubHandle h) { return Value(h); };
	return fieldsOf(event, none).first;
}

const std::vector<std::string>& QuickDC::Cli::eventNames()
{
	static const std::vector<std::string> names =
		namesOf(std::make_index_sequence<std::variant_size_v<Api::Event::Variant>>());
	return names;
}
