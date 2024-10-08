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

#include "numeric_effecter_handler.hpp"

#include "effecter.hpp"
#include "pdr_utils.hpp"
#include "platform.hpp"

#include <phosphor-logging/log.hpp>

namespace pldm
{
namespace platform
{
constexpr const uint8_t transitionIntervalSec = 3;

NumericEffecterHandler::NumericEffecterHandler(
    const pldm_tid_t tid, const EffecterID effecterID, const std::string& name,
    const std::shared_ptr<pldm_numeric_effecter_value_pdr>& pdr) :
    _tid(tid),
    _effecterID(effecterID), _name(name), _pdr(pdr)
{
}

NumericEffecterHandler::~NumericEffecterHandler()
{
    auto objectServer = getObjServer();
    if (setEffecterInterface)
    {

        objectServer->remove_interface(setEffecterInterface);
    }
}

bool NumericEffecterHandler::enableNumericEffecter(
    boost::asio::yield_context yield)
{
    uint8_t effecterOpState;
    switch (_pdr->effecter_init)
    {
        case PLDM_NO_INIT:
            effecterOpState = EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING;
            break;
        case PLDM_USE_INIT_PDR:
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "Numeric Effecter Initialization PDR not supported",
                phosphor::logging::entry("TID=%d", _tid),
                phosphor::logging::entry("EFFECTER_ID=%d", _effecterID));
            return false;
        case PLDM_ENABLE_EFFECTER:
            effecterOpState = EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING;
            break;
        case PLDM_DISABLE_EFECTER:
            effecterOpState = EFFECTER_OPER_STATE_DISABLED;
            break;
        default:
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Invalid effecterInit value in PDR",
                phosphor::logging::entry("TID=%d", _tid),
                phosphor::logging::entry("EFFECTER_ID=%d", _effecterID));
            return false;
    }

    int rc;
    std::vector<uint8_t> req(pldmMsgHdrSize +
                             sizeof(pldm_set_numeric_effecter_enable_req));
    pldm_msg* reqMsg = reinterpret_cast<pldm_msg*>(req.data());

    rc = encode_set_numeric_effecter_enable_req(
        createInstanceId(_tid), _effecterID, effecterOpState, reqMsg);
    if (!validatePLDMReqEncode(_tid, rc, "SetNumericEffecterEnable"))
    {
        return false;
    }

    std::vector<uint8_t> resp;
    if (!sendReceivePldmMessage(yield, _tid, commandTimeout, commandRetryCount,
                                req, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to send or receive SetNumericEffecterEnable request",
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }

    uint8_t completionCode;
    auto rspMsg = reinterpret_cast<pldm_msg*>(resp.data());

    rc = decode_cc_only_resp(rspMsg, resp.size() - pldmMsgHdrSize,
                             &completionCode);
    if (!validatePLDMRespDecode(_tid, rc, completionCode,
                                "SetNumericEffecterEnable"))
    {
        return false;
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "SetNumericEffecterEnable success",
        phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
        phosphor::logging::entry("TID=%d", _tid));
    return true;
}

bool NumericEffecterHandler::initEffecter()
{
    std::optional<float> maxVal =
        pdr::effecter::fetchEffecterValue(*_pdr, _pdr->max_set_table);
    if (maxVal == std::nullopt)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Unable to decode maxSetable",
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }
    maxSettable = pdr::effecter::calculateEffecterValue(*_pdr, *maxVal);

    std::optional<float> minVal =
        pdr::effecter::fetchEffecterValue(*_pdr, _pdr->min_set_table);
    if (minVal == std::nullopt)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Unable to decode minReadable",
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }
    minSettable = pdr::effecter::calculateEffecterValue(*_pdr, *minVal);

    try
    {
        _effecter = std::make_shared<NumericEffecter>(
            _name, _tid, maxSettable, minSettable, _pdr->base_unit);
    }
    catch (std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            e.what(),
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "Effecter Init success",
        phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
        phosphor::logging::entry("TID=%d", _tid));
    return true;
}

