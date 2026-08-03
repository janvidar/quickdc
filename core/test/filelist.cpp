/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Drives Share::XMLFileListHandler over file lists as they arrive from other
 * clients and checks the tree that comes out.
 *
 * This is attacker-controlled input: the list is whatever a remote peer chose
 * to send. So the malformed cases matter as much as the well formed ones, and
 * what they have to do is refuse the entry, not walk off the tree.
 */

#include "quickdc.h"
#include "share/dcfilelist.h"

#include <samurai/io/buffer.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int failures = 0;

/*
 * A RemoteFile is handed to its parent and never referred to again, so whether
 * the tree took ownership of it is only visible by counting.
 */
static size_t live = 0;

void* operator new(size_t n)
{
	void* p = malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	live++;
	return p;
}

void operator delete(void* p) noexcept
{
	if (!p) return;
	live--;
	free(p);
}

void* operator new[](size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/*
 * Renders the decoded tree as text so a whole list can be asserted in one go,
 * including the entries that should not be there.
 */
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
		const Share::RemoteFile* file = dir->getEntry(n);
		out += "(";
		out += file->getName();
		out += ",";
		out += std::to_string(file->getSize());
		out += ",";
		out += file->getTTH() ? file->getTTH() : "-";
		out += ")";
	}
}

static Share::RemoteDir* decode(const char* xml, bool& ok)
{
	Samurai::IO::Buffer buffer;
	buffer.append(xml, strlen(xml));

	Share::RemoteDir* root = new Share::RemoteDir("", 0);
	Share::XMLFileListHandler handler(root, &buffer);
	ok = handler.parse();
	return root;
}

static void check(const char* label, const char* xml, const char* expect)
{
	bool ok = false;
	Share::RemoteDir* root = decode(xml, ok);

	std::string out;
	render(root, out);

	if (out == expect)
		printf("  ok %s\n", label);
	else
		printf("FAIL %s\n    input:    %s\n    expected: %s\n    actual:   %s\n",
			label, xml, expect, out.c_str()), failures++;

	delete root;
}

#define LIST(body) "<FileListing Version=\"1\" Base=\"/\">" body "</FileListing>"

