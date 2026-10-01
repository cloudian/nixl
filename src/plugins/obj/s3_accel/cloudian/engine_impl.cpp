/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cloudian, Inc. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "engine_impl.h"
#include "client.h"
#include "common/nixl_log.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <utility>
#include <absl/strings/str_format.h>
#include <aws/s3/S3ClientRDMA.h>

#include "obj_engine_registry.h"

namespace {

objAccelEngineRegistrar reg_cloudian(
    "cloudian",
    [](const nixlBackendInitParams *p) { return std::make_unique<CloudianObjEngineImpl>(p); },
    [](const nixlBackendInitParams *p, std::shared_ptr<iS3Client> s3, std::shared_ptr<iS3Client>) {
        return std::make_unique<CloudianObjEngineImpl>(p, std::move(s3));
    });

/**
 * Validate parameters for prepXfer operation.
 * Ensures operation type, memory types, and descriptor counts are valid.
 *
 * @param operation Transfer operation type
 * @param local Local memory descriptor list
 * @param remote Remote memory descriptor list
 * @param remote_agent Remote agent identifier
 * @param local_agent Local agent identifier
 * @return true if parameters are valid, false otherwise
 */
bool
isValidPrepXferParams(const nixl_xfer_op_t &operation,
                      const nixl_meta_dlist_t &local,
                      const nixl_meta_dlist_t &remote,
                      const std::string &remote_agent,
                      const std::string &local_agent) {
    if (operation != NIXL_WRITE && operation != NIXL_READ) {
        NIXL_ERROR << absl::StrFormat("Error: Invalid operation type: %d", operation);
        return false;
    }

    if (remote_agent != local_agent) {
        NIXL_WARN << absl::StrFormat(
            "Warning: Remote agent doesn't match the requesting agent (%s). Got %s",
            local_agent,
            remote_agent);
    }

    if ((local.getType() != DRAM_SEG) && (local.getType() != VRAM_SEG)) {
        NIXL_ERROR << absl::StrFormat(
            "Error: Local memory type must be VRAM_SEG or DRAM_SEG, got %d", local.getType());
        return false;
    }

    if (remote.getType() != OBJ_SEG) {
        NIXL_ERROR << absl::StrFormat("Error: Remote memory type must be OBJ_SEG, got %d",
                                      remote.getType());
        return false;
    }

    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << absl::StrFormat(
            "Error: Local and remote descriptor counts must match. Got %d local, %d remote",
            local.descCount(),
            remote.descCount());
        return false;
    }

    return true;
}

/**
 * Backend request handle for Cloudian RDMA operations.
 * Tracks completion of the async {Put,Get}ObjectRDMAAsync calls issued in postXfer.
 */
class nixlCloudianObjBackendReqH : public nixlBackendReqH {
public:
    nixlCloudianObjBackendReqH() = default;
    ~nixlCloudianObjBackendReqH() = default;

    /// Futures for tracking completion status
    std::vector<std::future<nixl_status_t>> statusFutures_;
    nixl_status_t terminalStatus_ = NIXL_SUCCESS;

    /**
     * Get the overall status of all transfer requests.
     *
     * @return Overall transfer status
     */
    nixl_status_t
    getOverallStatus() {
        auto it = statusFutures_.begin();
        while (it != statusFutures_.end()) {
            if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto current_status = it->get();
                if (current_status != NIXL_SUCCESS && terminalStatus_ == NIXL_SUCCESS) {
                    terminalStatus_ = current_status;
                }
                it = statusFutures_.erase(it);
            } else {
                // Pending callbacks retain non-owning RdmaPtr slices, so they must complete
                // before the request can be released and its parent registration destroyed.
                return NIXL_IN_PROG;
            }
        }
        return terminalStatus_;
    }
};

/**
 * Metadata for Cloudian RDMA operations.
 * For OBJ_SEG, records the object key. For DRAM_SEG/VRAM_SEG, owns the RDMA registration
 * created in registerMem(); destroying it releases the registration.
 */
class nixlCloudianObjMetadata : public nixlBackendMD {
public:
    /**
     * Constructor for object segments.
     *
     * @param nixl_mem Memory type
     * @param dev_id Device ID
     * @param obj_key Object key in S3
     */
    nixlCloudianObjMetadata(nixl_mem_t nixl_mem, uint64_t dev_id, std::string obj_key)
        : nixlBackendMD(true),
          nixlMem(nixl_mem),
          devId(dev_id),
          objKey(std::move(obj_key)) {}

    /**
     * Constructor for memory segments (DRAM/VRAM), taking ownership of the RDMA registration.
     *
     * @param nixl_mem Memory type
     * @param rdma_ptr RDMA-registered buffer handle
     */
    nixlCloudianObjMetadata(nixl_mem_t nixl_mem, std::unique_ptr<Aws::S3::RdmaPtr> rdma_ptr)
        : nixlBackendMD(true),
          nixlMem(nixl_mem),
          devId(0),
          rdmaPtr(std::move(rdma_ptr)) {}

