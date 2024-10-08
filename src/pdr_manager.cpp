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

#include "pdr_manager.hpp"

#include "platform.hpp"
#include "platform_association.hpp"
#include "pldm.hpp"
#include "utils.hpp"

#include <codecvt>
#include <fstream>
#include <phosphor-logging/log.hpp>
#include <queue>
#include <regex>

#include "utils.h"

namespace pldm
{
namespace platform
{

PDRManager::PDRManager(const pldm_tid_t tid) : _tid(tid)
{
}

PDRManager::~PDRManager()
{
    auto objectServer = getObjServer();
    for (auto& iter : _systemHierarchyIntf)
    {
        objectServer->remove_interface(iter.second.first);
    }

    for (auto& iter : _sensorIntf)
    {
        objectServer->remove_interface(iter.second.first);
    }

    for (auto& iter : _effecterIntf)
    {
        objectServer->remove_interface(iter.second.first);
    }

    for (auto& iter : _fruRecordSetIntf)
    {
        objectServer->remove_interface(iter.second.first);
    }

#ifdef EXPOSE_CHASSIS
    if (inventoryIntf)
    {
        objectServer->remove_interface(inventoryIntf);
        association::setPath(_tid, {});
    }
#endif

    if (pdrDumpInterface)
    {
        objectServer->remove_interface(pdrDumpInterface);
    }
}

// TODO: remove this API after code complete
static inline void printDebug(const std::string& message)
{
    phosphor::logging::log<phosphor::logging::level::DEBUG>(message.c_str());
}

// TODO: remove this API after code complete
static void printPDRInfo(pldm_pdr_repository_info& pdrRepoInfo)
{
    printDebug("GetPDRRepositoryInfo: repositoryState -" +
               std::to_string(pdrRepoInfo.repository_state));
    printDebug("GetPDRRepositoryInfo: recordCount -" +
               std::to_string(pdrRepoInfo.record_count));
    printDebug("GetPDRRepositoryInfo: repositorySize -" +
               std::to_string(pdrRepoInfo.repository_size));
    printDebug("GetPDRRepositoryInfo: largestRecordSize -" +
               std::to_string(pdrRepoInfo.largest_record_size));
    printDebug("GetPDRRepositoryInfo: dataTransferHandleTimeout -" +
               std::to_string(pdrRepoInfo.data_transfer_handle_timeout));
}

// TODO: remove this API after code complete
static void printPDRResp(const RecordHandle& recordHandle,
                         const RecordHandle& nextRecordHandle,
                         const transfer_op_flag& transferOpFlag,
                         const uint16_t& recordChangeNumber,
                         const DataTransferHandle& nextDataTransferHandle,
                         const bool& transferComplete,
                         const std::vector<uint8_t>& pdrRecord)
{
    printDebug("GetPDR: recordHandle -" + std::to_string(recordHandle));
    printDebug("GetPDR: nextRecordHandle -" + std::to_string(nextRecordHandle));
    printDebug("GetPDR: transferOpFlag -" + std::to_string(transferOpFlag));
    printDebug("GetPDR: recordChangeNumber -" +
               std::to_string(recordChangeNumber));
    printDebug("GetPDR: nextDataTransferHandle -" +
               std::to_string(nextDataTransferHandle));
    printDebug("GetPDR: transferComplete -" + std::to_string(transferComplete));
    utils::printVect("PDR:", pdrRecord);
}

std::optional<pldm_pdr_repository_info>
    PDRManager::getPDRRepositoryInfo(boost::asio::yield_context yield)
{
    int rc;
    std::vector<uint8_t> req(sizeof(PLDMEmptyRequest));
    pldm_msg* reqMsg = reinterpret_cast<pldm_msg*>(req.data());

    rc = encode_get_pdr_repository_info_req(createInstanceId(_tid), reqMsg);
    if (!validatePLDMReqEncode(_tid, rc, "GetPDRRepositoryInfo"))
    {
        return std::nullopt;
    }

    std::vector<uint8_t> resp;
    if (!sendReceivePldmMessage(yield, _tid, commandTimeout, commandRetryCount,
                                req, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to send or receive GetPDRRepositoryInfo request",
            phosphor::logging::entry("TID=%d", _tid));
        return std::nullopt;
    }

    pldm_get_pdr_repository_info_resp pdrInfo;
    auto rspMsg = reinterpret_cast<pldm_msg*>(resp.data());

    rc = decode_get_pdr_repository_info_resp(
        rspMsg, resp.size() - pldmMsgHdrSize, &pdrInfo);
    if (!validatePLDMRespDecode(_tid, rc, pdrInfo.completion_code,
                                "GetPDRRepositoryInfo"))
    {
        return std::nullopt;
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "GetPDRRepositoryInfo success",
        phosphor::logging::entry("TID=%d", _tid));
    return pdrInfo.pdr_repo_info;
}

static bool handleGetPDRResp(pldm_tid_t tid, std::vector<uint8_t>& resp,
                             RecordHandle& nextRecordHandle,
                             transfer_op_flag& transferOpFlag,
                             uint16_t& recordChangeNumber,
                             DataTransferHandle& dataTransferHandle,
                             bool& transferComplete,
                             std::vector<uint8_t>& pdrRecord)
{
    int rc;
    uint8_t completionCode{};
    uint8_t transferFlag{};
    uint8_t transferCRC{};
    uint16_t recordDataLen{};
    DataTransferHandle nextDataTransferHandle{};
    auto respMsgPtr = reinterpret_cast<struct pldm_msg*>(resp.data());

    // Get the number of recordData bytes in the response
    rc = decode_get_pdr_resp(respMsgPtr, resp.size() - pldmMsgHdrSize,
                             &completionCode, &nextRecordHandle,
                             &nextDataTransferHandle, &transferFlag,
                             &recordDataLen, nullptr, 0, &transferCRC);
    if (!validatePLDMRespDecode(tid, rc, completionCode, "GetPDR"))
    {
        return false;
    }

    std::vector<uint8_t> pdrData(recordDataLen, 0);
    rc = decode_get_pdr_resp(
        respMsgPtr, resp.size() - pldmMsgHdrSize, &completionCode,
        &nextRecordHandle, &nextDataTransferHandle, &transferFlag,
        &recordDataLen, pdrData.data(), pdrData.size(), &transferCRC);

    if (!validatePLDMRespDecode(tid, rc, completionCode, "GetPDR"))
    {
        return false;
    }

    pdrRecord.insert(pdrRecord.end(), pdrData.begin(), pdrData.end());
    if (transferFlag == PLDM_START)
    {
        auto pdrHdr = reinterpret_cast<pldm_pdr_hdr*>(pdrRecord.data());
        recordChangeNumber = le16toh(pdrHdr->record_change_num);
    }

    dataTransferHandle = nextDataTransferHandle;
    if ((transferComplete =
             (transferFlag == PLDM_END || transferFlag == PLDM_START_AND_END)))
    {
        if (transferFlag == PLDM_END)
        {
            uint8_t calculatedCRC = crc8(pdrRecord.data(), pdrRecord.size());
            if (calculatedCRC != transferCRC)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "PDR record CRC check failed");
                return false;
            }
        }
    }
    else
    {
        transferOpFlag = PLDM_GET_NEXTPART;
    }
    return true;
}

