/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Generating the file list we hand to other clients: scanning a share, writing
 * files.xml, and compressing it to files.xml.bz2.
 *
 * The compressed copy is the interesting one. The loop that produced it used
 * to stop at the codec's first flush call, so a list whose last block did not
 * fit one 64k buffer was written truncated - a file no client could unpack.
 * That was found by inspection and fixed with nothing exercising it, which is
 * what the large case below is for.
 *
 * What comes out is also fed back through the decoder that reads other
 * people's lists, so the two halves are checked against each other.
 */

#include "quickdc.h"
#include "api/core.h"
#include "config/preferences.h"
#include "hash/hashmanager.h"
#include "share/sharemanager.h"
#include "share/dcfilelist.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <samurai/io/codec.h>
#include <samurai/io/compression.h>

#include <chrono>
#include <memory>
#include <span>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* The pid is in the name because two runs at once would otherwise share a
   share directory, and each cleans up after itself at the end. */
static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_listgen_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

static bool writeFile(const std::string& path, const std::string& content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) return false;
	const bool ok = fwrite(content.data(), 1, content.size(), fh) == content.size();
	fclose(fh);
	return ok;
}

static std::string readFile(const std::string& path)
{
	FILE* fh = fopen(path.c_str(), "rb");
	if (!fh) return std::string();

	std::string out;
	char buf[65536];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), fh)) > 0) out.append(buf, got);
	fclose(fh);
	return out;
}

static std::string unbzip2(const std::string& packed)
{
	Samurai::IO::BZip2Decompressor codec;
	std::string out;
	std::span<const char> in(packed);
	char buf[65536];

	while (true)
	{
		const Samurai::IO::Codec::Progress p = codec.step(in, buf);
		if (p.status == Samurai::IO::Codec::Status::Error) return std::string();
		out.append(buf, p.produced);
		in = in.subspan(p.consumed);
		if (p.status == Samurai::IO::Codec::Status::StreamEnd) break;
		if (!p.consumed && !p.produced) break;
	}
	return out;
}

/* Renders what our own decoder makes of the list we produced. */
static std::string decode(const std::string& xml)
{
	Samurai::IO::Buffer buffer;
	buffer.append(xml.data(), xml.size());

	Share::RemoteDir root("", 0);
	Share::XMLFileListHandler handler(&root, &buffer);
	if (!handler.parse()) return std::string("(refused)");

	/* Depth first, so the shape shows as well as the names. */
	struct Walk
	{
		static void render(const Share::RemoteDir* dir, std::string& out)
		{
			for (size_t n = 0; n < dir->countSubdirs(); n++)
			{
				const Share::RemoteDir* sub = dir->getSubdir(n);
				out += "[";
				out += sub->getName();
				render(sub, out);
				out += "]";
			}
			for (size_t n = 0; n < dir->countEntries(); n++)
			{
				out += "(";
				out += dir->getEntry(n)->getName();
				out += ")";
			}
		}
	};

	std::string out;
	Walk::render(&root, out);
	return out;
}

/*
 * A share directory, a core configured to share it, and the lists generated
 * from it.
 */
class Fixture
{
	public:
		std::string dir;      /* the shared directory */
		std::string out;      /* where the lists are written */
		std::string cfg;
		std::string ses;
		std::string hashcache;
		std::vector<std::string> madeFiles;
		std::vector<std::string> madeDirs;
		std::unique_ptr<QuickDC::Core> core;
		std::unique_ptr<QuickDC::Hash::Manager> hashes;
		QuickDC::Share::Manager* shares = 0;

		explicit Fixture(const char* tag)
			: dir(scratch(tag)), out(scratch("out")),
			  cfg(scratch("config.conf")), ses(scratch("session.conf")),
			  hashcache(scratch("hashdb.cache"))
		{
			Samurai::IO::File::mkdir(dir.c_str());
			Samurai::IO::File::mkdir(out.c_str());
		}

