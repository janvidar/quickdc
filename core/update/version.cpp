/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "update/version.h"

#include <charconv>
#include <tuple>

namespace {

/** A decimal number at the front of 'text', consumed; nothing for none or one that overflows. */
template<typename T>
std::optional<T> number(std::string_view& text)
{
	T value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc() || end == text.data()) return std::nullopt;
	text.remove_prefix(static_cast<size_t>(end - text.data()));
	return value;
}

bool takes(std::string_view& text, std::string_view prefix)
{
	if (text.substr(0, prefix.size()) != prefix) return false;
	text.remove_prefix(prefix.size());
	return true;
}

}

std::optional<QuickDC::Update::Version> QuickDC::Update::parseVersion(std::string_view text)
{
	Version out;
	const auto major = number<unsigned>(text);
	if (!major || !takes(text, ".")) return std::nullopt;
	const auto minor = number<unsigned>(text);
	if (!minor || !takes(text, ".")) return std::nullopt;
	const auto patch = number<unsigned>(text);
	if (!patch) return std::nullopt;
	out.major = *major;
	out.minor = *minor;
	out.patch = *patch;

	if (takes(text, "~nightly"))
	{
		const std::string_view digits = text.substr(0, 8);
		if (digits.size() != 8 || digits.find_first_not_of("0123456789") != std::string_view::npos)
			return std::nullopt;
		std::string_view at = digits;
		out.nightly = true;
		out.date = *number<uint32_t>(at);
		text.remove_prefix(8);
		if (!takes(text, ".") || text.empty() || text.find('+') != std::string_view::npos) return std::nullopt;
		out.commit = std::string(text);
		return out;
	}

	if (takes(text, "+"))
	{
		if (text.empty()) return std::nullopt;
		out.local = true;
		out.commit = std::string(text);
		return out;
	}

	if (!text.empty()) return std::nullopt;
	return out;
}

bool QuickDC::Update::isNewer(const Version& offered, const Version& running)
{
	const auto number = [](const Version& v) { return std::tie(v.major, v.minor, v.patch); };
	if (number(offered) != number(running)) return number(offered) > number(running);

	/* The same number: a release after its nightlies, and nightlies by their day. */
	if (offered.nightly != running.nightly) return !offered.nightly;
	if (offered.nightly) return offered.date > running.date;

	return false;
}
