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
#include "firmware_update.hpp"

#include "fwu_inventory.hpp"
#include "platform.hpp"
#include "pldm.hpp"
#include "pldm_fwu_image.hpp"

#include <filesystem>
#include <phosphor-logging/log.hpp>
#include <xyz/openbmc_project/PLDM/FWU/FWUBase/server.hpp>

#include "utils.h"
namespace pldm
{
namespace fwu
{

static std::unordered_map<
    pldm_tid_t, std::vector<std::unique_ptr<sdbusplus::asio::dbus_interface>>>
    fwuIface;

// Maximum timeout in milliseconds for fwu commad request
constexpr uint16_t timeout = 100;

// Timeout in milliseconds in between fwu command
constexpr uint16_t fdCmdTimeout = 5000;

// Maximum retry count
constexpr size_t retryCount = 3;

// Maximum delay in milliseconds used in between fwu commands
constexpr uint16_t delayBtw = 500;

// Time delay in milliseconds before retrying request update
constexpr uint16_t retryRequestForUpdateDelay = 5000;

// Time in milliseconds for the update agent to wait for request firmware
// data command
const uint32_t requestFirmwareDataIdleTimeoutMs = 90000;

// Maximum GetDeviceMetaData response count
constexpr size_t deviceMetaDataResponseCount = 100;

using FWUBase = sdbusplus::xyz::openbmc_project::PLDM::FWU::server::FWUBase;
extern std::map<pldm_tid_t, FDProperties> terminusFwuProperties;
std::shared_ptr<boost::asio::steady_timer> expectedCommandTimer = nullptr;
std::unique_ptr<PLDMImg> pldmImg = nullptr;
std::unique_ptr<FWUpdate> fwUpdate = nullptr;
std::unique_ptr<sdbusplus::asio::dbus_interface> associationsIntf = nullptr;
std::map<uint8_t, std::string> inventoryPaths;

FWUpdate::FWUpdate(const pldm_tid_t _tid, const uint8_t _deviceIDRecord) :
    reserveBWTimer(
        std::make_unique<boost::asio::steady_timer>(*getIoContext())),
    currentTid(_tid), currentDeviceIDRecord(_deviceIDRecord), state(FD_IDLE)
{
}

void FWUpdate::validateReqForFWUpdCmd(const pldm_tid_t tid,
                                      const uint8_t messageTag,
                                      const std::vector<uint8_t>& req)
{
    if (req.size() < hdrSize)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Invalid FW request");
        return;
    }
    const struct pldm_msg_hdr* msgHdr =
        reinterpret_cast<const pldm_msg_hdr*>(req.data());

    if (expectedCmd == PLDM_REQUEST_FIRMWARE_DATA &&
        msgHdr->command == PLDM_TRANSFER_COMPLETE)
    {
        expectedCmd = PLDM_TRANSFER_COMPLETE;
        fdTransferCompleted = true;
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("TransferComplete received from TID: " +
             std::to_string(currentTid))
                .c_str());
    }

    if (tid != currentTid || msgHdr->command != expectedCmd)
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("Firmware update in progress for TID: " +
             std::to_string(currentTid))
                .c_str());
        return;
    }
    msgTag = messageTag;
    fdReqMatched = true;
    fdReq = req;
    expectedCommandTimer->cancel();
    return;
}

bool FWUpdate::setMatchedFDDescriptors()
{
    auto itr = terminusFwuProperties.find(currentTid);
    if (itr == terminusFwuProperties.end())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("setMatchedFDDescriptors: targetFDProperties not found for "
             "TID: " +
             std::to_string(currentTid))
                .c_str());
        return false;
    }
    targetFDProperties = itr->second;
    return true;
}

bool FWUpdate::sendErrorCompletionCode(const boost::asio::yield_context yield,
                                       const uint8_t fdInstanceId,
                                       const uint8_t complCode,
                                       const uint8_t command)
{

    std::vector<uint8_t> pldmResp(PLDMCCOnlyResponse);
    struct pldm_msg* msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    int retVal = encode_cc_only_resp(fdInstanceId, PLDM_FWUP, command,
                                     complCode, msgResp);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "sendError: encode response failed",
            phosphor::logging::entry("TID=%d", currentTid),
            phosphor::logging::entry("RETVAL=%d", retVal));
        return false;
    }
    if (!sendPldmMessage(yield, currentTid, retryCount, msgTag, false,
                         pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "sendError: Failed to send PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return false;
    }
    return true;
}

void FWUpdate::terminateFwUpdate(const boost::asio::yield_context yield)
{
    bool8_t nonFunctioningComponentIndication = false;
    bitfield64_t nonFunctioningComponentBitmap = {};
    phosphor::logging::log<phosphor::logging::level::ERR>(
        "unexpected error: firmwareUpdate stopped");
    if (doCancelUpdate(yield, nonFunctioningComponentIndication,
                       nonFunctioningComponentBitmap) != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "unable to send/receive CancelUpdate");
    }
    if (isReserveBandwidthActive)
    {
        isReserveBandwidthActive = false;
        cancelReserveBWTimer();
        if (!releaseBandwidth(yield, currentTid, PLDM_FWUP))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "terminateFwUpdate: releaseBandwidth failed");
        }
    }

    return;
}

uint16_t FWUpdate::getApplicableComponentsCount(uint64_t val) const
{
    uint16_t updatableComponents = 0;
    while (val)
    {
        val &= (val - 1);
        updatableComponents++;
    }
    return updatableComponents;
}

bool FWUpdate::prepareRequestUpdateCommand()
{
    uint16_t tempShort = 0;
    updateProperties.max_transfer_size = PLDM_FWU_BASELINE_TRANSFER_SIZE;
    applicableComponentsVal = getApplicableComponents();
    updateProperties.no_of_comp =
        getApplicableComponentsCount(applicableComponentsVal);
    updateProperties.max_outstand_transfer_req = 1;
    if (!pldmImg->getDevIdRcrdProperty<uint16_t>(tempShort, "FWDevPkgDataLen",
                                                 currentDeviceIDRecord))
    {
        return false;
    }
    updateProperties.pkg_data_len = tempShort;
    if (!pldmImg->getDevIdRcrdProperty<uint8_t>(
            updateProperties.comp_image_set_ver_str_len, "ComImgSetVerStrLen",
            currentDeviceIDRecord))
    {
        return false;
    }
    if (!pldmImg->getDevIdRcrdProperty<uint8_t>(
            updateProperties.comp_image_set_ver_str_type, "ComImgSetVerStrType",
            currentDeviceIDRecord))
    {
        return false;
    }

    if (!pldmImg->getDevIdRcrdProperty<std::string>(
            componentImageSetVersionString, "CompImgSetVerStr",
            currentDeviceIDRecord))
    {
        return false;
    }
    return true;
}

bool FWUpdate::preparePassComponentRequest(
    struct pass_component_table_req& componentTable,
    std::string& compVersionString, const uint16_t compCnt)
{
    uint16_t tempShort = 0;
    uint32_t tempLong = 0;

    if (!pldmImg->getCompProperty<uint16_t>(tempShort, "CompClassification",
                                            compCnt))
    {
        return false;
    }
    componentTable.comp_classification = tempShort;
    componentTable.comp_classification_index = 0;
    if (!pldmImg->getCompProperty<uint32_t>(tempLong, "CompComparisonStamp",
                                            compCnt))
    {
        return false;
    }
    componentTable.comp_comparison_stamp = tempLong;
    if (!pldmImg->getCompProperty<uint16_t>(tempShort, "CompIdentifier",
                                            compCnt))
    {
        return false;
    }
    componentTable.comp_identifier = tempShort;
    if (!pldmImg->getCompProperty<uint8_t>(componentTable.comp_ver_str_len,
                                           "CompVerStrLen", compCnt))
    {
        return false;
    }
    if (!pldmImg->getCompProperty<uint8_t>(componentTable.comp_ver_str_type,
                                           "CmpVerStrType", compCnt))
    {
        return false;
    }
    if (!pldmImg->getCompProperty<std::string>(compVersionString, "CompVerStr",
                                               compCnt))
    {
        return false;
    }
    return initPassComponentTableTransferFlag(componentTable.transfer_flag);
}

bool FWUpdate::initPassComponentTableTransferFlag(uint8_t& flag)
{

    if (updateProperties.no_of_comp == 1)
    {
        flag = PLDM_START_AND_END;
        return true;
    }

    if (updateProperties.no_of_comp > 1)
    {
        if (passCompCount == 0)
        {
            flag = PLDM_START;
        }
        else if (passCompCount + 1 < updateProperties.no_of_comp)
        {
            flag = PLDM_MIDDLE;
        }
        else if (passCompCount + 1 == updateProperties.no_of_comp)
        {
            flag = PLDM_END;
        }
        passCompCount++;
        return true;
    }

    return false;
}

bool FWUpdate::prepareUpdateComponentRequest(
    std::string& compVersionString, struct update_component_req& component)
{
    uint16_t tempShort = 0;
    uint32_t tempLong = 0;

    if (!pldmImg->getCompProperty<uint16_t>(tempShort, "CompClassification",
                                            currentComp))
    {
        return false;
    }
    component.comp_classification = tempShort;
    if (!pldmImg->getCompProperty<uint16_t>(tempShort, "CompIdentifier",
                                            currentComp))
    {
        return false;
    }
    component.comp_identifier = tempShort;
    component.comp_classification_index = 0;
    if (!pldmImg->getCompProperty<uint32_t>(tempLong, "CompComparisonStamp",
                                            currentComp))
    {
        return false;
    }
    component.comp_comparison_stamp = tempLong;
    if (!pldmImg->getCompProperty<uint32_t>(tempLong, "CompSize", currentComp))
    {
        return false;
    }
    component.comp_image_size = tempLong;
    component.update_option_flags = {};
    if (!pldmImg->getCompProperty<uint8_t>(component.comp_ver_str_type,
                                           "CmpVerStrType", currentComp))
    {
        return false;
    }
    if (!pldmImg->getCompProperty<uint8_t>(component.comp_ver_str_len,
                                           "CompVerStrLen", currentComp))
    {
        return false;
    }
    if (!pldmImg->getCompProperty<std::string>(compVersionString, "CompVerStr",
                                               currentComp))
    {
        return false;
    }
    return true;
}

