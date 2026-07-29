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

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

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

int main()
{
	check("empty queue", QUEUE(""), "");

	check("one file",
		QUEUE(FILE1("></File>")),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("self-closing file",
		QUEUE(FILE1("/>")),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("two files",
		QUEUE("<File name=\"/tmp/a.bin\" size=\"1000\" tth=\"" TTH1 "\"/>"
		      "<File name=\"/tmp/b.bin\" size=\"2000\" tth=\"" TTH2 "\"/>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]"
		"[/tmp/b.bin 2000 " TTH2 " normal done=0]");

	check("every priority",
		QUEUE("<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\" priority=\"paused\"/>"
		      "<File name=\"/tmp/b\" size=\"1\" tth=\"" TTH2 "\" priority=\"high\"/>"),
		"[/tmp/a 1 " TTH1 " paused done=0]"
		"[/tmp/b 1 " TTH2 " high done=0]");

	check("unknown priority falls back to normal",
		QUEUE(FILE1(" priority=\"urgent\"/>")),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("escaped filename",
		QUEUE("<File name=\"/tmp/a &amp; b\" size=\"1\" tth=\"" TTH1 "\"/>"),
		"[/tmp/a & b 1 " TTH1 " normal done=0]");

	check("utf-8 filename",
		QUEUE("<File name=\"/tmp/Bj\xc3\xb8rn\" size=\"1\" tth=\"" TTH1 "\"/>"),
		"[/tmp/Bj\xc3\xb8rn 1 " TTH1 " normal done=0]");

	/* Sources hang off the file they were found for. */
	check("file with sources",
		QUEUE(FILE1(">")
		      "<Source url=\"adc://host:412/AAAA\"/>"
		      "<Source url=\"adc://host:412/BBBB\"/>"
		      "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0 {adc://host:412,AAAA} {adc://host:412,BBBB}]");

	check("the same source twice is kept once",
		QUEUE(FILE1(">")
		      "<Source url=\"adc://host:412/AAAA\"/>"
		      "<Source url=\"adc://host:412/AAAA\"/>"
		      "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0 {adc://host:412,AAAA}]");

	check("source with no url",
		QUEUE(FILE1(">") "<Source/>" "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("source with an unusable url",
		QUEUE(FILE1(">") "<Source url=\"not a url\"/>" "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	/* Too many failures to be worth carrying across a restart. */
	check("source that has failed too often",
		QUEUE(FILE1(">")
		      "<Source url=\"adc://host:412/AAAA\" retries=\"9999\"/>"
		      "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("source not seen for years",
		QUEUE(FILE1(">")
		      "<Source url=\"adc://host:412/AAAA\" lastseen=\"1000\"/>"
		      "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("source outside any file",
		QUEUE("<Source url=\"adc://host:412/AAAA\"/>"),
		"");

	/* Chunks say what is already on disk, so a wrong one corrupts a file. */
	check("file with a chunk",
		QUEUE(FILE1(">") "<Chunk start=\"0\" end=\"400\"/>" "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=400]");

	check("several chunks",
		QUEUE(FILE1(">")
		      "<Chunk start=\"0\" end=\"100\"/>"
		      "<Chunk start=\"500\" end=\"600\"/>"
		      "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=200]");

	/* addRange() coalesces, so an overlap cannot inflate the total. */
	check("overlapping chunks",
		QUEUE(FILE1(">")
		      "<Chunk start=\"0\" end=\"400\"/>"
		      "<Chunk start=\"200\" end=\"600\"/>"
		      "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=600]");

	check("chunk past the end of the file",
		QUEUE(FILE1(">") "<Chunk start=\"0\" end=\"5000\"/>" "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("chunk that ends before it starts",
		QUEUE(FILE1(">") "<Chunk start=\"400\" end=\"100\"/>" "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("empty chunk",
		QUEUE(FILE1(">") "<Chunk start=\"100\" end=\"100\"/>" "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("chunk with no bounds",
		QUEUE(FILE1(">") "<Chunk/>" "</File>"),
		"[/tmp/a.bin 1000 " TTH1 " normal done=0]");

	check("chunk outside any file",
		QUEUE("<Chunk start=\"0\" end=\"100\"/>"),
		"");

	/* A file we cannot trust is dropped rather than guessed at. */
	check("file with no name",
		QUEUE("<File size=\"1000\" tth=\"" TTH1 "\"/>"),
		"");

	check("file with no size",
		QUEUE("<File name=\"/tmp/a\" tth=\"" TTH1 "\"/>"),
		"");

	check("file with a zero size",
		QUEUE("<File name=\"/tmp/a\" size=\"0\" tth=\"" TTH1 "\"/>"),
		"");

	check("file with an unreadable size",
		QUEUE("<File name=\"/tmp/a\" size=\"lots\" tth=\"" TTH1 "\"/>"),
		"");

	check("file with no tth",
		QUEUE("<File name=\"/tmp/a\" size=\"1000\"/>"),
		"");

	check("file with a short tth",
		QUEUE("<File name=\"/tmp/a\" size=\"1000\" tth=\"TOOSHORT\"/>"),
		"");

	check("file with a long tth",
		QUEUE("<File name=\"/tmp/a\" size=\"1000\" tth=\"" TTH1 "EXTRA\"/>"),
		"");

	check("file with an empty name",
		QUEUE("<File name=\"\" size=\"1000\" tth=\"" TTH1 "\"/>"),
		"");

	/* A dropped file must not take the ones around it with it. */
	check("a dropped file among good ones",
		QUEUE("<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\"/>"
		      "<File name=\"/tmp/bad\" size=\"1\" tth=\"SHORT\"/>"
		      "<File name=\"/tmp/b\" size=\"1\" tth=\"" TTH2 "\"/>"),
		"[/tmp/a 1 " TTH1 " normal done=0]"
		"[/tmp/b 1 " TTH2 " normal done=0]");

	/* Sources and chunks after a dropped file have no file to attach to. */
	check("children of a dropped file",
		QUEUE("<File name=\"/tmp/bad\" size=\"0\" tth=\"" TTH1 "\">"
		      "<Source url=\"adc://host:412/AAAA\"/>"
		      "<Chunk start=\"0\" end=\"10\"/>"
		      "</File>"
		      "<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH2 "\"/>"),
		"[/tmp/a 1 " TTH2 " normal done=0]");

	/* Malformed structure. */
	check("nested files",
		QUEUE("<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\">"
		      "<File name=\"/tmp/b\" size=\"1\" tth=\"" TTH2 "\"/>"
		      "</File>"),
		"[/tmp/a 1 " TTH1 " normal done=0]");

	check("end tag with nothing open",
		QUEUE("</File><File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\"/>"),
		"[/tmp/a 1 " TTH1 " normal done=0]");

	check("truncated in the middle of a file",
		"<DownloadQueue><File name=\"/tmp/a\" size=\"1000\" tth=\"" TTH1 "\">"
		"<Chunk start=\"0\" end=\"100\"/>",
		"");

	check("not xml at all", "this is not a download queue", "");

	check("unknown elements are ignored",
		QUEUE("<Nonsense/><File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\"/>"),
		"[/tmp/a 1 " TTH1 " normal done=0]");

	/* A missing or empty file leaves an empty queue, not a failure. */
	{
		const std::string path = queuePath();
		removeFile(path);
		QuickDC::QueueManager queue(path.c_str());
		CHECK(queue.size() == 0);
		CHECK(queue.first() == 0);
		printf("  ok missing file\n");
	}

	check("empty file", "", "");

	/* Lookups have to find what was loaded. */
	{
		const std::string path = queuePath();
		writeFile(path,
			QUEUE("<File name=\"/tmp/a.bin\" size=\"1000\" tth=\"" TTH1 "\"/>"));
		{
			QuickDC::QueueManager queue(path.c_str());
			CHECK(queue.size() == 1);

			QuickDC::QueueFile* byTTH = queue.getFileByTTH(TTH1);
			CHECK(byTTH != 0);
			CHECK(byTTH && !strcmp(byTTH->getLocalFilename(), "/tmp/a.bin"));

			QuickDC::QueueFile* byName = queue.getFileByName("/tmp/a.bin");
			CHECK(byName == byTTH);

			CHECK(queue.getFileByTTH(TTH2) == 0);
			printf("  ok lookups find the loaded file\n");
		}
		removeFile(path);
	}

	/*
	 * What save() writes, load() has to read back. This is the path the client
	 * actually takes: read at start-up, queue something, write at shutdown.
	 *
	 * Queueing the extra file is what makes this test anything at all - load()
	 * clears the dirty flag and save() returns early while it is clear, so a
	 * shutdown that changed nothing rewrites nothing.
	 */
	{
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
			CHECK(queue.add("/tmp/added", TTH2, 55) != 0);
			first = render(queue);
		}

		std::string second;
		{
			QuickDC::QueueManager queue(path.c_str());
			second = render(queue);
		}

		CHECK(first == expect);
		if (first != second)
		{
			printf("FAIL save/load round trip\n    first:  %s\n    second: %s\n",
				first.c_str(), second.c_str());
			failures++;
		}
		else
		{
			printf("  ok save and load round trip\n");
		}

		removeFile(path);
	}

	/* A queue nothing touched is left on disk exactly as it was found. */
	{
		const std::string path = queuePath();
		const char* original =
			QUEUE("<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\"/>");
		writeFile(path, original);

		{
			QuickDC::QueueManager queue(path.c_str());
			CHECK(queue.size() == 1);
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

		CHECK(after == original);
		printf("  ok an untouched queue is not rewritten\n");
		removeFile(path);
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
