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
#include "fru_identity.hpp"

#include "pel_values.hpp"

namespace openpower
{
namespace pels
{
namespace src
{

FRUIdentity::FRUIdentity(Stream& pel)
{
    pel >> _type >> _size >> _flags;

    if (hasPN() || hasMP())
    {
        pel.read(_pnOrProcedureID.data(), _pnOrProcedureID.size());
    }

    if (hasCCIN())
    {
        pel.read(_ccin.data(), _ccin.size());
    }

    if (hasSN())
    {
        pel.read(_sn.data(), _sn.size());
    }
}

size_t FRUIdentity::flattenedSize() const
{
    size_t size = sizeof(_type) + sizeof(_size) + sizeof(_flags);

    if (hasPN() || hasMP())
    {
        size += _pnOrProcedureID.size();
    }

    if (hasCCIN())
    {
        size += _ccin.size();
    }

    if (hasSN())
    {
        size += _sn.size();
    }

    return size;
}

FRUIdentity::FRUIdentity(const std::string& partNumber, const std::string& ccin,
                         const std::string& serialNumber)
{
    _type = substructureType;
    _flags = hardwareFRU;

    setPartNumber(partNumber);
    setCCIN(ccin);
    setSerialNumber(serialNumber);

    _size = flattenedSize();
}

FRUIdentity::FRUIdentity(MaintProcedure procedure)
{
    _type = substructureType;
    _flags = maintenanceProc;

    setMaintenanceProcedure(procedure);

    _size = flattenedSize();
}

std::optional<std::string> FRUIdentity::getPN() const
{
    if (hasPN())
    {
        // NULL terminated
        std::string pn{_pnOrProcedureID.data()};
        return pn;
    }

    return std::nullopt;
}

std::optional<std::string> FRUIdentity::getMaintProc() const
{
    if (hasMP())
    {
        // NULL terminated
        std::string mp{_pnOrProcedureID.data()};
        return mp;
    }

    return std::nullopt;
}

std::optional<std::string> FRUIdentity::getCCIN() const
{
    if (hasCCIN())
    {
        std::string ccin{_ccin.begin(), _ccin.begin() + _ccin.size()};

        // Don't leave any NULLs in the string (not there usually)
        if (auto pos = ccin.find('\0'); pos != std::string::npos)
        {
            ccin.resize(pos);
        }
        return ccin;
    }

    return std::nullopt;
}

std::optional<std::string> FRUIdentity::getSN() const
{
    if (hasSN())
    {
        std::string sn{_sn.begin(), _sn.begin() + _sn.size()};

        // Don't leave any NULLs in the string (not there usually)
        if (auto pos = sn.find('\0'); pos != std::string::npos)
        {
            sn.resize(pos);
        }
        return sn;
    }

    return std::nullopt;
}

void FRUIdentity::flatten(Stream& pel) const
{
    pel << _type << _size << _flags;

    if (hasPN() || hasMP())
    {
        pel.write(_pnOrProcedureID.data(), _pnOrProcedureID.size());
    }

    if (hasCCIN())
    {
        pel.write(_ccin.data(), _ccin.size());
    }

    if (hasSN())
    {
        pel.write(_sn.data(), _sn.size());
    }
}

void FRUIdentity::setPartNumber(const std::string& partNumber)
{
    _flags |= pnSupplied;
    _flags &= ~maintProcSupplied;

    auto pn = partNumber;

    // Strip leading whitespace on this one.
    while (' ' == pn.front())
    {
        pn = pn.substr(1);
    }

    // Note: strncpy only writes NULLs if pn short
    strncpy(_pnOrProcedureID.data(), pn.c_str(), _pnOrProcedureID.size());

    // ensure null terminated
    _pnOrProcedureID.back() = 0;
}

void FRUIdentity::setCCIN(const std::string& ccin)
{
    _flags |= ccinSupplied;

    // Note: _ccin not null terminated, though strncpy writes NULLs if short
    strncpy(_ccin.data(), ccin.c_str(), _ccin.size());
}

void FRUIdentity::setSerialNumber(const std::string& serialNumber)
{
    _flags |= snSupplied;

    // Note: _sn not null terminated, though strncpy writes NULLs if short
    strncpy(_sn.data(), serialNumber.c_str(), _sn.size());
}

void FRUIdentity::setMaintenanceProcedure(MaintProcedure procedure)
{
    _flags |= maintProcSupplied;
    _flags &= ~pnSupplied;

    auto proc = pel_values::getMaintProcedure(procedure);

    strncpy(_pnOrProcedureID.data(), std::get<pel_values::mpNamePos>(*proc),
            _pnOrProcedureID.size());

    // ensure null terminated
    _pnOrProcedureID.back() = 0;
}

} // namespace src
} // namespace pels
} // namespace openpower
