/**
 * Copyright © 2021 Intel Corporation
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

#include "utils.hpp"

#include <iomanip>
#include <phosphor-logging/log.hpp>
#include <sstream>

namespace utils
{

void printVect(const std::string& msg, const std::vector<uint8_t>& vec)
{
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        ("Length:" + std::to_string(vec.size())).c_str());

    std::stringstream ssVec;
    ssVec << msg;
    for (auto re : vec)
    {
        ssVec << " 0x" << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<int>(re);
    }
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        ssVec.str().c_str());
}

bool interfaceInitialize(
    const std::shared_ptr<sdbusplus::asio::dbus_interface>& interface,
    const bool flag)
{
    try
    {
        return interface->initialize(flag);
    }
    catch (const std::exception& e)
    {
        const auto interfaceName = interface->get_interface_name();
        const auto objectPath = interface->get_object_path();
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("Error initializing interface for object: INTERFACE=" +
             interfaceName + " OBJECT_PATH=" + objectPath +
             " ERROR=" + e.what())
                .c_str());
        throw;
    }
}

bool interfaceInitialize(
    const std::unique_ptr<sdbusplus::asio::dbus_interface>& interface,
    const bool flag)
{
    try
    {
        return interface->initialize(flag);
    }
    catch (const std::exception& e)
    {
        const auto interfaceName = interface->get_interface_name();
        const auto objectPath = interface->get_object_path();
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("Error initializing interface for object: INTERFACE=" +
             interfaceName + " OBJECT_PATH=" + objectPath +
             " ERROR=" + e.what())
                .c_str());
        throw;
    }
}

} // namespace utils
