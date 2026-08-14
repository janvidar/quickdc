/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "bbs/poststore.h"

#include "network/adc/adcbbs.h"
#include "share/sharemanager.h"

#include <samurai/io/file.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

/** How much of a file to take at a time when reading or copying one. */
constexpr size_t CHUNK = 64 * 1024;

/**
 * What the journal is called, beside the documents it describes.
 *
 * In the same directory rather than above it, which is safe for a reason worth
 * writing down: a document is filed under 39 characters of upper case base32, and
 * this name is neither, so no document can ever land on it.
 */
constexpr const char* JOURNAL = "documents";

/** The one kind of record the journal holds; see remember(). */
constexpr char RECORD_DOCUMENT = 'D';

std::vector<std::string> split(const std::string& line)
{
	std::vector<std::string> parts;
	size_t at = 0;

	while (at <= line.size())
	{
		const size_t next = line.find(' ', at);

		if (next == std::string::npos)
		{
			parts.push_back(line.substr(at));
			break;
		}

		parts.push_back(line.substr(at, next - at));
		at = next + 1;
	}

	return parts;
}

/**
 * The field named by 'tag', or the empty string when the record has none.
 *
 * Nothing is unescaped, because nothing in this journal needs it: every value is
 * a hash this client has checked or a number it wrote itself. No remote text is
 * kept here at all - what a hub said about a post is in the board index, and what
 * an author said is in the document.
 */
std::string field(const std::vector<std::string>& parts, const char* tag)
{
	for (const std::string& part : parts)
	{
		if (part.size() >= 2 && part[0] == tag[0] && part[1] == tag[1])
			return part.substr(2);
	}

	return std::string();
}

uint64_t number(const std::vector<std::string>& parts, const char* tag)
{
	const std::string value = field(parts, tag);
	return value.empty() ? 0 : strtoull(value.c_str(), 0, 10);
}

/**
 * Every component of 'directory', so far as it can be made.
 *
 * A level at a time, because nothing above a board directory is made as the core
 * starts: a caller naming "~/.quickdc/boards/docs" on a fresh account would
 * otherwise have an unusable store for want of the directory in the middle.
 */
bool make_directories(const std::string& directory)
{
	if (directory.empty()) return false;

	const std::string path = Samurai::IO::File::resolvePath(directory);

	for (size_t at = 1; at < path.size(); at++)
	{
		if (path[at] != '/') continue;
		Samurai::IO::File::mkdir(path.substr(0, at).c_str(), 0700);
	}

	Samurai::IO::File::mkdir(path.c_str(), 0700);

	return Samurai::IO::File(path).isDirectory();
}

/** The leaf of a path, which for a document in this store is its hash. */
std::string leaf_of(const std::string& path)
{
	const size_t at = path.rfind('/');
	return at == std::string::npos ? path : path.substr(at + 1);
}

/**
 * A copy, for the move that could not be a rename.
 *
 * A document is fetched into the download directory and adopted from there, and
 * the two can be on different filesystems - a downloads directory on a removable
 * disk is ordinary - where a rename is refused however much the caller is
 * entitled to it. A half written copy is removed rather than left to be adopted:
 * bytes filed under a hash they do not have would be served to the next reader as
 * a post.
 */
bool copy_file(const std::string& from, const std::string& to)
{
	Samurai::IO::File source(from);
	if (!source.open(Samurai::IO::File::Mode::Read)) return false;

	Samurai::IO::File target(to);

	if (!target.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate))
	{
		source.close();
		return false;
	}

	char chunk[CHUNK];
	ssize_t got;
	bool ok = true;

	while (ok && (got = source.read(chunk, sizeof(chunk))) > 0)
	{
		ssize_t at = 0;

		while (at < got)
		{
			const ssize_t put = target.write(chunk + at, static_cast<size_t>(got - at));

			if (put <= 0) { ok = false; break; }

			at += put;
		}
	}

	source.close();
	target.close();

	if (!ok) Samurai::IO::File(to).remove();

	return ok;
}

}

QuickDC::PostStore::PostStore(const std::string& directory_, Share::Manager* shares_)
	: directory(directory_)
	, usable(make_directories(directory_))
	, shares(shares_)
	, loaded(false)
	, terminated(true)
{
}

/*
 * Nothing is detached and nothing is deleted.
 *
 * An offer lasts as long as the process that made it, so there is nothing to take
 * back that will not be taken back by the store's going away - and a detach() here
 * would be exactly the one release() exists to avoid, at the one moment a transfer
 * is most likely to still be running. A share manager that outlives this goes on
 * serving the documents from the files, which are still there and still correct.
 *
 * What is taken back is this store's own address. serve() hands it to the share
 * manager as the listener for a hash job, the manager holds that pointer until the
 * job finishes, and hashing happens on another thread - so a store destroyed while
 * one is outstanding is a store the manager is about to report a hash to. Every
 * other listener of a shorter life than the manager does the same; see
 * Share::Manager::forgetListener().
 */
