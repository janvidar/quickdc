/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace {

void quote(std::string& out, const std::string& text)
{
	out += '"';
	for (const char c : text)
	{
		switch (c)
		{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
				{
					char escaped[8];
					std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
					out += escaped;
				}
				else
				{
					out += c;
				}
		}
	}
	out += '"';
}

}

QuickDC::Cli::Json::Value::Value() = default;
QuickDC::Cli::Json::Value::Value(const Value& other) = default;
QuickDC::Cli::Json::Value::Value(Value&& other) noexcept = default;
QuickDC::Cli::Json::Value& QuickDC::Cli::Json::Value::operator=(const Value& other) = default;
QuickDC::Cli::Json::Value& QuickDC::Cli::Json::Value::operator=(Value&& other) noexcept = default;
QuickDC::Cli::Json::Value::~Value() = default;

QuickDC::Cli::Json::Value::Value(bool flag) : kind(Kind::Bool), flag(flag) { }

QuickDC::Cli::Json::Value::Value(const char* text) : kind(Kind::String), scalar(text ? text : "") { }

QuickDC::Cli::Json::Value::Value(std::string text) : kind(Kind::String), scalar(std::move(text)) { }

QuickDC::Cli::Json::Value::Value(Number number) : kind(Kind::Number), scalar(std::move(number.text)) { }

QuickDC::Cli::Json::Value::Value(double number)
{
	/* JSON has no spelling for either. */
	if (!std::isfinite(number)) return;

	char buffer[32];
	const auto written = std::to_chars(buffer, buffer + sizeof(buffer), number);
	kind = Kind::Number;
	scalar.assign(buffer, written.ptr);
}

QuickDC::Cli::Json::Value::Value(Array array) : kind(Kind::Array), items(std::move(array)) { }

QuickDC::Cli::Json::Value::Value(Object object) : kind(Kind::Object), members(std::move(object)) { }

const QuickDC::Cli::Json::Value* QuickDC::Cli::Json::Value::find(std::string_view path) const
{
	const Value* at = this;

	while (at)
	{
		const size_t dot = path.find('.');
		const std::string_view step = path.substr(0, dot);

		const Value* next = nullptr;

		if (at->kind == Kind::Object)
		{
			for (const Member& member : at->members)
				if (member.key == step) { next = &member.value; break; }
		}
		else if (at->kind == Kind::Array)
		{
			size_t index = 0;
			const auto parsed = std::from_chars(step.data(), step.data() + step.size(), index);
			if (parsed.ec == std::errc() && parsed.ptr == step.data() + step.size()
			    && index < at->items.size())
				next = &at->items[index];
		}

		at = next;
		if (dot == std::string_view::npos) break;
		path.remove_prefix(dot + 1);
	}

	return at;
}

void QuickDC::Cli::Json::Value::findAll(std::string_view path, std::vector<const Value*>& out) const
{
	const size_t dot = path.find('.');
	const std::string_view step = path.substr(0, dot);
	const std::string_view rest = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);

	if (step != "*" || kind != Kind::Array)
	{
		const Value* next = find(step);
		if (!next) return;
		if (dot == std::string_view::npos) out.push_back(next);
		else next->findAll(rest, out);
		return;
	}

	for (const Value& item : items)
	{
		if (dot == std::string_view::npos) out.push_back(&item);
		else item.findAll(rest, out);
	}
}

std::string QuickDC::Cli::Json::Value::text() const
{
	if (kind == Kind::String) return scalar;
	return serialized();
}

std::string QuickDC::Cli::Json::Value::serialized() const
{
	std::string out;
	write(out);
	return out;
}

void QuickDC::Cli::Json::Value::write(std::string& out) const
{
	switch (kind)
	{
		case Kind::Null:   out += "null"; break;
		case Kind::Bool:   out += flag ? "true" : "false"; break;
		case Kind::Number: out += scalar; break;
		case Kind::String: quote(out, scalar); break;

		case Kind::Array:
			out += '[';
			for (size_t n = 0; n < items.size(); ++n)
			{
				if (n) out += ',';
				items[n].write(out);
			}
			out += ']';
			break;

		case Kind::Object:
			out += '{';
			for (size_t n = 0; n < members.size(); ++n)
			{
				if (n) out += ',';
				quote(out, members[n].key);
				out += ':';
				members[n].value.write(out);
			}
			out += '}';
			break;
	}
}