bool PDRManager::getDevicePDRRecord(boost::asio::yield_context yield,
                                    const RecordHandle recordHandle,
                                    RecordHandle& nextRecordHandle,
                                    std::vector<uint8_t>& pdrRecord)
{
    std::vector<uint8_t> req(pldmMsgHdrSize + PLDM_GET_PDR_REQ_BYTES);
    auto reqMsgPtr = reinterpret_cast<pldm_msg*>(req.data());
    constexpr size_t requestCount =
        maxPLDMMessageLen - PLDM_GET_PDR_MIN_RESP_BYTES;
    bool transferComplete = false;
    uint16_t recordChangeNumber = 0;
    size_t multipartTransferLimit = 100;
    transfer_op_flag transferOpFlag = PLDM_GET_FIRSTPART;
    DataTransferHandle dataTransferHandle = 0x00;

    // Multipart PDR data transfer
    do
    {
        int rc;
        rc = encode_get_pdr_req(createInstanceId(_tid), recordHandle,
                                dataTransferHandle, transferOpFlag,
                                requestCount, recordChangeNumber, reqMsgPtr,
                                PLDM_GET_PDR_REQ_BYTES);
        if (!validatePLDMReqEncode(_tid, rc, "GetPDR"))
        {
            break;
        }

        std::vector<uint8_t> resp;
        if (!sendReceivePldmMessage(yield, _tid, commandTimeout,
                                    commandRetryCount, req, resp))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Failed to send or receive GetPDR request",
                phosphor::logging::entry("TID=%d", _tid));
            break;
        }

        bool ret = handleGetPDRResp(
            _tid, resp, nextRecordHandle, transferOpFlag, recordChangeNumber,
            dataTransferHandle, transferComplete, pdrRecord);
        if (!ret)
        {
            // Discard the record if decode failed
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "handleGetRecordResp failed");
            // Clear transferComplete if modified
            transferComplete = false;
            break;
        }

        // TODO: remove after code complete
        printPDRResp(recordHandle, nextRecordHandle, transferOpFlag,
                     recordChangeNumber, dataTransferHandle, transferComplete,
                     pdrRecord);

        // Limit the number of middle packets
        // Discard the record if exceeeded.
        if (pdrRecord.size() > pdrRepoInfo.largest_record_size ||
            !(--multipartTransferLimit))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "Max PDR record size limit reached",
                phosphor::logging::entry("TID=%d", _tid),
                phosphor::logging::entry("RECORD_HANDLE=%lu", recordHandle));
            // Clear transferComplete to make sure the record is not getting
            // added to the PDR repository
            transferComplete = false;
            break;
        }
    } while (!transferComplete);

    if (!transferComplete)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Multipart PDR data transfer failed. Discarding the record",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("RECORD_HANDLE=%lu", recordHandle));
        pdrRecord.clear();
        return false;
    }
    return true;
}

bool PDRManager::getDevicePDRRepo(
    boost::asio::yield_context yield, uint32_t recordCount,
    std::unordered_map<RecordHandle, std::vector<uint8_t>>& devicePDRs)
{
    RecordHandle recordHandle = 0x00;

    do
    {
        std::vector<uint8_t> pdrRecord{};
        RecordHandle nextRecordHandle{};
        if (!getDevicePDRRecord(yield, recordHandle, nextRecordHandle,
                                pdrRecord))
        {
            return false;
        }

        // Discard if an empty record
        if (!pdrRecord.empty())
        {
            pldm_pdr_hdr* pdrHdr =
                reinterpret_cast<pldm_pdr_hdr*>(pdrRecord.data());
            recordHandle = le32toh(pdrHdr->record_handle);
            devicePDRs.emplace(std::make_pair(recordHandle, pdrRecord));
        }
        recordHandle = nextRecordHandle;

    } while (recordHandle && --recordCount);

    if (recordCount)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Possible erroneous PDR repository. 'nextRecordHandle = "
            "0x0000_0000' but 'recordCount' says there are pending PDRs to "
            "fetch.",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("PENDING_RECORD_COUNT=%lu", recordCount));
    }

    if (recordHandle)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Possible erroneous PDR repository. 'pendingRecordCount = 0' but "
            "'nextRecordHandle' says there are pending PDRs to fetch.",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("NEXT_RECORD_HANDLE=%lu", recordHandle));
    }
    return true;
}

bool PDRManager::addDevicePDRToRepo(
    std::unordered_map<RecordHandle, std::vector<uint8_t>>& devicePDRs)
{
    bool terminusLPDRFound = false;
    for (auto& pdrRecord : devicePDRs)
    {
        // Update the TID in Terminus Locator PDR before adding to repo
        const pldm_pdr_hdr* pdrHdr =
            reinterpret_cast<const pldm_pdr_hdr*>(pdrRecord.second.data());
        if (pdrHdr->type == PLDM_TERMINUS_LOCATOR_PDR)
        {
            pldm_terminus_locator_pdr* tLocatorPDR =
                reinterpret_cast<pldm_terminus_locator_pdr*>(
                    pdrRecord.second.data());
            if (tLocatorPDR->validity == PLDM_TL_PDR_VALID)
            {
                if (terminusLPDRFound)
                {
                    phosphor::logging::log<phosphor::logging::level::ERR>(
                        "Multiple valid Terminus Locator PDRs found",
                        phosphor::logging::entry("TID=%d", _tid));
                    return false;
                }
                tLocatorPDR->tid = _tid;
                terminusLPDRFound = true;
                _containerID = tLocatorPDR->container_id;
            }
        }
        uint32_t pdrRecordSize = utils::to_uint32(pdrRecord.second.size());
        pldm_pdr_add(_pdrRepo.get(), pdrRecord.second.data(), pdrRecordSize,
                     pdrRecord.first, true);
    }
    if (!terminusLPDRFound)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Terminus Locator PDR not found");
    }
    return true;
}

bool PDRManager::constructPDRRepo(boost::asio::yield_context yield)
{
    uint32_t recordCount = pdrRepoInfo.record_count;

    if (pdrRepoInfo.repository_state != PLDM_PDR_REPOSITORY_STATE_AVAILABLE)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Device PDR record data is unavailable",
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }
    if (!recordCount)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "No PDR records to fetch",
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }

    std::unordered_map<RecordHandle, std::vector<uint8_t>> devicePDRs{};
    uint8_t noOfCommandTries = 3;
    while (noOfCommandTries--)
    {
        if (getDevicePDRRepo(yield, recordCount, devicePDRs))
        {
            break;
        }
        if (!noOfCommandTries)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Get PDR failed. Unable to fetch PDRs even after 3 tries",
                phosphor::logging::entry("TID=%d", _tid));
            return false;
        }
        devicePDRs.clear();
    }

    if (!addDevicePDRToRepo(devicePDRs))
    {
        return false;
    }

    uint32_t noOfRecordsFetched = pldm_pdr_get_record_count(_pdrRepo.get());
    if (noOfRecordsFetched != recordCount)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            ("Unable to fetch all PDR records. Expected number of records: " +
             std::to_string(recordCount) +
             " Records received: " + std::to_string(noOfRecordsFetched))
                .c_str(),
            phosphor::logging::entry("TID=%d", _tid));
        return false;
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("GetPDR success. Total number of records:" +
         std::to_string(noOfRecordsFetched))
            .c_str(),
        phosphor::logging::entry("TID=%d", _tid));
    return true;
} // namespace platform

