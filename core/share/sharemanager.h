/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SHAREMANAGER_H
#define HAVE_QUICKDC_SHAREMANAGER_H

#include <vector>
#include <map>
#include <memory>
#include <string>

#include "quickdc.h"
#include "hash/hashjob.h"
#include "network/dc/dcfile.h"

namespace Samurai {
	namespace IO {
		class File;
		class Buffer;
	}
}

namespace QuickDC {
namespace Hash {
class Manager;
}
class Preferences;
class TransferManager;

}

namespace QuickDC {
namespace Share {

class SearchRequest;
class File;
class Dir;
class AttachmentListener;

class Manager : public QuickDC::Hash::JobListener {

	public:
		/**
		 * @param dir where the generated file lists are written, so they can
		 *        be pointed somewhere other than the real ones.
		 */
		Manager(QuickDC::Preferences* config, QuickDC::Hash::Manager* mgr,
		        const char* dir = "~/.quickdc");
		virtual ~Manager();

		Manager(const Manager&) = delete;
		Manager& operator=(const Manager&) = delete;

		/**
		 * Read configuration and initialize.
		 */
		void initialize();
		
		/**
		 * Returns the number bytes being shared at this moment.
		 * Please note that this number is updated only after 
		 * recalculations.
		 */
		uint64_t getSharedSize() { return size; }

		/**
		 * Returns the number of files being shared.
		 */
		uint32_t getFiles() { return files; }

		/**
		 * Rebuild share database and update file indexes.
		 * If nothing has changed since the last time this 
		 * was called, this operation is fairly cheap.
		 * 
		 * NOTE: This will write a file to disk where
		 * all hashes are stored so that rebuilding between 
		 * different sessions is fairly cheap as well.
		 */
		void rebuild();
		void createFileLists(bool incomplete);

		/**
		 * The directory createFileLists() writes files.xml and files.xml.bz2 to.
		 *
		 * As it was configured, tilde and all: Samurai::IO::File is what expands
		 * it, and what wrote the list.
		 */
		const std::string& getFilelistDirectory() const { return listdir; }
		
		/**
		 * Search the local file database.
		 */
		void search(SearchRequest* req);

		/**
		 * Get a file from the share database based on the public
		 * name it has on DC (as perceived by other clients).
		 */
		Samurai::IO::File* getFile(const char* dcname) const;
		
		/**
		 * Get a file from the share database based on the TTH
		 * sum.
		 */
		Samurai::IO::File* getFileByTTH(const char* sum);

		/**
		 * Whether an attachment is remembered past this session.
		 *
		 * Remembered is what a chat attachment needs: the hash is written to
		 * [Attachments] as soon as it is known, so a magnet URI in a message
		 * somebody scrolls back to a month later still fetches the file.
		 *
		 * Session is for a caller that keeps records of its own and decides for
		 * itself what to offer again at start-up. A bulletin board cache is one:
		 * whether a post document it holds is served at all is a per-hub setting,
		 * so a document remembered here as well would be offered again whatever
		 * that setting says - and the configuration would grow a line for every
		 * post ever read.
		 */
		enum class Persistence { Remembered, Session };

		/**
		 * Whether serving an attachment costs an upload slot.
		 *
		 * Counted is every ordinary offer, and a peer asking for one meets the
		 * refusal a shared file would give when the slots are taken.
		 *
		 * Free is for a file whose being served is what keeps something else
		 * working. A bulletin board post is one: a board is durable only because
		 * everyone who has read a post hands it on, so a reader turned away
		 * because somebody else is downloading a film is a board that stops
		 * working under exactly the load that makes it worth having.
		 *
		 * Nothing about slots belongs in Persistence, which is about surviving a
		 * restart, and a file can want either of these with either of those: a
		 * post document is served freely and remembered nowhere, and a caller that
		 * wanted the opposite would be as sensible.
		 *
		 * Only this client can say so. The mark goes on a file we chose to offer,
		 * is asked after by hash, and there is nothing a peer can send to obtain
		 * it - which is the whole of what keeps it from being a way around the
		 * slot limit.
		 */
		enum class SlotCost { Counted, Free };