int FWUpdate::processRequestUpdate(const boost::asio::yield_context yield)

{
    if (updateMode)
    {
        return ALREADY_IN_UPDATE_MODE;
    }
    if (fdState != FD_IDLE)
    {
        return NOT_IN_UPDATE_MODE;
    }

    variable_field compImgSetVerStr;
    if (!prepareRequestUpdateCommand())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "RequestUpdateCommand preparation failed");
        return PLDM_ERROR;
    }
    compImgSetVerStr.ptr = reinterpret_cast<const uint8_t*>(
        componentImageSetVersionString.c_str());
    compImgSetVerStr.length = componentImageSetVersionString.length();
    return requestUpdate(yield, compImgSetVerStr);
}

int FWUpdate::requestUpdate(const boost::asio::yield_context yield,
                            struct variable_field& compImgSetVerStrn)
{

    uint8_t instanceID = createInstanceId(currentTid);
    std::vector<uint8_t> pldmReq(sizeof(struct PLDMEmptyRequest) +
                                 sizeof(struct request_update_req) +
                                 compImgSetVerStrn.length);
    struct pldm_msg* msgReq = reinterpret_cast<pldm_msg*>(pldmReq.data());

    int retVal = encode_request_update_req(
        instanceID, msgReq,
        sizeof(struct request_update_req) + compImgSetVerStrn.length,
        &updateProperties, &compImgSetVerStrn);
    if (!validatePLDMReqEncode(currentTid, retVal, "RequestUpdate"))
    {
        return retVal;
    }
    std::vector<uint8_t> pldmResp;
    size_t count = 0;
    do
    {
        if (retVal == RETRY_REQUEST_UPDATE)
        {
            createAsyncDelay(yield, retryRequestForUpdateDelay);
        }
        if (!sendReceivePldmMessage(yield, currentTid, timeout, retryCount,
                                    pldmReq, pldmResp))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "requestUpdate: Failed to send or receive PLDM message",
                phosphor::logging::entry("TID=%d", currentTid));
            return PLDM_ERROR;
        }
        auto msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
        retVal = decode_request_update_resp(
            msgResp, pldmResp.size() - hdrSize, &completionCode,
            &fwDeviceMetaDataLen, &fdWillSendGetPkgDataCmd);
    } while ((completionCode == RETRY_REQUEST_UPDATE) &&
             (++count < retryCount));
    if (completionCode == RETRY_REQUEST_UPDATE)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("requestUpdate: FD is not able to enter update mode immediately, "
             "requests for retry and count: " +
             std::to_string(++count))
                .c_str());

        return retVal;
    }
    if (!validatePLDMRespDecode(currentTid, retVal, completionCode,
                                "RequestUpdate"))
    {
        return retVal;
    }
    return PLDM_SUCCESS;
}

int FWUpdate::processGetDeviceMetaData(const boost::asio::yield_context yield)
{
    if (!updateMode)
    {
        return NOT_IN_UPDATE_MODE;
    }
    if (fdState != FD_LEARN_COMPONENTS)
    {
        return COMMAND_NOT_EXPECTED;
    }
    if (!fwDeviceMetaDataLen)
    {
        return PLDM_SUCCESS;
    }

    // GetDeviceMetaData
    uint32_t dataTransferHandle = 0;
    uint32_t nextDataTransferHandle = 0;

    uint8_t transferOperationFlag = PLDM_GET_FIRSTPART;
    uint8_t nextTransferFlag = PLDM_START;

    // Count responses received
    uint8_t responseCount = 0;

    while ((nextTransferFlag != PLDM_START_AND_END) &&
           (nextTransferFlag != PLDM_END))
    {
        int retVal =
            getDeviceMetaData(yield, dataTransferHandle, transferOperationFlag,
                              nextDataTransferHandle, nextTransferFlag);

        if (retVal != PLDM_SUCCESS)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("GetDeviceMetaData failed with retVal " +
                 std::to_string(retVal))
                    .c_str());
            return retVal;
        }
        dataTransferHandle = nextDataTransferHandle;
        transferOperationFlag = PLDM_GET_NEXTPART;

        // Limiting number of times while loop executes
        if (responseCount++ >= deviceMetaDataResponseCount)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "GetDeviceMetaData responses exceed limit");

            fwDeviceMetaData.clear();
            return PLDM_ERROR;
        }
    }
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        (std::string("GetDeviceMetaData successful. Received bytes ") +
         std::to_string(fwDeviceMetaData.size()))
            .c_str());
    return PLDM_SUCCESS;
}

int FWUpdate::getDeviceMetaData(const boost::asio::yield_context yield,
                                const uint32_t dataTransferHandle,
                                const uint8_t transferOperationFlag,
                                uint32_t& nextDataTransferHandle,
                                uint8_t& transferFlag)
{

    uint8_t instanceID = createInstanceId(currentTid);
    std::vector<uint8_t> pldmReq(sizeof(struct PLDMEmptyRequest) +
                                 sizeof(struct get_device_meta_data_req));
    struct pldm_msg* msgReq = reinterpret_cast<pldm_msg*>(pldmReq.data());

    int retVal = encode_get_device_meta_data_req(
        instanceID, msgReq, sizeof(struct get_device_meta_data_req),
        dataTransferHandle, transferOperationFlag);
    if (!validatePLDMReqEncode(currentTid, retVal, "GetDeviceMetaData"))
    {
        return retVal;
    }

    // Send GetDeviceMetaData Command
    std::vector<uint8_t> pldmResp;
    if (!sendReceivePldmMessage(yield, currentTid, timeout, retryCount, pldmReq,
                                pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "GetDeviceMetaData: Failed to send or receive PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return PLDM_ERROR;
    }
    struct variable_field metaData = {};
    auto msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());

    // Decode received response
    retVal = decode_get_device_meta_data_resp(
        msgResp, pldmResp.size() - hdrSize, &completionCode,
        &nextDataTransferHandle, &transferFlag, &metaData);
    if (!validatePLDMRespDecode(currentTid, retVal, completionCode,
                                "GetDeviceMetaData"))
    {
        return retVal;
    }

    // Save metadata
    std::copy(metaData.ptr, metaData.ptr + metaData.length,
              back_inserter(fwDeviceMetaData));

    return PLDM_SUCCESS;
}

int FWUpdate::processSendMetaData(const boost::asio::yield_context yield)
{

    if (fdState == FD_LEARN_COMPONENTS || fdState == FD_IDLE)
    {
        return COMMAND_NOT_EXPECTED;
    }
    if ((fwDeviceMetaDataLen == 0) || (fwDeviceMetaData.size() == 0))
    {
        return PLDM_SUCCESS;
    }
    transferHandle = 0; // Resetting transferHandle
    expectedCmd = PLDM_GET_META_DATA;

    size_t offset = 0;
    int retVal = 0;

    size_t length = PLDM_FWU_BASELINE_TRANSFER_SIZE; // max payload size

    // Calculate based on size of payload and maximum transfer size
    // Max number of requests including the requeries
    size_t maxNumReq = findMaxNumReq(fwDeviceMetaData.size());
    // Max number of unique requests (excluding requeries)
    size_t numExpectedRequests = calcMaxNumReq(fwDeviceMetaData.size());

    std::set<uint32_t> recvdRequests;

    if (maxNumReq == 0)
    {
        return PLDM_ERROR;
    }

    while (maxNumReq--)
    {
        startTimer(yield, fdCmdTimeout);
        if (!fdReqMatched)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "TimeoutWaiting for processsendMetaData packet");

            retVal = PLDM_ERROR;
            break;
        }

        retVal = sendMetaData(yield, offset, length, recvdRequests);
        if (retVal != PLDM_SUCCESS)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                ("processSendMetaData: Failed to run "
                 "sendMetaData"
                 "command, retVal=" +
                 std::to_string(retVal))
                    .c_str());

            break;
        }
        fdReq.clear();
        fdReqMatched = false;

        // Confirm if meta data is been transferred completely
        if (recvdRequests.size() == numExpectedRequests)
        {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "sendMetaData successful");
            expectedCmd = 0; // clear expected command
            return retVal;
        }

        expectedCmd = PLDM_GET_META_DATA;
    }

    if (maxNumReq == 0)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("processSendMetaData: Failed as requests exceed limit "));
    }
    // Clear expected command.
    expectedCmd = 0;
    fdReq.clear();
    return retVal;
}