static std::optional<std::string> getAuxName(const uint8_t nameStrCount,
                                             const size_t auxNamesLen,
                                             const uint8_t* auxNamesStart)
{
    if (!auxNamesStart)
    {
        return std::nullopt;
    }

    constexpr size_t strASCIInullSize = 1;
    constexpr size_t strUTF16nullSize = 2;
    constexpr size_t codeUnitSize = 2;
    constexpr size_t maxStrLen = 64;
    const std::string supportedLangTag = "en";
    const uint8_t* next = auxNamesStart;
    size_t advanced{};

    for (uint8_t nameCount = 0;
         nameCount < nameStrCount && advanced < auxNamesLen; nameCount++)
    {
        // If the nameLanguageTag and Auxiliary name in the PDR is not null
        // terminated, it will be an issue. Thus limit the string length to
        // maxStrLen. Provided additional one byte buffer to verify if the
        // length is more than maxStrLen. Why: Croping the string will result in
        // incorrect value for subsequent nameLanguageTags and Auxiliary names
        std::string langTag(reinterpret_cast<char const*>(next), 0,
                            maxStrLen + 1);
        // If the nameLanguageTag is not null terminated(Incorrect PDR data -
        // Assuming maximum possible Auxiliary name is of length maxStrLen),
        // further decodings will be erroneous
        if (langTag.size() > maxStrLen)
        {
            return std::nullopt;
        }
        next += langTag.size() + strASCIInullSize;

        std::u16string u16_str(reinterpret_cast<const char16_t*>(next), 0,
                               maxStrLen + 1);
        // If the Auxiliary name is not null terminated(Incorrect PDR data -
        // Assuming maximum possible Auxiliary name is of length maxStrLen),
        // further decodings will be erroneous
        if (u16_str.size() > maxStrLen)
        {
            return std::nullopt;
        }

#if __BYTE_ORDER != __BIG_ENDIAN
        // The Auxiliary Name is in big endian format
        std::transform(u16_str.cbegin(), u16_str.cend(), u16_str.begin(),
                       [](uint16_t utf16) { return be16toh(utf16); });
#endif

        // Only supports English
        if (langTag == supportedLangTag)
        {
            std::string auxName =
                std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,
                                     char16_t>{}
                    .to_bytes(u16_str);

            // Auxiliary names are used to create D-Bus object paths.
            // Replacing all non-alphanumeric with underscore
            std::string formattedAuxName =
                std::regex_replace(auxName, std::regex("[^a-zA-Z0-9_/]+"), "_");
            // Discard the name if all characters are non printable
            if (formattedAuxName == "_")
            {
                return std::nullopt;
            }
            return formattedAuxName;
        }
        next += (u16_str.size() * codeUnitSize) + strUTF16nullSize;
        advanced = next - auxNamesStart;
    }
    return std::nullopt;
}

void PDRManager::parseEntityAuxNamesPDR(std::vector<uint8_t>& pdrData)
{
    constexpr size_t sharedNameCountSize = 1;
    constexpr size_t nameStringCountSize = 1;
    constexpr size_t minEntityAuxNamesPDRLen =
        sizeof(pldm_pdr_hdr) + sizeof(pldm_entity) + sharedNameCountSize +
        nameStringCountSize;

    if (pdrData.size() >= minEntityAuxNamesPDRLen)
    {
        pldm_pdr_entity_auxiliary_names* namePDR =
            reinterpret_cast<pldm_pdr_entity_auxiliary_names*>(pdrData.data());
        LE16TOH(namePDR->entity.entity_type);
        LE16TOH(namePDR->entity.entity_instance_num);
        LE16TOH(namePDR->entity.entity_container_id);

        size_t auxNamesLen = pdrData.size() - minEntityAuxNamesPDRLen;

        auto name = getAuxName(namePDR->name_string_count, auxNamesLen,
                               namePDR->entity_auxiliary_names);

        if (!name)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Entity Auxiliary Name Invalid");
            return;
        }

        if (namePDR->shared_name_count <= 0)
        {
            // Cache the Entity Auxiliary Names
            _entityAuxNames.emplace(namePDR->entity, *name);

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                ("Entity Auxiliary Name: " + *name).c_str());
            return;
        }

        // entity_instance_num gives starting value of the range
        uint16_t instanceNumber = namePDR->entity.entity_instance_num;
        uint16_t count = 0;
        // e.g. sharedNameCount = 2 & entity_instance_num = 100, actually means
        // entity_instance range {100,101,102}
        while (instanceNumber <= (namePDR->entity.entity_instance_num +
                                  namePDR->shared_name_count))
        {

            std::string auxName = *name;
            auxName.append("_").append(std::to_string(count));

            _entityAuxNames.emplace(namePDR->entity, auxName);

            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                ("Entity Auxiliary Name: " + *name).c_str());
            ++count;
            ++instanceNumber;
        }
    }
}

// Create Entity Association node from parsed Entity Association PDR
static bool getEntityAssociation(const std::shared_ptr<pldm_entity[]>& entities,
                                 const size_t numEntities,
                                 EntityNode::NodePtr& entityAssociation)
{
    if (!(0 < numEntities) || !entities)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "No entities in Entity Association PDR");
        return false;
    }
    entityAssociation = std::make_shared<EntityNode>();
    entityAssociation->containerEntity = entities[0];

    for (size_t count = 1; count < numEntities; count++)
    {
        EntityNode::NodePtr containedPtr = std::make_shared<EntityNode>();
        containedPtr->containerEntity = entities[count];

        entityAssociation->containedEntities.emplace_back(
            std::move(containedPtr));
    }
    return true;
}

// Get the matching node from Entity Association Tree
static EntityNode::NodePtr
    getContainedNode(const EntityNode::NodePtr& rootNode,
                     const EntityNode::NodePtr& inputNode)
{
    if (!rootNode || !inputNode)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Invalid node input");
        return nullptr;
    }

    std::queue<EntityNode::NodePtr> containedEntityQueue;
    containedEntityQueue.push(rootNode); // Enqueue root
    // Search for EntityNode with matching entity info
    while (!containedEntityQueue.empty())
    {
        EntityNode::NodePtr node = containedEntityQueue.front();
        if (node->containerEntity.entity_type ==
                inputNode->containerEntity.entity_type &&
            node->containerEntity.entity_instance_num ==
                inputNode->containerEntity.entity_instance_num &&
            node->containerEntity.entity_container_id ==
                inputNode->containerEntity.entity_container_id)
        {
            return node;
        }

        containedEntityQueue.pop();

        // Enqueue all child node of the dequeued entity
        for (EntityNode::NodePtr& entityNode : node->containedEntities)
        {
            containedEntityQueue.push(entityNode);
        }
    }
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "No matching contained Node found");
    return nullptr;
}

