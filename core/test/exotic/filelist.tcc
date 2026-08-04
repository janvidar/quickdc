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

/* Sizes aggregate up the tree. */
EXO_TEST(filelist_sizes_aggregate_up_the_tree,
{
		bool ok = false;
		Share::RemoteDir* root = decode(
			LIST("<Directory Name=\"a\">"
			     "<File Name=\"x\" Size=\"100\" TTH=\"T\"/>"
			     "<Directory Name=\"b\">"
			     "<File Name=\"y\" Size=\"20\" TTH=\"U\"/>"
			     "</Directory>"
			     "</Directory>"), ok);

		EXO_ASSERT(root->getSize() == 120);
		EXO_ASSERT(root->getFiles() == 2);

		const Share::RemoteDir* a = root->getSubdir(0);
		EXO_ASSERT(a != 0);
		if (a)
		{
			EXO_ASSERT(a->getSize() == 120);
			EXO_ASSERT(a->getFiles() == 2);

			const Share::RemoteDir* b = a->getSubdir(0);
			EXO_ASSERT(b != 0);
			if (b)
			{
				EXO_ASSERT(b->getSize() == 20);
				EXO_ASSERT(b->getFiles() == 1);
			}
		}
		delete root;
	
	return 1;
});

/* Deleting the root has to account for every file and directory in it. */
EXO_TEST(filelist_decoding_and_deleting_balances,
{
	/* TCCIFY: reports failure or a skip its own way. Convert by hand
	   to EXO_FAIL(msg) / EXO_SKIP(reason) - this test cannot fail
	   as it stands. */

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
			EXO_ASSERT(root->getFiles() == 3);
			delete root;
		}

		if (live != before)
		{
			printf("FAIL decoding leaks: %zu allocations over 20 lists\n", live - before);
			failures++;
		}
		else
		{
		}
	
	return 1;
});

/*
	 * Nesting. A list arrives bzip2 compressed, so a few kilobytes of it is as
	 * many levels as the peer cared to write, and a level is a directory that is
	 * held on to and later walked.
	 *
	 * Ordinary depth is followed as before.
	 */
EXO_TEST(filelist_a_list_nested_deep_is_followed_all_the_way_down,
{
		const int depth = 100;

		std::string xml = "<FileListing Version=\"1\" Base=\"/\">";
		for (int n = 0; n < depth; n++) xml += "<Directory Name=\"d\">";
		xml += "<File Name=\"bottom\" Size=\"5\" TTH=\"T\"/>";
		for (int n = 0; n < depth; n++) xml += "</Directory>";
		xml += "</FileListing>";

		bool ok = false;
		Share::RemoteDir* root = decode(xml.c_str(), ok);

		EXO_ASSERT(ok);
		EXO_ASSERT(root->getSize() == 5);

		size_t levels = 0;
		for (const Share::RemoteDir* dir = root; dir; dir = dir->countSubdirs() ? dir->getSubdir(0) : 0)
			levels++;
		EXO_ASSERT(levels == (size_t) depth + 1);

		delete root;
	
	return 1;
});

/*
	 * Deeper than that is not a collection, and the list is refused rather than
	 * followed. Both the reader and the handler stop at 128, so the assertion is
	 * on the bound and not on which of the two got there first.
	 */
EXO_TEST(filelist_a_list_nested_deep_is_refused_and_followed_u_levels,
{
		const int depth = 4000;

		std::string xml = "<FileListing Version=\"1\" Base=\"/\">";
		for (int n = 0; n < depth; n++) xml += "<Directory Name=\"d\">";
		xml += "<File Name=\"bottom\" Size=\"5\" TTH=\"T\"/>";
		for (int n = 0; n < depth; n++) xml += "</Directory>";
		xml += "</FileListing>";

		bool ok = true;
		Share::RemoteDir* root = decode(xml.c_str(), ok);

		EXO_ASSERT(!ok);

		size_t levels = 0;
		for (const Share::RemoteDir* dir = root; dir; dir = dir->countSubdirs() ? dir->getSubdir(0) : 0)
			levels++;
		EXO_ASSERT(levels <= 128);

		/* Nothing inside the part that was refused, either. */
		EXO_ASSERT(root->getSize() == 0);
		EXO_ASSERT(root->getFiles() == 0);

		delete root;
	
	return 1;
});

/*
	 * And the tree itself, built by hand as deep as a list without a cap could
	 * have made it. Carrying a file's size up to the root and taking the tree
	 * apart again both used to recurse once per level.
	 */
EXO_TEST(filelist_a_u_level_tree_carries_a_size_to_the_root_and,
{
		const size_t depth = 400000;

		Share::RemoteDir* root = new Share::RemoteDir("", 0);
		Share::RemoteDir* leaf = root;
		for (size_t n = 0; n < depth; n++)
			leaf = new Share::RemoteDir("d", leaf);

		new Share::RemoteFile("bottom", 7, "T", leaf);

		EXO_ASSERT(leaf->getSize() == 7);
		EXO_ASSERT(root->getSize() == 7);
		EXO_ASSERT(root->getFiles() == 1);

		delete root;
	
	return 1;
});
