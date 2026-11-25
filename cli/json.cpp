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

namespace {

class Parser {
	public:
		explicit Parser(std::string_view text) : text(text) { }

		bool value(QuickDC::Cli::Json::Value& out, int depth = 0)
		{
			using namespace QuickDC::Cli::Json;

			space();
			if (at == text.size() || depth > 64) return false;

			const char c = text[at];
			if (c == '{')
			{
				++at;
				Object object;
				space();
				if (peek('}')) { out = Value(std::move(object)); return true; }
				while (true)
				{
					Member member;
					space();
					if (!string(member.key)) return false;
					space();
					if (!peek(':')) return false;
					if (!value(member.value, depth + 1)) return false;
					object.push_back(std::move(member));
					space();
					if (peek('}')) break;
					if (!peek(',')) return false;
				}
				out = Value(std::move(object));
				return true;
			}
			if (c == '[')
			{
				++at;
				Array array;
				space();
				if (peek(']')) { out = Value(std::move(array)); return true; }
				while (true)
				{
					Value item;
					if (!value(item, depth + 1)) return false;
					array.push_back(std::move(item));
					space();
					if (peek(']')) break;
					if (!peek(',')) return false;
				}
				out = Value(std::move(array));
				return true;
			}
			if (c == '"')
			{
				std::string scalar;
				if (!string(scalar)) return false;
				out = Value(std::move(scalar));
				return true;
			}
			if (word("true")) { out = Value(true); return true; }
			if (word("false")) { out = Value(false); return true; }
			if (word("null")) { out = Value(); return true; }

			const size_t begin = at;
			while (at < text.size() && std::string_view("+-0123456789.eE").find(text[at]) != std::string_view::npos) ++at;
			if (at == begin) return false;
			out = Value(Number{std::string(text.substr(begin, at - begin))});
			return true;
		}

		bool ended()
		{
			space();
			return at == text.size();
		}

	private:
		void space()
		{
			while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
		}

		bool peek(char c)
		{
			if (at < text.size() && text[at] == c) { ++at; return true; }
			return false;
		}

		bool word(std::string_view w)
		{
			if (text.substr(at, w.size()) != w) return false;
			at += w.size();
			return true;
		}

		bool hex(unsigned& out)
		{
			if (at + 4 > text.size()) return false;
			const auto parsed = std::from_chars(text.data() + at, text.data() + at + 4, out, 16);
			if (parsed.ptr != text.data() + at + 4) return false;
			at += 4;
			return true;
		}

		static void utf8(std::string& out, unsigned code)
		{
			if (code < 0x80) out += static_cast<char>(code);
			else if (code < 0x800)
			{
				out += static_cast<char>(0xc0 | (code >> 6));
				out += static_cast<char>(0x80 | (code & 0x3f));
			}
			else if (code < 0x10000)
			{
				out += static_cast<char>(0xe0 | (code >> 12));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
				out += static_cast<char>(0x80 | (code & 0x3f));
			}
			else
			{
				out += static_cast<char>(0xf0 | (code >> 18));
				out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
				out += static_cast<char>(0x80 | (code & 0x3f));
			}
		}

		bool string(std::string& out)
		{
			if (!peek('"')) return false;
			while (at < text.size())
			{
				const char c = text[at++];
				if (c == '"') return true;
				if (c != '\\') { out += c; continue; }
				if (at == text.size()) return false;

				switch (text[at++])
				{
					case '"':  out += '"'; break;
					case '\\': out += '\\'; break;
					case '/':  out += '/'; break;
					case 'b':  out += '\b'; break;
					case 'f':  out += '\f'; break;
					case 'n':  out += '\n'; break;
					case 'r':  out += '\r'; break;
					case 't':  out += '\t'; break;
					case 'u':
					{
						unsigned code = 0;
						if (!hex(code)) return false;
						if (code >= 0xd800 && code < 0xdc00 && word("\\u"))
						{
							unsigned low = 0;
							if (!hex(low)) return false;
							code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
						}
						utf8(out, code);
						break;
					}
					default: return false;
				}
			}
			return false;
		}

		std::string_view text;
		size_t at = 0;
};

}

std::optional<QuickDC::Cli::Json::Value> QuickDC::Cli::Json::parse(std::string_view text)
{
	Parser parser(text);
	Value out;
	if (!parser.value(out) || !parser.ended()) return std::nullopt;
	return out;
}