static void insertToAssociationTree(EntityNode::NodePtr& parentNode,
                                    EntityNode::NodePtr& entityAssociation)
{
    if (!parentNode || !entityAssociation)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Invalid NodePtr");
        return;
    }

    auto checkCyclicEntityAssociation =
        [&parentNode](const EntityNode::NodePtr& containedEntity) {
            if (getContainedNode(parentNode, containedEntity) == nullptr)
            {
                return true;
            }
            else
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "Discarding cyclic entity association");
                return false;
            }
        };

    std::back_insert_iterator<EntityNode::ContainedEntities>
        containedEntityBackItr(parentNode->containedEntities);

    std::copy_if(entityAssociation->containedEntities.begin(),
                 entityAssociation->containedEntities.end(),
                 containedEntityBackItr, checkCyclicEntityAssociation);
}

// Extract root node from the list of Entity Associations parsed by matching
// container ID. Remove the same from list once it is found. Note:- Merge the
// Entity Association PDRs if there is more than one with same root node
// container ID
static EntityNode::NodePtr
    extractRootNode(std::vector<EntityNode::NodePtr>& entityAssociations,
                    ContainerID containerID)
{
    EntityNode::NodePtr rootNode = nullptr;

    entityAssociations.erase(
        std::remove_if(
            entityAssociations.begin(), entityAssociations.end(),
            [&rootNode, &containerID](EntityNode::NodePtr& entityAssociation) {
                if (entityAssociation->containerEntity.entity_container_id !=
                    containerID)
                {
                    return false;
                }

                if (!rootNode)
                {
                    rootNode = std::make_shared<EntityNode>();
                    rootNode->containerEntity =
                        entityAssociation->containerEntity;
                }

                insertToAssociationTree(rootNode, entityAssociation);
                return true;
            }),
        entityAssociations.end());

    return rootNode;
}

void PDRManager::createEntityAssociationTree(
    std::vector<EntityNode::NodePtr>& entityAssociations)
{
    // Get parent entity association
    EntityNode::NodePtr rootNode =
        extractRootNode(entityAssociations, _containerID);

    if (!rootNode)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Unable to find root node ");
        return;
    }
    _entityAssociationTree = rootNode;

    while (!entityAssociations.empty())
    {
        size_t associationPDRCount = entityAssociations.size();

        entityAssociations.erase(
            std::remove_if(entityAssociations.begin(), entityAssociations.end(),
                           [&rootNode](EntityNode::NodePtr& entityAssociation) {
                               EntityNode::NodePtr node = getContainedNode(
                                   rootNode, entityAssociation);

                               if (node)
                               {
                                   insertToAssociationTree(node,
                                                           entityAssociation);
                                   return true;
                               }
                               return false;
                           }),
            entityAssociations.end());

        // Safe check in case there is an invalid PDR
        if (!(entityAssociations.size() < associationPDRCount))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "Invalid Entity Association PDRs found");
            break;
        }
    }
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        "Successfully created Entity Associaton Tree");
}

static bool mergeContainedEntities(EntityNode::NodePtr& node,
                                   EntityNode::NodePtr& entityAssociation)
{
    if (node->containerEntity.entity_type ==
            entityAssociation->containerEntity.entity_type &&
        node->containerEntity.entity_instance_num ==
            entityAssociation->containerEntity.entity_instance_num &&
        node->containerEntity.entity_container_id ==
            entityAssociation->containerEntity.entity_container_id)
    {
        std::move(entityAssociation->containedEntities.begin(),
                  entityAssociation->containedEntities.end(),
                  std::back_inserter(node->containedEntities));

        phosphor::logging::log<phosphor::logging::level::INFO>(
            "Successfully moved Entity Association");
        return true;
    }

    return false;
}

void PDRManager::parseEntityAssociationPDR(std::vector<uint8_t>& pdrData)
{
    size_t numEntities{};
    pldm_entity* entitiesPtr = nullptr;
    if (!pldm_entity_association_pdr_extract(
            pdrData.data(), static_cast<uint16_t>(pdrData.size()), &numEntities,
            &entitiesPtr))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Entity Association PDR parsing failed",
            phosphor::logging::entry("TID=%d", _tid));
        return;
    }
    std::shared_ptr<pldm_entity[]> entities(entitiesPtr, free);

    EntityNode::NodePtr entityAssociation = nullptr;
    if (getEntityAssociation(entities, numEntities, entityAssociation))
    {
        for (auto& iter : entityAssociationNodes)
        {
            if (mergeContainedEntities(iter, entityAssociation))
                return;
        }
        entityAssociationNodes.emplace_back(std::move(entityAssociation));
    }
}

void PDRManager::getEntityAssociationPaths(EntityNode::NodePtr& node,
                                           EntityAssociationPath path)
{
    if (node == nullptr)
    {
        return;
    }

    // Append node to the path
    path.emplace_back(node->containerEntity);

    auto getEntityAuxName = [this](const pldm_entity& entity) {
        std::string entityAuxName;
        auto itr = _entityAuxNames.find(entity);
        if (itr != _entityAuxNames.end())
        {
            entityAuxName = itr->second;
        }
        else
        {
            // Dummy name if no Auxilary Name found
            entityAuxName = std::to_string(entity.entity_type) + "_" +
                            std::to_string(entity.entity_instance_num) + "_" +
                            std::to_string(entity.entity_container_id);
        }
        return entityAuxName;
    };

    DBusObjectPath objectPathStr =
        "/xyz/openbmc_project/system/" + std::to_string(_tid);
    for (const pldm_entity& entity : path)
    {
        objectPathStr += "/" + getEntityAuxName(entity);
    }
    _entityObjectPathMap.try_emplace(objectPathStr, node->containerEntity);

    if (!node->containedEntities.empty())
    {
        for (EntityNode::NodePtr& child : node->containedEntities)
        {
            getEntityAssociationPaths(child, path);
        }
    }
}

static void populateEntity(DBusInterfacePtr& entityIntf,
                           const DBusObjectPath& path,
                           const pldm_entity& entity)
{
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        ("Entity object path: " + path).c_str());
    auto objServer = getObjServer();

    entityIntf =
        objServer->add_interface(path, "xyz.openbmc_project.PLDM.Entity");
    entityIntf->register_property("EntityType", entity.entity_type);
    entityIntf->register_property("EntityInstanceNumber",
                                  entity.entity_instance_num);
    entityIntf->register_property("EntityContainerID",
                                  entity.entity_container_id);
    entityIntf->initialize();
}

void PDRManager::populateSystemHierarchy()
{
    for (const auto& [objPath, entity] : _entityObjectPathMap)
    {
        DBusInterfacePtr entityIntf;
        try
        {
            populateEntity(entityIntf, objPath, entity);
        }
        catch (const std::exception&)
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                ("Entity object path " + objPath + " is already exposed")
                    .c_str());
        }
        _systemHierarchyIntf.emplace(entity,
                                     std::make_pair(entityIntf, objPath));
    }
    // Clear after usage
    _entityObjectPathMap.clear();
}

void PDRManager::extractDeviceAuxName(EntityNode::NodePtr& rootNode)
{
    std::optional<std::string> deviceName;
    std::optional<std::string> deviceLocation;
    if (rootNode != nullptr)
    {
        auto iter = _entityAuxNames.find(rootNode->containerEntity);
        if (iter != _entityAuxNames.end())
        {
            deviceName = iter->second;
        }
    }
    deviceLocation = getDeviceLocation(_tid);

    std::string auxName =
        deviceLocation.has_value()
            ? deviceLocation.value() + "_" + deviceName.value_or("PLDM_Device")
            : deviceName.value_or("PLDM_Device") + "_" + std::to_string(_tid);

    // Replace unsupported characters in DBus path
    _deviceAuxName =
        std::regex_replace(auxName, std::regex("[^a-zA-Z0-9_/]+"), "_");
}

