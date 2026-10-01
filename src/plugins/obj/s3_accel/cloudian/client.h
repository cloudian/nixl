/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cloudian, Inc. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include "s3_accel/client.h"

#include <aws/s3/RdmaPtr.h>

/**
 * S3 Accelerated Object Client for use with Cloudian HyperStore - Inherits from Accelerated S3
 * Client. Relies on a patched AWS S3 SDK that adds GetObjectRDMA(Async)/PutObjectRDMA(Async) to
 * Aws::S3::S3Client, and overrides putObjectAsync/getObjectAsync to route through them. Buffers
 * passed through the raw-address interface are registered by the Cloudian client; buffers passed
 * as RdmaPtr instances are already registered and may be slices of an existing registration.
 */
class CloudianClient : public awsS3AccelClient {
public:
    /**
     * Constructor that creates an AWS S3 client for use with Cloudian from custom parameters.
     * @param custom_params Custom parameters containing S3 configuration
     * @param executor Optional executor for async operations
     */
    CloudianClient(nixl_b_params_t *custom_params,
                   std::shared_ptr<Aws::Utils::Threading::Executor> executor = nullptr);

    virtual ~CloudianClient() = default;

    /**
     * Asynchronously puts an object to S3 using RDMA acceleration.
     *
     * @param key The object key to store
     * @param data_ptr Pointer to the data buffer
     * @param data_len Length of the data to transfer
     * @param offset Offset within the object (only 0 is currently supported)
     * @param callback Callback function invoked on completion
     */
    void
    putObjectAsync(std::string_view key,
                   uintptr_t data_ptr,
                   size_t data_len,
                   size_t offset,
                   put_object_callback_t callback) override;

    /**
     * Asynchronously gets an object from S3 using RDMA acceleration.
     *
     * @param key The object key to retrieve
     * @param data_ptr Pointer to the data buffer to fill
     * @param data_len Length of the data to transfer
     * @param offset Offset within the object to start reading from
     * @param callback Callback function invoked on completion
     */
    void
    getObjectAsync(std::string_view key,
                   uintptr_t data_ptr,
                   size_t data_len,
                   size_t offset,
                   get_object_callback_t callback) override;

    /**
     * Asynchronously puts an object to S3 from an already RDMA-registered buffer.
     * cuMemObjGetRDMAToken() requires the base address the buffer was registered with, so
     * sub-range transfers must be expressed as a slice of the original registration rather
     * than as a fresh RdmaPtr over the interior pointer.
     *
     * @param key The object key to store
     * @param buf Registered buffer (possibly a slice) holding the data to transfer
     * @param offset Offset within the object (only 0 is currently supported)
     * @param callback Callback function invoked on completion
     */
    void
    putObjectRdmaAsync(std::string_view key,
                       Aws::S3::RdmaPtr &&buf,
                       size_t offset,
                       put_object_callback_t callback);

    /**
     * Asynchronously gets an object from S3 into an already RDMA-registered buffer.
     * See putObjectRdmaAsync() for why a slice of the original registration is required.
     *
     * @param key The object key to retrieve
     * @param buf Registered buffer (possibly a slice) to fill
     * @param offset Offset within the object to start reading from
     * @param callback Callback function invoked on completion
     */
    void
    getObjectRdmaAsync(std::string_view key,
                       Aws::S3::RdmaPtr &&buf,
                       size_t offset,
                       get_object_callback_t callback);
};
