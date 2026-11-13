// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
	A small subset of TOML, enough for pack.toml (the subset is documented
	in docs/shaderpacks.md):

	    # comment
	    key = "string" | 'string' | 123 | 1.5 | true | false | [ "a", 2, ... ]
	    dotted.key = ...
	    [table]
	    [table.subtable]

	A list may span several lines. Arrays hold scalars only. Inline tables,
	arrays of tables, multi-line strings and quoted keys are rejected with an
	error that names them, and so is a key given twice.
*/
#include <map>
#include <string>
#include <vector>

namespace prisma::toml {

struct Value {
	enum Type { String, Number, Bool, Array } type = String;
	std::string str;
	double num = 0;
	bool boolean = false;
	std::vector<Value> items;
};

// Keys are flattened with dots: "options.fog_density.default".
struct Document {
	std::map<std::string, Value> values;
	std::vector<std::string> order; // keys in file order
	// [table] headers and keys, in file order: a table declared without keys
	// (all its values left to their defaults) still exists
	std::vector<std::string> names;

	const Value *get(const std::string &key) const
	{
		auto it = values.find(key);
		return it == values.end() ? nullptr : &it->second;
	}
};

// Returns false and fills `error` ("line N: ...") on a syntax error.
bool parse(const std::string &text, Document &doc, std::string &error);

} // namespace prisma::toml