#ifdef EXPOSE_CHASSIS
void PDRManager::initializeInventoryIntf()
{
    std::string inventoryObj =
        "/xyz/openbmc_project/inventory/system/board/" + _deviceAuxName;
    auto objServer = getObjServer();

    /** TODO: Use a PLDM-specific interface instead of Board
     *  Changes on the Redfish API server side required.
     */
    inventoryIntf = objServer->add_interface(
        inventoryObj, "xyz.openbmc_project.Inventory.Item.Board");
    inventoryIntf->register_property("Name", _deviceAuxName);
    inventoryIntf->initialize();

    association::setPath(_tid, inventoryObj);
}
#endif

void PDRManager::parseSensorAuxNamesPDR(std::vector<uint8_t>& pdrData)
{
    if (pdrData.size() < sizeof(pldm_sensor_auxiliary_names_pdr))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Sensor Auxiliary Names PDR empty");
        return;
    }
    pldm_sensor_auxiliary_names_pdr* namePDR =
        reinterpret_cast<pldm_sensor_auxiliary_names_pdr*>(pdrData.data());
    LE16TOH(namePDR->terminus_handle);
    LE16TOH(namePDR->sensor_id);

    // TODO: Handle Composite sensor names
    size_t auxNamesLen =
        pdrData.size() - (sizeof(pldm_sensor_auxiliary_names_pdr) -
                          sizeof(namePDR->sensor_auxiliary_names));
    if (auto name = getAuxName(namePDR->name_string_count, auxNamesLen,
                               namePDR->sensor_auxiliary_names))
    {
        // Cache the Sensor Auxiliary Names
        _sensorAuxNames[namePDR->sensor_id] = _deviceAuxName + "_" + *name;

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            ("SensorID:" +
             std::to_string(static_cast<int>(namePDR->sensor_id)) +
             " Sensor Auxiliary Name: " + _sensorAuxNames[namePDR->sensor_id])
                .c_str());
    }
}

void PDRManager::parseEffecterAuxNamesPDR(std::vector<uint8_t>& pdrData)
{
    if (pdrData.size() < sizeof(pldm_effecter_auxiliary_names_pdr))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Effecter Auxiliary Names PDR empty");
        return;
    }
    pldm_effecter_auxiliary_names_pdr* namePDR =
        reinterpret_cast<pldm_effecter_auxiliary_names_pdr*>(pdrData.data());
    LE16TOH(namePDR->terminus_handle);
    LE16TOH(namePDR->effecter_id);

    // TODO: Handle Composite effecter names
    size_t auxNamesLen =
        pdrData.size() - (sizeof(pldm_effecter_auxiliary_names_pdr) -
                          sizeof(namePDR->effecter_auxiliary_names));
    if (auto name = getAuxName(namePDR->name_string_count, auxNamesLen,
                               namePDR->effecter_auxiliary_names))
    {
        // Cache the Effecter Auxiliary Names
        _effecterAuxNames[namePDR->effecter_id] = _deviceAuxName + "_" + *name;

        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            ("EffecterID:" +
             std::to_string(static_cast<int>(namePDR->effecter_id)) +
             " Effecter Auxiliary Name: " +
             _effecterAuxNames[namePDR->effecter_id])
                .c_str());
    }
}

static void populateNumericSensor(DBusInterfacePtr& sensorIntf,
                                  const DBusObjectPath& path)
{
    const std::string sensorInterface =
        "xyz.openbmc_project.PLDM.NumericSensor";

    auto objServer = getObjServer();

    sensorIntf = objServer->add_interface(path, sensorInterface);
    // TODO: Expose more numeric sensor info from PDR
    sensorIntf->initialize();
}

std::optional<DBusObjectPath>
    PDRManager::getEntityObjectPath(const pldm_entity& entity)
{
    auto systemIter = _systemHierarchyIntf.find(entity);
    if (systemIter != _systemHierarchyIntf.end())
    {
        return systemIter->second.second;
    }
    return std::nullopt;
}

std::optional<std::string>
    PDRManager::getSensorAuxNames(const SensorID& sensorID)
{
    auto iter = _sensorAuxNames.find(sensorID);
    if (iter != _sensorAuxNames.end())
    {
        return iter->second;
    }
    return std::nullopt;
}

std::string PDRManager::createSensorName(const SensorID sensorID)
{
    std::string sensorName =
        _deviceAuxName + "_Sensor_" + std::to_string(sensorID);

    _sensorAuxNames[sensorID] = sensorName;

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        ("createSensorName " + sensorName).c_str());

    return sensorName;
}

std::optional<DBusObjectPath>
    PDRManager::createSensorObjPath(const pldm_entity& entity,
                                    const SensorID& sensorID,
                                    const bool8_t auxNamePDR)
{
    // Find sensor name
    std::string sensorName;
    if (auxNamePDR)
    {
        if (auto name = getSensorAuxNames(sensorID))
        {
            sensorName = *name;
        }
    }
    if (sensorName.empty())
    {
        // Dummy name if no Sensor Name found
        sensorName = createSensorName(sensorID);
    }

    DBusObjectPath entityPath;

    // Verify the Entity associated
    if (auto path = getEntityObjectPath(entity))
    {
        entityPath = *path;
    }
    else
    {
        // If no entity associated, sensor will not be exposed on system
        // hierarchy
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Unable to find Entity Associated with Sensor ID",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("SENSOR_ID=0x%x", sensorID));
        return std::nullopt;
    }

    return entityPath + "/" + sensorName;
}

void PDRManager::parseNumericSensorPDR(std ::vector<uint8_t>& pdrData)
{
    std::vector<uint8_t> pdrOut(sizeof(pldm_numeric_sensor_value_pdr), 0);

    if (!pldm_numeric_sensor_pdr_parse(pdrData.data(),
                                       static_cast<uint16_t>(pdrData.size()),
                                       pdrOut.data()))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Numeric Sensor PDR parsing failed",
            phosphor::logging::entry("TID=%d", _tid));
        return;
    }
    pldm_numeric_sensor_value_pdr* sensorPDR =
        reinterpret_cast<pldm_numeric_sensor_value_pdr*>(pdrOut.data());
    uint16_t sensorID = sensorPDR->sensor_id;

    std::shared_ptr<pldm_numeric_sensor_value_pdr> numericSensorPDR =
        std::make_shared<pldm_numeric_sensor_value_pdr>(*sensorPDR);

    _numericSensorPDR.emplace(sensorID, std::move(numericSensorPDR));

    pldm_entity entity = {sensorPDR->entity_type,
                          sensorPDR->entity_instance_num,
                          sensorPDR->container_id};
    std::optional<DBusObjectPath> sensorPath = createSensorObjPath(
        entity, sensorID, sensorPDR->sensor_auxiliary_names_pdr);
    if (!sensorPath)
    {
        return;
    }

    DBusInterfacePtr sensorIntf;
    populateNumericSensor(sensorIntf, *sensorPath);
    _sensorIntf.emplace(sensorID, std::make_pair(sensorIntf, *sensorPath));
}

