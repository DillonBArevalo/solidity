/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file MiniMoustache.cpp
 * @author Chris <chis@ethereum.org>
 * @date 2017
 *
 * Moustache-like templates.
 */

#include <libdevcore/MiniMoustache.h>

#include <libdevcore/Assertions.h>

#include <boost/regex.hpp>

using namespace std;
using namespace dev;

MiniMoustache::MiniMoustache(string const& _template):
m_template(_template)
{
}

MiniMoustache& MiniMoustache::operator ()(string const& _parameter, string const& _value)
{
	assertThrow(
		m_parameters.count(_parameter) == 0,
		MiniMoustacheError,
		_parameter + " already set."
	);
	assertThrow(
		m_listParameters.count(_parameter) == 0,
		MiniMoustacheError,
		_parameter + " already set as list parameter."
	);
	m_parameters[_parameter] = _value;

	return *this;
}

MiniMoustache& MiniMoustache::operator ()(
	string const& _listParameter,
	vector<map<string, string>> const& _values
)
{
	assertThrow(
		m_listParameters.count(_listParameter) == 0,
		MiniMoustacheError,
		_listParameter + " already set."
	);
	assertThrow(
		m_parameters.count(_listParameter) == 0,
		MiniMoustacheError,
		_listParameter + " already set as value parameter."
	);
	m_listParameters[_listParameter] = _values;

	return *this;
}

string MiniMoustache::render() const
{
	return replace(m_template, m_parameters, m_listParameters);
}

string MiniMoustache::replace(
	string const& _template,
	StringMap const& _parameters,
	map<string, vector<StringMap>> const& _listParameters
)
{
	using namespace boost;
	static regex listOrTag("<([^#/>]+)>|<#([^>]+)>(.*?)</\\2>");
	return regex_replace(_template, listOrTag, [&](match_results<string::const_iterator> _match) -> string
	{
		string tagName(_match[1]);
		if (!tagName.empty())
		{
			assertThrow(_parameters.count(tagName), MiniMoustacheError, "Tag " + tagName + " not found.");
			return _parameters.at(tagName);
		}
		else
		{
			string listName(_match[2]);
			string templ(_match[3]);
			assertThrow(!listName.empty(), MiniMoustacheError, "");
			assertThrow(
				_listParameters.count(listName),
				MiniMoustacheError, "List parameter " + listName + " not set."
			);
			string replacement;
			for (auto const& parameters: _listParameters.at(listName))
				replacement += replace(templ, joinMaps(_parameters, parameters));
			return replacement;
		}
	});
}

MiniMoustache::StringMap MiniMoustache::joinMaps(
	MiniMoustache::StringMap const& _a,
	MiniMoustache::StringMap const& _b
)
{
	MiniMoustache::StringMap ret = _a;
	for (auto const& x: _b)
		assertThrow(
			ret.insert(x).second,
			MiniMoustacheError,
			"Parameter collision"
		);
	return ret;
}

