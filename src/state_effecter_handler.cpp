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

#include "state_effecter_handler.hpp"

#include "effecter.hpp"
#include "platform.hpp"

#include <phosphor-logging/log.hpp>

namespace pldm
{
namespace platform
{
static const char* pldmPath = "/xyz/openbmc_project/pldm/";
constexpr const size_t errorThreshold = 5;
constexpr const uint8_t transitionIntervalSec = 3;

StateEffecterHandler::StateEffecterHandler(
    const pldm_tid_t tid, const EffecterID effecterID, const std::string& name,
    const std::shared_ptr<StateEffecterPDR>& pdr) :
    _tid(tid),
    _effecterID(effecterID), _name(name), _pdr(pdr)
{
    if (_pdr->possibleStates.empty())
    {
        throw std::runtime_error("State effecter PDR data invalid");
    }
    setInitialProperties();
}

void StateEffecterHandler::setInitialProperties()
{
    const std::string path =
        pldmPath + std::to_string(_tid) + "/state_effecter/" + _name;

    effecterInterface =
        addUniqueInterface(path, "xyz.openbmc_project.Effecter.State");
    // Composite effecters are not supported. Thus extract only first effecter
    // state
    effecterInterface->register_property("StateSetID",
                                         _pdr->possibleStates[0].stateSetID);
    effecterInterface->register_property(
        "PossibleStates", _pdr->possibleStates[0].possibleStateSetValues);

    availableInterface = addUniqueInterface(
        path, "xyz.openbmc_project.State.Decorator.Availability");

    operationalInterface = addUniqueInterface(
        path, "xyz.openbmc_project.State.Decorator.OperationalStatus");
}

void StateEffecterHandler::initializeInterface()
{
    if (!interfaceInitialized)
    {
        effecterInterface->register_property("PendingState",
                                             pendingStateReading);
        effecterInterface->register_property("CurrentState",
                                             currentStateReading);
        effecterInterface->initialize();

        availableInterface->register_property("Available", isAvailableReading);
        availableInterface->initialize();

        operationalInterface->register_property("Functional",
                                                isFuntionalReading);
        operationalInterface->initialize();
        interfaceInitialized = true;
    }
}

void StateEffecterHandler::markFunctional(bool isFunctional)
{
    if (!operationalInterface)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Operational interface not initialized",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID));
        return;
    }

    if (!interfaceInitialized)
    {
        isFuntionalReading = isFunctional;
    }
    else
    {
        operationalInterface->set_property("Functional", isFunctional);
    }

    if (isFunctional)
    {
        errCount = 0;
    }
}

void StateEffecterHandler::markAvailable(bool isAvailable)
{
    if (!availableInterface)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Avaliable interface not initialized",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID));
        return;
    }

    if (!interfaceInitialized)
    {
        isAvailableReading = isAvailable;
    }
    else
    {
        availableInterface->set_property("Available", isAvailable);
    }
}

void StateEffecterHandler::incrementError()
{
    if (errCount >= errorThreshold)
    {
        return;
    }

    errCount++;
    if (errCount == errorThreshold)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "State effecter reading failed",
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        updateState(PLDM_INVALID_VALUE, PLDM_INVALID_VALUE, effecterAvailable,
                    effecterNonFunctional);
    }
}

void StateEffecterHandler::updateState(const uint8_t currentState,
                                       const uint8_t pendingState,
                                       bool isAvailable, bool isFunctional)
{
    if (!effecterInterface)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Effecter interface not initialized");
        return;
    }

    if (!interfaceInitialized)
    {
        currentStateReading = currentState;
        pendingStateReading = pendingState;
    }
    else
    {
        effecterInterface->set_property("CurrentState", currentState);
        effecterInterface->set_property("PendingState", pendingState);
    }

    markAvailable(isAvailable);
    markFunctional(isFunctional);
    initializeInterface();
}