static void populateStateSensor(DBusInterfacePtr& sensorIntf,
                                const DBusObjectPath& path)
{
    const std::string sensorInterface = "xyz.openbmc_project.PLDM.StateSensor";

    auto objServer = getObjServer();

    sensorIntf = objServer->add_interface(path, sensorInterface);
    // TODO: Expose more state sensor info from PDR
    sensorIntf->initialize();
}

void PDRManager::parseStateSensorPDR(std::vector<uint8_t>& pdrData)
{
    // Without composite sensor support there is only one instance of sensor
    // possible states.
    // pldm_state_sensor_pdr holds a `uint8 possible_states[1]` which points to
    // state_sensor_possible_states. Subtract its size(1 byte) while calculating
    // total size.
    if (pdrData.size() < sizeof(pldm_state_sensor_pdr) - sizeof(uint8_t) +
                             sizeof(state_sensor_possible_states))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "State Sensor PDR length invalid or sensor disabled",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("PDR_SIZE=%d", pdrData.size()));
        return;
    }

    pldm_state_sensor_pdr* sensorPDR =
        reinterpret_cast<pldm_state_sensor_pdr*>(pdrData.data());
    LE16TOH(sensorPDR->sensor_id);
    LE16TOH(sensorPDR->entity_type);
    LE16TOH(sensorPDR->entity_instance);
    LE16TOH(sensorPDR->container_id);

    uint16_t sensorID = sensorPDR->sensor_id;

    // TODO: Composite sensor support
    if (sensorPDR->composite_sensor_count > 0x01)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Composite state sensor not supported",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("SENSOR_ID=0x%x", sensorID),
            phosphor::logging::entry("COMPOSITE_SENSOR_COUNT=%d",
                                     sensorPDR->composite_sensor_count));
    }

    state_sensor_possible_states* possibleState =
        reinterpret_cast<state_sensor_possible_states*>(
            sensorPDR->possible_states);
    LE16TOH(possibleState->state_set_id);

    if (pdrData.size() < sizeof(pldm_state_sensor_pdr) - sizeof(uint8_t) +
                             sizeof(state_sensor_possible_states) -
                             sizeof(uint8_t) +
                             possibleState->possible_states_size)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Invalid State Sensor PDR length",
            phosphor::logging::entry("TID=%d", _tid));
        return;
    }

    PossibleStates possibleStates;
    possibleStates.stateSetID = possibleState->state_set_id;
    // Max possibleStateSize as per spec DSP0248 Table 81
    constexpr uint8_t maxPossibleStatesSize = 0x20;
    int position = 0;
    for (uint8_t count = 0; count < possibleState->possible_states_size &&
                            count < maxPossibleStatesSize;
         count++)
    {
        for (uint8_t bits = 0; bits < 8; bits++)
        {
            if (possibleState->states[count].byte & (0x01 << bits))
            {
                possibleStates.possibleStateSetValues.emplace(position);
            }
            position++;
        }
    }

    // Cache PDR for later use
    std::shared_ptr<StateSensorPDR> stateSensorPDR =
        std::make_shared<StateSensorPDR>();
    stateSensorPDR->stateSensorData = *sensorPDR;
    // TODO: Multiple state sets in case of composite state sensor
    stateSensorPDR->possibleStates.emplace_back(std::move(possibleStates));
    _stateSensorPDR.emplace(sensorID, std::move(stateSensorPDR));

    pldm_entity entity = {sensorPDR->entity_type, sensorPDR->entity_instance,
                          sensorPDR->container_id};

    std::optional<DBusObjectPath> sensorPath = createSensorObjPath(
        entity, sensorID, sensorPDR->sensor_auxiliary_names_pdr);
    if (!sensorPath)
    {
        return;
    }

    DBusInterfacePtr sensorIntf;
    populateStateSensor(sensorIntf, *sensorPath);
    _sensorIntf.emplace(sensorID, std::make_pair(sensorIntf, *sensorPath));
}

std::optional<std::string>
    PDRManager::getEffecterAuxNames(const EffecterID& effecterID)
{
    auto iter = _effecterAuxNames.find(effecterID);
    if (iter != _effecterAuxNames.end())
    {
        return iter->second;
    }
    return std::nullopt;
}

std::string PDRManager::createEffecterName(const EffecterID effecterID)
{
    std::string effecterName =
        _deviceAuxName + "_Effecter_" + std::to_string(effecterID);

    _effecterAuxNames[effecterID] = effecterName;

    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        ("createEffecterName " + effecterName).c_str());

    return effecterName;
}

static void populateNumericEffecter(DBusInterfacePtr& effecterIntf,
                                    const DBusObjectPath& path)
{
    const std::string effecterInterface =
        "xyz.openbmc_project.PLDM.NumericEffecter";

    auto objServer = getObjServer();

    effecterIntf = objServer->add_interface(path, effecterInterface);
    // TODO: Expose more numeric effecter info from PDR
    effecterIntf->initialize();
}

std::optional<DBusObjectPath>
    PDRManager::createEffecterObjPath(const pldm_entity& entity,
                                      const EffecterID& effecterID,
                                      const bool8_t auxNamePDR)
{
    // Find effecter name
    std::optional<std::string> effecterName;
    if (auxNamePDR)
    {
        effecterName = getEffecterAuxNames(effecterID);
    }

    if (!effecterName)
    {
        // Dummy name if no effecter Name found
        effecterName = createEffecterName(effecterID);
    }

    std::optional<DBusObjectPath> entityPath = getEntityObjectPath(entity);
    if (!entityPath)
    {
        // If no entity associated, effecter will not be exposed on system
        // hierarchy
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Unable to find Entity Associated with Effecter ID",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("EFFECTER_ID=0x%x", effecterID));
        return std::nullopt;
    }

    return *entityPath + "/" + *effecterName;
}

void PDRManager::parseNumericEffecterPDR(std::vector<uint8_t>& pdrData)
{
    std::vector<uint8_t> pdrOut(sizeof(pldm_numeric_effecter_value_pdr), 0);

    if (!pldm_numeric_effecter_pdr_parse(pdrData.data(),
                                         static_cast<uint16_t>(pdrData.size()),
                                         pdrOut.data()))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Numeric effecter PDR parsing failed",
            phosphor::logging::entry("TID=%d", _tid));
        return;
    }
    pldm_numeric_effecter_value_pdr* effecterPDR =
        reinterpret_cast<pldm_numeric_effecter_value_pdr*>(pdrOut.data());

    uint16_t effecterID = effecterPDR->effecter_id;
    pldm_entity entity = {effecterPDR->entity_type,
                          effecterPDR->entity_instance,
                          effecterPDR->container_id};
    std::optional<DBusObjectPath> effecterPath = createEffecterObjPath(
        entity, effecterID, effecterPDR->effecter_auxiliary_names);
    if (!effecterPath)
    {
        return;
    }

    DBusInterfacePtr effecterIntf;
    populateNumericEffecter(effecterIntf, *effecterPath);
    _effecterIntf.emplace(effecterID,
                          std::make_pair(effecterIntf, *effecterPath));

    std::shared_ptr<pldm_numeric_effecter_value_pdr> numericEffectorPDR =
        std::make_shared<pldm_numeric_effecter_value_pdr>(*effecterPDR);

    _numericEffecterPDR.emplace(effecterID, std::move(numericEffectorPDR));
}