    ~nixlCloudianObjMetadata() = default;

    /// NIXL memory type
    nixl_mem_t nixlMem;
    /// Device ID for object segments
    uint64_t devId;
    /// Object key in S3
    std::string objKey;
    /// RDMA registration owned for DRAM_SEG/VRAM_SEG
    std::unique_ptr<Aws::S3::RdmaPtr> rdmaPtr;
};

} // namespace

/**
 * Constructor for CloudianObjEngineImpl.
 *
 * @param init_params Backend initialization parameters containing custom S3 configuration
 */
CloudianObjEngineImpl::CloudianObjEngineImpl(const nixlBackendInitParams *init_params)
    : S3AccelObjEngineImpl(init_params) {
    nixl_b_params_t local_params;
    nixl_b_params_t *params_to_use = init_params->customParams;
    if (params_to_use == nullptr) {
        params_to_use = &local_params;
    }
    s3Client_ = std::make_shared<CloudianClient>(params_to_use, executor_);
    NIXL_INFO << "Object storage backend initialized with S3 Cloudian client";
}

/**
 * Constructor for CloudianObjEngineImpl with injected S3 client.
 * Used primarily for testing with mock S3 clients.
 *
 * @param init_params Backend initialization parameters
 * @param s3_client Pre-configured S3 client (can be mock for testing)
 */
CloudianObjEngineImpl::CloudianObjEngineImpl(const nixlBackendInitParams *init_params,
                                             std::shared_ptr<iS3Client> s3_client)
    : S3AccelObjEngineImpl(init_params, s3_client) {
    if (!s3_client) {
        nixl_b_params_t local_params;
        nixl_b_params_t *params_to_use = init_params->customParams;
        if (params_to_use == nullptr) {
            local_params["resp_checksum"] = "required";
            params_to_use = &local_params;
        } else {
            params_to_use->emplace("resp_checksum", "required");
        }
        s3Client_ = std::make_shared<CloudianClient>(params_to_use, executor_);
    }
    NIXL_INFO << "Object storage backend initialized with S3 Cloudian client";
}

iS3Client *
CloudianObjEngineImpl::getClient() const {
    return s3Client_.get();
}

/**
 * Register memory with the backend.
 * For OBJ_SEG, maps the device ID to an object key. For DRAM_SEG/VRAM_SEG, RDMA-registers the
 * buffer up front so postXfer can pass registered=true and skip re-registration per transfer.
 *
 * @param mem Memory blob descriptor
 * @param nixl_mem Memory type (OBJ_SEG, DRAM_SEG, or VRAM_SEG)
 * @param out Output backend metadata handle
 * @return NIXL_SUCCESS on success, NIXL_ERR_BACKEND on registration failure, NIXL_ERR_NOT_SUPPORTED
 * for unsupported memory types
 */