int FWUpdate::sendMetaData(const boost::asio::yield_context yield,
                           size_t& offset, size_t& length,
                           std::set<uint32_t>& recvdRequests)
{

    const struct pldm_msg* msgReq =
        reinterpret_cast<const pldm_msg*>(fdReq.data());

    uint32_t dataTransferHandle = 1;
    uint8_t transferOperationFlag = PLDM_GET_FIRSTPART;

    size_t dataSize = 0;

    int retVal =
        decode_get_meta_data_req(msgReq, sizeof(struct get_fd_data_req),
                                 &dataTransferHandle, &transferOperationFlag);
    if (retVal != PLDM_SUCCESS)
    {

        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("sendMetaData: decode request failed"
             "RETVAL=" +
             std::to_string(retVal))
                .c_str());

        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     static_cast<uint8_t>(retVal),
                                     PLDM_GET_META_DATA))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "sendMetaData: Failed to send PLDM message");
        }
        return retVal;
    }
    // GetFirstPart can be received in 2 cases
    // 1. first request to start the data transfer
    // 2. If the FD sends GetFirstPart in any upcoming request of the same
    // command
    // then we are supposed to start the transfer starting from
    // start of the meta data again.
    // In both the cases transfer should start from start of meta data.
    if (transferOperationFlag == PLDM_GET_FIRSTPART)
    {
        offset = 0;
        length = PLDM_FWU_BASELINE_TRANSFER_SIZE;
        // Ignoring the very first dataTransferHandle received and setting it as
        // 0.
        recvdRequests.insert(0);
    }
    else
    {
        offset = dataTransferHandle * PLDM_FWU_BASELINE_TRANSFER_SIZE;
        recvdRequests.insert(dataTransferHandle);
    }

    dataSize = fwDeviceMetaData.size();

    // If dataSize is not multiple of max tranfer unit, last packet will have
    // payload which is less that max transfer unit

    if (offset + length > dataSize)
    {
        if (offset < dataSize)
        {
            length = dataSize -
                     offset; // To calculate actual length depending on the what
                             // data size  left and what offset position
        }
        else
        {
            if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                         PLDM_ERROR, PLDM_GET_META_DATA))
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "sendMetaData: Failed to send PLDM message");
            }
            return PLDM_ERROR;
        }
    }

    struct get_fd_data_resp dataHeader;
    dataHeader.completion_code = PLDM_SUCCESS;
    // Add a member variable and increment
    dataHeader.next_data_transfer_handle = ++dataTransferHandle;

    // Setting the Transfer flag that indiates what part of the transfer this
    // response represents
    dataHeader.transfer_flag = setTransferFlag(offset, length, dataSize);

    // Set the Portion of Metadata using offset and length
    struct variable_field portionOfData = {};

    portionOfData.length = length;
    portionOfData.ptr = fwDeviceMetaData.data() + offset;

    offset += length;

    /* header plus requested data length */
    size_t respLen = sizeof(struct PLDMEmptyRequest) +
                     sizeof(struct get_fd_data_resp) + length;

    std::vector<uint8_t> pldmResp(respLen);
    struct pldm_msg* msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    retVal = encode_get_meta_data_resp(msgReq->hdr.instance_id, respLen,
                                       msgResp, &dataHeader, &portionOfData);

    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("sendMetaData: encode request failed"
             "RETVAL=" +
             std::to_string(retVal))
                .c_str());
        return retVal;
    }

    if (!sendPldmMessage(yield, currentTid, retryCount, msgTag, false,
                         pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "sendMetaData: Failed to send PLDM message");
        return PLDM_ERROR;
    }

    return PLDM_SUCCESS;
}

int FWUpdate::processPassComponentTable(const boost::asio::yield_context yield)
{
    if (!updateMode)
    {
        return NOT_IN_UPDATE_MODE;
    }
    if (fdState != FD_LEARN_COMPONENTS)
    {
        return COMMAND_NOT_EXPECTED;
    }
    uint8_t totalCompsAcceptedByFd = 0;
    for (uint16_t count = 0; count < compCount; ++count)
    {
        struct pass_component_table_req componentTable = {};
        struct variable_field componentVersionString = {};
        std::string versionStr;
        uint8_t compResp = 0;
        uint8_t compRespCode = 0;
        currentComp = count;

        if (!isComponentApplicable())
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("component not applicable. Skipping PassComponentRequest for "
                 "COMPONENT: " +
                 std::to_string(currentComp))
                    .c_str());
            continue;
        }
        if (!preparePassComponentRequest(componentTable, versionStr, count))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "processPassComponentTable: PassComponentRequest preparation "
                "failed");
            return PLDM_ERROR;
        }

        componentVersionString.ptr =
            reinterpret_cast<const uint8_t*>(versionStr.c_str());
        componentVersionString.length = versionStr.length();
        int retVal =
            passComponentTable(yield, componentTable, componentVersionString,
                               compResp, compRespCode);
        if (retVal != PLDM_SUCCESS)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("PassComponentTable command failed, component: " +
                 std::to_string(count) + " retVal: " + std::to_string(retVal))
                    .c_str());

            continue;
        }
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("PassComponentTable command success, component: " +
             std::to_string(count))
                .c_str());
        ++totalCompsAcceptedByFd;
        createAsyncDelay(yield, delayBtw);
    }
    return (totalCompsAcceptedByFd > 0) ? PLDM_SUCCESS : PLDM_ERROR;
}

int FWUpdate::passComponentTable(
    const boost::asio::yield_context yield,
    const struct pass_component_table_req& componentTable,
    struct variable_field& compImgSetVerStr, uint8_t& compResp,
    uint8_t& compRespCode)
{
    uint8_t instanceID = createInstanceId(currentTid);
    std::vector<uint8_t> pldmReq(sizeof(struct PLDMEmptyRequest) +
                                 sizeof(struct pass_component_table_req) +
                                 compImgSetVerStr.length);
    struct pldm_msg* msgReq = reinterpret_cast<pldm_msg*>(pldmReq.data());

    int retVal = encode_pass_component_table_req(
        instanceID, msgReq,
        sizeof(struct pass_component_table_req) + compImgSetVerStr.length,
        &componentTable, &compImgSetVerStr);
    if (!validatePLDMReqEncode(currentTid, retVal,
                               std::string("PassComponentTable")))
    {
        return retVal;
    }
    std::vector<uint8_t> pldmResp;
    if (!sendReceivePldmMessage(yield, currentTid, timeout, retryCount, pldmReq,
                                pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "passComponentTable: Failed to send or receive PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return PLDM_ERROR;
    }
    auto msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    retVal = decode_pass_component_table_resp(
        msgResp, pldmResp.size() - hdrSize, &completionCode, &compResp,
        &compRespCode);
    if (!validatePLDMRespDecode(currentTid, retVal, completionCode,
                                std::string("PassComponentTable")))
    {
        return retVal;
    }

    return PLDM_SUCCESS;
}

int FWUpdate::processUpdateComponent(const boost::asio::yield_context yield,
                                     uint8_t& compCompatabilityResp,
                                     uint8_t& compCompatabilityRespCode,
                                     bitfield32_t& updateOptFlagsEnabled,
                                     uint16_t& estimatedTimeReqFd)
{
    if (!updateMode)
    {
        return NOT_IN_UPDATE_MODE;
    }
    if (fdState != FD_READY_XFER)
    {
        return COMMAND_NOT_EXPECTED;
    }
    variable_field componentVersionString;
    struct update_component_req component;
    std::string versionStr;

    if (!prepareUpdateComponentRequest(versionStr, component))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "UpdateComponentRequest preparation failed");

        return PLDM_SUCCESS;
    }
    componentVersionString.ptr =
        reinterpret_cast<const uint8_t*>(versionStr.c_str());
    componentVersionString.length = versionStr.length();
    return updateComponent(yield, component, componentVersionString,
                           compCompatabilityResp, compCompatabilityRespCode,
                           updateOptFlagsEnabled, estimatedTimeReqFd);
}

int FWUpdate::updateComponent(const boost::asio::yield_context yield,
                              const struct update_component_req& component,
                              variable_field& compVerStr,
                              uint8_t& compCompatabilityResp,
                              uint8_t& compCompatabilityRespCode,
                              bitfield32_t& updateOptFlagsEnabled,
                              uint16_t& estimatedTimeReqFd)
{
    uint8_t instanceID = createInstanceId(currentTid);
    std::vector<uint8_t> pldmReq(sizeof(struct PLDMEmptyRequest) +
                                 sizeof(struct update_component_req) +
                                 compVerStr.length);
    struct pldm_msg* msgReq = reinterpret_cast<pldm_msg*>(pldmReq.data());

    int retVal = encode_update_component_req(
        instanceID, msgReq,
        sizeof(struct update_component_req) + compVerStr.length, &component,
        &compVerStr);
    if (!validatePLDMReqEncode(currentTid, retVal, "UpdateComponent"))
    {
        return retVal;
    }
    std::vector<uint8_t> pldmResp;
    if (!sendReceivePldmMessage(yield, currentTid, timeout, retryCount, pldmReq,
                                pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "updateComponent: Failed to send or receive PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return PLDM_ERROR;
    }
    auto msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    retVal = decode_update_component_resp(
        msgResp, pldmResp.size() - hdrSize, &completionCode,
        &compCompatabilityResp, &compCompatabilityRespCode,
        &updateOptFlagsEnabled, &estimatedTimeReqFd);
    if (!validatePLDMRespDecode(currentTid, retVal, completionCode,
                                "UpdateComponent"))
    {
        return retVal;
    }

    return PLDM_SUCCESS;
}

uint8_t FWUpdate::validateTransferComplete(const uint8_t transferResult)
{
    return (transferResult == PLDM_FWU_TRASFER_SUCCESS)
               ? PLDM_SUCCESS
               : PLDM_ERROR_INVALID_DATA;
}

int FWUpdate::processTransferComplete(const boost::asio::yield_context yield,
                                      const std::vector<uint8_t>& pldmReq,
                                      uint8_t& transferResult)
{
    if (!updateMode || fdState != FD_DOWNLOAD)
    {
        const struct pldm_msg* msgReq =
            reinterpret_cast<const pldm_msg*>(pldmReq.data());
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     COMMAND_NOT_EXPECTED,
                                     PLDM_TRANSFER_COMPLETE))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "TransferComplete: sendErrorCompletionCode failed");
        }
        return COMMAND_NOT_EXPECTED;
    }
    int retVal = transferComplete(yield, pldmReq, transferResult);
    if (retVal != PLDM_SUCCESS)
    {
        return retVal;
    }

    return PLDM_SUCCESS;
}

