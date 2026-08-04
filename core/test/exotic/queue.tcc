/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Drives QueueXMLReader over ~/.quickdc/queue.xml as QueueManager::load() does,
 * and checks the queue that comes out.
 *
 * This file is written by the last run rather than by a peer, so the danger is
 * not a hostile list so much as a truncated or half-written one: whatever
 * survived a crash still has to load into something coherent. An entry that
 * cannot be trusted has to be dropped, because a wrong size or a wrong chunk
 * map means writing into the middle of somebody's file.
 *
 * QueueManager reads and writes its own file, so each case gets a path of its
 * own - never the real ~/.quickdc/queue.xml.
 */

#include "quickdc.h"
#include "network/downloadqueue.h"
#include "network/filesource.h"

#include <samurai/io/file.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

/* 39 base32 characters, which is what the reader insists on. */
#define TTH1 "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define TTH2 "AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"

static std::string queuePath()
{
	static int counter = 0;
	return "/tmp/quickdc_queue_test_" + std::to_string(counter++) + ".xml";
}

static void writeFile(const std::string& path, const char* content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) { printf("FAIL cannot write %s\n", path.c_str()); failures++; return; }
	fwrite(content, 1, strlen(content), fh);
	fclose(fh);
}

static void removeFile(const std::string& path)
{
	Samurai::IO::File(path).remove();
	Samurai::IO::File(path + ".new").remove();
}

/*
 * Renders the loaded queue so a whole file can be asserted at once, including
 * the entries that should not have survived.
 */
static std::string render(QuickDC::QueueManager& queue)
{
	std::string out;
	for (QuickDC::QueueFile* file = queue.first(); file; file = queue.next())
	{
		out += "[";
		out += file->getLocalFilename();
		out += " ";
		out += std::to_string(file->getSize());
		out += " ";
		out += file->getTTH() ? file->getTTH() : "-";
		out += " ";
		out += QuickDC::QueueFile::convertPriority(file->getPriority());
		out += " done=";
		out += std::to_string(file->getDoneBytes());

		for (size_t n = 0; n < file->countSources(); n++)
		{
			QuickDC::FileSource* source = file->getSource(n);
			out += " {";
			out += source->getHubUrl() ? source->getHubUrl() : "-";
			out += ",";
			out += source->getClientID() ? source->getClientID() : "-";
			out += "}";
		}
		out += "]";
	}
	return out;
}

static void check(const char* label, const char* xml, const char* expect)
{
	const std::string path = queuePath();
	writeFile(path, xml);

	std::string out;
	{
		QuickDC::QueueManager queue(path.c_str());
		out = render(queue);
	}

	if (out == expect)
		printf("  ok %s\n", label);
	else
	{
		printf("FAIL %s\n    expected: %s\n    actual:   %s\n", label, expect, out.c_str());
		failures++;
	}

	removeFile(path);
}

#define QUEUE(body) \
	"<?xml version=\"1.1\" encoding=\"utf-8\" standalone=\"yes\"?>\r\n" \
	"<DownloadQueue version=\"1\" updated=\"1000\">\r\n" body "</DownloadQueue>\r\n"

#define FILE1(extra) \
	"<File name=\"/tmp/a.bin\" size=\"1000\" tth=\"" TTH1 "\" added=\"1138411637\"" extra

/* A missing or empty file leaves an empty queue, not a failure. */
EXO_TEST(queue_missing_file,
{
		const std::string path = queuePath();
		removeFile(path);
		QuickDC::QueueManager queue(path.c_str());
		EXO_ASSERT(queue.size() == 0);
		EXO_ASSERT(queue.first() == 0);
	
	return 1;
});

/* Lookups have to find what was loaded. */
EXO_TEST(queue_lookups_find_the_loaded_file,
{
		const std::string path = queuePath();
		writeFile(path,
			QUEUE("<File name=\"/tmp/a.bin\" size=\"1000\" tth=\"" TTH1 "\"/>"));
		{
			QuickDC::QueueManager queue(path.c_str());
			EXO_ASSERT(queue.size() == 1);

			QuickDC::QueueFile* byTTH = queue.getFileByTTH(TTH1);
			EXO_ASSERT(byTTH != 0);
			EXO_ASSERT(byTTH && !strcmp(byTTH->getLocalFilename(), "/tmp/a.bin"));

			QuickDC::QueueFile* byName = queue.getFileByName("/tmp/a.bin");
			EXO_ASSERT(byName == byTTH);

			EXO_ASSERT(queue.getFileByTTH(TTH2) == 0);
		}
		removeFile(path);
	
	return 1;
});

/*
	 * What save() writes, load() has to read back. This is the path the client
	 * actually takes: read at start-up, queue something, write at shutdown.
	 *
	 * Queueing the extra file is what makes this test anything at all - load()
	 * clears the dirty flag and save() returns early while it is clear, so a
	 * shutdown that changed nothing rewrites nothing.
	 */
EXO_TEST(queue_save_and_load_round_trip,
{
	/* TCCIFY: reports failure or a skip its own way. Convert by hand
	   to EXO_FAIL(msg) / EXO_SKIP(reason) - this test cannot fail
	   as it stands. */

		const std::string path = queuePath();
		writeFile(path,
			QUEUE("<File name=\"/tmp/a &amp; b\" size=\"1000\" tth=\"" TTH1 "\" priority=\"high\">"
			      "<Source url=\"adc://host:412/AAAA\"/>"
			      "<Chunk start=\"0\" end=\"400\"/>"
			      "</File>"));

		const char* expect =
			"[/tmp/a & b 1000 " TTH1 " high done=400 {adc://host:412,AAAA}]"
			"[/tmp/added 55 " TTH2 " normal done=0]";

		std::string first;
		{
			QuickDC::QueueManager queue(path.c_str());
			EXO_ASSERT(queue.add("/tmp/added", TTH2, 55) != 0);
			first = render(queue);
		}

		std::string second;
		{
			QuickDC::QueueManager queue(path.c_str());
			second = render(queue);
		}

		EXO_ASSERT(first == expect);
		if (first != second)
		{
			printf("FAIL save/load round trip\n    first:  %s\n    second: %s\n",
				first.c_str(), second.c_str());
			failures++;
		}
		else
		{
		}

		removeFile(path);
	
	return 1;
});

/* A queue nothing touched is left on disk exactly as it was found. */
EXO_TEST(queue_an_untouched_queue_is_not_rewritten,
{
		const std::string path = queuePath();
		const char* original =
			QUEUE("<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\"/>");
		writeFile(path, original);

		{
			QuickDC::QueueManager queue(path.c_str());
			EXO_ASSERT(queue.size() == 1);
		}

		FILE* fh = fopen(path.c_str(), "rb");
		std::string after;
		if (fh)
		{
			char buf[4096];
			size_t got = fread(buf, 1, sizeof(buf), fh);
			after.assign(buf, got);
			fclose(fh);
		}

		EXO_ASSERT(after == original);
		removeFile(path);
	
	return 1;
});
