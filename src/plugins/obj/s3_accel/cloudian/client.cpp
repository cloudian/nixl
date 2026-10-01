/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 Cloudian, Inc. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "client.h"
#include "common/nixl_log.h"

#include <absl/strings/str_format.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/PutObjectRDMARequest.h>
#include <aws/s3/RdmaPtr.h>

/**
 * Implementation of CloudianClient constructor.
 * Initializes the Cloudian S3 client with custom parameters.
 *
 * @param custom_params Custom parameters containing S3 configuration
 * @param executor Optional executor for async operations
 */
CloudianClient::CloudianClient(nixl_b_params_t *custom_params,
                               std::shared_ptr<Aws::Utils::Threading::Executor> executor)
    : awsS3AccelClient(custom_params, executor) {
    NIXL_DEBUG << "Initialized Cloudian Client";
}

/**
 * Asynchronously puts an object to S3 using RDMA acceleration.
 * The patched SDK registers the caller's buffer as an RdmaPtr and transfers it over RDMA
 * without any manual header/descriptor handling.
 *
 * @param key The object key to store
 * @param data_ptr Pointer to the data buffer
 * @param data_len Length of the data to transfer
 * @param offset Offset within the object (only 0 is currently supported)
 * @param callback Callback function invoked on completion with success/failure status
 */
void
CloudianClient::putObjectAsync(std::string_view key,
                               uintptr_t data_ptr,
                               size_t data_len,
                               size_t offset,
                               put_object_callback_t callback) {
    putObjectRdmaAsync(key,
                       Aws::S3::RdmaPtr(reinterpret_cast<const void *>(data_ptr), data_len),
                       offset,
                       std::move(callback));
}

/**
 * Asynchronously puts an already-registered (possibly sliced) buffer to S3 using RDMA.
 *
 * @param key The object key to store
 * @param buf Registered buffer holding the data to transfer
 * @param offset Offset within the object (only 0 is currently supported)
 * @param callback Callback function invoked on completion with success/failure status
 */
void
CloudianClient::putObjectRdmaAsync(std::string_view key,
                                   Aws::S3::RdmaPtr &&buf,
                                   size_t offset,
                                   put_object_callback_t callback) {
    if (!buf) {
        NIXL_ERROR << "putObjectAsync: failed to register RDMA buffer";
        callback(false);
        return;
    }

    if (buf.size() == 0) {
        NIXL_ERROR << "putObjectAsync: data_len is 0, returning failure";
        callback(false);
        return;
    }

    if (offset != 0) {
        NIXL_ERROR << "putObjectAsync: offset is not 0, returning failure";
        callback(false);
        return;
    }

    Aws::S3::Model::PutObjectRDMARequest request;
    request.WithBucket(bucketName_).WithKey(Aws::String(key));

    s3Client_->PutObjectRDMAAsync(
        request,
        std::move(buf),
        [callback](const Aws::S3::S3Client *,
                   const Aws::S3::Model::PutObjectRDMARequest &,
                   Aws::S3::RdmaPtr &&,
                   Aws::S3::Model::PutObjectRDMAOutcome &&outcome,
                   const std::shared_ptr<const Aws::Client::AsyncCallerContext> &) {
            if (!outcome.IsSuccess()) {
                const auto &error = outcome.GetError();
                NIXL_ERROR << absl::StrFormat("putObjectAsync: PutObjectRDMAAsync failed - %s: %s",
                                              error.GetExceptionName().c_str(),
                                              error.GetMessage().c_str());
            }
            callback(outcome.IsSuccess());
        });
}

/**
 * Asynchronously gets an object from S3 using RDMA acceleration.
 * The patched SDK registers the caller's buffer as an RdmaPtr and writes into it over RDMA
 * without any manual header/descriptor handling.
 *
 * @param key The object key to retrieve
 * @param data_ptr Pointer to the data buffer to fill
 * @param data_len Length of the data to transfer
 * @param offset Offset within the object to start reading from
 * @param callback Callback function invoked on completion with success/failure status
 */
void
CloudianClient::getObjectAsync(std::string_view key,
                               uintptr_t data_ptr,
                               size_t data_len,
                               size_t offset,
                               get_object_callback_t callback) {
    getObjectRdmaAsync(key,
                       Aws::S3::RdmaPtr(reinterpret_cast<void *>(data_ptr), data_len),
                       offset,
                       std::move(callback));
}

/**
 * Asynchronously gets an object from S3 into an already-registered (possibly sliced) buffer.
 *
 * @param key The object key to retrieve
 * @param buf Registered buffer to fill
 * @param offset Offset within the object to start reading from
 * @param callback Callback function invoked on completion with success/failure status
 */
void
CloudianClient::getObjectRdmaAsync(std::string_view key,
                                   Aws::S3::RdmaPtr &&buf,
                                   size_t offset,
                                   get_object_callback_t callback) {
    if (!buf) {
        NIXL_ERROR << "getObjectAsync: failed to register RDMA buffer";
        callback(false);
        return;
    }

    const size_t data_len = buf.size();
    if (data_len == 0) {
        NIXL_ERROR << "getObjectAsync: data_len is 0, returning failure";
        callback(false);
        return;
    }

    if (offset > (SIZE_MAX - (data_len - 1))) {
        NIXL_ERROR << "getObjectAsync: offset + data_len would overflow, returning failure";
        callback(false);
        return;
    }

    Aws::S3::Model::GetObjectRequest request;
    request.WithBucket(bucketName_)
        .WithKey(Aws::String(key))
        .WithRange(absl::StrFormat("bytes=%zu-%zu", offset, offset + data_len - 1));

    s3Client_->GetObjectRDMAAsync(
        request,
        std::move(buf),
        [callback](const Aws::S3::S3Client *client,
                   const Aws::S3::Model::GetObjectRequest &req,
                   Aws::S3::RdmaPtr &&ptr,
                   Aws::S3::Model::GetObjectRDMAOutcome &&outcome,
                   const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context) {
            if (!outcome.IsSuccess()) {
                const auto &error = outcome.GetError();
                NIXL_ERROR << absl::StrFormat("getObjectAsync: GetObjectRDMAAsync failed - %s: %s",
                                              error.GetExceptionName().c_str(),
                                              error.GetMessage().c_str());
            }
            callback(outcome.IsSuccess());
        },
        nullptr);
}