		~Fixture()
		{
			/* The share files hold jobs in the hash manager, so they go first. */
			shares = 0;
			core.reset();
			hashes.reset();

			Samurai::IO::File(hashcache).remove();
			Samurai::IO::File(hashcache + ".new").remove();

			for (size_t n = 0; n < madeFiles.size(); n++)
				Samurai::IO::File(madeFiles[n]).remove();

			/* Deepest first, and with rmdir: remove() unlinks, which never
			   takes a directory away, so the parents stayed behind too. */
			for (size_t n = madeDirs.size(); n-- > 0; )
				Samurai::IO::File::rmdir(madeDirs[n].c_str());

			Samurai::IO::File(out + "/files.xml").remove();
			Samurai::IO::File(out + "/files.xml.bz2").remove();
			Samurai::IO::File::rmdir(out.c_str());
			Samurai::IO::File::rmdir(dir.c_str());
			Samurai::IO::File(cfg).remove();
			Samurai::IO::File(ses).remove();
		}

		Fixture(const Fixture&) = delete;
		Fixture& operator=(const Fixture&) = delete;

		void file(const std::string& name, const std::string& content = "x")
		{
			const std::string path = dir + "/" + name;
			writeFile(path, content);
			madeFiles.push_back(path);
		}

		void subdir(const std::string& name)
		{
			const std::string path = dir + "/" + name;
			Samurai::IO::File::mkdir(path.c_str());
			madeDirs.push_back(path);
		}

		/* A symlink inside the share, aimed at whatever is given. */
		void symlink_to(const std::string& name, const std::string& target)
		{
			const std::string path = dir + "/" + name;
			::symlink(target.c_str(), path.c_str());
			madeFiles.push_back(path);
		}

		/* A file outside the share, standing in for anything private. */
		std::string outsideFile(const std::string& leaf, const std::string& content)
		{
			const std::string path = scratch(leaf.c_str());
			writeFile(path, content);
			madeFiles.push_back(path);
			return path;
		}

		/* Scans the share and writes both lists. */
		bool generate()
		{
			writeFile(cfg,
				"[User]\nNickname=tester\n\n"
				"[Shares]\nTotal shares=1\nShare 0=shared:" + dir + "\n");

			core = std::make_unique<QuickDC::Core>(cfg, ses);

			/*
			 * A share file registers a hashing job as it is found, so there
			 * has to be a manager to register with - pointed at a cache of its
			 * own rather than the real database.
			 */
			hashes = std::make_unique<QuickDC::Hash::Manager>(hashcache.c_str());
			shares = new QuickDC::Share::Manager(core->config, hashes.get(), out.c_str());

			/* The core owns whatever it built; this one replaces it. */
			delete core->shares;
			core->shares = shares;

			shares->initialize();

			/*
			 * Scanning only queues the hashing; a file has no TTH until the
			 * worker has been round to it, and the main loop is what announces
			 * that. A list generated before then carries empty TTHs, which
			 * every decoder refuses - including ours.
			 */
			const auto deadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(120);
			while (hashes->process() || hashes->isBusy())
			{
				if (std::chrono::steady_clock::now() > deadline) return false;
				usleep(1000);
			}
			hashes->process();

			shares->createFileLists(false);
			return true;
		}

		std::string plain() const { return readFile(out + "/files.xml"); }
		std::string packed() const { return readFile(out + "/files.xml.bz2"); }
};

