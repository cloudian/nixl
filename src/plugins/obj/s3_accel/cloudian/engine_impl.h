/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cloudian, Inc. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include "s3_accel/engine_impl.h"
#include "s3_accel/cloudian/client.h"

/**
 * S3 Cloudian Engine Implementation.
 * Accelerated engine for Cloudian HyperStore object storage. CloudianClient overrides the
 * base putObjectAsync/getObjectAsync to route through the patched AWS S3 SDK's RDMA-capable
 * Put/GetObjectRDMA methods, so this engine can reuse the plain iS3Client interface.
 */
class CloudianObjEngineImpl : public S3AccelObjEngineImpl {
public:
    /**
     * Constructor that initializes the S3 Cloudian engine.
     *
     * @param init_params Backend initialization parameters
     */
    explicit CloudianObjEngineImpl(const nixlBackendInitParams *init_params);
    /**
     * Constructor that accepts an injected S3 client (for testing).
     *
     * @param init_params Backend initialization parameters
     * @param s3_client Pre-configured S3 client (can be mock for testing)
     */
    CloudianObjEngineImpl(const nixlBackendInitParams *init_params,
                          std::shared_ptr<iS3Client> s3_client);

    /**
     * Get the list of supported memory types.
     *
     * @return List of supported memory segments (OBJ_SEG, DRAM_SEG, VRAM_SEG)
     */
    nixl_mem_list_t
    getSupportedMems() const override {
        return {DRAM_SEG, OBJ_SEG, VRAM_SEG};
    }

    /**
     * Register memory with the backend.
     * For OBJ_SEG, maps the device ID to an object key. For DRAM_SEG/VRAM_SEG, RDMA-registers
     * the buffer up front so postXfer can reuse the registration instead of registering on
     * every transfer.
     *
     * @param mem Memory blob descriptor
     * @param nixl_mem Memory type
     * @param out Output backend metadata handle
     * @return NIXL_SUCCESS on success, error code on failure
     */
    nixl_status_t
    registerMem(const nixlBlobDesc &mem, const nixl_mem_t &nixl_mem, nixlBackendMD *&out) override;

    /**
     * Deregister memory from the backend.
     * Releases the RDMA registration created in registerMem, if any.
     *
     * @param meta Backend metadata handle to deregister
     * @return NIXL_SUCCESS on success
     */
    nixl_status_t
    deregisterMem(nixlBackendMD *meta) override;

    /**
     * Prepare a transfer operation between local and remote memory.
     * Validates parameters and creates a transfer request handle for subsequent execution.
     *
     * @param operation Transfer operation (NIXL_READ or NIXL_WRITE)
     * @param local Local memory descriptor list
     * @param remote Remote memory descriptor list
     * @param remote_agent Remote agent identifier
     * @param local_agent Local agent identifier
     * @param handle Output transfer request handle
     * @param opt_args Optional backend arguments
     * @return NIXL_SUCCESS on success, error code on failure
     */
    nixl_status_t
    prepXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             const std::string &local_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args) const override;

    /**
     * Post a transfer operation for execution.
     * Initiates asynchronous RDMA S3 operations via the underlying iS3Client, which for Cloudian
     * routes through the patched SDK's RDMA-capable methods. Uses futures/promises to bridge
     * callback and polling interfaces.
     *
     * @param operation Transfer operation (NIXL_READ or NIXL_WRITE)
     * @param local Local memory descriptor list
     * @param remote Remote memory descriptor list
     * @param remote_agent Remote agent identifier
     * @param handle Transfer request handle from prepXfer
     * @param opt_args Optional backend arguments
     * @return NIXL_IN_PROG if operation started, error code on failure
     */
    nixl_status_t
    postXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    /**
     * Check the status of an ongoing transfer operation.
     *
     * @param handle Transfer request handle to check
     * @return NIXL_SUCCESS if completed, NIXL_IN_PROG if ongoing, error code on failure
     */
    nixl_status_t
    checkXfer(nixlBackendReqH *handle) const override;

    /**
     * Release a transfer request handle.
     *
     * @param handle Transfer request handle to release
     * @return NIXL_SUCCESS on success
     */
    nixl_status_t
    releaseReqH(nixlBackendReqH *handle) const override;

protected:
    /**
     * Get the S3 client instance.
     *
     * @return Pointer to the S3 client interface
     */
    iS3Client *
    getClient() const override;
};