nixl_status_t
CloudianObjEngineImpl::registerMem(const nixlBlobDesc &mem,
                                   const nixl_mem_t &nixl_mem,
                                   nixlBackendMD *&out) {
    NIXL_DEBUG << "Cloudian-specific registerMem called";
    const nixl_mem_list_t supported_mems = getSupportedMems();
    if (std::find(supported_mems.begin(), supported_mems.end(), nixl_mem) == supported_mems.end()) {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    if (nixl_mem == OBJ_SEG) {
        auto obj_md = std::make_unique<nixlCloudianObjMetadata>(
            nixl_mem, mem.devId, mem.metaInfo.empty() ? std::to_string(mem.devId) : mem.metaInfo);
        devIdToObjKey_[mem.devId] = obj_md->objKey;
        out = obj_md.release();
        return NIXL_SUCCESS;
    }

    if (nixl_mem == VRAM_SEG && !Aws::S3::S3ClientRDMA::IsRDMAEnabled()) {
        // Without RDMA every transfer goes over TCP through a host stream buffer, which
        // cannot touch device memory. Fail here rather than at every postXfer.
        NIXL_ERROR << "Cannot register VRAM_SEG: RDMA is unavailable, so GPU buffers cannot be "
                      "transferred. Use DRAM_SEG instead.";
        return NIXL_ERR_NOT_SUPPORTED;
    }

    auto rdma_ptr =
        std::make_unique<Aws::S3::RdmaPtr>(reinterpret_cast<const void *>(mem.addr), mem.len);
    if (!static_cast<bool>(*rdma_ptr)) {
        NIXL_ERROR << "Failed to RDMA-register buffer at addr=" << mem.addr << ", len=" << mem.len;
        return NIXL_ERR_BACKEND;
    }

    out = std::make_unique<nixlCloudianObjMetadata>(nixl_mem, std::move(rdma_ptr)).release();
    return NIXL_SUCCESS;
}

/**
 * Deregister memory from the backend.
 * Releases the RDMA registration created in registerMem(), if any.
 *
 * @param meta Backend metadata handle to deregister
 * @return NIXL_SUCCESS on success
 */
nixl_status_t
CloudianObjEngineImpl::deregisterMem(nixlBackendMD *meta) {
    std::unique_ptr<nixlCloudianObjMetadata> md(static_cast<nixlCloudianObjMetadata *>(meta));
    if (md && md->nixlMem == OBJ_SEG) {
        devIdToObjKey_.erase(md->devId);
    }
    // For DRAM_SEG/VRAM_SEG, destroying md releases rdmaPtr, deregistering the buffer.
    return NIXL_SUCCESS;
}

/**
 * Prepare a transfer operation between local and remote memory.
 * Validates parameters and creates a transfer request handle for subsequent execution. No
 * further setup is needed here; local buffers are RDMA-registered up front in registerMem().
 *
 * @param operation Transfer operation (NIXL_READ or NIXL_WRITE)
 * @param local Local memory descriptor list (must be DRAM_SEG or VRAM_SEG)
 * @param remote Remote memory descriptor list (must be OBJ_SEG)
 * @param remote_agent Remote agent identifier
 * @param local_agent Local agent identifier
 * @param handle Output transfer request handle
 * @param opt_args Optional backend arguments (unused)
 * @return NIXL_SUCCESS on success, NIXL_ERR_INVALID_PARAM on validation failure
 */
nixl_status_t
CloudianObjEngineImpl::prepXfer(const nixl_xfer_op_t &operation,
                                const nixl_meta_dlist_t &local,
                                const nixl_meta_dlist_t &remote,
                                const std::string &remote_agent,
                                const std::string &local_agent,
                                nixlBackendReqH *&handle,
                                const nixl_opt_b_args_t *opt_args) const {
    if (!isValidPrepXferParams(operation, local, remote, remote_agent, local_agent)) {
        return NIXL_ERR_INVALID_PARAM;
    }

    auto req_h = std::make_unique<nixlCloudianObjBackendReqH>();
    handle = req_h.release();
    return NIXL_SUCCESS;
}

/**
 * Post a transfer operation for execution.
 * Initiates asynchronous RDMA S3 operations via s3Client_->{put,get}ObjectAsync, which for
 * Cloudian route through the patched SDK's {Put,Get}ObjectRDMAAsync methods.
 *
 * @param operation Transfer operation (NIXL_READ or NIXL_WRITE)
 * @param local Local memory descriptor list
 * @param remote Remote memory descriptor list
 * @param remote_agent Remote agent identifier
 * @param handle Transfer request handle from prepXfer
 * @param opt_args Optional backend arguments (unused)
 * @return NIXL_IN_PROG if operation started successfully, error code on failure
 */
nixl_status_t
CloudianObjEngineImpl::postXfer(const nixl_xfer_op_t &operation,
                                const nixl_meta_dlist_t &local,
                                const nixl_meta_dlist_t &remote,
                                const std::string &remote_agent,
                                nixlBackendReqH *&handle,
                                const nixl_opt_b_args_t *opt_args) const {
    if (handle == nullptr) {
        NIXL_ERROR << "transfer request handle is null";
        return NIXL_ERR_INVALID_PARAM;
    }

    // Re-checked here as well as in registerMem: the SDK disables RDMA process-wide the first
    // time a server declines it, which can happen long after the buffers were registered.
    if (local.getType() == VRAM_SEG && !Aws::S3::S3ClientRDMA::IsRDMAEnabled()) {
        NIXL_ERROR << "RDMA has been disabled (the server declined it), so GPU buffers can no "
                      "longer be transferred. Re-register the memory as DRAM_SEG.";
        return NIXL_ERR_BACKEND;
    }

    nixlCloudianObjBackendReqH *req_h = static_cast<nixlCloudianObjBackendReqH *>(handle);
    auto *cloudian_client = dynamic_cast<CloudianClient *>(s3Client_.get());

    // Validate every descriptor before submitting any asynchronous operation. This keeps an
    // invalid request from leaving earlier operations active while the caller tears it down.
    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];
        const auto &remote_desc = remote[i];
        if (devIdToObjKey_.find(remote_desc.devId) == devIdToObjKey_.end()) {
            NIXL_ERROR << "The object segment key " << remote_desc.devId
                       << " is not registered with the backend";
            return NIXL_ERR_INVALID_PARAM;
        }
        if (local_desc.len == 0) {
            NIXL_ERROR << "transfer length must be non-zero";
            return NIXL_ERR_INVALID_PARAM;
        }

        auto *local_md = static_cast<nixlCloudianObjMetadata *>(local_desc.metadataP);
        if (cloudian_client && local_md && local_md->rdmaPtr) {
            const auto &registration = *local_md->rdmaPtr;
            const auto base = reinterpret_cast<uintptr_t>(registration.registered_data());
            if (local_desc.addr < base || local_desc.addr - base > registration.size() ||
                local_desc.len > registration.size() - (local_desc.addr - base)) {
                NIXL_ERROR << "Descriptor [" << local_desc.addr << ", +" << local_desc.len
                           << ") is outside its registered buffer [" << base << ", +"
                           << registration.size() << ")";
                return NIXL_ERR_INVALID_PARAM;
            }
        }
    }

    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];
        const auto &remote_desc = remote[i];

        auto obj_key_search = devIdToObjKey_.find(remote_desc.devId);

        auto status_promise = std::make_shared<std::promise<nixl_status_t>>();
        req_h->statusFutures_.push_back(status_promise->get_future());

        uintptr_t data_ptr = local_desc.addr;
        size_t data_len = local_desc.len;
        size_t offset = remote_desc.addr;

        // S3 client interface signals completion via a callback, but NIXL API polls request handle
        // for the status code. Use future/promise pair to bridge the gap.
        auto status_callback = [status_promise](bool success) {
            status_promise->set_value(success ? NIXL_SUCCESS : NIXL_ERR_BACKEND);
        };

        // cuMemObjGetRDMAToken() only accepts the address the buffer was registered with, so a
        // descriptor covering part of a registration must be sent as a slice of it, not as a
        // fresh RdmaPtr over the interior address.
        auto *local_md = static_cast<nixlCloudianObjMetadata *>(local_desc.metadataP);
        if (cloudian_client && local_md && local_md->rdmaPtr) {
            const auto &registration = *local_md->rdmaPtr;
            const auto base = reinterpret_cast<uintptr_t>(registration.registered_data());
            auto slice = registration.slice(data_ptr - base, data_len);

            if (operation == NIXL_WRITE) {
                cloudian_client->putObjectRdmaAsync(
                    obj_key_search->second, std::move(slice), offset, status_callback);
            } else {
                cloudian_client->getObjectRdmaAsync(
                    obj_key_search->second, std::move(slice), offset, status_callback);
            }
        } else {
            if (operation == NIXL_WRITE) {
                s3Client_->putObjectAsync(
                    obj_key_search->second, data_ptr, data_len, offset, status_callback);
            } else {
                s3Client_->getObjectAsync(
                    obj_key_search->second, data_ptr, data_len, offset, status_callback);
            }
        }
    }

    return NIXL_IN_PROG;
}

