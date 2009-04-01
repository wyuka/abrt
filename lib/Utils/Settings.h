/*
    Settings.h - header file for settings functions

    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    */

#ifndef SETTINGSFUNC_H_
#define SETTINGSFUNC_H_

#include <fstream>
#include <map>
#include <set>

typedef std::map<std::string, std::string> map_settings_t;
typedef std::set<std::string> set_settings_t;

inline void load_settings(const std::string& path, map_settings_t& settings)
{
    std::ifstream fIn;
    fIn.open(path.c_str());
    if (fIn.is_open())
    {
        std::string line;
        while (!fIn.eof())
        {
            getline(fIn, line);

            int ii;
            bool is_value = false;
            bool valid = false;
            std::string key = "";
            std::string value = "";
            for (ii = 0; ii < line.length(); ii++)
            {
                if (isspace(line[ii]))
                {
                    continue;
                }
                if (line[ii] == '#')
                {
                    break;
                }
                else if (line[ii] == '=')
                {
                    is_value = true;
                }
                else if (line[ii] == '=' && is_value)
                {
                    key = "";
                    value = "";
                    break;
                }
                else if (!is_value)
                {
                    key += line[ii];
                }
                else
                {
                    valid = true;
                    value += line[ii];
                }
            }
            if (valid)
            {
                settings[key] = value;
            }
        }
        fIn.close();
    }
}

inline void save_settings(const std::string& path, const map_settings_t& settings)
{
    map_settings_t::const_iterator it;
    std::ofstream fOut;
    fOut.open(path.c_str());
    if (fOut.is_open())
    {
        fOut << "# !DO NOT EDIT THIS FILE BY HAND. IT IS GENERATED BY ABRT!" << std::endl;
        for (it = settings.begin(); it != settings.end(); it++)
        {
            fOut << it->first << " = " << it->second << std::endl << std::endl;
        }
        fOut.close();
    }
    else
    {
        throw std::string("save_settings():  Cannot write configuration file '"+path+"'.");
    }
}

inline void parse_settings(const std::string& pLine, set_settings_t& settings)
{
    std::string::size_type ii_old = 0, ii_new = 0;
    ii_new = pLine.find(",");
    while (ii_new != std::string::npos)
    {
        settings.insert(pLine.substr(ii_old, ii_new - ii_old));
        ii_old = ii_new + 1;
        ii_new = pLine.find(",",ii_old);
    }
    settings.insert(pLine.substr(ii_old));
}

#endif /* SETTINGSFUNC_H_ */