int FWUpdate::transferComplete(const boost::asio::yield_context yield,
                               const std::vector<uint8_t>& pldmReq,
                               uint8_t& transferResult)
{
    const struct pldm_msg* msgReq =
        reinterpret_cast<const pldm_msg*>(pldmReq.data());
    int retVal = decode_transfer_complete_req(msgReq, &transferResult);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("transferComplete: decode request failed. RETVAL:" +
             std::to_string(retVal))
                .c_str());
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     static_cast<uint8_t>(retVal),
                                     PLDM_TRANSFER_COMPLETE))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "transferComplete: sendErrorCompletionCode failed.");
        }
        return retVal;
    }
    retVal = validateTransferComplete(transferResult);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("transferComplete: invalid transferResult. transferResult: " +
             std::to_string(transferResult))
                .c_str());
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     static_cast<uint8_t>(retVal),
                                     PLDM_TRANSFER_COMPLETE))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "transferComplete: sendErrorCompletionCode failed");
        }
        return retVal;
    }
    std::vector<uint8_t> pldmResp(PLDMCCOnlyResponse);
    struct pldm_msg* msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    retVal = encode_transfer_complete_resp(
        msgReq->hdr.instance_id, static_cast<uint8_t>(retVal), msgResp);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("transferComplete: encode response failed. RETVAL:" +
             std::to_string(retVal))
                .c_str());
        return retVal;
    }
    // tag Owner bit cleared to false for respose message
    if (!sendPldmMessage(yield, currentTid, retryCount, msgTag, false,
                         pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "TransferComplete: Failed to send PLDM message");
        return PLDM_ERROR;
    }
    return PLDM_SUCCESS;
}

uint8_t FWUpdate::validateVerifyComplete(const uint8_t verifyResult)
{
    return (verifyResult == PLDM_FWU_VERIFY_SUCCESS) ? PLDM_SUCCESS
                                                     : PLDM_ERROR_INVALID_DATA;
}

int FWUpdate::processVerifyComplete(const boost::asio::yield_context yield,
                                    const std::vector<uint8_t>& pldmReq,
                                    uint8_t& verifyResult)
{
    if (!updateMode || fdState != FD_VERIFY)
    {
        auto msgReq = reinterpret_cast<const pldm_msg*>(pldmReq.data());
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     COMMAND_NOT_EXPECTED,
                                     PLDM_VERIFY_COMPLETE))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "VerifyComplete: sendErrorCompletionCode failed");
        }
        return COMMAND_NOT_EXPECTED;
    }
    int retVal = verifyComplete(yield, pldmReq, verifyResult);
    if (retVal != PLDM_SUCCESS)
    {
        return retVal;
    }

    return PLDM_SUCCESS;
}

int FWUpdate::verifyComplete(const boost::asio::yield_context yield,
                             const std::vector<uint8_t>& pldmReq,
                             uint8_t& verifyResult)
{

    auto msgReq = reinterpret_cast<const pldm_msg*>(pldmReq.data());
    int retVal = decode_verify_complete_req(msgReq, &verifyResult);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("verifyComplete: decode request failed. RETVAL:" +
             std::to_string(retVal))
                .c_str());
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     static_cast<uint8_t>(retVal),
                                     PLDM_VERIFY_COMPLETE))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "verifyComplete: sendErrorCompletionCode failed");
        }
        return retVal;
    }
    retVal = validateVerifyComplete(verifyResult);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("verifyComplete: invalid verifyResult. verifyResult: " +
             std::to_string(verifyResult))
                .c_str());
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     static_cast<uint8_t>(retVal),
                                     PLDM_VERIFY_COMPLETE))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "verifyComplete: sendErrorCompletionCode failed.");
        }
        return retVal;
    }
    std::vector<uint8_t> pldmResp(PLDMCCOnlyResponse);
    struct pldm_msg* msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    retVal = encode_verify_complete_resp(msgReq->hdr.instance_id,
                                         static_cast<uint8_t>(retVal), msgResp);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("VerifyComplete: encode response failed. RETVAL:" +
             std::to_string(retVal))
                .c_str());
        return retVal;
    }
    // tag Owner bit cleared to false for respose message
    if (!sendPldmMessage(yield, currentTid, retryCount, msgTag, false,
                         pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "verifyComplete: sendErrorCompletionCode failed.");
        return PLDM_ERROR;
    }
    return PLDM_SUCCESS;
}

uint8_t FWUpdate::validateApplyComplete(const uint8_t applyResult)
{
    if (applyResult == PLDM_FWU_APPLY_SUCCESS ||
        applyResult == PLDM_FWU_APPLY_SUCCESS_WITH_ACTIVATION_METHOD)
    {
        return PLDM_SUCCESS;
    }
    return PLDM_ERROR_INVALID_DATA;
}

int FWUpdate::processApplyComplete(
    const boost::asio::yield_context yield, const std::vector<uint8_t>& pldmReq,
    uint8_t& applyResult, bitfield16_t& compActivationMethodsModification)
{
    if (!updateMode || fdState != FD_APPLY)
    {
        auto msgReq = reinterpret_cast<const pldm_msg*>(pldmReq.data());
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     COMMAND_NOT_EXPECTED, PLDM_APPLY_COMPLETE))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "ApplyComplete: Failed to send PLDM message",
                phosphor::logging::entry("TID=%d", currentTid));
        }
        return COMMAND_NOT_EXPECTED;
    }
    int retVal = applyComplete(yield, pldmReq, applyResult,
                               compActivationMethodsModification);
    if (retVal != PLDM_SUCCESS)
    {
        return retVal;
    }

    return PLDM_SUCCESS;
}

int FWUpdate::applyComplete(const boost::asio::yield_context yield,
                            const std::vector<uint8_t>& pldmReq,
                            uint8_t& applyResult,
                            bitfield16_t& compActivationMethodsModification)
{

    auto msgReq = reinterpret_cast<const pldm_msg*>(pldmReq.data());
    int retVal = decode_apply_complete_req(msgReq, pldmReq.size() - hdrSize,
                                           &applyResult,
                                           &compActivationMethodsModification);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ApplyComplete: decode request failed",
            phosphor::logging::entry("TID=%d", currentTid),
            phosphor::logging::entry("RETVAL=%d", retVal));
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     static_cast<uint8_t>(retVal),
                                     PLDM_APPLY_COMPLETE))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "ApplyComplete: Failed to send PLDM message",
                phosphor::logging::entry("TID=%d", currentTid));
        }
        return retVal;
    }

    uint8_t compCode = validateApplyComplete(applyResult);
    std::vector<uint8_t> pldmResp(PLDMCCOnlyResponse);
    struct pldm_msg* msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    retVal =
        encode_apply_complete_resp(msgReq->hdr.instance_id, compCode, msgResp);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ApplyComplete: encode response failed",
            phosphor::logging::entry("TID=%d", currentTid),
            phosphor::logging::entry("RETVAL=%d", retVal));
        return retVal;
    }
    // tag Owner bit cleared to false for respose message
    if (!sendPldmMessage(yield, currentTid, retryCount, msgTag, false,
                         pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ApplyComplete: Failed to send PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return PLDM_ERROR;
    }

    return PLDM_SUCCESS;
}

int FWUpdate::processRequestFirmwareData(const boost::asio::yield_context yield)
{
    if (!updateMode || fdState != FD_DOWNLOAD)
    {
        return COMMAND_NOT_EXPECTED;
    }
    uint32_t offset = 0;
    uint32_t length = 0;
    int retVal = PLDM_ERROR;
    int prevProgress = 0;
    // Log interval for progess percentage
    constexpr int progressPercentLogLimit = 25;
    uint32_t componentSize;
    uint32_t componentOffset;
    if (!pldmImg->getCompProperty<uint32_t>(componentSize, "CompSize",
                                            currentComp))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("Failed to get component size. COMPONENT: " +
             std::to_string(currentComp))
                .c_str());
        return PLDM_ERROR;
    }
    if (!pldmImg->getCompProperty<uint32_t>(componentOffset,
                                            "CompLocationOffset", currentComp))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("Failed to get component location offset. COMPONENT: " +
             std::to_string(currentComp))
                .c_str());
        return PLDM_ERROR;
    }
    uint32_t maxNumReq = findMaxNumReq(componentSize);
    initialize_fw_update(updateProperties.max_transfer_size, componentSize);

    while (--maxNumReq)
    {
        startTimer(yield, requestFirmwareDataIdleTimeoutMs);
        if (!fdReqMatched)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("TimeoutWaiting for requestFirmwareData packet. COMPONENT: " +
                 std::to_string(currentComp))
                    .c_str());
            return PLDM_ERROR;
        }
        fdReqMatched = false;

        if (fdTransferCompleted)
        {
            fdTransferCompleted = false;
            break;
        }
        retVal = requestFirmwareData(yield, fdReq, offset, length,
                                     componentSize, componentOffset);
        if (retVal != PLDM_SUCCESS)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("processRequestFirmwareData: Failed to run "
                 "RequestFirmwareData"
                 "command, retVal=" +
                 std::to_string(retVal) +
                 " component=" + std::to_string(currentComp))
                    .c_str());
            continue;
        }
        fdReq.clear();
        int progress = ((offset + length) * 100) / componentSize;
        if ((progress - prevProgress) >= progressPercentLogLimit)
        {
            prevProgress = progress;
            phosphor::logging::log<phosphor::logging::level::INFO>(
                ("TID: " + std::to_string(currentTid) +
                 " Component: " + std::to_string(currentComp + 1) +
                 " update package transfered: " + std::to_string(progress) +
                 "%")
                    .c_str());
        }
        if (offset + length > componentSize)
        {
            expectedCmd = PLDM_TRANSFER_COMPLETE;
            break;
        }
    }
    if (!maxNumReq)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Exceeded maximum no of RequestFirmwareData requests");
        return PLDM_ERROR;
    }

    return retVal;
}