int main()
{
	setbuf(stdout, 0);

	/* A share with nothing in it still produces a well formed list. */
	{
		Fixture f("empty");
		f.generate();

		const std::string xml = f.plain();
		CHECK(!xml.empty());
		CHECK(xml.find("<FileListing") != std::string::npos);
		CHECK(xml.find("</FileListing>") != std::string::npos);
		CHECK(decode(xml) == "[shared]");
		printf("  ok an empty share produces a well formed list\n");
	}

	/* Files and directories come out, and our own decoder reads them back. */
	{
		Fixture f("simple");
		f.file("alpha.bin");
		f.file("beta.bin");
		f.subdir("music");
		f.file("music/song.mp3");
		f.generate();

		const std::string decoded = decode(f.plain());
		CHECK(decoded.find("(alpha.bin)") != std::string::npos);
		CHECK(decoded.find("(beta.bin)") != std::string::npos);
		CHECK(decoded.find("[music") != std::string::npos);
		CHECK(decoded.find("(song.mp3)") != std::string::npos);
		printf("  ok files and directories survive the round trip: %s\n", decoded.c_str());
	}

	/* Nesting is preserved rather than flattened. */
	{
		Fixture f("nested");
		f.subdir("a");
		f.subdir("a/b");
		f.file("a/b/deep.txt");
		f.generate();

		const std::string decoded = decode(f.plain());
		CHECK(decoded.find("[a[b(deep.txt)]]") != std::string::npos);
		printf("  ok nesting is preserved: %s\n", decoded.c_str());
	}

	/* A name with XML in it is escaped on the way out and read back whole. */
	{
		Fixture f("escaping");
		f.file("a & b <c>.txt");
		f.generate();

		const std::string xml = f.plain();
		CHECK(xml.find("&amp;") != std::string::npos);
		CHECK(xml.find("&lt;") != std::string::npos);
		CHECK(decode(xml).find("(a & b <c>.txt)") != std::string::npos);
		printf("  ok a name containing XML is escaped and read back\n");
	}

	/* The compressed copy is the same list. */
	{
		Fixture f("compressed");
		f.file("one.bin");
		f.file("two.bin");
		f.generate();

		const std::string packed = f.packed();
		CHECK(!packed.empty());
		CHECK(packed.compare(0, 3, "BZh") == 0);
		CHECK(unbzip2(packed) == f.plain());
		printf("  ok files.xml.bz2 unpacks to exactly files.xml\n");
	}

	/*
	 * The case the truncation bug needed. Enough files with names that do not
	 * compress well that the codec's final flush is bigger than one output
	 * buffer, so producing it takes several calls.
	 */
	{
		Fixture f("large");

		unsigned seed = 12345;
		for (int n = 0; n < 4000; n++)
		{
			std::string name;
			for (int c = 0; c < 40; c++)
			{
				seed = seed * 1103515245u + 12345u;
				name += (char) ('a' + ((seed >> 16) % 26));
			}
			f.file(name + ".bin");
		}
		f.generate();

		const std::string plain = f.plain();
		const std::string packed = f.packed();

		CHECK(plain.size() > 200000);
		CHECK(!packed.empty());

		/* Several buffers' worth, or this is not testing what it claims. */
		CHECK(packed.size() > 64 * 1024);

		CHECK(unbzip2(packed) == plain);
		printf("  ok a %zu byte list compresses to %zu bytes and unpacks whole\n",
			plain.size(), packed.size());
	}

	/* Regenerating replaces the lists rather than appending to them. */
	{
		Fixture f("regen");
		f.file("first.bin");
		f.generate();

		const size_t before = f.plain().size();
		CHECK(before > 0);

		f.shares->createFileLists(false);

		const std::string again = f.plain();
		CHECK(again.size() == before);
		CHECK(unbzip2(f.packed()) == again);
		printf("  ok regenerating replaces rather than appends\n");
	}

	/*
	 * A symlink is not followed. isDirectory() and isRegular() answer for a
	 * link's target, so the scanner classified one as whatever it aimed at and
	 * the branch meant to skip it could never run - which is how a link inside a
	 * shared directory came to publish a file from outside the share root to
	 * anyone who asked for the list.
	 */
	{
		Fixture f("symlink");
		const std::string secret = f.outsideFile("private.key", "PRIVATE KEY");
		f.file("public.bin");
		f.symlink_to("leaked.key", secret);
		f.generate();

		const std::string xml = f.plain();
		const std::string decoded = decode(xml);

		CHECK(decoded.find("(public.bin)") != std::string::npos);
		CHECK(decoded.find("leaked.key") == std::string::npos);
		CHECK(xml.find("leaked.key") == std::string::npos);
		CHECK(xml.find("PRIVATE KEY") == std::string::npos);
		printf("  ok a symlink out of the share is not published: %s\n", decoded.c_str());
	}

	/* A symlinked directory is skipped too, so a loop cannot be walked into. */
	{
		Fixture f("symlinkdir");
		f.subdir("real");
		f.file("real/inside.txt");
		f.symlink_to("loop", f.dir);
		f.generate();

		const std::string decoded = decode(f.plain());
		CHECK(decoded.find("(inside.txt)") != std::string::npos);
		CHECK(decoded.find("loop") == std::string::npos);
		printf("  ok a symlinked directory is not descended into: %s\n", decoded.c_str());
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
