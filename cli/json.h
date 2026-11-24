/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_JSON_H
#define HAVE_QUICKDC_CLI_JSON_H

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace QuickDC::Cli {
namespace Json {

struct Member;
class Value;

using Array = std::vector<Value>;

/** Members in the order they were added, which is the order they are written in. */
using Object = std::vector<Member>;

/** A number, kept as the text it is written as, so matching compares what is shown. */
struct Number {
	std::string text;
};

/*
 * Every constructor and the destructor are out of line, because Member holds a
 * Value and is incomplete here: anything inline would make the compiler destroy
 * an Object, which it cannot do without knowing what a Member is.
 */
class Value {
	public:
		Value();
		Value(bool flag);
		Value(const char* text);
		Value(std::string text);
		Value(double number);
		Value(Number number);
		Value(Array array);
		Value(Object object);

		template<std::integral T>
			requires (!std::same_as<T, bool>)
		Value(T number) : Value(Number{std::to_string(number)}) { }

		Value(const Value& other);
		Value(Value&& other) noexcept;
		Value& operator=(const Value& other);
		Value& operator=(Value&& other) noexcept;
		~Value();

		/** The member at 'path', dot separated, an array indexed by number; or null. */
		const Value* find(std::string_view path) const;

		/** What a field=value test compares with: text unquoted, anything else as written. */
		std::string text() const;

		std::string serialized() const;

	private:
		enum class Kind { Null, Bool, Number, String, Array, Object };

		void write(std::string& out) const;

		Kind kind = Kind::Null;
		bool flag = false;
		std::string scalar;
		Array items;
		Object members;
};

struct Member {
	std::string key;
	Value value;
};

}
}

#endif // HAVE_QUICKDC_CLI_JSON_H