int FWUpdate::requestFirmwareData(const boost::asio::yield_context yield,
                                  const std ::vector<uint8_t>& pldmReq,
                                  uint32_t& offset, uint32_t& length,
                                  const uint32_t componentSize,
                                  const uint32_t componentOffset)
{
    const struct pldm_msg* msgReq =
        reinterpret_cast<const pldm_msg*>(pldmReq.data());
    int retVal = decode_request_firmware_data_req(
        msgReq, pldmReq.size() - hdrSize, &offset, &length);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "requestfirmware: decode request failed",
            phosphor::logging::entry("TID=%d", currentTid),
            phosphor::logging::entry("RETVAL=%d", retVal));
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     static_cast<uint8_t>(retVal),
                                     PLDM_REQUEST_FIRMWARE_DATA))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "RequestFirmwareData: Failed to send PLDM message",
                phosphor::logging::entry("TID=%d", currentTid));
        }
        return retVal;
    }

    /* completion code plus requested data length */
    size_t payload_length = 1 + length;

    std::vector<uint8_t> pldmResp(PLDMCCOnlyResponse + length);
    std ::vector<uint8_t> data(length);
    if (offset + length > componentSize)
    {
        if (offset < componentSize)
        {
            length = componentSize - offset;
        }
        else
        {
            if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                         PLDM_ERROR,
                                         PLDM_REQUEST_FIRMWARE_DATA))
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "RequestFirmwareData: Failed to send PLDM message",
                    phosphor::logging::entry("TID=%d", currentTid));
            }
            return PLDM_ERROR;
        }
    }

    if (!pldmImg->readData(offset + componentOffset, data, length))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "update image read failed",
            phosphor::logging::entry("TID=%d", currentTid));
        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id, PLDM_ERROR,
                                     PLDM_REQUEST_FIRMWARE_DATA))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "RequestFirmwareData: Failed to send PLDM message",
                phosphor::logging::entry("TID=%d", currentTid));
        }
        return PLDM_ERROR;
    }

    struct variable_field componentImagePortion = {};
    componentImagePortion.length = data.size();
    componentImagePortion.ptr = data.data();
    struct pldm_msg* msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    retVal = encode_request_firmware_data_resp(msgReq->hdr.instance_id, msgResp,
                                               payload_length, completionCode,
                                               &componentImagePortion);

    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "requestfirmware: encode request failed",
            phosphor::logging::entry("TID=%d", currentTid),
            phosphor::logging::entry("RETVAL=%d", retVal));
        return retVal;
    }

    // tag Owner bit cleared to false for respose message
    if (!sendPldmMessage(yield, currentTid, retryCount, msgTag, false,
                         pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "requestFirmwareData: Failed to send PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return PLDM_ERROR;
    }

    return PLDM_SUCCESS;
}

size_t FWUpdate::calcMaxNumReq(const size_t dataSize)
{
    if (!dataSize)
    {
        return 0;
    }

    size_t maxNumReq = dataSize / PLDM_FWU_BASELINE_TRANSFER_SIZE;

    if (dataSize % PLDM_FWU_BASELINE_TRANSFER_SIZE > 0)
    {
        maxNumReq = maxNumReq + 1;
    }

    return maxNumReq;
}

int FWUpdate::processSendPackageData(const boost::asio::yield_context yield)
{
    if (fdState != FD_LEARN_COMPONENTS || !updateMode)
    {
        return COMMAND_NOT_EXPECTED;
    }
    /* fdWillSendGetPkgDataCmd will be set to 0x01 if there is package data
     * that the FD should obtain
     */
    if (fdWillSendGetPkgDataCmd != 0x01)
    {
        return PLDM_SUCCESS;
    }

    // get package data from pldmImg
    if (!pldmImg->getDevIdRcrdProperty<std::vector<uint8_t>>(
            packageData, "FirmwareDevicePackageData", currentDeviceIDRecord) ||
        !packageData.size())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to get FirmwareDevicePackageData or packageData size is 0");
        return PLDM_ERROR;
    }
    expectedCmd = PLDM_GET_PACKAGE_DATA;

    size_t offset = 0;
    int retVal = 0;
    size_t length = PLDM_FWU_BASELINE_TRANSFER_SIZE; // max payload size
    const size_t dataSize = packageData.size();

    // Calculate based on size of payload and maximum transfer size
    // Max number of requests including the requeries
    size_t maxNumReq = findMaxNumReq(dataSize);
    // Max number of unique requests (excluding requeries)
    size_t numExpectedRequests = calcMaxNumReq(dataSize);

    std::set<uint32_t> recvdRequests;

    while (maxNumReq--)
    {
        startTimer(yield, fdCmdTimeout);
        if (!fdReqMatched)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "TimeoutWaiting for packageData packet");
            break;
        }

        retVal = sendPackageData(yield, offset, length, recvdRequests);
        if (retVal != PLDM_SUCCESS)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("processSendPackageData: Failed to run "
                 "sendPackageData"
                 "command, retVal=" +
                 std::to_string(retVal))
                    .c_str());
            fdReq.clear();
            break;
        }
        fdReq.clear();
        fdReqMatched = false;

        // Confirm if complete package data is been transferred
        if (recvdRequests.size() == numExpectedRequests)
        {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "sendPackageData successful");
            expectedCmd = 0; // clear expected command
            return retVal;
        }

        expectedCmd = PLDM_GET_PACKAGE_DATA;
    }

    if (maxNumReq == 0)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("processSendPackageData: Failed as requests exceed limit "));
    }
    expectedCmd = 0; // clear expected command
    return retVal;
}

int FWUpdate::sendPackageData(const boost::asio::yield_context yield,
                              size_t& offset, size_t& length,
                              std::set<uint32_t>& recvdRequests)
{
    uint32_t dataTransferHandle = 1;
    uint8_t transferOperationFlag = PLDM_GET_FIRSTPART;
    size_t dataSize = 0;

    const struct pldm_msg* msgReq =
        reinterpret_cast<const pldm_msg*>(fdReq.data());

    int retVal = decode_get_pacakge_data_req(
        msgReq, sizeof(struct get_fd_data_req), &dataTransferHandle,
        &transferOperationFlag);

    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("sendPackageData: decode request failed"
             "RETVAL=" +
             std::to_string(retVal))
                .c_str());

        if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                     static_cast<uint8_t>(retVal),
                                     PLDM_GET_PACKAGE_DATA))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "sendPackageData: Failed to send PLDM message");
        }
        return retVal;
    }
    // GetFirstPart can be received in 2 cases
    // 1. first request to start the data transfer
    // 2. If the FD sends GetFirstPart in any upcoming request of the same
    // command
    //   then we are supposed to start the transfer starting from
    //   start of the package data again.
    // In both the cases transfer should start from start of the package data.
    if (transferOperationFlag == PLDM_GET_FIRSTPART)
    {
        offset = 0;
        length = PLDM_FWU_BASELINE_TRANSFER_SIZE;
        // Ignoring the very first dataTransferHandle received and setting it as
        // 0.
        recvdRequests.insert(0);
    }
    else
    {
        offset = dataTransferHandle * PLDM_FWU_BASELINE_TRANSFER_SIZE;
        recvdRequests.insert(dataTransferHandle);
    }

    dataSize = packageData.size();

    // If dataSize is not multiple of max transfer unit, last packet will have
    // payload which is less that max transfer unit
    if (offset + length > dataSize)
    {
        if (offset < dataSize)
        {
            length = dataSize - offset; // To calculate actual length depending
                                        // on the data size and offset
        }
        else
        {
            if (!sendErrorCompletionCode(yield, msgReq->hdr.instance_id,
                                         PLDM_ERROR, PLDM_GET_PACKAGE_DATA))
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    ("sendPackageData: Failed to send PLDM message"));
            }
            return PLDM_ERROR;
        }
    }

    struct get_fd_data_resp dataHeader;
    dataHeader.completion_code = PLDM_SUCCESS;
    dataHeader.next_data_transfer_handle = ++dataTransferHandle;

    // Setting the Transfer flag that indiates what part of the transfer this
    // response represents
    dataHeader.transfer_flag = setTransferFlag(offset, length, dataSize);

    struct variable_field portionOfData = {};
    portionOfData.length = length;
    portionOfData.ptr = packageData.data() + offset;

    // Set the Portion of Metadata using offset and length
    offset = offset + length;

    // header plus requested data length
    size_t respLen = sizeof(struct PLDMEmptyRequest) +
                     sizeof(struct get_fd_data_resp) + length;

    std::vector<uint8_t> pldmResp(respLen);
    struct pldm_msg* msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    retVal = encode_get_package_data_resp(msgReq->hdr.instance_id, respLen,
                                          msgResp, &dataHeader, &portionOfData);

    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("sendPackageData: encode request failed"
             "RETVAL=" +
             std::to_string(retVal))
                .c_str());
        return retVal;
    }

    // tag Owner bit cleared to false for respose message
    if (!sendPldmMessage(yield, currentTid, retryCount, msgTag, false,
                         pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("sendPackageData: Failed to send PLDM message"));
        return PLDM_ERROR;
    }

    return PLDM_SUCCESS;
}