QuickDC::PostStore::~PostStore()
{
	if (shares) shares->forgetListener(this);
}

bool QuickDC::PostStore::isUsable() const
{
	return usable;
}

std::string QuickDC::PostStore::journalPath() const
{
	return directory + "/" + JOURNAL;
}

std::string QuickDC::PostStore::pathFor(const std::string& tth) const
{
	/*
	 * The hash and nothing else. A name that is not a tiger tree root gets no
	 * path, so a board name, a magnet's 'dn' or anything else a hub chose cannot
	 * reach this directory even by accident: none of them is 39 characters of
	 * upper case base32, and a check is cheaper than an argument about it.
	 */
	if (!ADC::isValidTTH(tth)) return std::string();

	return directory + "/" + tth;
}

void QuickDC::PostStore::remember() const
{
	if (loaded || !usable) return;
	loaded = true;

	Samurai::IO::File file(journalPath());
	if (!file.open(Samurai::IO::File::Mode::Read)) return;

	std::string content;
	char chunk[CHUNK];
	ssize_t got;

	while ((got = file.read(chunk, sizeof(chunk))) > 0)
		content.append(chunk, static_cast<size_t>(got));

	file.close();

	terminated = content.empty() || content[content.size() - 1] == '\n';

	size_t at = 0;

	while (at < content.size())
	{
		const size_t end = content.find('\n', at);
		const std::string line = content.substr(at,
			end == std::string::npos ? std::string::npos : end - at);

		at = (end == std::string::npos) ? content.size() : end + 1;

		/*
		 * A line that does not parse is skipped rather than taken as the end of
		 * the journal. The last one may have been half written when the process
		 * went, and everything else the client holds is not worth losing over it.
		 */
		if (line.size() < 2 || line[0] != RECORD_DOCUMENT || line[1] != ' ') continue;

		const std::vector<std::string> parts = split(line);
		const std::string tth = field(parts, "TR");

		if (!ADC::isValidTTH(tth)) continue;

		/* The last record for a hash is the one that counts; the first is where it
		   goes in the order eviction chooses by. */
		if (!held.count(tth)) order.push_back(tth);

		Held& document = held[tth];
		document.tth = tth;
		document.size = number(parts, "SI");
		document.adopted = number(parts, "TS");
		document.leaving = field(parts, "RM") == "1";

		switch (number(parts, "VF"))
		{
			case 1:  document.verified = Verification::Verified; break;
			case 2:  document.verified = Verification::Failed;   break;
			default: document.verified = Verification::Unknown;  break;
		}
	}

	/*
	 * A record whose document is not on disk describes nothing. The user emptying
	 * the directory, a disk that lost it, or a release() that removed the file and
	 * did not live to see the journal read again: in every case the bytes are what
	 * matters and they are gone, so the client must not offer the hash or tell a
	 * user the post is held.
	 */
	std::vector<std::string> kept;
	kept.reserve(order.size());

	for (const std::string& tth : order)
	{
		if (Samurai::IO::File::exists(pathFor(tth).c_str()))
		{
			kept.push_back(tth);
			continue;
		}

		held.erase(tth);
	}

	order = kept;
}

QuickDC::PostStore::Held* QuickDC::PostStore::find(const std::string& tth) const
{
	remember();

	const std::map<std::string, Held>::iterator at = held.find(tth);

	return at == held.end() ? 0 : &at->second;
}

bool QuickDC::PostStore::write(const Held& document)
{
	if (!usable) return false;

	std::string out = std::string(1, RECORD_DOCUMENT) + " TR" + document.tth
		+ " SI" + std::to_string(document.size)
		+ " TS" + std::to_string(document.adopted)
		+ " VF" + std::to_string(document.verified == Verification::Verified ? 1
			: document.verified == Verification::Failed ? 2 : 0);

	if (document.leaving) out += " RM1";

	out += "\n";

	/* A newline where the last write did not manage one, so that this record does
	   not join onto a half written one and be lost with it. */
	if (!terminated) out = "\n" + out;

	Samurai::IO::File file(journalPath());

	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Append))
		return false;

	const bool ok = file.write(out.data(), out.size()) >= 0;

	if (ok) terminated = true;
	file.close();

	return ok;
}