static void populateStateEffecter(DBusInterfacePtr& effecterIntf,
                                  const DBusObjectPath& path)
{
    const std::string effecterInterface =
        "xyz.openbmc_project.PLDM.StateEffecter";

    auto objServer = getObjServer();

    effecterIntf = objServer->add_interface(path, effecterInterface);
    // TODO: Expose more state effecter info from PDR
    effecterIntf->initialize();
}

void PDRManager::parseStateEffecterPDR(std::vector<uint8_t>& pdrData)
{
    // Without composite effecter support there is only one instance of
    // effecter possible states
    // pldm_state_effecter_pdr holds a `uint8 possible_states[1]` which points
    // to state_effecter_possible_states. Subtract its size(1 byte) while
    // calculating total size.
    if (pdrData.size() < sizeof(pldm_state_effecter_pdr) - sizeof(uint8_t) +
                             sizeof(state_effecter_possible_states))
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "State effecter PDR length invalid or effecter disabled",
            phosphor::logging::entry("TID=%d", _tid));
        return;
    }

    pldm_state_effecter_pdr* effecterPDR =
        reinterpret_cast<pldm_state_effecter_pdr*>(pdrData.data());
    LE16TOH(effecterPDR->effecter_id);
    LE16TOH(effecterPDR->entity_type);
    LE16TOH(effecterPDR->entity_instance);
    LE16TOH(effecterPDR->container_id);

    uint16_t effecterID = effecterPDR->effecter_id;

    // TODO: Composite effecter support
    constexpr uint8_t supportedEffecterCount = 0x01;
    if (effecterPDR->composite_effecter_count > supportedEffecterCount)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Composite state effecter not supported",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("EFFECTER_ID=0x%x", effecterID));
    }

    state_effecter_possible_states* possibleState =
        reinterpret_cast<state_effecter_possible_states*>(
            effecterPDR->possible_states);
    LE16TOH(possibleState->state_set_id);

    if (pdrData.size() < sizeof(pldm_state_effecter_pdr) - sizeof(uint8_t) +
                             sizeof(state_effecter_possible_states) -
                             sizeof(uint8_t) +
                             possibleState->possible_states_size)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "State Effecter PDR length invalid",
            phosphor::logging::entry("TID=%d", _tid));
        return;
    }

    PossibleStates possibleStates;
    possibleStates.stateSetID = possibleState->state_set_id;
    constexpr uint8_t maxPossibleStatesSize = 0x20;
    int position = 0;
    for (uint8_t count = 0; count < possibleState->possible_states_size &&
                            count < maxPossibleStatesSize;
         count++)
    {
        for (size_t bits = 0; bits < 8; bits++)
        {
            if (possibleState->states[count].byte & (0x01 << bits))
            {
                possibleStates.possibleStateSetValues.emplace(position);
            }
            position++;
        }
    }

    // Cache PDR for later use
    std::shared_ptr<StateEffecterPDR> stateEffecterPDR =
        std::make_shared<StateEffecterPDR>();
    stateEffecterPDR->stateEffecterData = *effecterPDR;
    // TODO: Multiple state sets in case of composite state effecter
    stateEffecterPDR->possibleStates.emplace_back(std::move(possibleStates));
    _stateEffecterPDR.emplace(effecterID, std::move(stateEffecterPDR));

    pldm_entity entity = {effecterPDR->entity_type,
                          effecterPDR->entity_instance,
                          effecterPDR->container_id};
    std::optional<DBusObjectPath> effecterPath = createEffecterObjPath(
        entity, effecterID, effecterPDR->has_description_pdr);
    if (!effecterPath)
    {
        return;
    }

    DBusInterfacePtr effecterIntf;
    populateStateEffecter(effecterIntf, *effecterPath);
    _effecterIntf.emplace(effecterID,
                          std::make_pair(effecterIntf, *effecterPath));
}

static void populateFRURecordSet(DBusInterfacePtr& fruRSIntf,
                                 const DBusObjectPath& path,
                                 const FRURecordSetIdentifier& fruRSIdentifier)
{
    const std::string effecterInterface =
        "xyz.openbmc_project.PLDM.FRURecordSet";

    auto objServer = getObjServer();

    fruRSIntf = objServer->add_interface(path, effecterInterface);
    fruRSIntf->register_property("FRURecordSetIdentifier", fruRSIdentifier,
                                 sdbusplus::asio::PropertyPermission::readOnly);
    fruRSIntf->initialize();
}

void PDRManager::parseFRURecordSetPDR(std::vector<uint8_t>& pdrData)
{
    if (pdrData.size() != sizeof(pldm_fru_record_set_pdr))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "FRU Record Set PDR length invalid",
            phosphor::logging::entry("TID=%d", _tid));
        return;
    }

    pldm_fru_record_set_pdr* fruRecordSetPDR =
        reinterpret_cast<pldm_fru_record_set_pdr*>(pdrData.data());
    LE16TOH(fruRecordSetPDR->fru_record_set.entity_type);
    LE16TOH(fruRecordSetPDR->fru_record_set.entity_instance_num);
    LE16TOH(fruRecordSetPDR->fru_record_set.container_id);
    LE16TOH(fruRecordSetPDR->fru_record_set.fru_rsi);

    pldm_entity entity = {fruRecordSetPDR->fru_record_set.entity_type,
                          fruRecordSetPDR->fru_record_set.entity_instance_num,
                          fruRecordSetPDR->fru_record_set.container_id};
    FRURecordSetIdentifier fruRSI = fruRecordSetPDR->fru_record_set.fru_rsi;

    std::optional<DBusObjectPath> fruRSPath = getEntityObjectPath(entity);
    if (!fruRSPath)
    {
        // Discard the FRU if there is no entity info matching with Entity
        // Association PDR
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "Unable to find Entity Associated with FRU",
            phosphor::logging::entry("TID=%d", _tid),
            phosphor::logging::entry("FRU_RSI=0x%x", fruRSI));
        return;
    }

    DBusInterfacePtr fruRSIntf;
    populateFRURecordSet(fruRSIntf, *fruRSPath, fruRSI);
    _fruRecordSetIntf.emplace(fruRSI, std::make_pair(fruRSIntf, *fruRSPath));
}