		/**
		 * How many slot-free uploads may run at once where the configuration does
		 * not say, as "Slot free uploads" in [Network].
		 *
		 * A bound of its own, which neither takes from Slots nor is taken from by
		 * it: what is given away for nothing has to be bounded by how much of it
		 * there is, and counting it against the slots the user configured would
		 * turn the exemption back into the refusal it exists to avoid. Generous,
		 * because it must never be what a reader meets in ordinary use - a post is
		 * a couple of megabytes by the hub's own rule and is over in a moment, so
		 * sixteen of them at once is a burst rather than a state.
		 *
		 * Zero means none, and not the unlimited a zero means for Slots: this is a
		 * ceiling on serving for free, and an operator who writes zero on it is
		 * asking for that to stop rather than to become boundless.
		 */
		static constexpr int DEFAULT_SLOT_FREE_UPLOADS = 16;

		/**
		 * Offer a file to other clients by its tiger tree hash alone, without
		 * sharing it. This is what a chat attachment needs: the message carries
		 * a magnet URI naming the hash, and whoever has that URI can fetch the
		 * file.
		 *
		 * The file stays invisible everywhere else. It is in no file list, no
		 * search by name matches it, and the number of files and bytes a hub is
		 * told about does not change. The hash is therefore the capability:
		 * anyone who has it can fetch the file, and there is no other way to
		 * discover that we hold it.
		 *
		 * Hashing is asynchronous, so this returns long before the hash of a
		 * large file is known. The listener is called on the thread that drives
		 * the core, from Hash::Manager::process() - unless the file has been
		 * hashed before, in which case the hash comes out of the hash cache and
		 * is reported before this returns.
		 *
		 * An attachment is remembered in the configuration as soon as its hash is
		 * known, under [Attachments], so a magnet URI handed out in a message
		 * still works after a restart. A remembered file that has been edited
		 * since - one the hash cache can no longer vouch for - is dropped when
		 * the configuration is read, since the hash no longer describes it.
		 *
		 * @param path a regular, readable file.
		 * @param listener told the hash once it is known, or why it never will
		 *        be. Attaching a file that is already attached, or one whose
		 *        hash is known because it is genuinely shared, reports the hash
		 *        it already has.
		 * @param persistence whether the configuration remembers the file. A file
		 *        that is already attached for this session only is promoted to
		 *        being remembered, and never the other way about: a caller
		 *        promised a URI that outlives the session cannot have that taken
		 *        away by a later caller who did not need it.
		 * @param slot whether serving the file takes an upload slot; see SlotCost.
		 *        Promoted the same way persistence is, and for the same reason. One
		 *        hash is one content, so a file that becomes slot-free by being
		 *        attached a second time is not a larger file than it was.
		 * @return false when there is nothing to work with - no path, no
		 *         listener, or a path that is not a readable regular file - and
		 *         then the listener is not called at all. Anything that can only
		 *         be discovered later is reported to the listener instead.
		 */
		bool attach(const char* path, QuickDC::Share::AttachmentListener* listener,
		            QuickDC::Share::Manager::Persistence persistence = Persistence::Remembered,
		            QuickDC::Share::Manager::SlotCost slot = SlotCost::Counted);

		/**
		 * Whether the file behind this hash is one of the slot-free offers.
		 *
		 * Only an attachment this client marked answers yes. A shared file never
		 * does, however small, and neither does a remembered attachment offered
		 * again at start-up: nothing of the mark reaches [Attachments], so a file
		 * that is served for free is one something in this session asked to be.
		 */
		bool isSlotFreeByTTH(const char* tth) const;

		/**
		 * Whether this hash may be served without taking an upload slot as things
		 * stand: marked slot-free, and not already at the bound on how many such
		 * uploads run at once.
		 *
		 * The one place that decision is made, because both the ADC and the NMDC
		 * request paths make it and a second copy of the reasoning is how the two
		 * come to disagree about what this client gives away.
		 *
		 * @param transfers what is running, to count the slot-free uploads among.
		 *        A null one counts as nothing running.
		 */
		bool mayServeWithoutSlot(const char* tth, QuickDC::TransferManager* transfers) const;

		/**
		 * Stop reporting to a listener that is going away.
		 *
		 * A file handed to attach() is hashed on the worker thread, which can take as
		 * long as reading it takes, and the listener is called when that finishes. A
		 * caller destroyed in between - a window closed while a large picture is still
		 * being hashed - leaves its pointer in the record, and the hash completing then
		 * reports into freed memory.
		 *
		 * The file itself is unaffected: it goes on being hashed and goes on being
		 * offered once it has been. All that is dropped is the promise to say so to
		 * somebody who is no longer there to be told.
		 *
		 * Anything with a shorter life than this manager has to call it as it goes.
		 */
		void forgetListener(QuickDC::Share::AttachmentListener* listener);

