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

const char* name(Ev::PostStatus::State state)
{
	using S = Ev::PostStatus::State;
	switch (state)
	{
		case S::Announced:   return "Announced";
		case S::Fetching:    return "Fetching";
		case S::Verified:    return "Verified";
		case S::Disputed:    return "Disputed";
		case S::Unavailable: return "Unavailable";
		case S::Unreadable:  return "Unreadable";
		case S::Withdrawn:   return "Withdrawn";
	}
	return "?";
}

const char* name(Ev::PostStatus::Why why)
{
	using W = Ev::PostStatus::Why;
	switch (why)
	{
		case W::None:           return "None";
		case W::NotAHash:       return "NotAHash";
		case W::Withdrawn:      return "Withdrawn";
		case W::TooLarge:       return "TooLarge";
		case W::Unsized:        return "Unsized";
		case W::Allowance:      return "Allowance";
		case W::NobodyAnswered: return "NobodyAnswered";
		case W::Oversent:       return "Oversent";
		case W::Mismatch:       return "Mismatch";
		case W::NoStore:        return "NoStore";
		case W::Unreadable:     return "Unreadable";
		case W::OverCap:        return "OverCap";
		case W::WrongHash:      return "WrongHash";
		case W::NotAPost:       return "NotAPost";
		case W::KnownBad:       return "KnownBad";
		case W::Evicted:        return "Evicted";
	}
	return "?";
}