bool StateEffecterHandler::enableStateEffecter(boost::asio::yield_context yield)
{
    uint8_t effecterOpState;
    switch (_pdr->stateEffecterData.effecter_init)
    {
        case PLDM_NO_INIT:
            effecterOpState = EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING;
            break;
        case PLDM_USE_INIT_PDR:
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "State Effecter Initialization PDR not supported",
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
    // TODO: PLDM events and composite effecter supported
    constexpr uint8_t compositeEffecterCount = 1;
    std::array<state_effecter_op_field, compositeEffecterCount> opFields = {
        {effecterOpState, PLDM_DISABLE_EVENTS}};
    std::vector<uint8_t> req(pldmMsgHdrSize +
                             sizeof(pldm_set_state_effecter_enable_req));
    pldm_msg* reqMsg = reinterpret_cast<pldm_msg*>(req.data());

    rc = encode_set_state_effecter_enable_req(
        createInstanceId(_tid), _effecterID, compositeEffecterCount,
        opFields.data(), reqMsg);
    if (!validatePLDMReqEncode(_tid, rc, "SetStateEffecterEnable"))
    {
        return false;
    }

    std::vector<uint8_t> resp;
    if (!sendReceivePldmMessage(yield, _tid, commandTimeout, commandRetryCount,
                                req, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to send or receive SetStateEffecterEnable request",
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }

    uint8_t completionCode;
    auto rspMsg = reinterpret_cast<pldm_msg*>(resp.data());

    rc = decode_cc_only_resp(rspMsg, resp.size() - pldmMsgHdrSize,
                             &completionCode);
    if (!validatePLDMRespDecode(_tid, rc, completionCode,
                                "SetStateEffecterEnable"))
    {
        return false;
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "SetStateEffecterEnable success",
        phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
        phosphor::logging::entry("TID=%d", _tid));
    return true;
}

bool StateEffecterHandler::handleStateEffecterState(
    boost::asio::yield_context yield, get_effecter_state_field& stateReading)
{
    switch (stateReading.effecter_op_state)
    {
        case EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING: {

            transitionIntervalTimer->expires_after(
                boost::asio::chrono::milliseconds(transitionIntervalSec));
            boost::system::error_code ec;
            transitionIntervalTimer->async_wait(yield[ec]);

            if (ec == boost::asio::error::operation_aborted)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "populateStateEffecterValue call invoke aborted");
                return false;
            }
            else if (ec)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "populateStateEffecterValue call invoke failed");
                return false;
            }

            stateCmdRetryCount++;

            if (stateCmdRetryCount > commandRetryCount)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    ("EFFECTER_STATE_UPDATEPENDING max retry count reached" +
                     std::to_string(stateCmdRetryCount))
                        .c_str());
                stateCmdRetryCount = 0;
                return false;
            }

            populateEffecterValue(yield);
            break;
        }
        case EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING: {
            updateState(stateReading.present_state, stateReading.pending_state,
                        effecterAvailable, effecterFunctional);

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "GetStateEffecterStates success",
                phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                phosphor::logging::entry("TID=%d", _tid));
            break;
        }
        case EFFECTER_OPER_STATE_DISABLED: {
            updateState(PLDM_INVALID_VALUE, PLDM_INVALID_VALUE,
                        effecterAvailable, effecterNonFunctional);

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "State effecter disabled",
                phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                phosphor::logging::entry("TID=%d", _tid));
            break;
        }
        case EFFECTER_OPER_STATE_UNAVAILABLE: {
            updateState(PLDM_INVALID_VALUE, PLDM_INVALID_VALUE,
                        effecterUnavailable, effecterNonFunctional);

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "State effecter unavailable",
                phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                phosphor::logging::entry("TID=%d", _tid));
            return false;
        }
        default:
            // TODO: Handle other effecter operational states like
            // statusUnknown, initializing etc.
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                "State effecter operational status unknown",
                phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                phosphor::logging::entry("TID=%d", _tid));
            return false;
    }

    if (stateReading.effecter_op_state !=
        EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING)
    {
        stateCmdRetryCount = 0;
    }
    return true;
}

bool StateEffecterHandler::getStateEffecterStates(
    boost::asio::yield_context yield)
{
    int rc;
    std::vector<uint8_t> req(pldmMsgHdrSize +
                             sizeof(pldm_get_state_effecter_states_req));
    pldm_msg* reqMsg = reinterpret_cast<pldm_msg*>(req.data());

    rc = encode_get_state_effecter_states_req(createInstanceId(_tid),
                                              _effecterID, reqMsg);
    if (!validatePLDMReqEncode(_tid, rc, "GetStateEffecterStates"))
    {
        return false;
    }

    std::vector<uint8_t> resp;
    if (!sendReceivePldmMessage(yield, _tid, commandTimeout, commandRetryCount,
                                req, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to send or receive GetStateEffecterStates request",
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }

    uint8_t completionCode;
    // Pass compositeEffecterCount as 1 to indicate that only one effecter
    // instance is supported
    uint8_t compositeEffecterCount = PLDM_COMPOSITE_EFFECTER_COUNT_MIN;
    std::array<get_effecter_state_field, PLDM_COMPOSITE_EFFECTER_COUNT_MAX>
        stateField{};
    auto rspMsg = reinterpret_cast<pldm_msg*>(resp.data());

    rc = decode_get_state_effecter_states_resp(
        rspMsg, resp.size() - pldmMsgHdrSize, &completionCode,
        &compositeEffecterCount, stateField.data());
    if (!validatePLDMRespDecode(_tid, rc, completionCode,
                                "GetStateEffecterStates"))
    {
        return false;
    }

    if (!compositeEffecterCount)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "GetStateEffecterStates: Invalid composite effecter count",
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }
    // Handle only first value.
    // TODO: Composite effecter support.
    return handleStateEffecterState(yield, stateField[0]);
}

