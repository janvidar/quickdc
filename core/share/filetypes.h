/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_FILETYPES_H
#define HAVE_QUICKDC_FILETYPES_H

enum DCFileType { 
	FAny = 1,
	FAudio = 2,
	FCompressed = 4,
	FDocument = 8,
	FExecutable = 16,
	FPicture = 32,
	FVideo = 64,
	FDir = 128,
	FHash = 256
};

static const int typeLookup[] = {
	0, 
	FAny,          // 1
	FAudio,        // 2
	FCompressed,   // 3
	FDocument,     // 4
	FExecutable,   // 5
	FPicture,      // 6
	FVideo,        // 7
	FDir,          // 8
	FHash          // 9
};

#define KNOWN_EXTENSIONS 78
static const struct {
	const char* ext;
	int ftype;
} known_ext[KNOWN_EXTENSIONS] = {
	/* audio */
	{"mp2",  FAudio},
	{"mp3",  FAudio},
	{"ogg",  FAudio},
	{"au",   FAudio},
	{"wav",  FAudio},
	{"wma",  FAudio},
	{"aif",  FAudio},
	{"aiff", FAudio},
	{"mod",  FAudio}, // soundtracker
	{"smp",  FAudio},
	{"snd",  FAudio},
	{"mid",  FAudio}, // midi
	{"xm",   FAudio}, // fasttracker
	{"s3m",  FAudio}, // scream tracker
	{"it",   FAudio}, // impulse tracker
	
	/* compressed & archive */
	{"zip",  FCompressed},
	{"gz",   FCompressed},
	{"bz2",  FCompressed},
	{"tar",  FCompressed},
	{"rar",  FCompressed},
	{"arj",  FCompressed},
	{"tgz",  FCompressed},
	{"lzh",  FCompressed},
	{"lha",  FCompressed},
	{"ace",  FCompressed},
	{"msi",  FCompressed}, // Microsoft installer
	{"cab",  FCompressed}, // Microsoft package
	{"deb",  FCompressed}, // Debian package
	{"rpm",  FCompressed}, // Redhat package
	{"dmg",  FCompressed}, // Apple disk image
	{"iso",  FCompressed}, // CD/DVD image
	
	/* document type */
	{"doc",  FDocument},
	{"txt",  FDocument},
	{"pdf",  FDocument},
	{"xls",  FDocument}, // excel
	{"ps",   FDocument}, // post script
	{"sxw",  FDocument}, // openoffice/staroffice writer
	{"sxc",  FDocument}, // openoffice/staroffice calc
	{"htm",  FDocument},
	{"html", FDocument},
	{"xml",  FDocument},
	{"xhtml",FDocument},
	{"nfo",  FDocument},
	{"wri",  FDocument},
	{"odt",  FDocument}, // Open Document Format (text)
	{"ods",  FDocument}, // Open Document Format (spreadsheet)
	
	/* executable type -- very Microsoft  */
	{"exe",  FExecutable},
	{"com",  FExecutable},
	{"bat",  FExecutable},
	{"dll",  FExecutable},
	{"bin",  FExecutable},

	/* picture/image type */
	{"gif",  FPicture},
	{"jpg",  FPicture},
	{"jpeg", FPicture},
	{"bmp",  FPicture},
	{"xpm",  FPicture},
	{"pcx",  FPicture},
	{"png",  FPicture},
	{"psd",  FPicture},
	{"wmf",  FPicture},
	{"svg",  FPicture},
	{"svgz", FPicture},
	{"apng", FPicture},
	{"tiff", FPicture},
	{"tif",  FPicture},

	/* video type */
	{"mpg",  FVideo},
	{"mpe",  FVideo},
	{"mpeg", FVideo},
	{"avi",  FVideo},
	{"divx", FVideo},
	{"mov",  FVideo},
	{"rm",   FVideo},
	{"asf",  FVideo},
	{"wmv",  FVideo},
	{"qt",   FVideo},
	{"3gp",  FVideo},
	{"mp4",  FVideo},
	{"sfv",  FVideo},
};


#endif // HAVE_QUICKDC_FILETYPES_H