uint8_t FWUpdate::setTransferFlag(const size_t offset, const size_t length,
                                  const size_t dataSize)
{
    uint8_t transferFlag;

    if (offset + length < dataSize)
    {
        if (offset == 0)
        {
            transferFlag = PLDM_START;
        }
        else
        {
            transferFlag = PLDM_MIDDLE;
        }
    }
    else if (offset + length >= dataSize)
    {
        if (offset == 0)
        {
            transferFlag = PLDM_START_AND_END;
        }
        else
        {
            transferFlag = PLDM_END;
        }
    }

    return transferFlag;
}

int FWUpdate::processActivateFirmware(
    const boost::asio::yield_context yield, bool8_t selfContainedActivationReq,
    uint16_t& estimatedTimeForSelfContainedActivation)
{
    if (!updateMode)
    {
        return NOT_IN_UPDATE_MODE;
    }
    if (fdState != FD_READY_XFER)
    {
        return COMMAND_NOT_EXPECTED;
    }
    int retVal = activateFirmware(yield, selfContainedActivationReq,
                                  estimatedTimeForSelfContainedActivation);
    if (retVal != PLDM_SUCCESS)
    {
        return retVal;
    }
    fdState = FD_ACTIVATE;
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "FD changed state to ACTIVATE");
    return PLDM_SUCCESS;
}

int FWUpdate::activateFirmware(
    const boost::asio::yield_context yield, bool8_t selfContainedActivationReq,
    uint16_t& estimatedTimeForSelfContainedActivation)
{
    uint8_t instanceID = createInstanceId(currentTid);
    std::vector<uint8_t> pldmReq(sizeof(struct PLDMEmptyRequest) +
                                 sizeof(struct activate_firmware_req));
    struct pldm_msg* msgReq = reinterpret_cast<pldm_msg*>(pldmReq.data());
    int retVal = encode_activate_firmware_req(
        instanceID, msgReq, sizeof(struct activate_firmware_req),
        selfContainedActivationReq);

    if (!validatePLDMReqEncode(currentTid, retVal, "ActivateFirmware"))
    {
        return retVal;
    }
    std::vector<uint8_t> pldmResp;
    if (!sendReceivePldmMessage(yield, currentTid, timeout, retryCount, pldmReq,
                                pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "ActivateFirmware: Failed to send or receive PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return PLDM_ERROR;
    }
    auto msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    size_t payloadLen = pldmResp.size() - hdrSize;
    retVal =
        decode_activate_firmware_resp(msgResp, payloadLen, &completionCode,
                                      &estimatedTimeForSelfContainedActivation);

    if (!validatePLDMRespDecode(currentTid, retVal, completionCode,
                                "ActivateFirmware"))
    {
        return retVal;
    }
    return PLDM_SUCCESS;
}

int FWUpdate::getStatus(const boost::asio::yield_context yield)
{
    uint8_t instanceID = createInstanceId(currentTid);
    std::vector<uint8_t> pldmReq(sizeof(struct PLDMEmptyRequest));
    struct pldm_msg* msgReq = reinterpret_cast<pldm_msg*>(pldmReq.data());
    int retVal = encode_get_status_req(instanceID, msgReq);
    if (!validatePLDMReqEncode(currentTid, retVal, std::string("GetStatus")))
    {
        return retVal;
    }
    std::vector<uint8_t> pldmResp;
    if (!sendReceivePldmMessage(yield, currentTid, timeout, retryCount, pldmReq,
                                pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "GetStatus: Failed to send or receive PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return PLDM_ERROR;
    }
    auto msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    size_t payloadLen = pldmResp.size() - hdrSize;
    retVal = decode_get_status_resp(msgResp, payloadLen, &completionCode,
                                    &currentState, &previousState, &auxState,
                                    &auxStateStatus, &progressPercent,
                                    &reasonCode, &updateOptionFlagsEnabled);
    // TODO: need to add the GetStatus response data to D-Bus interface
    if (!validatePLDMRespDecode(currentTid, retVal, completionCode,
                                std::string("GetStatus")))
    {
        return retVal;
    }

    return PLDM_SUCCESS;
}

int FWUpdate::doCancelUpdateComponent(const boost::asio::yield_context yield)
{
    if (!updateMode)
    {
        return NOT_IN_UPDATE_MODE;
    }
    if (!cancelUpdateComponentState.count(fdState))
    {
        return COMMAND_NOT_EXPECTED;
    }
    int retVal = cancelUpdateComponent(yield);
    if (retVal != PLDM_SUCCESS)
    {
        return retVal;
    }

    fdState = FD_READY_XFER;
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "FD changed state to READY XFER");
    return PLDM_SUCCESS;
}

int FWUpdate::cancelUpdateComponent(const boost::asio::yield_context yield)
{
    uint8_t instanceID = createInstanceId(currentTid);
    std::vector<uint8_t> pldmReq(sizeof(struct PLDMEmptyRequest));
    struct pldm_msg* msgReq = reinterpret_cast<pldm_msg*>(pldmReq.data());
    int retVal = encode_cancel_update_component_req(instanceID, msgReq);
    if (!validatePLDMReqEncode(currentTid, retVal, "CancelUpdateComponent"))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("CancelUpdateComponent: encode_cancel_update_component_req "
             "failed. RETVAL: " +
             std::to_string(retVal))
                .c_str());
        return retVal;
    }
    std::vector<uint8_t> pldmResp;
    if (!sendReceivePldmMessage(yield, currentTid, timeout, retryCount, pldmReq,
                                pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "CancelUpdateComponent: Failed to send or receive PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return PLDM_ERROR;
    }
    auto msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    size_t payloadLen = pldmResp.size() - hdrSize;
    retVal = decode_cancel_update_component_resp(msgResp, payloadLen,
                                                 &completionCode);
    if (!validatePLDMRespDecode(currentTid, retVal, completionCode,
                                "CancelUpdateComponent"))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("CancelUpdateComponent: decode_cancel_update_component_resp "
             "failed. RETVAL: " +
             std::to_string(retVal) +
             ". COMPLETION_CODE: " + std::to_string(completionCode))
                .c_str());
        return retVal;
    }
    return PLDM_SUCCESS;
}

int FWUpdate::doCancelUpdate(const boost ::asio ::yield_context yield,
                             bool8_t& nonFunctioningComponentIndication,
                             bitfield64_t& nonFunctioningComponentBitmap)
{
    if (!updateMode)
    {
        return PLDM_ERROR;
    }
    if ((fdState == FD_IDLE) || (fdState == FD_ACTIVATE))
    {
        return COMMAND_NOT_EXPECTED;
    }
    // TODO:Need to provide D-Bus interface to invoke cancelUpdate
    int retVal = cancelUpdate(yield, nonFunctioningComponentIndication,
                              nonFunctioningComponentBitmap);
    if (retVal != PLDM_SUCCESS)
    {
        return retVal;
    }
    fdState = FD_IDLE;
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "FD changed state to IDLE");
    return PLDM_SUCCESS;
}

int FWUpdate::cancelUpdate(const boost::asio::yield_context yield,
                           bool8_t& nonFunctioningComponentIndication,
                           bitfield64_t& nonFunctioningComponentBitmap)

{
    uint8_t instanceID = createInstanceId(currentTid);
    std::vector<uint8_t> pldmReq(sizeof(struct PLDMEmptyRequest));
    struct pldm_msg* msgReq = reinterpret_cast<pldm_msg*>(pldmReq.data());
    int retVal = encode_cancel_update_req(instanceID, msgReq);
    if (!validatePLDMReqEncode(currentTid, retVal, "CancelUpdate"))
    {
        return retVal;
    }
    std::vector<uint8_t> pldmResp;
    if (!sendReceivePldmMessage(yield, currentTid, timeout, retryCount, pldmReq,
                                pldmResp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "CancelUpdate: Failed to send or receive PLDM message",
            phosphor::logging::entry("TID=%d", currentTid));
        return PLDM_ERROR;
    }
    auto msgResp = reinterpret_cast<pldm_msg*>(pldmResp.data());
    size_t payloadLen = pldmResp.size() - hdrSize;
    retVal = decode_cancel_update_resp(msgResp, payloadLen, &completionCode,
                                       &nonFunctioningComponentIndication,
                                       &nonFunctioningComponentBitmap);
    if (!validatePLDMRespDecode(currentTid, retVal, completionCode,
                                std::string("CancelUpdate")))
    {
        return retVal;
    }
    return PLDM_SUCCESS;
}

uint64_t FWUpdate::getApplicableComponents()
{
    std::vector<uint8_t> applicableComp;
    uint64_t value = 0;
    pldmImg->getDevIdRcrdProperty(applicableComp, "ApplicableComponents",
                                  currentDeviceIDRecord);
    int byteCount = 0;
    for (auto byte : applicableComp)
    {
        value = value | (static_cast<uint64_t>(byte) << (8 * byteCount++));
        if (byteCount > static_cast<int>(sizeof(uint64_t) - 1))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "ApplicableComponents exceeding 8 bytes");
            break;
        }
    }

    return value;
}

bool FWUpdate::isComponentApplicable()
{
    return (applicableComponentsVal >> currentComp) & 1;
}

constexpr uint32_t convertSecondsToMilliseconds(const uint16_t seconds)
{
    return (seconds * 1000);
}

boost::system::error_code
    FWUpdate::startTimer(const boost::asio::yield_context yield,
                         const uint32_t interval)
{
    boost::system::error_code ec;
    expectedCommandTimer->expires_after(std::chrono::milliseconds(interval));
    expectedCommandTimer->async_wait(yield[ec]);
    return ec;
}

