/**
 * Copyright © 2020 Intel Corporation
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
#pragma once

#include "pldm.hpp"
#include "thresholds.hpp"

#include <boost/asio.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <xyz/openbmc_project/Sensor/Value/server.hpp>

using SensorUnit = sdbusplus::xyz::openbmc_project::Sensor::server::Value::Unit;

struct ThresholdInterface
{
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
    std::string level;
    std::string alarm;
};

struct NumericSensor
{
    NumericSensor(const std::string& sensorName,
                  std::vector<thresholds::Threshold>& thresholdData,
                  const double max, const double min, const double hysteresis,
                  const SensorUnit sensorUnit, const bool sensorDisabled,
                  const std::string& assocationPath);

    ~NumericSensor();

    std::string name;
    double maxValue;
    double minValue;
    std::vector<thresholds::Threshold> thresholds;
    std::shared_ptr<sdbusplus::asio::dbus_interface> associationInterface =
        nullptr;
    std::shared_ptr<sdbusplus::asio::dbus_interface> sensorInterface = nullptr;
    std::shared_ptr<sdbusplus::asio::dbus_interface> thresholdInterfaceWarning =
        nullptr;
    std::shared_ptr<sdbusplus::asio::dbus_interface>
        thresholdInterfaceCritical = nullptr;
    std::shared_ptr<sdbusplus::asio::dbus_interface> availableInterface =
        nullptr;
    std::shared_ptr<sdbusplus::asio::dbus_interface> operationalInterface =
        nullptr;
    double value = std::numeric_limits<double>::quiet_NaN();
    double rawValue = std::numeric_limits<double>::quiet_NaN();

    /** @brief hysteresis value to trigger the alarm*/
    double hysteresisTrigger;

    /** @brief minimum change required to update Sensor.Value interface. This is
     * required to avoid property updates if the sensor is reporting minuscule
     * change in value*/
    double hysteresisPublish = 0;

    size_t errCount = 0;
    SensorUnit unit;

    /** @brief Increment the error count in case of failure*/
    void incrementError();

    /** @brief Check if error threshold crossed*/
    bool checkErrorThreshold();

    /** @brief Update sensor value*/
    void updateValue(const double& newValue, const bool isAvaliable,
                     const bool isFunctional);

    /** @brief Select the threshold interface as per the threshold passed*/
    std::optional<ThresholdInterface>
        selectThresholdInterface(const thresholds::Threshold& threshold);

  private:
    void updateProperty(
        std::shared_ptr<sdbusplus::asio::dbus_interface>& interface,
        double& oldValue, const double& newValue, const char* dbusPropertyName);

    bool requiresUpdate(const double& lVal, const double& rVal);

    void checkThresholds();

    void setInitialProperties(const bool sensorDisabled);

    /** @brief Update the sensor functionality*/
    void markFunctional(bool isFunctional);

    /** @brief Update the sensor availability*/
    void markAvailable(bool isAvailable);
};