		/**
		 * Stop offering an attachment: take back everything attach() made
		 * reachable, and forget the file.
		 *
		 * A client that no longer holds what it once offered has to stop saying
		 * that it does. A peer that asks for a hash we still answer for and then
		 * cannot be served gets a failed transfer, having been told by a search
		 * that we had the file.
		 *
		 * Every record naming this hash goes, not just the first, since the same
		 * content can have been attached under more than one name. Nothing is left
		 * for a search by hash to find, and nothing is left in [Attachments] to be
		 * offered again at the next start-up.
		 *
		 * What is genuinely shared is left alone: a file in one of the shared
		 * directories is reachable by its hash because it is shared, not because
		 * it was attached, and it goes on being served. This still returns true,
		 * for the attachment record it forgot.
		 *
		 * The one thing it cannot do is wait. An upload takes the very handle
		 * getFileByTTH() returns - a member of the Share::File this destroys - and
		 * reads through it until the transfer ends, so calling this while a peer is
		 * being sent the file leaves that transfer reading an object that has been
		 * freed. Nothing here can tell: the share manager hears nothing about
		 * transfers, and asking the transfer manager means driving the one iterator
		 * it has from underneath whoever else is walking it. A caller that cannot
		 * rule out being in the middle of serving the file has to wait for itself.
		 *
		 * @param tth the tiger tree root, base32, as attach() reported it.
		 * @return false when nothing is attached under that hash - which includes
		 *         an attachment whose hash is not known yet, since it has none to
		 *         be named by.
		 */
		bool detach(const std::string& tth);

	protected:
		/**
		 * Hash::JobListener, for the files handed to attach(). A shared file
		 * hashes itself; these have no Share::File to do it for them until the
		 * hash is known.
		 */
		void EventFileStarted(const QuickDC::Hash::Job*) override;
		void EventFileQueued(const QuickDC::Hash::Job*) override;
		void EventFileHashed(const QuickDC::Hash::Job*) override;
		void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) override;

	protected:
		/**
		 * Add a directory to the share manager.
		 * This is a very fast operation since the 
		 * share database is not being rebuild at this point.
		 * To rebuild the database, use rebuild().
		 *
		 * The ShareManager will ensure that the file/share
		 * is not added more than once.
		 */
		void addShare(QuickDC::Share::Dir* dir);

		/**
		 * Remove a directory from the share manager.
		 * This is a very fast operation since the share
		 * database aren't performed at this point.
		 * To rebuild the database, use rebuild().
		 */
		void removeShare(QuickDC::Share::Dir* dir);

		void addSize(uint64_t sz);

		
		/**
		 * Create file lists:
		 * - files.xml
		 * - files.xml.bz2
		 */
		void createFileList(Samurai::IO::Buffer* xmlout, QuickDC::Share::Dir* dir = 0, bool incomplete = false);
		
		/**
		 * This ensures file can be looked up by TTH.
		 */
		void addLookupFile(const char* file, const char* tth, QuickDC::Share::File*);

		/**
		 * A file handed to attach(): offered by TTH, part of no share.
		 */
		struct Attachment {
			/** The path as attach() was given it, which is what the listener is told. */
			std::string path;
			/** The same file resolved, which is how the hash manager names it. */
			std::string name;
			/** Empty until the file has been hashed. */
			std::string tth;
			uint64_t size = 0;
			/** Owns 'file', and is deliberately not one of 'dirs'. */
			QuickDC::Share::Dir* dir = 0;
			/** Null while the hash is unknown, and when a shared file has the same hash. */
			QuickDC::Share::File* file = 0;
			/** Whether the record reaches [Attachments]; see Persistence. */
			QuickDC::Share::Manager::Persistence persistence = Persistence::Remembered;
			/** Whether serving it takes an upload slot; see SlotCost. */
			QuickDC::Share::Manager::SlotCost slot = SlotCost::Counted;
			/** Waiting to be told the hash. Emptied once they have been. */
			std::vector<QuickDC::Share::AttachmentListener*> listeners;

			~Attachment();
		};

		/**
		 * The file behind a TTH, whether it is shared or attached.
		 */
		QuickDC::Share::File* findByTTH(const char* tth) const;

