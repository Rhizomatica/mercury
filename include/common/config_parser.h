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

#ifndef CONFIG_PARSER_H_
#define CONFIG_PARSER_H_

#include <string>
#include <fstream>
#include <map>

class SimpleConfigParser
{
public:
	SimpleConfigParser();
	~SimpleConfigParser();

	bool load(const std::string& filename);
	std::string get(const std::string& key, const std::string& default_value = "");
	int getInt(const std::string& key, int default_value = 0);
	bool getBool(const std::string& key, bool default_value = false);

private:
	std::map<std::string, std::string> config_values;
	std::string trim(const std::string& str);
	std::string toLower(const std::string& str);
};

#endif