uint16_t FWUpdate::getReserveEidTimeOut()
{
    size_t updatableImagesize = pldmImg->getUpdatableImagesize();
    // From the test results we observed that it took around 60 seconds for
    // updating a pldm image of size 160KB, based on this bytesPerSec is
    // calculated.
    constexpr uint16_t bytesPerSec = 2730;
    // choosing 3x of expected duration for PLDM firmware update timeout
    return static_cast<uint16_t>((1 + updatableImagesize / bytesPerSec) * 3);
}

void FWUpdate::cancelReserveBWTimer()
{
    reserveBWTimer->cancel();
}

void FWUpdate::activateReserveBandwidth()
{
    boost::asio::spawn([this](boost::asio::yield_context yield) {
        uint16_t reserveEidTimeOut = getReserveEidTimeOut();
        if (!reserveBandwidth(yield, currentTid, PLDM_FWUP, reserveEidTimeOut))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("reserveBandwidth failed. TID: " + std::to_string(currentTid))
                    .c_str());
            return;
        }
        isReserveBandwidthActive = true;
        // Subtracting reserveEidTimeOut by 5 seconds to make sure the reserve
        // bandwidth is reactivated before reserve bandwidth is released.
        reserveBWTimer->expires_after(
            std::chrono::milliseconds((reserveEidTimeOut - 5) * 1000));
        reserveBWTimer->async_wait([this](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                phosphor::logging::log<phosphor::logging::level::DEBUG>(
                    "reserveBWTimer operation_aborted");
                return;
            }
            else if (ec)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "reserveBWTimer failed");
                return;
            }
            activateReserveBandwidth();
        });
    });
}

struct SelfContainedActivationCache
{
    void updateTime(pldm_tid_t tid, int status, uint16_t time)
    {
        Device device{status, time};
        cache.insert(std::make_pair(tid, device));
    }

    uint16_t getMaxTime()
    {
        uint16_t maxTime = 0;
        for (auto& [tid, device] : cache)
        {
            if (device.updateStatus == PLDM_SUCCESS)
            {
                if (device.estimatedTimeForSelfContainedActivation > maxTime)
                {
                    maxTime = device.estimatedTimeForSelfContainedActivation;
                }
            }
        }
        return maxTime;
    }

  private:
    struct Device
    {
        int updateStatus;
        uint16_t estimatedTimeForSelfContainedActivation;
    };

    std::unordered_map<pldm_tid_t, Device> cache{};
};

int FWUpdate::runUpdate(
    const boost::asio::yield_context yield,
    SelfContainedActivationCache& selfContainedActivationCache)
{
    compCount = pldmImg->getTotalCompCount();
    int retVal = processRequestUpdate(yield);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "FD cannot be put in update mode");
        return retVal;
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "RequestUpdate command is success");
    updateMode = true;
    fdState = FD_LEARN_COMPONENTS;
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "FD changed state to LEARN COMPONENTS");
    createAsyncDelay(yield, delayBtw);
    activateReserveBandwidth();
    retVal = processSendPackageData(yield);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "processSendPackageData failed");
        return retVal;
    }

    // GetDeviceMetaData
    retVal = processGetDeviceMetaData(yield);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("GetDeviceMetaData failed with retVal " + std::to_string(retVal))
                .c_str());
        return retVal;
    }

    retVal = processPassComponentTable(yield);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "runUpdate: processPassComponentTable failed");
        return retVal;
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "PassComponentTable command is success");
    fdState = FD_READY_XFER;
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "FD changed state to READY XFER");

    for (uint16_t count = 0; count < compCount; ++count)
    {
        uint8_t compCompatabilityResp = 0;
        uint8_t compCompatabilityRespCode = 0;
        bitfield32_t updateOptFlagsEnabled = {};
        uint16_t estimatedTimeReqFd = 0;
        currentComp = count;
        if (!isComponentApplicable())
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "component not applicable");
            compUpdateProgress(yield);
            continue;
        }

        retVal = processUpdateComponent(
            yield, compCompatabilityResp, compCompatabilityRespCode,
            updateOptFlagsEnabled, estimatedTimeReqFd);
        if (retVal != PLDM_SUCCESS)
        {

            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("runUpdate: processUpdateComponent failed. RETVAL: " +
                 std::to_string(retVal) +
                 ". COMPONENT: " + std::to_string(count))
                    .c_str());

            continue;
        }
        if (compCompatabilityResp != COMPONENT_CAN_BE_UPDATED)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("COMPONENT: " + std::to_string(count) +
                 " will not be updated, "
                 "ComponentCompatibilityResponse Code: " +
                 std::to_string(compCompatabilityRespCode))
                    .c_str());
            compUpdateProgress(yield);
            continue;
        }

        fdState = FD_DOWNLOAD;
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "FD changed state to DOWNLOAD");
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("UpdateComponent command is success. COMPONENT: " +
             std::to_string(count))
                .c_str());

        uint8_t verifyResult = 0;
        uint8_t transferResult = 0;
        uint8_t applyResult = 0;
        bitfield16_t compActivationMethodsModification = {};
        expectedCmd = PLDM_REQUEST_FIRMWARE_DATA;

        retVal = processRequestFirmwareData(yield);
        if (retVal != PLDM_SUCCESS)
        {
            processTransferComplete(yield, fdReq, transferResult);
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("runUpdate: processRequestFirmwareData failed. RETVAL: " +
                 std::to_string(retVal) +
                 ". COMPONENT: " + std::to_string(count))
                    .c_str());
            int ret = doCancelUpdateComponent(yield);
            if (ret != PLDM_SUCCESS)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    ("runUpdate: Failed to run CancelUpdateComponent. "
                     "RETVAL: " +
                     std::to_string(ret) +
                     ". COMPONENT: " + std::to_string(count))
                        .c_str());
            }
            continue;
        }
        // Add Activation progress percentage of update to D-Bus interface
        compUpdateProgress(yield);

        retVal = processTransferComplete(yield, fdReq, transferResult);
        if (retVal != PLDM_SUCCESS)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("runUpdate: processTransferComplete failed. RETVAL: " +
                 std::to_string(retVal) +
                 ". COMPONENT: " + std::to_string(count))
                    .c_str());
            int ret = doCancelUpdateComponent(yield);
            if (ret != PLDM_SUCCESS)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    ("runUpdate: Failed to run CancelUpdateComponent. "
                     "RETVAL: " +
                     std::to_string(ret) +
                     ". COMPONENT: " + std::to_string(count))
                        .c_str());
            }
            continue;
        }
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("TransferComplete command is success. COMPONENT: " +
             std::to_string(count))
                .c_str());
        fdState = FD_VERIFY;
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "FD changed state to VERIFY");

        expectedCmd = PLDM_VERIFY_COMPLETE;

        startTimer(yield, fdCmdTimeout);

        if (!fdReqMatched)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "Timeout waiting for Verify complete",
                phosphor::logging::entry("COMPONENT=%d", count));
            continue;
        }
        fdReqMatched = false;
        retVal = processVerifyComplete(yield, fdReq, verifyResult);
        if (retVal != PLDM_SUCCESS)
        {

            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("runUpdate: processVerifyComplete failed for COMPONENT: " +
                 std::to_string(count) + ".RETVAL: " + std::to_string(retVal))
                    .c_str());
            int ret = doCancelUpdateComponent(yield);
            if (ret != PLDM_SUCCESS)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    ("runUpdate: Failed to run CancelUpdateComponent. "
                     "RETVAL: " +
                     std::to_string(ret) +
                     ". COMPONENT: " + std::to_string(count))
                        .c_str());
            }
            continue;
        }
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("VerifyComplete command is success. COMPONENT: " +
             std::to_string(count))
                .c_str());
        fdState = FD_APPLY;
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "FD changed state to APPLY");

        expectedCmd = PLDM_APPLY_COMPLETE;

        startTimer(yield, fdCmdTimeout);

        if (!fdReqMatched)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("Timeout waiting for Apply complete. COMPONENT: " +
                 std::to_string(count + 1))
                    .c_str());

            continue;
        }
        fdReqMatched = false;
        retVal = processApplyComplete(yield, fdReq, applyResult,
                                      compActivationMethodsModification);
        if (retVal != PLDM_SUCCESS)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                ("runUpdate: processApplyComplete failed. RETVAL: " +
                 std::to_string(retVal) +
                 ". COMPONENT: " + std::to_string(count))
                    .c_str());

            continue;
        }
        isComponentAvailableForUpdate = true;
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("ApplyComplete command is success. COMPONENT: " +
             std::to_string(count))
                .c_str());
        fdState = FD_READY_XFER;
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "FD changed state to READY XFER");
    }
    // TODO: The FD can send this command when it is in any state, except
    // the IDLE and LEARN COMPONENTS state. we need to note a limitation
    // that FD cannot send this command when UA(BMC) is acting as requester
    // and if FD present behind the MUX. In this patch we support send
    // matadata command only after apply complete command.

    if (fwDeviceMetaDataLen != 0)
    {
        retVal = processSendMetaData(yield);
        if (retVal != PLDM_SUCCESS)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "processSendMetaData failed");
            return retVal;
        }
    }

    if (isReserveBandwidthActive)
    {
        isReserveBandwidthActive = false;
        cancelReserveBWTimer();
        if (!releaseBandwidth(yield, currentTid, PLDM_FWUP))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "runUpdate: releaseBandwidth failed");
        }
    }

    if (!isComponentAvailableForUpdate)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("firmware update failed. RETVAL: " + std::to_string(retVal))
                .c_str());
        return PLDM_ERROR;
    }

    bool8_t selfContainedActivationReq = true;
    uint16_t estimatedTimeForSelfContainedActivation = 0;
    retVal = processActivateFirmware(yield, selfContainedActivationReq,
                                     estimatedTimeForSelfContainedActivation);
    if (retVal != PLDM_SUCCESS)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("runUpdate: doActivateFirmware failed. RETVAL: " +
             std::to_string(retVal))
                .c_str());
        return retVal;
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ActivateFirmware command is success");

    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("Firmware update completed successfully for TID:" +
         std::to_string(currentTid))
            .c_str());
    selfContainedActivationCache.updateTime(
        currentTid, retVal, estimatedTimeForSelfContainedActivation);

    return PLDM_SUCCESS;
}