bool StateEffecterHandler::populateEffecterValue(
    boost::asio::yield_context yield)
{
    if (!getStateEffecterStates(yield))
    {
        incrementError();
        return false;
    }
    return true;
}

bool StateEffecterHandler::isEffecterStateSettable(const uint8_t state)
{
    // Note:- possibleStates will never be empty
    auto itr =
        std::find(_pdr->possibleStates[0].possibleStateSetValues.begin(),
                  _pdr->possibleStates[0].possibleStateSetValues.end(), state);
    if (itr != _pdr->possibleStates[0].possibleStateSetValues.end())
    {
        return true;
    }
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "State not supported by effecter",
        phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
        phosphor::logging::entry("TID=%d", _tid));
    return false;
}

bool StateEffecterHandler::setEffecter(boost::asio::yield_context yield,
                                       const uint8_t state)
{
    int rc;

    // Composite effecters not spported
    constexpr size_t minSetStateEffecterStatesSize = 5;
    std::vector<uint8_t> req(pldmMsgHdrSize + minSetStateEffecterStatesSize);
    pldm_msg* reqMsg = reinterpret_cast<pldm_msg*>(req.data());
    set_effecter_state_field stateField = {PLDM_REQUEST_SET, state};

    constexpr size_t compositeEffecterCount = 1;
    rc = encode_set_state_effecter_states_req(
        createInstanceId(_tid), _effecterID, compositeEffecterCount,
        &stateField, reqMsg);
    if (!validatePLDMReqEncode(_tid, rc, "SetStateEffecterStates"))
    {
        return false;
    }

    std::vector<uint8_t> resp;
    if (!sendReceivePldmMessage(yield, _tid, commandTimeout, commandRetryCount,
                                req, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to send or receive SetStateEffecterStates request",
            phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }

    uint8_t completionCode;
    auto rspMsg = reinterpret_cast<pldm_msg*>(resp.data());

    rc = decode_cc_only_resp(rspMsg, resp.size() - pldmMsgHdrSize,
                             &completionCode);
    if (!validatePLDMRespDecode(_tid, rc, completionCode,
                                "SetStateEffecterStates"))
    {
        incrementError();
        return false;
    }

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "SetStateEffecterStates success",
        phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
        phosphor::logging::entry("TID=%d", _tid));
    return true;
}

void StateEffecterHandler::registerSetEffecter()
{
    const std::string path =
        pldmPath + std::to_string(_tid) + "/state_effecter/" + _name;
    setEffecterInterface = addUniqueInterface(
        path, "xyz.openbmc_project.Effecter.SetStateEffecter");
    setEffecterInterface->register_method(
        "SetEffecter",
        [this](boost::asio::yield_context yield, uint8_t effecterState) {
            if (!isEffecterStateSettable(effecterState))
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "Unsupported effecter data state received",
                    phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                    phosphor::logging::entry("TID=%d", _tid),
                    phosphor::logging::entry("STATE=%d", effecterState));

                throw sdbusplus::exception::SdBusError(
                    -EINVAL, "Unsupported effecter state");
            }
            if (!setEffecter(yield, effecterState))
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Failed to SetStateEffecterStates",
                    phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
                    phosphor::logging::entry("TID=%d", _tid));

                throw sdbusplus::exception::SdBusError(
                    -EINVAL, "SetStateEffecterStates failed");
            }

            auto refreshEffecterInterfaces = [this]() {
                boost::system::error_code ec;
                if (stateCmdRetryCount != 0)
                {
                    phosphor::logging::log<phosphor::logging::level::DEBUG>(
                        "state effecter UpdatePending Retry In Progress");
                    return;
                }

                transitionIntervalTimer->expires_after(
                    boost::asio::chrono::seconds(transitionIntervalSec));
                transitionIntervalTimer->async_wait(
                    [this](const boost::system::error_code& e) {
                        if (e)
                        {
                            phosphor::logging::log<
                                phosphor::logging::level::ERR>(
                                "SetStateEffecter: async_wait error");
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

bool StateEffecterHandler::effecterHandlerInit(boost::asio::yield_context yield)
{
    transitionIntervalTimer =
        std::make_unique<boost::asio::steady_timer>(*getIoContext());

    if (!enableStateEffecter(yield))
    {
        return false;
    }

    if (!populateEffecterValue(yield))
    {
        return false;
    }

    registerSetEffecter();

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "State Effecter Init Success",
        phosphor::logging::entry("EFFECTER_ID=0x%0X", _effecterID),
        phosphor::logging::entry("TID=%d", _tid));
    return true;
}

} // namespace platform
} // namespace pldm
