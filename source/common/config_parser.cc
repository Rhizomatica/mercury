/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, caisresearch.com, ieee.org>
 * ORCID: 0000-0002-2076-5831
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "common/config_parser.h"
#include <algorithm>
#include <sstream>

SimpleConfigParser::SimpleConfigParser()
{
}

SimpleConfigParser::~SimpleConfigParser()
{
}

std::string SimpleConfigParser::trim(const std::string& str)
{
	size_t start = str.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) return "";
	size_t end = str.find_last_not_of(" \t\r\n");
	return str.substr(start, end - start + 1);
}

std::string SimpleConfigParser::toLower(const std::string& str)
{
	std::string result = str;
	std::transform(result.begin(), result.end(), result.begin(), ::tolower);
	return result;
}

bool SimpleConfigParser::load(const std::string& filename)
{
	std::ifstream file(filename.c_str());
	if (!file.is_open())
	{
		return false;
	}

	std::string line;
	std::string current_section = "";

	while (std::getline(file, line))
	{
		line = trim(line);

		// Skip empty lines and comments
		if (line.empty() || line[0] == '#')
			continue;

		// Check for section headers [Section]
		if (line[0] == '[' && line[line.length()-1] == ']')
		{
			current_section = line.substr(1, line.length()-2);
			continue;
		}

		// Parse key=value pairs
		size_t eq_pos = line.find('=');
		if (eq_pos != std::string::npos)
		{
			std::string key = trim(line.substr(0, eq_pos));
			std::string value = trim(line.substr(eq_pos + 1));

			// Remove quotes if present
			if (value.length() >= 2 && value[0] == '"' && value[value.length()-1] == '"')
			{
				value = value.substr(1, value.length()-2);
			}

			// Store with section prefix if in a section
			if (!current_section.empty())
			{
				key = current_section + "." + key;
			}

			config_values[key] = value;
		}
	}

	file.close();
	return true;
}

std::string SimpleConfigParser::get(const std::string& key, const std::string& default_value)
{
	if (config_values.find(key) != config_values.end())
	{
		return config_values[key];
	}
	return default_value;
}

int SimpleConfigParser::getInt(const std::string& key, int default_value)
{
	std::string value = get(key, "");
	if (value.empty())
		return default_value;

	std::istringstream iss(value);
	int result;
	if (iss >> result)
		return result;

	return default_value;
}

bool SimpleConfigParser::getBool(const std::string& key, bool default_value)
{
	std::string value = toLower(get(key, ""));
	if (value.empty())
		return default_value;

	return (value == "true" || value == "yes" || value == "1" || value == "on");
}