template <typename propertyType>
void FWUpdate::updateFWUProperty(const boost::asio::yield_context yield,
                                 const std::string& interfaceName,
                                 const std::string& propertyName,
                                 const propertyType& propertyValue)
{
    auto bus = getSdBus();
    boost::system::error_code ec;
    // pldm image filename from image path
    std::string pldm_image =
        std::filesystem::path(pldmImg->getImagePath()).parent_path().filename();
    std::string objPath = "/xyz/openbmc_project/software/" + pldm_image;

    bus->yield_method_call<>(
        yield, ec, "xyz.openbmc_project.Software.BMC.Updater", objPath,
        "org.freedesktop.DBus.Properties", "Set", interfaceName, propertyName,
        std::variant<std::decay_t<decltype(propertyValue)>>(propertyValue));
    if (ec)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("Firmware update property updation failed. PROPERTY: " +
             propertyName)
                .c_str());
    }
}

void FWUpdate::compUpdateProgress(const boost::asio::yield_context yield)
{
    uint8_t compUpdateProgress =
        static_cast<uint8_t>(((currentComp + 1) * 100) / (compCount));
    fwUpdate->updateFWUProperty(
        yield, "xyz.openbmc_project.Software.ActivationProgress", "Progress",
        compUpdateProgress);
}

void pldmMsgRecvFwUpdCallback(const pldm_tid_t tid, const uint8_t msgTag,
                              const bool tagOwner,
                              std::vector<uint8_t>& message)
{
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "PLDM Firmware update message received",
        phosphor::logging::entry("TID=0x%X", tid));
    // pldmImg points to null if FW update is not in progress at this point
    // firmware device should not send any firmware update commands
    if (!pldmImg || !fwUpdate)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Firmware update is not in process, command not excepted");
        return;
    }
    if (!tagOwner)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "MCTP Tag Owner is not set, dropping unexpected packet");
        return;
    }
    fwUpdate->validateReqForFWUpdCmd(tid, msgTag, message);
    return;
}

static void updateAssociationsProperty()
{
    if (!associationsIntf)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Associations interface does not exist");
        return;
    }
    std::vector<std::tuple<std::string, std::string, std::string>> association;
    for (const auto& it : inventoryPaths)
    {
        association.insert(
            association.begin(),
            std::make_tuple("updateable", "software_version", it.second));
    }
    associationsIntf->set_property("Associations", association);
}

/** @brief API that deletes PLDM firmware device resorces. This API should
 * be called when PLDM firmware update capable device is removed from the
 * platform.
 */
bool deleteFWDevice(const pldm_tid_t tid)
{
    auto itr = terminusFwuProperties.find(tid);
    if (itr == terminusFwuProperties.end())
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("PLDM firmware update device not matched for TID " +
             std::to_string(tid))
                .c_str());
        return false;
    }
    terminusFwuProperties.erase(itr);

    if (fwuIface.erase(tid) == 0)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("FWU D-Bus interface not present for TID " + std::to_string(tid))
                .c_str());
        return false;
    }

    if (inventoryPaths.erase(tid) == 0)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            ("Firmware inventory path not present for TID " +
             std::to_string(tid))
                .c_str());
        return false;
    }

    updateAssociationsProperty();

    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("PLDM firmware update device resources deleted for TID " +
         std::to_string(tid))
            .c_str());
    return true;
}

static bool updateMode = false;
static int initUpdate(const boost::asio::yield_context yield)
{
    if (updateMode)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "initUpdate: Cannot start firmware update. Firmware update is "
            "already in progress");
        return PLDM_ERROR;
    }

    bool fwUpdateStatus = true;
    SelfContainedActivationCache selfContainedActivationCache{};

    pldm::platform::pauseSensorPolling();
    auto matchedTermini = pldmImg->getMatchedTermini();
    for (const auto& it : matchedTermini)
    {
        pldm_tid_t matchedTid = it.second;
        uint8_t matchedDevIdRecord = it.first;
        fwUpdate = std::make_unique<FWUpdate>(matchedTid, matchedDevIdRecord);
        if (!fwUpdate->setMatchedFDDescriptors())
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                ("initUpdate: Failed to set TargetFDProperties for "
                 "TID: " +
                 std::to_string(matchedTid))
                    .c_str());
            continue;
        }
        int retVal = fwUpdate->runUpdate(yield, selfContainedActivationCache);
        if (retVal != PLDM_SUCCESS)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                ("runUpdate failed for TID: " + std::to_string(matchedTid) +
                 ". RETVAL:" + std::to_string(retVal))
                    .c_str());
            fwUpdateStatus = false;
            fwUpdate->terminateFwUpdate(yield);
        }
        updateMode = false;
    }

    if (!updateMode)
    {
        boost::system::error_code ec;
        auto waitTimer =
            std::make_unique<boost::asio::steady_timer>(*getIoContext());
        waitTimer->expires_after(
            std::chrono::seconds(selfContainedActivationCache.getMaxTime()));
        waitTimer->async_wait(yield[ec]);

        for (const auto& [matchedDevIdRecord, matchedTID] : matchedTermini)
        {
            triggerDeviceDiscovery(matchedTID);
        }
    }

    pldm::platform::resumeSensorPolling();
    if (!fwUpdateStatus)
    {
        fwUpdate->updateFWUProperty(
            yield, "xyz.openbmc_project.Software.Activation", "Activation",
            "xyz.openbmc_project.Software.Activation.Activations.Failed");
    }
    else
    {
        fwUpdate->updateFWUProperty(
            yield, "xyz.openbmc_project.Software.Activation", "Activation",
            "xyz.openbmc_project.Software.Activation.Activations.Active");
    }
    return PLDM_SUCCESS;
}

static bool fwuBaseInitialized = false;
static void initializeFWUBase()
{
    std::string objPath = "/xyz/openbmc_project/pldm/fwu";
    expectedCommandTimer =
        std::make_shared<boost::asio::steady_timer>(*getIoContext());
    auto objServer = getObjServer();
    auto fwuBaseIface = objServer->add_interface(objPath, FWUBase::interface);
    fwuBaseIface->register_method(
        "StartFWUpdate", [](const std::string filePath) {
            int rc = -1;
            if (pldmImg)
            {
                return rc;
            }
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "StartFWUpdate is called");
            try
            {
                pldmImg = std::make_unique<PLDMImg>(filePath);
                if (!pldmImg->processPkgHdr())
                {
                    phosphor::logging::log<phosphor::logging::level::ERR>(
                        "processPkgHdr: Failed");
                }
                else
                {
                    rc = 0;
                }
            }
            catch (const std::exception&)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Failed to process pldm image",
                    phosphor::logging::entry("PLDM_IMAGE=%s",
                                             filePath.c_str()));
                return rc;
            }
            boost::asio::spawn([](boost::asio::yield_context yield) {
                int ret = initUpdate(yield);
                if (ret != PLDM_SUCCESS)
                {
                    phosphor::logging::log<phosphor::logging::level::ERR>(
                        "StartFWUpdate: initUpdate failed.");
                }
                pldmImg = nullptr;
            });
            return rc;
        });
    fwuBaseIface->initialize();
}

static void registerAssociationsProperty()
{
    auto objServer = getObjServer();
    const std::string associationsPath = "/xyz/openbmc_project/software";
    associationsIntf = objServer->add_unique_interface(
        associationsPath, "xyz.openbmc_project.Association.Definitions");
    if (!associationsIntf)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Failed to add associations interface");
        return;
    }
    std::vector<std::tuple<std::string, std::string, std::string>>
        emptyAssociation{std::make_tuple("", "", "")};
    associationsIntf->register_property("Associations", emptyAssociation);
    associationsIntf->initialize();
}

bool fwuInit(boost::asio::yield_context yield, const pldm_tid_t tid)
{

    if (!fwuBaseInitialized)
    {
        initializeFWUBase();
        registerAssociationsProperty();
        fwuBaseInitialized = true;
    }
    FWInventoryInfo inventoryInfo(tid);
    std::optional<FDProperties> properties =
        inventoryInfo.runInventoryCommands(yield);

    if (properties == std::nullopt)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("Failed to run runInventory commands for TID: " +
             std::to_string(tid))
                .c_str());
        return false;
    }
    inventoryInfo.addInventoryInfoToDBus();
    fwuIface.insert(
        std::make_pair(tid, std::move(inventoryInfo.getInterfaces())));
    inventoryPaths.insert(
        std::make_pair(tid, inventoryInfo.getInventoryPath()));
    // TODO: There could be some pldmd devices which supports inventory
    // commands but does not support pldm firmware update. For such devices
    // updateAssociationsProperty() should not be called
    updateAssociationsProperty();
    terminusFwuProperties[tid] = *properties;
    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("fwuInit success for TID:" + std::to_string(tid)).c_str());

    return true;
}
} // namespace fwu
} // namespace pldm