int main()
{
	/* Unbuffered, so that what has already passed is on the terminal if a case
	   below takes the process down rather than failing. */
	setbuf(stdout, 0);

	check("empty list", LIST(""), "");

	check("one file",
		LIST("<File Name=\"a.mp3\" Size=\"123\" TTH=\"ROOT\"/>"),
		"(a.mp3,123,ROOT)");

	check("one directory",
		LIST("<Directory Name=\"music\"></Directory>"),
		"[music]");

	check("file inside a directory",
		LIST("<Directory Name=\"music\">"
		     "<File Name=\"a.mp3\" Size=\"123\" TTH=\"ROOT\"/>"
		     "</Directory>"),
		"[music(a.mp3,123,ROOT)]");

	check("nested directories",
		LIST("<Directory Name=\"a\">"
		     "<Directory Name=\"b\">"
		     "<File Name=\"deep\" Size=\"1\" TTH=\"T\"/>"
		     "</Directory>"
		     "</Directory>"),
		"[a[b(deep,1,T)]]");

	/*
	 * Siblings only stay siblings if the self-closing directory pops. It used
	 * to be that no end tag was reported for one at all.
	 */
	check("self-closing directory is not left open",
		LIST("<Directory Name=\"empty\"/>"
		     "<Directory Name=\"next\"/>"),
		"[empty][next]");

	check("sibling directories",
		LIST("<Directory Name=\"a\"><File Name=\"x\" Size=\"1\" TTH=\"T\"/></Directory>"
		     "<Directory Name=\"b\"><File Name=\"y\" Size=\"2\" TTH=\"U\"/></Directory>"),
		"[a(x,1,T)][b(y,2,U)]");

	/* The form other clients write: no space before the slash. */
	check("no space before the slash",
		LIST("<File Name=\"a.mp3\" Size=\"123\" TTH=\"ROOT\"/>"),
		"(a.mp3,123,ROOT)");

	check("space before the slash",
		LIST("<File Name=\"a.mp3\" Size=\"123\" TTH=\"ROOT\" />"),
		"(a.mp3,123,ROOT)");

	/* Attribute order is not fixed by the schema. */
	check("attributes in another order",
		LIST("<File TTH=\"ROOT\" Name=\"a.mp3\" Size=\"123\"/>"),
		"(a.mp3,123,ROOT)");

	check("element names are matched without case",
		LIST("<directory NAME=\"music\"><file name=\"a\" size=\"5\" tth=\"T\"/></directory>"),
		"[music(a,5,T)]");

	/* Names carry entities, and a file list is where the interesting ones are. */
	check("escaped name",
		LIST("<File Name=\"a &amp; b &lt;c&gt;\" Size=\"1\" TTH=\"T\"/>"),
		"(a & b <c>,1,T)");

	check("name that is literally an entity",
		LIST("<File Name=\"a&amp;lt;b\" Size=\"1\" TTH=\"T\"/>"),
		"(a&lt;b,1,T)");

	check("numeric reference in a name",
		LIST("<File Name=\"caf&#233;\" Size=\"1\" TTH=\"T\"/>"),
		"(caf\xc3\xa9,1,T)");

	check("utf-8 name",
		LIST("<Directory Name=\"Bj\xc3\xb8rn\"/>"),
		"[Bj\xc3\xb8rn]");

	/* Incomplete entries are refused rather than half built. */
	check("file with no tth",
		LIST("<File Name=\"a.mp3\" Size=\"123\"/>"),
		"");

	check("file with no name",
		LIST("<File Size=\"123\" TTH=\"ROOT\"/>"),
		"");

	check("file with an empty name",
		LIST("<File Name=\"\" Size=\"123\" TTH=\"ROOT\"/>"),
		"");

	check("file with an empty tth",
		LIST("<File Name=\"a.mp3\" Size=\"123\" TTH=\"\"/>"),
		"");

	check("directory with no name",
		LIST("<Directory><File Name=\"a\" Size=\"1\" TTH=\"T\"/></Directory>"),
		"(a,1,T)");

	check("directory with an empty name",
		LIST("<Directory Name=\"\"/>"),
		"");

	/* A missing size is not a reason to drop the file. */
	check("file with no size",
		LIST("<File Name=\"a.mp3\" TTH=\"ROOT\"/>"),
		"(a.mp3,0,ROOT)");

	check("file with a junk size",
		LIST("<File Name=\"a.mp3\" Size=\"abc\" TTH=\"ROOT\"/>"),
		"(a.mp3,0,ROOT)");

	/* Sizes aggregate up the tree. */
	{
		bool ok = false;
		Share::RemoteDir* root = decode(
			LIST("<Directory Name=\"a\">"
			     "<File Name=\"x\" Size=\"100\" TTH=\"T\"/>"
			     "<Directory Name=\"b\">"
			     "<File Name=\"y\" Size=\"20\" TTH=\"U\"/>"
			     "</Directory>"
			     "</Directory>"), ok);

		CHECK(root->getSize() == 120);
		CHECK(root->getFiles() == 2);

		const Share::RemoteDir* a = root->getSubdir(0);
		CHECK(a != 0);
		if (a)
		{
			CHECK(a->getSize() == 120);
			CHECK(a->getFiles() == 2);

			const Share::RemoteDir* b = a->getSubdir(0);
			CHECK(b != 0);
			if (b)
			{
				CHECK(b->getSize() == 20);
				CHECK(b->getFiles() == 1);
			}
		}
		printf("  ok sizes aggregate up the tree\n");
		delete root;
	}

	/*
	 * Nothing here may reach through a null currentDir, whatever the list says.
	 * The tree is empty in each case; that it returns at all is the assertion.
	 */
	check("file outside any listing",
		"<File Name=\"a\" Size=\"1\" TTH=\"T\"/>",
		"");

	check("directory outside any listing",
		"<Directory Name=\"a\"/>",
		"");

	check("end tag with nothing open",
		"</Directory><File Name=\"a\" Size=\"1\" TTH=\"T\"/>",
		"");

	check("more end tags than start tags",
		LIST("<Directory Name=\"a\"></Directory></Directory></Directory>"
		     "<File Name=\"x\" Size=\"1\" TTH=\"T\"/>"),
		"[a]");

	check("unclosed directory",
		LIST("<Directory Name=\"a\"><File Name=\"x\" Size=\"1\" TTH=\"T\"/>"),
		"[a(x,1,T)]");

	check("nested listings",
		LIST(LIST("<File Name=\"a\" Size=\"1\" TTH=\"T\"/>")),
		"(a,1,T)");

	check("junk instead of a listing", "not xml at all", "");

	check("empty document", "", "");

	check("unknown elements are ignored",
		LIST("<Nonsense Name=\"x\"/>"
		     "<File Name=\"a\" Size=\"1\" TTH=\"T\"/>"),
		"(a,1,T)");

	/* Deleting the root has to account for every file and directory in it. */
	{
		const char* xml =
			LIST("<Directory Name=\"a\">"
			     "<File Name=\"x\" Size=\"1\" TTH=\"T\"/>"
			     "<File Name=\"y\" Size=\"2\" TTH=\"U\"/>"
			     "<Directory Name=\"b\">"
			     "<File Name=\"z\" Size=\"3\" TTH=\"V\"/>"
			     "</Directory>"
			     "</Directory>");

		/* Warm up whatever the first parse allocates once and keeps. */
		{
			bool ok = false;
			delete decode(xml, ok);
		}

		const size_t before = live;
		for (int n = 0; n < 20; n++)
		{
			bool ok = false;
			Share::RemoteDir* root = decode(xml, ok);
			CHECK(root->getFiles() == 3);
			delete root;
		}

		if (live != before)
		{
			printf("FAIL decoding leaks: %zu allocations over 20 lists\n", live - before);
			failures++;
		}
		else
		{
			printf("  ok decoding and deleting balances\n");
		}
	}

	/*
	 * Nesting. A list arrives bzip2 compressed, so a few kilobytes of it is as
	 * many levels as the peer cared to write, and a level is a directory that is
	 * held on to and later walked.
	 *
	 * Ordinary depth is followed as before.
	 */
	{
		const int depth = 100;

		std::string xml = "<FileListing Version=\"1\" Base=\"/\">";
		for (int n = 0; n < depth; n++) xml += "<Directory Name=\"d\">";
		xml += "<File Name=\"bottom\" Size=\"5\" TTH=\"T\"/>";
		for (int n = 0; n < depth; n++) xml += "</Directory>";
		xml += "</FileListing>";

		bool ok = false;
		Share::RemoteDir* root = decode(xml.c_str(), ok);

		CHECK(ok);
		CHECK(root->getSize() == 5);

		size_t levels = 0;
		for (const Share::RemoteDir* dir = root; dir; dir = dir->countSubdirs() ? dir->getSubdir(0) : 0)
			levels++;
		CHECK(levels == (size_t) depth + 1);

		delete root;
		printf("  ok a list nested %d deep is followed all the way down\n", depth);
	}

	/*
	 * Deeper than that is not a collection, and the list is refused rather than
	 * followed. Both the reader and the handler stop at 128, so the assertion is
	 * on the bound and not on which of the two got there first.
	 */
	{
		const int depth = 4000;

		std::string xml = "<FileListing Version=\"1\" Base=\"/\">";
		for (int n = 0; n < depth; n++) xml += "<Directory Name=\"d\">";
		xml += "<File Name=\"bottom\" Size=\"5\" TTH=\"T\"/>";
		for (int n = 0; n < depth; n++) xml += "</Directory>";
		xml += "</FileListing>";

		bool ok = true;
		Share::RemoteDir* root = decode(xml.c_str(), ok);

		CHECK(!ok);

		size_t levels = 0;
		for (const Share::RemoteDir* dir = root; dir; dir = dir->countSubdirs() ? dir->getSubdir(0) : 0)
			levels++;
		CHECK(levels <= 128);

		/* Nothing inside the part that was refused, either. */
		CHECK(root->getSize() == 0);
		CHECK(root->getFiles() == 0);

		delete root;
		printf("  ok a list nested %d deep is refused and followed %zu levels\n",
			depth, levels);
	}

	/*
	 * And the tree itself, built by hand as deep as a list without a cap could
	 * have made it. Carrying a file's size up to the root and taking the tree
	 * apart again both used to recurse once per level.
	 */
	{
		const size_t depth = 400000;

		Share::RemoteDir* root = new Share::RemoteDir("", 0);
		Share::RemoteDir* leaf = root;
		for (size_t n = 0; n < depth; n++)
			leaf = new Share::RemoteDir("d", leaf);

		new Share::RemoteFile("bottom", 7, "T", leaf);

		CHECK(leaf->getSize() == 7);
		CHECK(root->getSize() == 7);
		CHECK(root->getFiles() == 1);

		delete root;
		printf("  ok a %zu level tree carries a size to the root and is deleted\n", depth);
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
