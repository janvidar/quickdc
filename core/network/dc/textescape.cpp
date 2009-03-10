/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dc/textescape.h"

#define DC_TRANSLATIONS 3
static const struct
{
		const char* escaped;
		const char* unescaped;
		const size_t escaped_length;
		const size_t unescaped_length;
}
dc_text_translation_table[DC_TRANSLATIONS] =
{
	{ "&#124;",  "|", 6, 1 },
	{ "&#36;",   "$", 5, 1 },
	{ "&amp;",   "&", 5, 1 },
};

// FIXME: Make char* version of this also.
std::string DC::TextEscape::escape(const std::string& str)
{
	std::string message = str;
	size_t offset;
	while ((offset = message.find('|')) != std::string::npos) message.replace(offset, 1, "&#124;");
	while ((offset = message.find('$')) != std::string::npos) message.replace(offset, 1, "&#36;");
	while ((offset = message.find('&')) != std::string::npos) message.replace(offset, 1, "&amp;");
	
	return message;
}


char* DC::TextEscape::unescape(char* str/*_ , size_t max*/)
{
	// char* str = *str_;

	for (size_t t = 0; t < DC_TRANSLATIONS; t++)
	{
		size_t newsize = 0;
		size_t max = strlen(str);
		for (size_t i = 0; i < max; i++)
		{
			if (strncmp(&str[i], dc_text_translation_table[t].escaped,
				dc_text_translation_table[t].escaped_length) == 0)
			{
				for (size_t c = 0; c < dc_text_translation_table[t].unescaped_length; c++)
					str[newsize++] = dc_text_translation_table[t].unescaped[c];

				i += dc_text_translation_table[t].escaped_length-1;
				
			}
			else
			{
				str[newsize++] = str[i];
			}
		}
		str[newsize] = '\0';
	}
	return str;
}

