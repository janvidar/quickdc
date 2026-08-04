/*
 * QuickDC::QueueFile: one queued download - its priority, the name it occupies
 * while incomplete, whether it is worth storing across a restart, and the set
 * of sources it is being fetched from.
 */

#include <samurai/samurai.h>
#include <string.h>

#include "network/filesource.h"
#include "network/downloadqueue.h"

#define QUEUE_TEST_TTH_A "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"

EXO_TEST(queuefile_priority_names, {
	return !strcmp(QuickDC::QueueFile::convertPriority(QuickDC::QueueFile::PrioPaused), "paused")
		&& !strcmp(QuickDC::QueueFile::convertPriority(QuickDC::QueueFile::PrioLow), "low")
		&& !strcmp(QuickDC::QueueFile::convertPriority(QuickDC::QueueFile::PrioNormal), "normal")
		&& !strcmp(QuickDC::QueueFile::convertPriority(QuickDC::QueueFile::PrioHigh), "high")
		&& QuickDC::QueueFile::convertPriority("paused") == QuickDC::QueueFile::PrioPaused
		&& QuickDC::QueueFile::convertPriority("LOW") == QuickDC::QueueFile::PrioLow
		&& QuickDC::QueueFile::convertPriority("High") == QuickDC::QueueFile::PrioHigh
		&& QuickDC::QueueFile::convertPriority("nonsense") == QuickDC::QueueFile::PrioNormal
		&& QuickDC::QueueFile::convertPriority(0) == QuickDC::QueueFile::PrioNormal;
});

EXO_TEST(queuefile_tempname, {
	QuickDC::QueueFile f("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
	return !strcmp(f.getLocalFilename(), "/tmp/dl/file.bin")
		&& !strcmp(f.getTempFilename(), "/tmp/dl/file.bin" QUICKDC_PART_SUFFIX);
});

EXO_TEST(queuefile_persistent_needs_tth_and_size, {
	/* This is the "not every queued download should be stored" rule. */
	QuickDC::QueueFile good("/tmp/dl/a.bin", QUEUE_TEST_TTH_A, 1000);
	QuickDC::QueueFile noTth("/tmp/dl/b.bin", 0, 1000);
	QuickDC::QueueFile shortTth("/tmp/dl/c.bin", "TOOSHORT", 1000);
	QuickDC::QueueFile noSize("/tmp/dl/d.bin", QUEUE_TEST_TTH_A, 0);

	if (!good.isPersistent()) return false;
	if (noTth.isPersistent()) return false;
	if (shortTth.isPersistent()) return false;
	if (noSize.isPersistent()) return false;

	/* A finished file has nothing left to resume. */
	good.getChunks().addRange(0, 1000);
	return good.isComplete() && !good.isPersistent();
});

EXO_TEST(queuefile_ignores_duplicate_sources, {
	QuickDC::QueueFile f("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);

	f.addSource(new QuickDC::FileSource("adc://h:1/CID/f"));
	f.addSource(new QuickDC::FileSource("adc://h:1/CID/f"));
	if (f.countSources() != 1) return false;

	/* An unusable source is dropped rather than stored. */
	f.addSource(new QuickDC::FileSource("garbage"));
	if (f.countSources() != 1) return false;

	f.addSource(new QuickDC::FileSource("adc://h:1/OTHER/f"));
	return f.countSources() == 2;
});

EXO_TEST(queuefile_active_sources, {
	QuickDC::QueueFile f("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
	f.addSource(new QuickDC::FileSource("adc://h:1/A/f"));
	f.addSource(new QuickDC::FileSource("adc://h:1/B/f"));

	QuickDC::FileSource* first = f.getIdleSource();
	if (!first) return false;

	f.setSourceActive(first, true);
	if (f.countActiveSources() != 1) return false;
	if (!f.isSourceActive(first)) return false;

	/* An idle source must never be one we are already downloading from. */
	QuickDC::FileSource* second = f.getIdleSource();
	if (!second || second == first) return false;

	f.setSourceActive(first, false);
	return f.countActiveSources() == 0;
});

EXO_TEST(queuefile_expire_spares_active_sources, {
	QuickDC::QueueFile f("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
	f.addSource(new QuickDC::FileSource("adc://h:1/BURNT/f"));
	f.addSource(new QuickDC::FileSource("adc://h:1/ALSOBURNT/f"));

	for (size_t n = 0; n < f.countSources(); n++)
		for (int t = 0; t <= QUICKDC_SOURCE_MAX_RETRIES; t++)
			f.getSource(n)->onFailure();

	/* Pin one of them; a source we are mid-transfer with must survive. */
	QuickDC::FileSource* pinned = f.getSource(0);
	f.setSourceActive(pinned, true);

	f.expireSources();
	return f.countSources() == 1 && f.getSource(0) == pinned;
});