const char* name(Ev::PostRefused::Why why)
{
	using W = Ev::PostRefused::Why;
	switch (why)
	{
		case W::NoIdentity: return "NoIdentity";
		case W::NoSubject:  return "NoSubject";
		case W::TooLarge:   return "TooLarge";
		case W::NotWritten: return "NotWritten";
		case W::NotOffered: return "NotOffered";
		case W::NoHub:      return "NoHub";
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

const char* name(Ev::ChatMedia::State state)
{
	using S = Ev::ChatMedia::State;
	switch (state)
	{
		case S::Queued:  return "Queued";
		case S::Ready:   return "Ready";
		case S::Skipped: return "Skipped";
	}
	return "?";
}

const char* name(Ev::ChatMedia::Why why)
{
	using W = Ev::ChatMedia::Why;
	switch (why)
	{
		case W::None:     return "None";
		case W::Off:      return "Off";
		case W::Unsized:  return "Unsized";
		case W::TooLarge: return "TooLarge";
		case W::Refused:  return "Refused";
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

const char* name(Ev::BoardUnavailable::Asked asked)
{
	using A = Ev::BoardUnavailable::Asked;
	switch (asked)
	{
		case A::Subscribe:   return "Subscribe";
		case A::Unsubscribe: return "Unsubscribe";
		case A::Entry:       return "Entry";
		case A::Withdraw:    return "Withdraw";
	}
	return "?";
}

const char* name(Ev::HubRedirect::Why why)
{
	using W = Ev::HubRedirect::Why;
	switch (why)
	{
		case W::None:          return "None";
		case W::Policy:        return "Policy";
		case W::Invalid:       return "Invalid";
		case W::OtherProtocol: return "OtherProtocol";
		case W::Downgrade:     return "Downgrade";
		case W::Loop:          return "Loop";
	}
	return "?";
}

const char* name(Ev::KickRefused::Why why)
{
	using W = Ev::KickRefused::Why;
	switch (why)
	{
		case W::NoSession:   return "NoSession";
		case W::NoSuchUser:  return "NoSuchUser";
		case W::NotOperator: return "NotOperator";
		case W::Unsupported: return "Unsupported";
	}
	return "?";
}

const char* name(Ev::LocalHubRefused::Why why)
{
	using W = Ev::LocalHubRefused::Why;
	switch (why)
	{
		case W::NoHub:       return "NoHub";
		case W::NoSuchUser:  return "NoSuchUser";
		case W::NoSuchEntry: return "NoSuchEntry";
		case W::Invalid:     return "Invalid";
		case W::NotSaved:    return "NotSaved";
	}
	return "?";
}

const char* name(Api::LocalHubCredential credential)
{
	switch (credential)
	{
		case Api::LocalHubCredential::User:     return "User";
		case Api::LocalHubCredential::Operator: return "Operator";
		case Api::LocalHubCredential::Admin:    return "Admin";
	}
	return "?";
}

const char* name(Api::LocalHubUser::State state)
{
	using S = Api::LocalHubUser::State;
	switch (state)
	{
		case S::LoggingIn: return "LoggingIn";
		case S::Verifying: return "Verifying";
		case S::Normal:    return "Normal";
		case S::Leaving:   return "Leaving";
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

Value peerConnection(const Api::PeerConnection& c)
{
	return Object{
		{"protocol", c.protocol},
		{"address", c.address},
		{"port", c.port},
		{"peer", c.peer},
		{"secure", c.secure},
		{"outbound", c.outbound},
	};
}

Value localHubUser(const Api::LocalHubUser& u)
{
	return Object{
		{"sid", u.sid},
		{"nick", u.nick},
		{"cid", u.cid},
		{"address", u.address},
		{"credential", name(u.credential)},
		{"state", name(u.state)},
		{"sendQueue", u.sendQueue},
	};
}

Value localHubAccount(const Api::LocalHubAccount& a)
{
	return Object{{"nick", a.nick}, {"credential", name(a.credential)}};
}

Value localHubBan(const Api::LocalHubBan& b)
{
	return Object{
		{"name", b.name},
		{"reason", b.reason},
		{"until", b.until},
		{"cid", b.cid},
		{"nick", b.nick},
		{"address", b.address},
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

Value hubListSource(const Api::HubListSource& s)
{
	return Object{
		{"url", s.url},
		{"fetched", s.fetched},
		{"hubs", s.hubs},
		{"cached", s.cached},
		{"fetching", s.fetching},
		{"error", s.error},
	};
}

Value directoryHub(const Api::DirectoryHub& h)
{
	return Object{
		{"name", h.hub.name},
		{"address", h.hub.address},
		{"description", h.hub.description},
		{"country", h.hub.country},
		{"users", h.hub.users},
		{"shared", h.hub.shared},
		{"minshare", h.hub.minshare},
		{"minslots", h.hub.minslots},
		{"maxhubs", h.hub.maxhubs},
		{"maxusers", h.hub.maxusers},
		{"listedBy", each(h.listedBy, [](uint32_t n) { return Value(n); })},
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

	Out operator()(const Ev::HubListSources& e) const
	{
		return {"HubListSources", {{"request", e.request}, {"sources", each(e.sources, hubListSource)}}};
	}

	Out operator()(const Ev::HubDirectory& e) const
	{
		return {"HubDirectory", {
			{"request", e.request}, {"sources", strings(e.sources)}, {"hubs", each(e.hubs, directoryHub)}}};
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

	Out operator()(const Ev::PastedKept& e) const
	{
		return {"PastedKept", {{"request", e.request}, {"path", e.path}, {"reason", e.reason}}};
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
		return {"SearchSent", {{"request", e.request}, {"search", e.search}, {"hubsAsked", e.hubsAsked}}};
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
			{"hub", hub(e.hub)}, {"address", e.address}, {"port", e.port}, {"followed", e.followed},
			{"url", e.url}, {"why", name(e.why)}}};
	}

	Out operator()(const Ev::ChatReceived& e) const
	{
		return {"ChatReceived", {{"hub", hub(e.hub)}, {"line", chatLine(e.line)}}};
	}

	Out operator()(const Ev::ChatMedia& e) const
	{
		return {"ChatMedia", {{"hub", hub(e.hub)}, {"tth", e.tth}, {"name", e.name}, {"size", e.size},
		                      {"state", name(e.state)}, {"why", name(e.why)}, {"path", e.path}}};
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
		return {"SearchResult", {{"hub", hub(e.hub)}, {"search", e.search}, {"hit", searchHit(e.hit)}}};
	}

	Out operator()(const Ev::BoardsAvailable& e) const
	{
		return {"BoardsAvailable", {{"hub", hub(e.hub)}, {"available", e.available}}};
	}

	Out operator()(const Ev::BoardUnavailable& e) const
	{
		return {"BoardUnavailable", {
			{"hubUrl", e.hubUrl}, {"board", e.board}, {"tth", e.tth}, {"asked", name(e.asked)}}};
	}

	Out operator()(const Ev::BoardRefused& e) const
	{
		return {"BoardRefused", {
			{"hub", hub(e.hub)}, {"code", e.code}, {"message", e.message},
			{"board", e.board}, {"tth", e.tth}}};
	}

	Out operator()(const Ev::BoardEntries& e) const
	{
		return {"BoardEntries", {
			{"hubUrl", e.hubUrl}, {"board", e.board}, {"entries", each(e.entries, boardEntry)},
			{"whole", e.whole}, {"unread", e.unread}, {"read", strings(e.read)}}};
	}

	Out operator()(const Ev::BoardState& e) const
	{
		return {"BoardState", {
			{"hubUrl", e.hubUrl}, {"board", e.board}, {"described", e.described},
			{"descriptor", board(e.descriptor)}, {"cursor", e.cursor}, {"view", name(e.view)},
			{"subscribed", e.subscribed}, {"left", e.left}, {"gone", e.gone}}};
	}

	Out operator()(const Ev::BoardRead& e) const
	{
		return {"BoardRead", {
			{"hubUrl", e.hubUrl}, {"board", e.board}, {"unread", e.unread},
			{"read", strings(e.read)}, {"whole", e.whole}}};
	}

	Out operator()(const Ev::PostStatus& e) const
	{
		return {"PostStatus", {
			{"tth", e.tth}, {"state", name(e.state)}, {"why", name(e.why)}, {"size", e.size},
			{"post", postDocument(e.post)}, {"attributedTo", e.attributedTo}}};
	}

	Out operator()(const Ev::PostSubmitted& e) const
	{
		return {"PostSubmitted", {
			{"request", e.request}, {"board", e.board}, {"tth", e.tth}, {"size", e.size}}};
	}

	Out operator()(const Ev::PostRefused& e) const
	{
		return {"PostRefused", {
			{"request", e.request}, {"why", name(e.why)}, {"size", e.size}, {"limit", e.limit},
			{"reason", e.reason}}};
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

	Out operator()(const Ev::KickRefused& e) const
	{
		return {"KickRefused", {{"hub", hub(e.hub)}, {"userId", e.userId}, {"why", name(e.why)}}};
	}

	Out operator()(const Ev::CoreStatus& e) const
	{
		return {"CoreStatus", {
			{"request", e.request}, {"version", e.version}, {"started", e.started}, {"port", e.port},
			{"tcp", e.tcp}, {"tcp6", e.tcp6}, {"udp", e.udp}, {"udp6", e.udp6}, {"localHub", e.localHub}}};
	}

	Out operator()(const Ev::ConnectionsSnapshot& e) const
	{
		return {"ConnectionsSnapshot", {
			{"request", e.request}, {"connections", each(e.connections, peerConnection)}}};
	}

	Out operator()(const Ev::SharedFile& e) const
	{
		return {"SharedFile", {
			{"request", e.request}, {"tth", e.tth}, {"found", e.found}, {"path", e.path},
			{"name", e.name}, {"size", e.size}, {"attached", e.attached}}};
	}

	Out operator()(const Ev::LocalHubState& e) const
	{
		return {"LocalHubState", {
			{"request", e.request}, {"enabled", e.enabled}, {"name", e.name}, {"maxUsers", e.maxUsers},
			{"started", e.started}, {"users", each(e.users, localHubUser)},
			{"accounts", each(e.accounts, localHubAccount)}, {"bans", each(e.bans, localHubBan)}}};
	}

	Out operator()(const Ev::LocalHubRefused& e) const
	{
		return {"LocalHubRefused", {{"request", e.request}, {"why", name(e.why)}}};
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
