/*
// Copyright (c) 2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "base.hpp"
#include "mctp_wrapper.hpp"
#include "platform.hpp"
#include "pldm.hpp"
#include "utils.hpp"

#include <queue>

extern "C" {
#include <signal.h>
}

#include <phosphor-logging/log.hpp>

static constexpr const char* pldmService = "xyz.openbmc_project.pldm";
static constexpr const char* pldmPath = "/xyz/openbmc_project/pldm";
bool debug = false;

namespace pldm
{

static bool rsvBWActive = false;
static pldm_tid_t reservedTID = pldmInvalidTid;
static uint8_t reservedPLDMType = pldmInvalidType;

TIDMapper tidMapper;
std::unique_ptr<mctpw::MCTPWrapper> mctpWrapper;

void triggerDeviceDiscovery(const pldm_tid_t tid)
{
    if (auto eidPtr = tidMapper.getMappedEID(tid))
    {
        mctpWrapper->triggerMCTPDeviceDiscovery(*eidPtr);
    }
}

static bool validateReserveBW(const pldm_tid_t tid, const uint8_t pldmType)
{
    return rsvBWActive && !(tid == reservedTID && pldmType == reservedPLDMType);
}

bool reserveBandwidth(const boost::asio::yield_context yield,
                      const pldm_tid_t tid, const uint8_t pldmType,
                      const uint16_t timeout)
{
    if (validateReserveBW(tid, pldmType))
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("Reserve bandwidth is active for TID: " +
             std::to_string(reservedTID) +
             ". RESERVED_PLDM_TYPE: " + std::to_string(reservedPLDMType))
                .c_str());
        return false;
    }
    mctpw_eid_t eid = 0;
    if (auto eidPtr = tidMapper.getMappedEID(tid))
    {
        eid = *eidPtr;
    }
    else
    {
        return false;
    }
    if (mctpWrapper->reserveBandwidth(yield, eid, timeout) < 0)
    {
        return false;
    }
    rsvBWActive = true;
    reservedTID = tid;
    reservedPLDMType = pldmType;
    return true;
}

bool releaseBandwidth(const boost::asio::yield_context yield,
                      const pldm_tid_t tid, const uint8_t pldmType)
{
    if (!rsvBWActive)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "releaseBandwidth: Reserve bandwidth is not active.");
        return false;
    }
    if (tid != reservedTID || pldmType != reservedPLDMType)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "releaseBandwidth: Invalid TID or pldm type");
        return false;
    }
    std::optional<mctpw_eid_t> eid = tidMapper.getMappedEID(tid);
    if (eid == std::nullopt)
    {
        return false;
    }
    if (mctpWrapper->releaseBandwidth(yield, *eid) < 0)
    {
        return false;
    }
    rsvBWActive = false;
    reservedTID = pldmInvalidTid;
    reservedPLDMType = pldmInvalidType;
    return true;
}

std::optional<std::string> getDeviceLocation(const pldm_tid_t tid)
{
    std::optional<mctpw_eid_t> eid = tidMapper.getMappedEID(tid);
    if (eid.has_value()) {
        return mctpWrapper->getDeviceLocation(eid.value());
    }
    return std::nullopt;
}

std::optional<pldm_tid_t> TIDMapper::getMappedTID(const mctpw_eid_t eid)
{
    for (auto& eidMap : tidMap)
    {
        if (eidMap.second == eid)
        {
            return eidMap.first;
        }
    }
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        ("Mapper: EID " + std::to_string(static_cast<int>(eid)) +
         " is not mapped to any TID")
            .c_str());
    return std::nullopt;
}

bool TIDMapper::addEntry(const pldm_tid_t tid, const mctpw_eid_t eid)
{
    for (auto& eidMap : tidMap)
    {
        if (eidMap.second == eid)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                ("Unable to add entry. EID: " +
                 std::to_string(static_cast<int>(eid)) +
                 " is already mapped with another TID: " +
                 std::to_string(static_cast<int>(tid)))
                    .c_str());
            return false;
        }
    }

    tidMap.insert_or_assign(tid, eid);
    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("Mapper: TID " + std::to_string(static_cast<int>(tid)) +
         " mapped to EID " + std::to_string(static_cast<int>(eid)))
            .c_str());
    return true;
}

void TIDMapper::removeEntry(const pldm_tid_t tid)
{
    if (1 == tidMap.erase(tid))
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("TID " + std::to_string(static_cast<int>(tid)) +
             " removed from mapper")
                .c_str());
    }
}

std::optional<mctpw_eid_t> TIDMapper::getMappedEID(const pldm_tid_t tid)
{
    auto mapperPtr = tidMap.find(tid);
    if (mapperPtr != tidMap.end())
    {
        return mapperPtr->second;
    }
    phosphor::logging::log<phosphor::logging::level::WARNING>(
        "TID not found in the mapper");
    return std::nullopt;
}

std::optional<uint8_t> getInstanceId(std::vector<uint8_t>& message)
{
    if (message.empty())
    {
        return std::nullopt;
    }
    return message[0] & PLDM_INSTANCE_ID_MASK;
}

std::optional<uint8_t> getPldmMessageType(std::vector<uint8_t>& message)
{
    constexpr int msgTypeIndex = 1;
    if (message.size() < 2)
    {
        return std::nullopt;
    }
    return message[msgTypeIndex] & PLDM_MSG_TYPE_MASK;
}

// Returns type of message(response,request, Reserved or Unacknowledged PLDM
// request messages)
std::optional<MessageType> getPldmPacketType(std::vector<uint8_t>& message)
{
    constexpr int rqD = 0;
    if (message.size() < 1)
    {
        return std::nullopt;
    }

    uint8_t rqDValue = (message[rqD] & PLDM_RQ_D_MASK) >> PLDM_RQ_D_SHIFT;
    return static_cast<MessageType>(rqDValue);
}

bool validatePLDMReqEncode(const pldm_tid_t tid, const int rc,
                           const std::string& commandString)
{
    if (rc != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            (commandString + ": Request encode failed").c_str(),
            phosphor::logging::entry("TID=%d", tid),
            phosphor::logging::entry("RC=%d", rc));
        return false;
    }
    return true;
}

bool validatePLDMRespDecode(const pldm_tid_t tid, const int rc,
                            const uint8_t completionCode,
                            const std::string& commandString)
{
    if (rc != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            (commandString + ": Response decode failed").c_str(),
            phosphor::logging::entry("TID=%d", tid),
            phosphor::logging::entry("RC=%d", rc));
        return false;
    }

    // Completion code value is considered as valid only if decode is success(rc
    // = PLDM_SUCCESS)
    if (completionCode != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            (commandString + ": Invalid completion code").c_str(),
            phosphor::logging::entry("TID=%d", tid),
            phosphor::logging::entry("CC=%d", completionCode));
        return false;
    }
    return true;
}

static bool doSendReceievePldmMessage(boost::asio::yield_context yield,
                                      const mctpw_eid_t dstEid,
                                      const uint16_t timeout,
                                      std::vector<uint8_t>& pldmReq,
                                      std::vector<uint8_t>& pldmResp)
{
    auto sendStatus = mctpWrapper->sendReceiveYield(
        yield, dstEid, pldmReq, std::chrono::milliseconds(timeout));
    pldmResp = sendStatus.second;
    utils::printVect("Request(MCTP payload):", pldmReq);
    utils::printVect("Response(MCTP payload):", pldmResp);
    return sendStatus.first ? false : true;
}

bool sendReceivePldmMessage(boost::asio::yield_context yield,
                            const pldm_tid_t tid, const uint16_t timeout,
                            size_t retryCount, std::vector<uint8_t> pldmReq,
                            std::vector<uint8_t>& pldmResp,
                            std::optional<mctpw_eid_t> eid)
{
    pldm_msg_hdr* hdr = reinterpret_cast<pldm_msg_hdr*>(pldmReq.data());
    if (validateReserveBW(tid, hdr->type))
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("sendReceivePldmMessage is not allowed. Reserve bandwidth is "
             "active for TID: " +
             std::to_string(reservedTID) +
             " RESERVED_PLDM_TYPE: " + std::to_string(reservedPLDMType))
                .c_str());
        return false;
    }
    // Retry the request if
    //  1) No response
    //  2) payload.size() < 4
    //  3) If response bit is not set in PLDM header
    //  4) Invalid message type
    //  5) Invalid instance id

    // Upper cap of retryCount = 5
    constexpr size_t maxRetryCount = 5;
    if (retryCount > maxRetryCount)
    {
        retryCount = maxRetryCount;
    }

    for (size_t retry = 0; retry < retryCount; retry++)
    {
        mctpw_eid_t dstEid;

        // Input EID takes precedence over TID
        // Usecase: TID reassignment
        if (eid)
        {
            dstEid = eid.value();
        }
        else
        {
            // A PLDM device removal can cause an update to TID mapper. In such
            // case the retry should be aborted immediately.
            if (auto eidPtr = tidMapper.getMappedEID(tid))
            {
                dstEid = *eidPtr;
            }
            else
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "PLDM message send failed. Invalid TID/EID");
                return false;
            }
        }

        // Insert MCTP Message Type to start of the payload
        if (retry == 0)
        {
            pldmReq.insert(pldmReq.begin(),
                           static_cast<uint8_t>(mctpw::MessageType::pldm));
        }

        // Clear the resp vector each time before a retry
        pldmResp.clear();
        if (doSendReceievePldmMessage(yield, dstEid, timeout, pldmReq,
                                      pldmResp))
        {
            constexpr size_t minPldmMsgSize = 4;
            if (pldmResp.size() < minPldmMsgSize)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "Invalid response length");
                continue;
            }

            // Verify the message received is a response
            if (auto msgTypePtr = getPldmPacketType(pldmResp))
            {
                if (*msgTypePtr != PLDM_RESPONSE)
                {
                    phosphor::logging::log<phosphor::logging::level::WARNING>(
                        "PLDM message received is not response");
                    continue;
                }
            }
            else
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "Unable to get message type");
                continue;
            }

            // Verify the response received is of type PLDM
            constexpr int mctpMsgType = 0;
            if (pldmResp.at(mctpMsgType) ==
                static_cast<uint8_t>(mctpw::MessageType::pldm))
            {
                // Remove the MCTP message type and IC bit from req and resp
                // payload.
                // Why: Upper layer handlers(PLDM message type handlers)
                // are not intrested in MCTP message type information and
                // integrity check fields.
                pldmResp.erase(pldmResp.begin());
                pldmReq.erase(pldmReq.begin());
            }
            else
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "Response received is not of message type PLDM");
                continue;
            }

            // Verify request and response instance ID matches
            if (auto reqInstanceId = getInstanceId(pldmReq))
            {
                if (auto respInstanceId = getInstanceId(pldmResp))
                {
                    if (*reqInstanceId == *respInstanceId)
                    {
                        return true;
                    }
                }
            }
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "Instance ID check failed");
            continue;
        }
    }
    phosphor::logging::log<phosphor::logging::level::ERR>(
        "Retry count exceeded. No response");
    return false;
}

bool sendPldmMessage(boost::asio::yield_context yield, const pldm_tid_t tid,
                     uint8_t retryCount, const uint8_t msgTag,
                     const bool tagOwner, std::vector<uint8_t> payload)

{
    constexpr size_t maxRetryCount = 5;
    pldm_msg_hdr* hdr = reinterpret_cast<pldm_msg_hdr*>(payload.data());
    if (validateReserveBW(tid, hdr->type))
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("sendPldmMessage is not allowed. Reserve bandwidth is active for "
             "TID: " +
             std::to_string(reservedTID) +
             " RESERVED_PLDM_TYPE: " + std::to_string(reservedPLDMType))
                .c_str());
        return false;
    }

    mctpw_eid_t dstEid;
    if (auto eidPtr = tidMapper.getMappedEID(tid))
    {
        dstEid = *eidPtr;
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "PLDM message send failed. Invalid TID");
        return false;
    }
    // Insert MCTP Message Type to start of the payload
    payload.insert(payload.begin(),
                   static_cast<uint8_t>(mctpw::MessageType::pldm));
    utils::printVect("Send PLDM message(MCTP payload):", payload);
    std::pair<boost::system::error_code, int> rc;

    if (retryCount > maxRetryCount)
    {
        retryCount = maxRetryCount;
    }

    for (size_t retry = 0; retry < retryCount; retry++)
    {
        rc = mctpWrapper->sendYield(yield, dstEid, msgTag, tagOwner, payload);
        if (rc.first || rc.second < 0)
        {
            continue;
        }
        break;
    }

    if (rc.first || rc.second < 0)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("SendMCTPPayload Failed, retry count exceeded  rc: " +
             std::to_string(rc.second) + " " + rc.first.message())
                .c_str());
        return false;
    }
    return true;
}

auto msgRecvCallback = [](void*, mctpw::eid_t srcEid, bool tagOwner,
                          uint8_t msgTag, const std::vector<uint8_t>& data,
                          int) {
    // Intentional copy. MCTPWrapper provides const reference in callback
    auto payload = data;
    // Verify the response received is of type PLDM
    if (!payload.empty() &&
        payload.at(0) == static_cast<uint8_t>(mctpw::MessageType::pldm))
    {
        // Discard the packet if no matching TID is found
        // Why: We do not have to process packets from uninitialised Termini
        auto tid = tidMapper.getMappedTID(srcEid);
        if (!tid)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("EID " + std::to_string(static_cast<int>(srcEid)) +
                 " is not mapped to any TID; Discarding the packet")
                    .c_str());
            return;
        }

        utils::printVect("PLDM message received(MCTP payload):", payload);
        payload.erase(payload.begin());
        if (auto pldmMsgType = getPldmMessageType(payload))
        {
            switch (*pldmMsgType)
            {
                case PLDM_FWUP:
                    pldm::fwu::pldmMsgRecvFwUpdCallback(*tid, msgTag, tagOwner,
                                                        payload);
                    break;
                    // No use case for other PLDM message types
                default:
                    phosphor::logging::log<phosphor::logging::level::INFO>(
                        "Unsupported PLDM message received",
                        phosphor::logging::entry("TID=%d", *tid),
                        phosphor::logging::entry("EID=%d", srcEid),
                        phosphor::logging::entry("MSG_TYPE=%d", *pldmMsgType));
                    break;
            }
        }
    }
};

uint8_t createInstanceId(pldm_tid_t tid)
{
    static std::unordered_map<pldm_tid_t, uint8_t> instanceMap;

    auto& instanceId = instanceMap[tid];

    instanceId = (instanceId + 1) & PLDM_INSTANCE_ID_MASK;
    return instanceId;
}
} // namespace pldm

void initDevice(const mctpw_eid_t eid, boost::asio::yield_context yield)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("Initializing MCTP EID " + std::to_string(eid)).c_str());

    pldm_tid_t assignedTID = 0x00;
    pldm::base::CommandSupportTable cmdSupportTable;
    if (!pldm::base::baseInit(yield, eid, assignedTID, cmdSupportTable))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "PLDM base init failed", phosphor::logging::entry("EID=%d", eid));
        return;
    }

    auto isSupported = [&cmdSupportTable](pldm_type_t type) {
        return cmdSupportTable.end() != cmdSupportTable.find(type);
    };

    if (isSupported(PLDM_PLATFORM) &&
        !pldm::platform::platformInit(yield, assignedTID, {}))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "PLDM platform init failed",
            phosphor::logging::entry("TID=%d", assignedTID));
    }
    if (isSupported(PLDM_FRU) && !pldm::fru::fruInit(yield, assignedTID))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "PLDM fru init failed",
            phosphor::logging::entry("TID=%d", assignedTID));
    }
    if (isSupported(PLDM_FWUP) && !pldm::fwu::fwuInit(yield, assignedTID))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "PLDM firmware update init failed",
            phosphor::logging::entry("TID=%d", assignedTID));
    }
}

// Parallel inits fail for devices behind SMBus mux due to timeouts waiting for
// response. Also, sending pldm init messages in parallel causes inits to take a
// longer duration due to the retries required for devices behind i2c mux. Thus,
// serialize the device inits by implementing a queue to cache new EIDs if a
// device init is already in progress.
void deviceInitEventHandler(const mctpw_eid_t eid,
                            boost::asio::yield_context yield)
{
    static std::queue<mctpw_eid_t> pendingDevices;
    pendingDevices.emplace(eid);
    if (pendingDevices.size() > 1)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Another device init in progress. Adding EID to queue.");
        return;
    }

    while (pendingDevices.size())
    {
        initDevice(pendingDevices.front(), yield);
        pendingDevices.pop();
    }
}

void deleteDevice(const pldm_tid_t tid)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("Delete PLDM device with TID " + std::to_string(tid)).c_str());

    // Delete the resources in reverse order of init to avoid errors due to
    // dependency if any
    if (pldm::base::isSupported(tid, PLDM_FWUP))
    {
        pldm::fwu::deleteFWDevice(tid);
    }
    if (pldm::base::isSupported(tid, PLDM_FRU))
    {
        pldm::fru::deleteFRUDevice(tid);
    }
    if (pldm::base::isSupported(tid, PLDM_PLATFORM))
    {
        pldm::platform::deleteMnCTerminus(tid);
    }
    pldm::base::deleteDeviceBaseInfo(tid);
}

// These are expected to be used only here, so declare them here
extern void setIoContext(const std::shared_ptr<boost::asio::io_context>& newIo);
extern void
    setSdBus(const std::shared_ptr<sdbusplus::asio::connection>& newBus);
extern void setObjServer(
    const std::shared_ptr<sdbusplus::asio::object_server>& newServer);

void onDeviceUpdate(void*, const mctpw::Event& evt,
                    boost::asio::yield_context yield)
{
    switch (evt.type)
    {
        case mctpw::Event::EventType::deviceAdded: {
            pldm::platform::pauseSensorPolling();
            deviceInitEventHandler(evt.eid, yield);
            pldm::platform::resumeSensorPolling();
            break;
        }
        case mctpw::Event::EventType::deviceRemoved: {
            auto tid = pldm::tidMapper.getMappedTID(evt.eid);
            if (tid)
            {
                deleteDevice(tid.value());
            }
            else
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    ("EID " + std::to_string(static_cast<int>(evt.eid)) +
                     " is not mapped to any TID")
                        .c_str());
            }
            break;
        }
        default:
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Unsupported event type in onDeviceUpdate",
                phosphor::logging::entry("TYPE=%d",
                                         static_cast<int>(evt.type)));
            break;
    }
    return;
}

void enableDebug()
{
    if (auto envPtr = std::getenv("PLDM_DEBUG"))
    {
        std::string value(envPtr);
        if (value == "1")
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "PLDM debug enabled");
            debug = true;
        }
    }
}

int main(void)
{
    auto ioc = std::make_shared<boost::asio::io_context>();
    setIoContext(ioc);
    boost::asio::signal_set signals(*ioc, SIGINT, SIGTERM);
    signals.async_wait(
        [&ioc](const boost::system::error_code&, const int sigNum) {
            pldm::platform::pauseSensorPolling();
            pldm::TIDMapper::TIDMap tidMap = pldm::tidMapper.getTIDMap();
            for (auto& [tid, eid] : tidMap)
            {
                deleteDevice(tid);
            }
            ioc->stop();
            signal(sigNum, SIG_DFL);
            raise(sigNum);
        });

    auto conn = std::make_shared<sdbusplus::asio::connection>(*ioc);

    auto objectServer = std::make_shared<sdbusplus::asio::object_server>(conn);
    objectServer->add_manager("/xyz/openbmc_project/sensors");
    conn->request_name(pldmService);
    setSdBus(conn);
    setObjServer(objectServer);

    enableDebug();

    // TODO - Read from entity manager about the transport bindings to be
    // supported by PLDM
    mctpw::MCTPConfiguration config(mctpw::MessageType::pldm,
                                    mctpw::BindingType::mctpOverSmBus);

    pldm::mctpWrapper = std::make_unique<mctpw::MCTPWrapper>(
        conn, config, onDeviceUpdate, pldm::msgRecvCallback);

    boost::asio::spawn(*ioc, [](boost::asio::yield_context yield) {
        pldm::mctpWrapper->detectMctpEndpoints(yield);
        mctpw::MCTPWrapper::EndpointMap eidMap =
            pldm::mctpWrapper->getEndpointMap();
        for (auto& [eid, service] : eidMap)
        {
            pldm::platform::pauseSensorPolling();
            initDevice(eid, yield);
            pldm::platform::resumeSensorPolling();
        }
    });

    ioc->run();

    return 0;
}