bool QuickDC::PostStore::adopt(const std::string& tth, const std::string& path)
{
	if (!usable || path.empty()) return false;

	const std::string target = pathFor(tth);
	if (target.empty()) return false;

	remember();

	/*
	 * Already held, and a document is immutable: the bytes in hand are the bytes
	 * on disk, since both hash to the name they are filed under - or the caller
	 * has handed over something that does not, and storing it would replace a
	 * document that verified with one that has not been looked at.
	 */
	if (held.count(tth))
	{
		Samurai::IO::File(path).remove();
		return true;
	}

	Samurai::IO::File source(path);
	if (!source.isRegular() || !source.isReadable()) return false;

	const off_t size = source.size();

	if (!source.rename(target))
	{
		if (!copy_file(path, target)) return false;

		Samurai::IO::File(path).remove();
	}

	Held document;
	document.tth = tth;
	document.size = size > 0 ? static_cast<uint64_t>(size) : 0;
	document.adopted = static_cast<uint64_t>(time(0));

	/*
	 * A document nothing describes is worse than a document that was not stored.
	 * The journal is the only thing that knows what is here, so a file it has no
	 * record of would sit in the directory for ever: eviction would never choose
	 * it, release() would never remove it, and nothing would ever offer it.
	 */
	if (!write(document))
	{
		Samurai::IO::File(target).remove();
		return false;
	}

	held[tth] = document;
	order.push_back(tth);

	return true;
}

void QuickDC::PostStore::record(const std::string& tth, Verification outcome)
{
	Held* document = find(tth);
	if (!document || document->verified == outcome) return;

	document->verified = outcome;

	/*
	 * A document that is no longer held to be what it claims stops being offered,
	 * and stopping is a detach(): marked here and carried out by release(), like
	 * every other withdrawal.
	 */
	if (outcome != Verification::Verified && document->offered)
		document->withheld = true;

	write(*document);
}

QuickDC::PostStore::Verification
QuickDC::PostStore::verification(const std::string& tth) const
{
	const Held* document = find(tth);

	return document ? document->verified : Verification::Unknown;
}

bool QuickDC::PostStore::has(const std::string& tth) const
{
	/*
	 * A document marked to go is still held. The mark is a decision and not a
	 * deletion, and until release() carries it out the bytes are on disk and are
	 * still the post: a reader that has one open goes on reading it.
	 */
	return find(tth) != 0;
}

std::vector<std::string> QuickDC::PostStore::documents() const
{
	remember();

	return order;
}

uint64_t QuickDC::PostStore::bytes() const
{
	remember();

	uint64_t total = 0;

	for (const std::pair<const std::string, Held>& document : held)
		total += document.second.size;

	return total;
}

bool QuickDC::PostStore::serve(const std::string& tth)
{
	if (!usable || !shares) return false;

	Held* document = find(tth);
	if (!document) return false;

	/* Never, whatever the caller asks: serving unverified bytes under a hash
	   other clients have checked would spread a forgery for them. */
	if (document->verified != Verification::Verified) return false;

	/* Nor a document on its way out, which is a hash this client is in the middle
	   of ceasing to answer for. */
	if (document->leaving) return false;

	const std::string path = pathFor(tth);
	if (path.empty() || !Samurai::IO::File::exists(path.c_str())) return false;

	/* Asked for again while a withhold() was waiting on a release(): the offer is
	   still standing, so the withdrawal is simply called off. */
	document->withheld = false;

	if (document->offered) return true;

	/*
	 * attach() and not adoptAttachment(): the hash cache is keyed on the path a
	 * file was hashed under, and a document was hashed in the download directory
	 * under a temporary name before it was renamed here, so there is no cache
	 * entry under this path however recently these bytes went through it. attach()
	 * hashes, and answers on the thread that drives the core.
	 *
	 * Session, so the document never enters [Attachments]: whether it is offered
	 * at all is the caller's decision to make again at every start-up, and a
	 * remembered attachment would be offered again regardless of it.
	 *
	 * And served without taking an upload slot, which is what a board rests on: it
	 * is durable only because everybody who has read a post hands it on, and a
	 * reader turned away because somebody else is downloading a film is a board
	 * that stops working under exactly the load that makes it worth having. What
	 * keeps that affordable is the limit a hub puts on how large a post may be, so
	 * that bound and this exemption are chosen together - see
	 * ADC::LocalHub::BOARD_DEFAULT_MAX_SIZE.
	 */
	if (!shares->attach(path.c_str(), this, Share::Manager::Persistence::Session,
	                    Share::Manager::SlotCost::Free))
		return false;

	document->offered = true;

	return true;
}

