// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/toml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace prisma::toml {

namespace {

struct Cursor {
	const std::string &s;
	size_t i = 0;

	void skip_spaces()
	{
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
			i++;
	}
	bool at_end_of_line()
	{
		skip_spaces();
		return i >= s.size() || s[i] == '#';
	}
};

bool is_key_char(char c)
{
	return std::isalnum((unsigned char)c) || c == '_' || c == '-';
}

bool parse_key(Cursor &c, std::string &key)
{
	c.skip_spaces();
	key.clear();
	while (c.i < c.s.size() && (is_key_char(c.s[c.i]) || c.s[c.i] == '.'))
		key += c.s[c.i++];
	return !key.empty();
}

bool parse_value(Cursor &c, Value &v, std::string &error)
{
	c.skip_spaces();
	if (c.i >= c.s.size()) {
		error = "missing value";
		return false;
	}
	char ch = c.s[c.i];
	if (ch == '{') {
		error = "inline tables ({...}) are not supported in pack.toml; use a [table] section";
		return false;
	}
	if (c.s.compare(c.i, 3, "\"\"\"") == 0 || c.s.compare(c.i, 3, "'''") == 0) {
		error = "multi-line strings are not supported in pack.toml";
		return false;
	}
	if (ch == '"' || ch == '\'') {
		v.type = Value::String;
		c.i++;
		while (c.i < c.s.size() && c.s[c.i] != ch) {
			if (ch == '"' && c.s[c.i] == '\\' && c.i + 1 < c.s.size()) {
				char e = c.s[++c.i];
				v.str += e == 'n' ? '\n' : e == 't' ? '\t' : e;
			} else {
				v.str += c.s[c.i];
			}
			c.i++;
		}
		if (c.i >= c.s.size()) {
			error = "unclosed text";
			return false;
		}
		c.i++;
		return true;
	}
	if (ch == '[') {
		v.type = Value::Array;
		c.i++;
		for (;;) {
			c.skip_spaces();
			if (c.i < c.s.size() && c.s[c.i] == ']') {
				c.i++;
				return true;
			}
			Value item;
			if (!parse_value(c, item, error))
				return false;
			if (item.type == Value::Array) {
				error = "lists inside lists are not allowed";
				return false;
			}
			v.items.push_back(item);
			c.skip_spaces();
			if (c.i < c.s.size() && c.s[c.i] == ',')
				c.i++;
			else if (c.i >= c.s.size() || c.s[c.i] != ']') {
				error = "missing ']' or ','";
				return false;
			}
		}
	}
	if (c.s.compare(c.i, 4, "true") == 0 || c.s.compare(c.i, 5, "false") == 0) {
		v.type = Value::Bool;
		v.boolean = c.s[c.i] == 't';
		c.i += v.boolean ? 4 : 5;
		return true;
	}
	const char *start = c.s.c_str() + c.i;
	char *end = nullptr;
	v.num = std::strtod(start, &end);
	if (end == start) {
		error = "unrecognized value";
		return false;
	}
	v.type = Value::Number;
	v.str.assign(start, (const char *)end);
	c.i += end - start;
	return true;
}

// Calls f(index, char) for each character of `text` outside strings
template <typename F>
void outside_strings(const std::string &text, F &&f)
{
	char quote = 0;
	for (size_t i = 0; i < text.size(); i++) {
		char ch = text[i];
		if (quote) {
			if (ch == '\\' && quote == '"')
				i++;
			else if (ch == quote)
				quote = 0;
		} else if (ch == '"' || ch == '\'') {
			quote = ch;
		} else if (!f(i, ch)) {
			return;
		}
	}
}

// `line` without its comment: from a '#' that is not inside a string
std::string without_comment(const std::string &line)
{
	size_t end = line.size();
	outside_strings(line, [&](size_t i, char ch) {
		if (ch != '#')
			return true;
		end = i;
		return false;
	});
	return line.substr(0, end);
}

// Brackets opened and not yet closed in `text`, outside strings
int open_brackets(const std::string &text)
{
	int depth = 0;
	outside_strings(text, [&](size_t, char ch) {
		depth += ch == '[' ? 1 : ch == ']' ? -1 : 0;
		return true;
	});
	return depth;
}

} // namespace

bool parse(const std::string &text, Document &doc, std::string &error)
{
	std::istringstream in(text);
	std::string line, table;
	int number = 0;
	auto fail = [&](const std::string &what) {
		error = "line " + std::to_string(number) + ": " + what;
		return false;
	};
	auto next_line = [&](std::string &out) {
		if (!std::getline(in, out))
			return false;
		if (!out.empty() && out.back() == '\r')
			out.pop_back();
		return true;
	};

	while (next_line(line)) {
		number++;
		Cursor c{line};
		if (c.at_end_of_line())
			continue;
		if (line.compare(c.i, 2, "[[") == 0)
			return fail("arrays of tables ([[...]]) are not supported in pack.toml");
		if (line[c.i] == '[') {
			c.i++;
			if (!parse_key(c, table))
				return fail("empty table name");
			c.skip_spaces();
			if (c.i >= line.size() || line[c.i] != ']')
				return fail("missing ']'");
			c.i++;
			if (!c.at_end_of_line())
				return fail("extra text after the table");
			if (std::find(doc.names.begin(), doc.names.end(), table) != doc.names.end())
				return fail("duplicate table [" + table + "]");
			doc.names.push_back(table);
			continue;
		}
		if (line[c.i] == '"' || line[c.i] == '\'')
			return fail("quoted keys are not supported in pack.toml");
		std::string key;
		if (!parse_key(c, key))
			return fail("expected a key");
		c.skip_spaces();
		if (c.i >= line.size() || line[c.i] != '=')
			return fail("missing '=' after '" + key + "'");
		c.i++;
		// A list may go on over the next lines, until its ']'
		const int first = number;
		std::string joined = without_comment(line);
		std::string more;
		while (open_brackets(joined.substr(c.i)) > 0 && next_line(more)) {
			number++;
			joined += " " + without_comment(more);
		}
		const int last = number;
		number = first; // errors point at the line where the key is
		Cursor value{joined, c.i};
		Value v;
		std::string err;
		if (!parse_value(value, v, err))
			return fail(err);
		if (!value.at_end_of_line())
			return fail("extra text after the value");
		std::string full = table.empty() ? key : table + "." + key;
		if (doc.values.count(full))
			return fail("duplicate key '" + full + "'");
		doc.order.push_back(full);
		doc.names.push_back(full);
		doc.values[full] = v;
		number = last;
	}
	return true;
}

} // namespace prisma::toml