/**
 * Check the status of an ongoing transfer operation.
 *
 * @param handle Transfer request handle to check
 * @return NIXL_SUCCESS if completed, NIXL_IN_PROG if ongoing, error code on failure
 */
nixl_status_t
CloudianObjEngineImpl::checkXfer(nixlBackendReqH *handle) const {
    if (handle == nullptr) {
        NIXL_ERROR << "transfer request handle is null";
        return NIXL_ERR_INVALID_PARAM;
    }
    // Once releaseReqH has reported a failed release attempt, nixlXferReqH caches that
    // negative status and this method is no longer called for that handle (per
    // docs/BackendGuide.md: "checkXferReq should return error if there was a call to
    // abort which was not successful") - enforced by the agent, not by this backend.
    nixlCloudianObjBackendReqH *req_h = static_cast<nixlCloudianObjBackendReqH *>(handle);
    return req_h->getOverallStatus();
}

/**
 * Release a transfer request handle.
 *
 * @param handle Transfer request handle to release
 * @return NIXL_SUCCESS on success
 */
nixl_status_t
CloudianObjEngineImpl::releaseReqH(nixlBackendReqH *handle) const {
    if (handle == nullptr) {
        NIXL_ERROR << "transfer request handle is null";
        return NIXL_ERR_INVALID_PARAM;
    }
    nixlCloudianObjBackendReqH *req_h = static_cast<nixlCloudianObjBackendReqH *>(handle);
    // nixlAgent::releaseXferReq only treats a negative status as failure, so returning
    // NIXL_IN_PROG here would make it delete req_h anyway while callbacks referencing
    // non-owning RdmaPtr slices are still outstanding. In-flight RDMA ops can stall
    // indefinitely on network congestion/disconnects, so poll rather than block: per
    // docs/BackendGuide.md ("calls to releaseXferReq would return error until the
    // transfer is completed and it returns success"), report failure until the ops
    // drain on their own, letting the caller retry the release.
    if (req_h->getOverallStatus() == NIXL_IN_PROG) {
        return NIXL_ERR_BACKEND;
    }
    delete req_h;
    return NIXL_SUCCESS;
}