bool NumericEffecterHandler::handleEffecterReading(
    boost::asio::yield_context yield, uint8_t effecterOperationalState,
    uint8_t effecterDataSize, union_effecter_data_size& presentReading)
{
    switch (effecterOperationalState)
    {
        case EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING: {

            transitionIntervalTimer->expires_after(
                boost::asio::chrono::milliseconds(transitionIntervalSec));
            boost::system::error_code ec;
            transitionIntervalTimer->async_wait(yield[ec]);

            if (ec == boost::asio::error::operation_aborted)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "populateEffecterValue call invoke aborted");
                return false;
            }
            else if (ec)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "populateEffecterValue call invoke failed");
                return false;
            }

            cmdRetryCount++;

            if (cmdRetryCount > commandRetryCount)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    ("NUMERIC EFFECTER_UPDATEPENDING max retry count reached" +
                     std::to_string(cmdRetryCount))
                        .c_str());
                cmdRetryCount = 0;
                return false;
            }

            populateEffecterValue(yield);
            break;
        }
        case EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING: {
            if (_pdr->effecter_data_size != effecterDataSize)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Invalid effecter reading. Effecter data size mismatch",
                    phosphor::logging::entry("TID=%d", _tid),
                    phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                    phosphor::logging::entry("DATA_SIZE=%d", effecterDataSize));
                return false;
            }

            std::optional<float> effecterReading =
                pdr::effecter::fetchEffecterValue(*_pdr, presentReading);
            if (effecterReading == std::nullopt)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Numeric effecter value decode failed",
                    phosphor::logging::entry("TID=%d", _tid),
                    phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                    phosphor::logging::entry("DATA_SIZE=%d", effecterDataSize));
                return false;
            }

            double value =
                pdr::effecter::calculateEffecterValue(*_pdr, *effecterReading);
            _effecter->updateValue(value, effecterAvailable,
                                   effecterFunctional);

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "GetNumericEffecterValue success",
                phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                phosphor::logging::entry("TID=%d", _tid),
                phosphor::logging::entry("VALUE=%lf", value));
            break;
        }
        case EFFECTER_OPER_STATE_DISABLED: {
            _effecter->updateValue(std::numeric_limits<double>::quiet_NaN(),
                                   effecterAvailable, effecterNonFunctional);

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "Numeric effecter disabled",
                phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                phosphor::logging::entry("TID=%d", _tid));
            break;
        }
        case EFFECTER_OPER_STATE_UNAVAILABLE: {
            _effecter->updateValue(std::numeric_limits<double>::quiet_NaN(),
                                   effecterUnavailable, effecterNonFunctional);

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "Numeric effecter unavailable",
                phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                phosphor::logging::entry("TID=%d", _tid));
            return false;
        }
        default:
            // TODO: Handle other effecter operational states like
            // statusUnknown, initializing etc.
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "Numeric effecter operational status unknown",
                phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                phosphor::logging::entry("TID=%d", _tid));
            return false;
    }

    if (effecterOperationalState != EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING)
    {
        cmdRetryCount = 0;
    }

    return true;
}

bool NumericEffecterHandler::getEffecterReading(
    boost::asio::yield_context yield)
{
    int rc;
    std::vector<uint8_t> req(pldmMsgHdrSize +
                             sizeof(pldm_get_numeric_effecter_value_req));
    pldm_msg* reqMsg = reinterpret_cast<pldm_msg*>(req.data());

    rc = encode_get_numeric_effecter_value_req(createInstanceId(_tid),
                                               _effecterID, reqMsg);
    if (!validatePLDMReqEncode(_tid, rc, "GetNumericEffecterValue"))
    {
        return false;
    }

    std::vector<uint8_t> resp;
    if (!sendReceivePldmMessage(yield, _tid, commandTimeout, commandRetryCount,
                                req, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to send or receive GetNumericEffecterValue request",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID));
        return false;
    }

    uint8_t completionCode;
    uint8_t effecterDataSize;
    uint8_t effecterOperationalState;
    union_effecter_data_size pendingValue;
    union_effecter_data_size presentValue;
    auto rspMsg = reinterpret_cast<pldm_msg*>(resp.data());

    rc = decode_get_numeric_effecter_value_resp(
        rspMsg, resp.size() - pldmMsgHdrSize, &completionCode,
        &effecterDataSize, &effecterOperationalState,
        reinterpret_cast<uint8_t*>(&pendingValue),
        reinterpret_cast<uint8_t*>(&presentValue));
    if (!validatePLDMRespDecode(_tid, rc, completionCode,
                                "GetNumericEffecterValue"))
    {
        return false;
    }

    return handleEffecterReading(yield, effecterOperationalState,
                                 effecterDataSize, presentValue);
}

bool NumericEffecterHandler::populateEffecterValue(
    boost::asio::yield_context yield)
{
    if (!getEffecterReading(yield))
    {
        _effecter->incrementError();
        return false;
    }
    return true;
}

static std::optional<size_t> getEffecterValueSize(const uint8_t dataSize)
{
    switch (dataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            return sizeof(uint8_t);
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            return sizeof(uint16_t);
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            return sizeof(uint32_t);
        default:
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Effecter data size not recognized");
            return std::nullopt;
    }
}

bool NumericEffecterHandler::setEffecter(boost::asio::yield_context yield,
                                         double& value)
{
    if (value < minSettable || value > maxSettable)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Invalid effecter value");
        return false;
    }

    std::optional<double> settableValue =
        pdr::effecter::calculateSettableEffecterValue(*_pdr, value);
    if (settableValue == std::nullopt)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Effecter value calculation failed");
        return false;
    }

    std::optional<union_effecter_data_size> effecterValue =
        pdr::effecter::formatSettableEffecterValue(*_pdr, *settableValue);
    if (effecterValue == std::nullopt)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Effecter value formatting failed. Invalid effecter value");
        return false;
    }

    std::optional<size_t> dataSize =
        getEffecterValueSize(_pdr->effecter_data_size);
    if (dataSize == std::nullopt)
    {
        return false;
    }

    // pldm_set_numeric_effecter_value_req already have a effecter_value[1]
    // Hence subtract `sizeof(uint8_t)` from the whole size
    size_t payloadLength = sizeof(pldm_set_numeric_effecter_value_req) -
                           sizeof(uint8_t) + *dataSize;
    std::vector<uint8_t> req(pldmMsgHdrSize + payloadLength);
    pldm_msg* reqMsg = reinterpret_cast<pldm_msg*>(req.data());

    int rc;
    rc = encode_set_numeric_effecter_value_req(
        createInstanceId(_tid), _effecterID, _pdr->effecter_data_size,
        reinterpret_cast<uint8_t*>(&(*effecterValue)), reqMsg, payloadLength);
    if (!validatePLDMReqEncode(_tid, rc, "SetNumericEffecterValue"))
    {
        return false;
    }

    std::vector<uint8_t> resp;
    if (!sendReceivePldmMessage(yield, _tid, commandTimeout, commandRetryCount,
                                req, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to send or receive SetNumericEffecterValue request",
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }

    uint8_t completionCode;
    auto rspMsg = reinterpret_cast<pldm_msg*>(resp.data());

    rc = decode_cc_only_resp(rspMsg, resp.size() - pldmMsgHdrSize,
                             &completionCode);
    if (!validatePLDMRespDecode(_tid, rc, completionCode,
                                "SetNumericEffecterValue"))
    {
        _effecter->incrementError();
        return false;
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "SetNumericEffecterValue success",
        phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
        phosphor::logging::entry("TID=%d", _tid));
    return true;
}

