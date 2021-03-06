/**
 * Copyright © 2019 IBM Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "pel.hpp"

#include "bcd_time.hpp"
#include "extended_user_header.hpp"
#include "failing_mtms.hpp"
#include "json_utils.hpp"
#include "log_id.hpp"
#include "pel_rules.hpp"
#include "pel_values.hpp"
#include "section_factory.hpp"
#include "src.hpp"
#include "stream.hpp"
#include "user_data_formats.hpp"

#include <iostream>
#include <phosphor-logging/log.hpp>

namespace openpower
{
namespace pels
{
namespace message = openpower::pels::message;
namespace pv = openpower::pels::pel_values;

constexpr auto unknownValue = "Unknown";

PEL::PEL(const message::Entry& entry, uint32_t obmcLogID, uint64_t timestamp,
         phosphor::logging::Entry::Level severity,
         const AdditionalData& additionalData,
         const DataInterfaceBase& dataIface)
{
    _ph = std::make_unique<PrivateHeader>(entry.componentID, obmcLogID,
                                          timestamp);
    _uh = std::make_unique<UserHeader>(entry, severity);

    auto src = std::make_unique<SRC>(entry, additionalData, dataIface);

    auto euh = std::make_unique<ExtendedUserHeader>(dataIface, entry, *src);

    _optionalSections.push_back(std::move(src));
    _optionalSections.push_back(std::move(euh));

    auto mtms = std::make_unique<FailingMTMS>(dataIface);
    _optionalSections.push_back(std::move(mtms));

    auto ud = util::makeSysInfoUserDataSection(additionalData, dataIface);
    _optionalSections.push_back(std::move(ud));

    if (!additionalData.empty())
    {
        ud = util::makeADUserDataSection(additionalData);

        // To be safe, check there isn't too much data
        if (size() + ud->header().size <= _maxPELSize)
        {
            _optionalSections.push_back(std::move(ud));
        }
    }

    _ph->setSectionCount(2 + _optionalSections.size());

    checkRulesAndFix();
}

PEL::PEL(std::vector<uint8_t>& data) : PEL(data, 0)
{
}

PEL::PEL(std::vector<uint8_t>& data, uint32_t obmcLogID)
{
    populateFromRawData(data, obmcLogID);
}

void PEL::populateFromRawData(std::vector<uint8_t>& data, uint32_t obmcLogID)
{
    Stream pelData{data};
    _ph = std::make_unique<PrivateHeader>(pelData);
    if (obmcLogID != 0)
    {
        _ph->setOBMCLogID(obmcLogID);
    }

    _uh = std::make_unique<UserHeader>(pelData);

    // Use the section factory to create the rest of the objects
    for (size_t i = 2; i < _ph->sectionCount(); i++)
    {
        auto section = section_factory::create(pelData);
        _optionalSections.push_back(std::move(section));
    }
}

bool PEL::valid() const
{
    bool valid = _ph->valid();

    if (valid)
    {
        valid = _uh->valid();
    }

    if (valid)
    {
        if (!std::all_of(_optionalSections.begin(), _optionalSections.end(),
                         [](const auto& section) { return section->valid(); }))
        {
            valid = false;
        }
    }

    return valid;
}

void PEL::setCommitTime()
{
    auto now = std::chrono::system_clock::now();
    _ph->setCommitTimestamp(getBCDTime(now));
}

void PEL::assignID()
{
    _ph->setID(generatePELID());
}

void PEL::flatten(std::vector<uint8_t>& pelBuffer) const
{
    Stream pelData{pelBuffer};

    if (!valid())
    {
        using namespace phosphor::logging;
        log<level::WARNING>("Unflattening an invalid PEL");
    }

    _ph->flatten(pelData);
    _uh->flatten(pelData);

    for (auto& section : _optionalSections)
    {
        section->flatten(pelData);
    }
}

std::vector<uint8_t> PEL::data() const
{
    std::vector<uint8_t> pelData;
    flatten(pelData);
    return pelData;
}

size_t PEL::size() const
{
    size_t size = 0;

    if (_ph)
    {
        size += _ph->header().size;
    }

    if (_uh)
    {
        size += _uh->header().size;
    }

    for (const auto& section : _optionalSections)
    {
        size += section->header().size;
    }

    return size;
}

std::optional<SRC*> PEL::primarySRC() const
{
    auto src = std::find_if(
        _optionalSections.begin(), _optionalSections.end(), [](auto& section) {
            return section->header().id ==
                   static_cast<uint16_t>(SectionID::primarySRC);
        });
    if (src != _optionalSections.end())
    {
        return static_cast<SRC*>(src->get());
    }

    return std::nullopt;
}

void PEL::checkRulesAndFix()
{
    auto [actionFlags, eventType] =
        pel_rules::check(_uh->actionFlags(), _uh->eventType(), _uh->severity());

    _uh->setActionFlags(actionFlags);
    _uh->setEventType(eventType);
}

void PEL::printSectionInJSON(const Section& section, std::string& buf,
                             std::map<uint16_t, size_t>& pluralSections,
                             message::Registry& registry) const
{
    char tmpB[5];
    uint8_t id[] = {static_cast<uint8_t>(section.header().id >> 8),
                    static_cast<uint8_t>(section.header().id)};
    sprintf(tmpB, "%c%c", id[0], id[1]);
    std::string sectionID(tmpB);
    std::string sectionName = pv::sectionTitles.count(sectionID)
                                  ? pv::sectionTitles.at(sectionID)
                                  : "Unknown Section";

    // Add a count if there are multiple of this type of section
    auto count = pluralSections.find(section.header().id);
    if (count != pluralSections.end())
    {
        sectionName += " " + std::to_string(count->second);
        count->second++;
    }

    if (section.valid())
    {
        auto json = (sectionID == "PS" || sectionID == "SS")
                        ? section.getJSON(registry)
                        : section.getJSON();
        if (json)
        {
            buf += "\"" + sectionName + "\": {\n";
            buf += *json + "\n},\n";
        }
        else
        {
            buf += "\"" + sectionName + "\": [\n";
            std::vector<uint8_t> data;
            Stream s{data};
            section.flatten(s);
            std::string dstr = dumpHex(std::data(data), data.size());
            buf += dstr + "],\n";
        }
    }
    else
    {
        buf += "\n\"Invalid Section\": [\n    \"invalid\"\n],\n";
    }
}

std::map<uint16_t, size_t> PEL::getPluralSections() const
{
    std::map<uint16_t, size_t> sectionCounts;

    for (const auto& section : optionalSections())
    {
        if (sectionCounts.find(section->header().id) == sectionCounts.end())
        {
            sectionCounts[section->header().id] = 1;
        }
        else
        {
            sectionCounts[section->header().id]++;
        }
    }

    std::map<uint16_t, size_t> sections;
    for (const auto& [id, count] : sectionCounts)
    {
        if (count > 1)
        {
            // Start with 0 here and printSectionInJSON()
            // will increment it as it goes.
            sections.emplace(id, 0);
        }
    }

    return sections;
}

void PEL::toJSON(message::Registry& registry) const
{
    auto sections = getPluralSections();

    std::string buf = "{\n";
    printSectionInJSON(*(_ph.get()), buf, sections, registry);
    printSectionInJSON(*(_uh.get()), buf, sections, registry);
    for (auto& section : this->optionalSections())
    {
        printSectionInJSON(*(section.get()), buf, sections, registry);
    }
    buf += "}";
    std::size_t found = buf.rfind(",");
    if (found != std::string::npos)
        buf.replace(found, 1, "");
    std::cout << buf << std::endl;
}

namespace util
{

std::unique_ptr<UserData> makeJSONUserDataSection(const nlohmann::json& json)
{
    auto jsonString = json.dump();
    std::vector<uint8_t> jsonData(jsonString.begin(), jsonString.end());

    // Pad to a 4 byte boundary
    while ((jsonData.size() % 4) != 0)
    {
        jsonData.push_back(0);
    }

    return std::make_unique<UserData>(
        static_cast<uint16_t>(ComponentID::phosphorLogging),
        static_cast<uint8_t>(UserDataFormat::json),
        static_cast<uint8_t>(UserDataFormatVersion::json), jsonData);
}

std::unique_ptr<UserData> makeADUserDataSection(const AdditionalData& ad)
{
    assert(!ad.empty());
    nlohmann::json json;

    // Remove the 'ESEL' entry, as it contains a full PEL in the value.
    if (ad.getValue("ESEL"))
    {
        auto newAD = ad;
        newAD.remove("ESEL");
        json = newAD.toJSON();
    }
    else
    {
        json = ad.toJSON();
    }

    return makeJSONUserDataSection(json);
}

void addProcessNameToJSON(nlohmann::json& json,
                          const std::optional<std::string>& pid,
                          const DataInterfaceBase& dataIface)
{
    std::string name{unknownValue};

    try
    {
        if (pid)
        {
            auto n = dataIface.getProcessName(*pid);
            if (n)
            {
                name = *n;
            }
        }
    }
    catch (std::exception& e)
    {
    }

    json["Process Name"] = std::move(name);
}

void addBMCFWVersionIDToJSON(nlohmann::json& json,
                             const DataInterfaceBase& dataIface)
{
    auto id = dataIface.getBMCFWVersionID();
    if (id.empty())
    {
        id = unknownValue;
    }

    json["BMC Version ID"] = std::move(id);
}

std::string lastSegment(char separator, std::string data)
{
    auto pos = data.find_last_of(separator);
    if (pos != std::string::npos)
    {
        data = data.substr(pos + 1);
    }

    return data;
}

void addStatesToJSON(nlohmann::json& json, const DataInterfaceBase& dataIface)
{
    json["BMCState"] = lastSegment('.', dataIface.getBMCState());
    json["ChassisState"] = lastSegment('.', dataIface.getChassisState());
    json["HostState"] = lastSegment('.', dataIface.getHostState());
}

std::unique_ptr<UserData>
    makeSysInfoUserDataSection(const AdditionalData& ad,
                               const DataInterfaceBase& dataIface)
{
    nlohmann::json json;

    addProcessNameToJSON(json, ad.getValue("_PID"), dataIface);
    addBMCFWVersionIDToJSON(json, dataIface);
    addStatesToJSON(json, dataIface);

    return makeJSONUserDataSection(json);
}

} // namespace util

} // namespace pels
} // namespace openpower
