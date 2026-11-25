/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "human.h"

#include <algorithm>
#include <vector>

namespace Json = QuickDC::Cli::Json;

namespace {

std::string shown(const Json::Value& value)
{
	const std::string text = value.text();
	if (!value.isString()) return text;

	const bool plain = !text.empty() && std::none_of(text.begin(), text.end(), [](char c) {
		return c == ' ' || c == '"' || c == '\t' || c == '\n';
	});
	return plain ? text : value.serialized();
}

bool scalars(const Json::Array& items)
{
	return std::none_of(items.begin(), items.end(),
	                    [](const Json::Value& item) { return item.isArray() || item.isObject(); });
}

void flatten(const std::string& path, const Json::Value& value, std::string& line,
             std::vector<std::string>& more)
{
	if (value.isObject())
	{
		for (const Json::Member& member : value.fields())
			flatten(path.empty() ? member.key : path + "." + member.key, member.value, line, more);
		return;
	}

	if (value.isArray())
	{
		const Json::Array& items = value.elements();
		if (items.empty()) return;

		if (scalars(items))
		{
			std::string joined;
			for (const Json::Value& item : items) joined += (joined.empty() ? "" : ",") + shown(item);
			line += " " + path + "=[" + joined + "]";
			return;
		}

		for (size_t n = 0; n < items.size(); ++n)
		{
			std::string item = "    " + path + "." + std::to_string(n) + ":";
			std::vector<std::string> nested;
			flatten(std::string(), items[n], item, nested);
			more.push_back(std::move(item));
			for (std::string& deeper : nested) more.push_back("  " + deeper);
		}
		return;
	}

	if (value.isString() && value.text().empty()) return;
	line += " " + path + "=" + shown(value);
}

}

std::string QuickDC::Cli::readable(const Json::Object& event)
{
	std::string line;
	std::vector<std::string> more;

	for (const Json::Member& member : event)
	{
		if (member.key == "core") line = "[" + member.value.text() + "]" + line;
		else if (member.key == "event") line += " " + member.value.text();
		else flatten(member.key, member.value, line, more);
	}

	for (const std::string& extra : more) line += "\n" + extra;
	return line;
}