void NumericEffecterHandler::registerSetEffecter()
{
    auto objServer = getObjServer();
    setEffecterInterface = std::make_shared<sdbusplus::asio::dbus_interface>(
        getSdBus(), _effecter->effecterInterface->get_object_path(),
        "xyz.openbmc_project.Effecter.SetNumericEffecter");
    setEffecterInterface->register_method(
        "SetEffecter",
        [this](boost::asio::yield_context yield, double effecterValue) {
            if (!setEffecter(yield, effecterValue))
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Failed to SetNumericEffecterValue",
                    phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                    phosphor::logging::entry("TID=%d", _tid));

                throw sdbusplus::exception::SdBusError(
                    -EINVAL, "SetNumericEffecterValue failed");
            }

            auto refreshEffecterInterfaces = [this]() {
                boost::system::error_code ec;

                if (cmdRetryCount != 0)
                {
                    phosphor::logging::log<phosphor::logging::level::DEBUG>(
                        "Numeric UpdatePending Retry In Progress");
                    return;
                }

                uint64_t transitionIntervalMilliSec = 0;
                if (!std::isnan(_pdr->transition_interval) &&
                    _pdr->transition_interval > 0)
                {
                    // Convert to millisec to get more accurate value
                    transitionIntervalMilliSec = static_cast<uint64_t>(
                        std::round(_pdr->transition_interval * 1000));
                }
                transitionIntervalTimer->expires_after(
                    boost::asio::chrono::milliseconds(
                        transitionIntervalMilliSec));
                transitionIntervalTimer->async_wait(
                    [this](const boost::system::error_code& e) {
                        if (e)
                        {
                            phosphor::logging::log<
                                phosphor::logging::level::ERR>(
                                "SetNumericEffecterValue: async_wait error");
                        }
                        boost::asio::spawn(
                            *getIoContext(),
                            [this](boost::asio::yield_context yieldCtx) {
                                if (!populateEffecterValue(yieldCtx))
                                {
                                    phosphor::logging::log<
                                        phosphor::logging::level::ERR>(
                                        "Read state effecter failed",
                                        phosphor::logging::entry(
                                            "EFFECTER_ID=0x%0X", _effecterID),
                                        phosphor::logging::entry("TID=%d",
                                                                 _tid));
                                }
                            });
                    });
            };

            // Refresh the value on D-Bus
            getIoContext()->post(refreshEffecterInterfaces);
        });
    setEffecterInterface->initialize();
}

bool NumericEffecterHandler::effecterHandlerInit(
    boost::asio::yield_context yield)
{
    if (!enableNumericEffecter(yield))
    {
        return false;
    }

    if (!initEffecter())
    {
        return false;
    }

    transitionIntervalTimer =
        std::make_unique<boost::asio::steady_timer>(*getIoContext());

    // Read and populate the effecter initial value
    if (!populateEffecterValue(yield))
    {
        return false;
    }

    registerSetEffecter();

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "Effecter Handler Init Success",
        phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
        phosphor::logging::entry("TID=%d", _tid));
    return true;
}

} // namespace platform
} // namespace pldm
