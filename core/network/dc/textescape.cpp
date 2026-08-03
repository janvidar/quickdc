/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dc/textescape.h"
#include <string_view>

#define DC_TRANSLATIONS 3
static const struct
{
		std::string_view escaped;
		std::string_view unescaped;
}
dc_text_translation_table[DC_TRANSLATIONS] =
{
	{ "&#124;",  "|" },
	{ "&#36;",   "$" },
	{ "&amp;",   "&" },
};

// FIXME: Make char* version of this also.
/*
 * One pass, because a replacement may contain a character that is itself
 * escaped: replacing each in turn from the start of the string meant the
 * ampersand pass kept finding the ampersand it had just written, and the
 * ampersands already written by the two passes before it.
 */
std::string DC::TextEscape::escape(const std::string& str)
{
	std::string message;
	message.reserve(str.size());

	for (size_t n = 0; n < str.size(); n++)
	{
		switch (str[n])
		{
			case '&': message += "&amp;";  break;
			case '|': message += "&#124;"; break;
			case '$': message += "&#36;";  break;
			default:  message += str[n];   break;
		}
	}

	return message;
}


char* DC::TextEscape::unescape(char* str/*_ , size_t max*/)
{
	// char* str = *str_;

	for (size_t t = 0; t < DC_TRANSLATIONS; t++)
	{
		const std::string_view escaped = dc_text_translation_table[t].escaped;
		const std::string_view unescaped = dc_text_translation_table[t].unescaped;

		size_t newsize = 0;
		size_t max = strlen(str);
		for (size_t i = 0; i < max; i++)
		{
			/* Only what is left of the string is a candidate: a sequence cannot
			   match on fewer characters than it is made of. */
			if (std::string_view(&str[i], max - i).starts_with(escaped))
			{
				for (size_t c = 0; c < unescaped.size(); c++)
					str[newsize++] = unescaped[c];

				i += escaped.size()-1;
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