		/**
		 * A file in one of the shared directories that carries this hash.
		 *
		 * Only tthTable answers for a shared file, and one hash names one file
		 * there, so an attachment that claimed the hash first is the file that
		 * entry points at: the shared file's own insertion, which happens whenever
		 * its hash arrives, found the entry taken and did nothing. Since a share is
		 * hashed in the background, that is a matter of which finished first.
		 * Taking such an attachment back therefore has to hand the entry over, or
		 * a shared file is left with no hash for a peer to ask for it by.
		 */
		QuickDC::Share::File* findSharedByTTH(const std::string& tth) const;

		/** findSharedByTTH() over one directory and everything below it. */
		static QuickDC::Share::File* findInDirByTTH(QuickDC::Share::Dir* dir, const std::string& tth);

		QuickDC::Share::Manager::Attachment* findAttachment(const std::string& name) const;
		bool isAttachment(const QuickDC::Share::File* ptr) const;

		/**
		 * Whether this is the file handle a slot-free offer is served through.
		 *
		 * How a running upload is recognised as one of them: an upload holds the
		 * very handle getFileByTTH() returned, and asking which Share::File answers
		 * for the hash rather than which one the record built covers the case where
		 * a genuinely shared file holds the hash table entry and is therefore what
		 * a peer is being sent.
		 */
		bool isSlotFreeHandle(const Samurai::IO::File* handle) const;

		/** How many of the running uploads are slot-free offers. */
		size_t countSlotFreeUploads(QuickDC::TransferManager* transfers) const;

		/**
		 * Make an attachment reachable by its hash, and by nothing else.
		 *
		 * @return false if it cannot be, which the caller has to report.
		 */
		bool registerAttachment(QuickDC::Share::Manager::Attachment& record);

		/**
		 * Take back what registerAttachment() made reachable, and forget the
		 * record.
		 *
		 * The one way an attachment ends, whether it is withdrawn by detach() or
		 * abandoned because its hash is never coming. Both have to undo exactly
		 * what the registration did, and a second copy of that reasoning is how a
		 * hash table entry naming a destroyed file gets left behind.
		 */
		void forgetAttachment(QuickDC::Share::Manager::Attachment& record);

		/**
		 * Tell an attachment's listeners it cannot be offered, and forget it.
		 */
		void dropAttachment(QuickDC::Share::Manager::Attachment& record, const char* reason);

		/**
		 * Report the attachments whose hash is never coming.
		 */
		void sweepAttachments();

		/**
		 * Write the attachments to the configuration, so a magnet URI handed out
		 * in a message still works after a restart.
		 *
		 * The whole group is written every time, since Preferences cannot
		 * enumerate the keys in a group and a Total left behind from a longer
		 * list would name entries that are no longer there.
		 *
		 * Only the ones attached as Persistence::Remembered are written; a session
		 * attachment is the caller's business and none of the configuration's.
		 */
		void saveAttachments();

		/**
		 * Offer the remembered attachments again.
		 */
		void loadAttachments();

		/**
		 * Offer a remembered file again, if it is still the file the remembered
		 * hash was taken from.
		 *
		 * The hash cache is what answers that, and it is keyed on
		 * Samurai::IO::File::getName() - the whole path, as a string. So this can
		 * only adopt a file the cache has an entry for under the exact path it has
		 * now, and it is no use for a file that has just arrived: a document
		 * downloaded to a temporary name and renamed on completion is a path the
		 * cache has never been told about, however recently its contents were
		 * hashed, and this returns false for it however sure the caller is of the
		 * hash. Offering such a file is attach()'s business, which hashes the file
		 * instead of asking what is remembered about it.
		 *
		 * @return false when it is not, and the entry should be forgotten.
		 */
		bool adoptAttachment(const std::string& tth, const std::string& path);

	protected:
		std::vector<QuickDC::Share::Dir*> dirs;
		/* Owns the name; the callers pass one that does not outlive the call. */
		std::vector< std::pair<std::string, QuickDC::Share::File*> > searchTable;
		std::map<std::string, QuickDC::Share::File*> tthTable;
		/* Held by pointer so a record keeps its address while a listener that is
		   being called from it attaches another file. */
		std::vector< std::unique_ptr<QuickDC::Share::Manager::Attachment> > attachments;

		// The number of files and bytes shared.
		uint64_t size;
		uint32_t files;
		QuickDC::Preferences* config;
		QuickDC::Hash::Manager* hash;
		std::string listdir;
		bool filelists;
		/* Set while an attachment's Share::File is being built, since such a file
		   reaches the manager the same way a shared one does. */
		bool attaching;

	friend class QuickDC::Share::Dir;
	friend class QuickDC::Share::File;
};

}
}

#endif // HAVE_QUICKDC_SHAREMANAGER_H