bool QuickDC::PostStore::withhold(const std::string& tth)
{
	Held* document = find(tth);

	if (!document || !document->offered || document->withheld) return false;

	document->withheld = true;

	return true;
}

bool QuickDC::PostStore::isServed(const std::string& tth) const
{
	const Held* document = find(tth);

	return document && document->offered && !document->withheld;
}

bool QuickDC::PostStore::forget(const std::string& tth)
{
	Held* document = find(tth);
	if (!document) return false;

	if (document->leaving) return true;

	document->leaving = true;
	write(*document);

	return true;
}

std::vector<std::string> QuickDC::PostStore::evict(const Bounds& bounds, uint64_t now)
{
	std::vector<std::string> chosen;

	remember();

	/*
	 * By age first, because it is a statement about a document and not about the
	 * store: a post nobody has looked at for a year goes whether or not there is
	 * room for it. Subtracted rather than added, so that a bound near the top of
	 * the range cannot wrap round and take everything.
	 */
	if (bounds.age)
	{
		for (const std::string& tth : order)
		{
			Held* document = find(tth);
			if (!document || document->leaving) continue;

			if (now >= document->adopted && now - document->adopted >= bounds.age)
			{
				document->leaving = true;
				write(*document);
				chosen.push_back(tth);
			}
		}
	}

	if (!bounds.total) return chosen;

	uint64_t remaining = 0;

	for (const std::pair<const std::string, Held>& document : held)
	{
		if (!document.second.leaving) remaining += document.second.size;
	}

	/* Oldest adoption first, until what is left fits. Nothing here is a judgement
	   about which post matters; the store knows nothing about the boards. */
	for (const std::string& tth : order)
	{
		if (remaining <= bounds.total) break;

		Held* document = find(tth);
		if (!document || document->leaving) continue;

		document->leaving = true;
		write(*document);
		chosen.push_back(tth);

		remaining = remaining > document->size ? remaining - document->size : 0;
	}

	return chosen;
}

std::vector<std::string> QuickDC::PostStore::pending() const
{
	remember();

	std::vector<std::string> waiting;

	for (const std::string& tth : order)
	{
		const std::map<std::string, Held>::const_iterator at = held.find(tth);

		if (at != held.end() && (at->second.leaving || at->second.withheld))
			waiting.push_back(tth);
	}

	return waiting;
}

bool QuickDC::PostStore::release(const std::string& tth)
{
	Held* document = find(tth);
	if (!document) return false;

	if (!document->leaving && !document->withheld) return false;

	/*
	 * The caller has said nothing is reading the file, which is the whole of what
	 * this call adds: this is the only detach() the store makes, and it is made on
	 * somebody else's word because nothing here can earn it.
	 */
	if (document->offered && shares) shares->detach(tth);

	document->offered = false;
	document->withheld = false;

	if (!document->leaving) return true;

	const std::string path = pathFor(tth);
	if (!path.empty()) Samurai::IO::File(path).remove();

	/*
	 * No record of its going is written. The journal describes what is held, and a
	 * document whose file is not there is dropped as the journal is read, so the
	 * absence of the file is the record - which is also what makes a directory
	 * somebody emptied by hand read correctly.
	 */
	held.erase(tth);

	for (size_t at = 0; at < order.size(); at++)
	{
		if (order[at] != tth) continue;

		order.erase(order.begin() + static_cast<long>(at));
		break;
	}

	return true;
}

void QuickDC::PostStore::EventAttachmentReady(const char* path, const char* tth,
                                              uint64_t)
{
	if (!path || !tth) return;

	const std::string name = leaf_of(path);

	Held* document = find(name);
	if (!document) return;

	if (name == tth) return;

	/*
	 * The bytes on disk do not hash to the name they are filed under, so they are
	 * not the post that name refers to - a document that was adopted without being
	 * checked, or a file something has since written over. It is not offered, and
	 * the outcome is recorded so that it is not fetched and checked again.
	 *
	 * The share manager attached it under the hash it computed, so that is the hash
	 * to take back, and this is the one detach() here that nobody vouched for. It
	 * is safe for a reason that does not generalise: the hash is not on any board,
	 * nothing has ever offered it, and it is withdrawn on the same turn of the loop
	 * it was reported on.
	 */
	document->offered = false;
	if (shares) shares->detach(tth);

	record(name, Verification::Failed);
}

void QuickDC::PostStore::EventAttachmentFailed(const char* path, const char*)
{
	if (!path) return;

	/* Nothing to take back: no hash was ever established, so nothing was offered
	   under one. */
	Held* document = find(leaf_of(path));

	if (document) document->offered = false;
}
