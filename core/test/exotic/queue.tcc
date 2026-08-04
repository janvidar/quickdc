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
	if (!fh) return;
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

static bool check(const char* label, const char* xml, const char* expect)
{
	const std::string path = queuePath();
	writeFile(path, xml);

	std::string out;
	{
		QuickDC::QueueManager queue(path.c_str());
		out = render(queue);
	}

	const bool matched = (out == expect);
	if (!matched)
		printf("    %s\n      expected: %s\n      actual:   %s\n",
			label, expect, out.c_str());

	removeFile(path);
	return matched;
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
			const std::string why = "a save/load round trip changed the queue:\n"
				"      first:  " + first + "\n      second: " + second;
			EXO_FAIL(why.c_str());
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

EXO_TEST(queue_empty_queue,
{
	EXO_ASSERT(check("empty queue", QUEUE(""), ""));
	return 1;
});

EXO_TEST(queue_one_file,
{
	EXO_ASSERT(check("one file",
			QUEUE(FILE1("></File>")),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_self_closing_file,
{
	EXO_ASSERT(check("self-closing file",
			QUEUE(FILE1("/>")),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_two_files,
{
	EXO_ASSERT(check("two files",
			QUEUE("<File name=\"/tmp/a.bin\" size=\"1000\" tth=\"" TTH1 "\"/>"
			      "<File name=\"/tmp/b.bin\" size=\"2000\" tth=\"" TTH2 "\"/>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"
			"[/tmp/b.bin 2000 " TTH2 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_every_priority,
{
	EXO_ASSERT(check("every priority",
			QUEUE("<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\" priority=\"paused\"/>"
			      "<File name=\"/tmp/b\" size=\"1\" tth=\"" TTH2 "\" priority=\"high\"/>"),
			"[/tmp/a 1 " TTH1 " paused done=0]"
			"[/tmp/b 1 " TTH2 " high done=0]"));
	return 1;
});

EXO_TEST(queue_unknown_priority_falls_back_to_normal,
{
	EXO_ASSERT(check("unknown priority falls back to normal",
			QUEUE(FILE1(" priority=\"urgent\"/>")),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_escaped_filename,
{
	EXO_ASSERT(check("escaped filename",
			QUEUE("<File name=\"/tmp/a &amp; b\" size=\"1\" tth=\"" TTH1 "\"/>"),
			"[/tmp/a & b 1 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_utf_8_filename,
{
	EXO_ASSERT(check("utf-8 filename",
			QUEUE("<File name=\"/tmp/Bj\xc3\xb8rn\" size=\"1\" tth=\"" TTH1 "\"/>"),
			"[/tmp/Bj\xc3\xb8rn 1 " TTH1 " normal done=0]"));
	return 1;
});

/* Sources hang off the file they were found for. */
EXO_TEST(queue_file_with_sources,
{
	EXO_ASSERT(check("file with sources",
			QUEUE(FILE1(">")
			      "<Source url=\"adc://host:412/AAAA\"/>"
			      "<Source url=\"adc://host:412/BBBB\"/>"
			      "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0 {adc://host:412,AAAA} {adc://host:412,BBBB}]"));
	return 1;
});

EXO_TEST(queue_the_same_source_twice_is_kept_once,
{
	EXO_ASSERT(check("the same source twice is kept once",
			QUEUE(FILE1(">")
			      "<Source url=\"adc://host:412/AAAA\"/>"
			      "<Source url=\"adc://host:412/AAAA\"/>"
			      "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0 {adc://host:412,AAAA}]"));
	return 1;
});

EXO_TEST(queue_source_with_no_url,
{
	EXO_ASSERT(check("source with no url",
			QUEUE(FILE1(">") "<Source/>" "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_source_with_an_unusable_url,
{
	EXO_ASSERT(check("source with an unusable url",
			QUEUE(FILE1(">") "<Source url=\"not a url\"/>" "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

/* Too many failures to be worth carrying across a restart. */
EXO_TEST(queue_source_that_has_failed_too_often,
{
	EXO_ASSERT(check("source that has failed too often",
			QUEUE(FILE1(">")
			      "<Source url=\"adc://host:412/AAAA\" retries=\"9999\"/>"
			      "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_source_not_seen_for_years,
{
	EXO_ASSERT(check("source not seen for years",
			QUEUE(FILE1(">")
			      "<Source url=\"adc://host:412/AAAA\" lastseen=\"1000\"/>"
			      "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_source_outside_any_file,
{
	EXO_ASSERT(check("source outside any file",
			QUEUE("<Source url=\"adc://host:412/AAAA\"/>"),
			""));
	return 1;
});

/* Chunks say what is already on disk, so a wrong one corrupts a file. */
EXO_TEST(queue_file_with_a_chunk,
{
	EXO_ASSERT(check("file with a chunk",
			QUEUE(FILE1(">") "<Chunk start=\"0\" end=\"400\"/>" "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=400]"));
	return 1;
});

EXO_TEST(queue_several_chunks,
{
	EXO_ASSERT(check("several chunks",
			QUEUE(FILE1(">")
			      "<Chunk start=\"0\" end=\"100\"/>"
			      "<Chunk start=\"500\" end=\"600\"/>"
			      "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=200]"));
	return 1;
});

/* addRange() coalesces, so an overlap cannot inflate the total. */
EXO_TEST(queue_overlapping_chunks,
{
	EXO_ASSERT(check("overlapping chunks",
			QUEUE(FILE1(">")
			      "<Chunk start=\"0\" end=\"400\"/>"
			      "<Chunk start=\"200\" end=\"600\"/>"
			      "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=600]"));
	return 1;
});

EXO_TEST(queue_chunk_past_the_end_of_the_file,
{
	EXO_ASSERT(check("chunk past the end of the file",
			QUEUE(FILE1(">") "<Chunk start=\"0\" end=\"5000\"/>" "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_chunk_that_ends_before_it_starts,
{
	EXO_ASSERT(check("chunk that ends before it starts",
			QUEUE(FILE1(">") "<Chunk start=\"400\" end=\"100\"/>" "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_empty_chunk,
{
	EXO_ASSERT(check("empty chunk",
			QUEUE(FILE1(">") "<Chunk start=\"100\" end=\"100\"/>" "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_chunk_with_no_bounds,
{
	EXO_ASSERT(check("chunk with no bounds",
			QUEUE(FILE1(">") "<Chunk/>" "</File>"),
			"[/tmp/a.bin 1000 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_chunk_outside_any_file,
{
	EXO_ASSERT(check("chunk outside any file",
			QUEUE("<Chunk start=\"0\" end=\"100\"/>"),
			""));
	return 1;
});

/* A file we cannot trust is dropped rather than guessed at. */
EXO_TEST(queue_file_with_no_name,
{
	EXO_ASSERT(check("file with no name",
			QUEUE("<File size=\"1000\" tth=\"" TTH1 "\"/>"),
			""));
	return 1;
});

EXO_TEST(queue_file_with_no_size,
{
	EXO_ASSERT(check("file with no size",
			QUEUE("<File name=\"/tmp/a\" tth=\"" TTH1 "\"/>"),
			""));
	return 1;
});

EXO_TEST(queue_file_with_a_zero_size,
{
	EXO_ASSERT(check("file with a zero size",
			QUEUE("<File name=\"/tmp/a\" size=\"0\" tth=\"" TTH1 "\"/>"),
			""));
	return 1;
});

EXO_TEST(queue_file_with_an_unreadable_size,
{
	EXO_ASSERT(check("file with an unreadable size",
			QUEUE("<File name=\"/tmp/a\" size=\"lots\" tth=\"" TTH1 "\"/>"),
			""));
	return 1;
});

EXO_TEST(queue_file_with_no_tth,
{
	EXO_ASSERT(check("file with no tth",
			QUEUE("<File name=\"/tmp/a\" size=\"1000\"/>"),
			""));
	return 1;
});

EXO_TEST(queue_file_with_a_short_tth,
{
	EXO_ASSERT(check("file with a short tth",
			QUEUE("<File name=\"/tmp/a\" size=\"1000\" tth=\"TOOSHORT\"/>"),
			""));
	return 1;
});

EXO_TEST(queue_file_with_a_long_tth,
{
	EXO_ASSERT(check("file with a long tth",
			QUEUE("<File name=\"/tmp/a\" size=\"1000\" tth=\"" TTH1 "EXTRA\"/>"),
			""));
	return 1;
});

EXO_TEST(queue_file_with_an_empty_name,
{
	EXO_ASSERT(check("file with an empty name",
			QUEUE("<File name=\"\" size=\"1000\" tth=\"" TTH1 "\"/>"),
			""));
	return 1;
});

/* A dropped file must not take the ones around it with it. */
EXO_TEST(queue_a_dropped_file_among_good_ones,
{
	EXO_ASSERT(check("a dropped file among good ones",
			QUEUE("<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\"/>"
			      "<File name=\"/tmp/bad\" size=\"1\" tth=\"SHORT\"/>"
			      "<File name=\"/tmp/b\" size=\"1\" tth=\"" TTH2 "\"/>"),
			"[/tmp/a 1 " TTH1 " normal done=0]"
			"[/tmp/b 1 " TTH2 " normal done=0]"));
	return 1;
});

/* Sources and chunks after a dropped file have no file to attach to. */
EXO_TEST(queue_children_of_a_dropped_file,
{
	EXO_ASSERT(check("children of a dropped file",
			QUEUE("<File name=\"/tmp/bad\" size=\"0\" tth=\"" TTH1 "\">"
			      "<Source url=\"adc://host:412/AAAA\"/>"
			      "<Chunk start=\"0\" end=\"10\"/>"
			      "</File>"
			      "<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH2 "\"/>"),
			"[/tmp/a 1 " TTH2 " normal done=0]"));
	return 1;
});

/* Malformed structure. */
EXO_TEST(queue_nested_files,
{
	EXO_ASSERT(check("nested files",
			QUEUE("<File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\">"
			      "<File name=\"/tmp/b\" size=\"1\" tth=\"" TTH2 "\"/>"
			      "</File>"),
			"[/tmp/a 1 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_end_tag_with_nothing_open,
{
	EXO_ASSERT(check("end tag with nothing open",
			QUEUE("</File><File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\"/>"),
			"[/tmp/a 1 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_truncated_in_the_middle_of_a_file,
{
	EXO_ASSERT(check("truncated in the middle of a file",
			"<DownloadQueue><File name=\"/tmp/a\" size=\"1000\" tth=\"" TTH1 "\">"
			"<Chunk start=\"0\" end=\"100\"/>",
			""));
	return 1;
});

EXO_TEST(queue_not_xml_at_all,
{
	EXO_ASSERT(check("not xml at all", "this is not a download queue", ""));
	return 1;
});

EXO_TEST(queue_unknown_elements_are_ignored,
{
	EXO_ASSERT(check("unknown elements are ignored",
			QUEUE("<Nonsense/><File name=\"/tmp/a\" size=\"1\" tth=\"" TTH1 "\"/>"),
			"[/tmp/a 1 " TTH1 " normal done=0]"));
	return 1;
});

EXO_TEST(queue_empty_file,
{
	EXO_ASSERT(check("empty file", "", ""));
	return 1;
});