template <pldm_pdr_types pdrType>
void PDRManager::parsePDR()
{
    size_t count = 0;
    uint8_t* pdrData = nullptr;
    uint32_t pdrSize{};
    auto record = pldm_pdr_find_record_by_type(_pdrRepo.get(), pdrType, NULL,
                                               &pdrData, &pdrSize);
    while (record)
    {
        std::vector<uint8_t> pdrVec(pdrData, pdrData + pdrSize);
        // TODO: Move Entity Auxiliary Name PDR and Entity Association PDR
        // parsing here
        if constexpr (pdrType == PLDM_SENSOR_AUXILIARY_NAMES_PDR)
        {
            parseSensorAuxNamesPDR(pdrVec);
        }
        else if constexpr (pdrType == PLDM_EFFECTER_AUXILIARY_NAMES_PDR)
        {
            parseEffecterAuxNamesPDR(pdrVec);
        }
        else if constexpr (pdrType == PLDM_NUMERIC_SENSOR_PDR)
        {
            parseNumericSensorPDR(pdrVec);
        }
        else if constexpr (pdrType == PLDM_STATE_SENSOR_PDR)
        {
            parseStateSensorPDR(pdrVec);
        }
        else if constexpr (pdrType == PLDM_NUMERIC_EFFECTER_PDR)
        {
            parseNumericEffecterPDR(pdrVec);
        }
        else if constexpr (pdrType == PLDM_STATE_EFFECTER_PDR)
        {
            parseStateEffecterPDR(pdrVec);
        }
        else if constexpr (pdrType == PLDM_PDR_FRU_RECORD_SET)
        {
            parseFRURecordSetPDR(pdrVec);
        }
        else if constexpr (pdrType == PLDM_ENTITY_AUXILIARY_NAMES_PDR)
        {
            parseEntityAuxNamesPDR(pdrVec);
        }
        else if constexpr (pdrType == PLDM_PDR_ENTITY_ASSOCIATION)
        {
            parseEntityAssociationPDR(pdrVec);
        }
        else
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Not supported. Unknown PDR type");
            return;
        }

        count++;
        pdrData = nullptr;
        pdrSize = 0;
        record = pldm_pdr_find_record_by_type(_pdrRepo.get(), pdrType, record,
                                              &pdrData, &pdrSize);
    }

    if constexpr (pdrType == PLDM_PDR_ENTITY_ASSOCIATION)
    {
        if (entityAssociationNodes.size())
        {
            createEntityAssociationTree(entityAssociationNodes);
        }
    }
    phosphor::logging::log<phosphor::logging::level::DEBUG>(
        ("Number of type " + std::to_string(pdrType) +
         " PDR parsed: " + std::to_string(count))
            .c_str());
}

std::optional<std::shared_ptr<pldm_numeric_sensor_value_pdr>>
    PDRManager::getNumericSensorPDR(const SensorID& sensorID)
{
    auto iter = _numericSensorPDR.find(sensorID);
    if (iter != _numericSensorPDR.end())
    {
        return iter->second;
    }
    return std::nullopt;
}

std::optional<std::shared_ptr<StateSensorPDR>>
    PDRManager::getStateSensorPDR(const SensorID& sensorID)
{
    auto iter = _stateSensorPDR.find(sensorID);
    if (iter != _stateSensorPDR.end())
    {
        return iter->second;
    }
    return std::nullopt;
}

std::optional<std::shared_ptr<pldm_numeric_effecter_value_pdr>>
    PDRManager::getNumericEffecterPDR(const EffecterID& effecterID)
{
    auto iter = _numericEffecterPDR.find(effecterID);
    if (iter != _numericEffecterPDR.end())
    {
        return iter->second;
    }
    return std::nullopt;
}

std::shared_ptr<StateEffecterPDR>
    PDRManager::getStateEffecterPDR(const SensorID& effecterID)
{
    auto iter = _stateEffecterPDR.find(effecterID);
    if (iter != _stateEffecterPDR.end())
    {
        return iter->second;
    }
    return nullptr;
}

bool PDRManager::pdrManagerInit(boost::asio::yield_context yield)
{
    std::optional<pldm_pdr_repository_info> pdrInfo =
        getPDRRepositoryInfo(yield);
    if (!pdrInfo)
    {
        return false;
    }
    pdrRepoInfo = *pdrInfo;
    printPDRInfo(pdrRepoInfo);

    PDRRepo pdrRepo(pldm_pdr_init(), pldm_pdr_destroy);
    _pdrRepo = std::move(pdrRepo);

    if (!constructPDRRepo(yield))
    {
        return false;
    }

    initializePDRDumpIntf();

    parsePDR<PLDM_ENTITY_AUXILIARY_NAMES_PDR>();
    parsePDR<PLDM_PDR_ENTITY_ASSOCIATION>();
    getEntityAssociationPaths(_entityAssociationTree, {});
    populateSystemHierarchy();
    extractDeviceAuxName(_entityAssociationTree);
#ifdef EXPOSE_CHASSIS
    initializeInventoryIntf();
#endif
    parsePDR<PLDM_SENSOR_AUXILIARY_NAMES_PDR>();
    parsePDR<PLDM_EFFECTER_AUXILIARY_NAMES_PDR>();
    parsePDR<PLDM_NUMERIC_SENSOR_PDR>();
    parsePDR<PLDM_STATE_SENSOR_PDR>();
    parsePDR<PLDM_NUMERIC_EFFECTER_PDR>();
    parsePDR<PLDM_STATE_EFFECTER_PDR>();
    parsePDR<PLDM_PDR_FRU_RECORD_SET>();

    return true;
}

struct PDRDump
{
    PDRDump(const std::string& fileName) : pdrFile(fileName)
    {
    }
    void dumpPDRData(const std::vector<uint8_t>& pdr)
    {
        std::stringstream ss;
        const pldm_pdr_hdr* pdrHdr =
            reinterpret_cast<const pldm_pdr_hdr*>(pdr.data());
        ss << "PDR Type: " << static_cast<int>(pdrHdr->type) << std::endl;
        ss << "Length: " << pdr.size() << std::endl;
        ss << "Data: ";
        for (auto re : pdr)
        {
            ss << " 0x" << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(re);
        }
        pdrFile << ss.rdbuf() << std::endl;
    }

  private:
    std::ofstream pdrFile;
};

void PDRManager::initializePDRDumpIntf()
{
    std::string pldmDevObj =
        "/xyz/openbmc_project/system/" + std::to_string(_tid);
    auto objServer = getObjServer();
    pdrDumpInterface =
        objServer->add_interface(pldmDevObj, "xyz.openbmc_project.PLDM.PDR");
    pdrDumpInterface->register_method("DumpPDR", [this](void) {
        uint32_t noOfRecords = pldm_pdr_get_record_count(_pdrRepo.get());
        if (!noOfRecords)
        {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "PDR repo empty!");
            return;
        }

        std::unique_ptr<PDRDump> pdrDump = std::make_unique<PDRDump>(
            "/tmp/pldm_pdr_dump_" + std::to_string(_tid) + ".txt");

        for (uint8_t pdrType = PLDM_TERMINUS_LOCATOR_PDR;
             pdrType != PLDM_OEM_PDR; pdrType++)
        {

            uint8_t* pdrData = nullptr;
            uint32_t pdrSize{};
            auto record = pldm_pdr_find_record_by_type(
                _pdrRepo.get(), pdrType, NULL, &pdrData, &pdrSize);
            while (record)
            {
                std::vector<uint8_t> pdrVec(pdrData, pdrData + pdrSize);
                pdrDump->dumpPDRData(pdrVec);

                if (!(--noOfRecords))
                {
                    return;
                }
                pdrData = nullptr;
                pdrSize = 0;
                record = pldm_pdr_find_record_by_type(
                    _pdrRepo.get(), pdrType, record, &pdrData, &pdrSize);
            }
        }
    });
    pdrDumpInterface->initialize();
}
} // namespace platform
} // namespace pldm
