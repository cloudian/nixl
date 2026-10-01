/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined HAVE_CLOUDIAN_RDMA_SDK

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

#include <absl/time/clock.h>
#include <aws/s3/S3ClientRDMA.h>
#include <gtest/gtest.h>

#ifdef HAVE_CUDA
#include <cuda_runtime.h>
#endif

#include "plugins_common.h"
#include "transfer_handler.h"
#include "obj/obj_backend.h"
#include "../common.h"

namespace gtest::plugins::obj {

nixl_b_params_t obj_cloudian_params = {{"accelerated", "true"},
                                       {"type", "cloudian"},
                                       {"req_checksum", "required"},
                                       {"scheme", "http"}};
const std::string cloudian_agent_name = "Agent5-Cloudian";
constexpr auto cloudian_test_timeout = absl::Seconds(30);

const nixlBackendInitParams obj_cloudian_test_params = {
    .localAgent = cloudian_agent_name,
    .type = "OBJ",
    .customParams = &obj_cloudian_params,
    .enableProgTh = false,
    .pthrDelay = 0,
    .syncMode = nixl_thread_sync_t::NIXL_THREAD_SYNC_RW};

class setupObjCloudianTestFixture : public setupBackendTestFixture {
protected:
    nixl_b_params_t localParams_;

    setupObjCloudianTestFixture() {
        localParams_ = *GetParam().customParams;
        const char *endpoint = std::getenv("NIXL_OBJ_ENDPOINT_OVERRIDE");
        if (endpoint && endpoint[0] != '\0') {
            localParams_["endpoint_override"] = endpoint;
            localParams_["req_checksum"] = "required";
            nixlBackendInitParams initParams = GetParam();
            initParams.customParams = &localParams_;
            localBackendEngine_ = std::make_shared<nixlObjEngine>(&initParams);
        }
    }

    void
    SetUp() override {
        const char *endpoint = std::getenv("NIXL_OBJ_ENDPOINT_OVERRIDE");
        if (!endpoint || endpoint[0] == '\0') {
            GTEST_SKIP() << "NIXL_OBJ_ENDPOINT_OVERRIDE not set, skipping Cloudian tests";
        }
        setupBackendTestFixture::SetUp();
    }
};

TEST_P(setupObjCloudianTestFixture, CloudianXferMultiBufsTest) {
    transferHandler<DRAM_SEG, OBJ_SEG> transfer(localBackendEngine_,
                                                localBackendEngine_,
                                                cloudian_agent_name,
                                                cloudian_agent_name,
                                                false,
                                                3);
    transfer.setLocalMem();
    transfer.testTransfer(NIXL_WRITE);
    transfer.resetLocalMem();
    transfer.testTransfer(NIXL_READ);
    transfer.checkLocalMem();
}

TEST_P(setupObjCloudianTestFixture, CloudianSlicedTransferTest) {
    constexpr size_t slice_offset = 16;
    constexpr size_t slice_length = 32;
    std::vector<char> buffer(slice_offset + slice_length, 'A');
    const std::string object_key = "nixl-cloudian-slice-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    nixlBlobDesc local_desc{reinterpret_cast<uintptr_t>(buffer.data()), buffer.size(), 1};
    nixlBlobDesc remote_desc;
    remote_desc.devId = 2;
    remote_desc.metaInfo = object_key;
    nixlBackendMD *local_metadata = nullptr;
    nixlBackendMD *remote_metadata = nullptr;
    ASSERT_EQ(localBackendEngine_->registerMem(local_desc, DRAM_SEG, local_metadata), NIXL_SUCCESS);
    ASSERT_EQ(localBackendEngine_->registerMem(remote_desc, OBJ_SEG, remote_metadata),
              NIXL_SUCCESS);

    nixl_meta_dlist_t local_descs(DRAM_SEG);
    nixl_meta_dlist_t remote_descs(OBJ_SEG);
    local_descs.addDesc(nixlMetaDesc(
        local_desc.addr + slice_offset, slice_length, local_desc.devId, local_metadata));
    remote_descs.addDesc(nixlMetaDesc(0, slice_length, remote_desc.devId, remote_metadata));

    nixlBackendReqH *handle = nullptr;
    ASSERT_EQ(localBackendEngine_->prepXfer(
                  NIXL_WRITE, local_descs, remote_descs, cloudian_agent_name, handle, nullptr),
              NIXL_SUCCESS);
    ASSERT_EQ(localBackendEngine_->postXfer(
                  NIXL_WRITE, local_descs, remote_descs, cloudian_agent_name, handle, nullptr),
              NIXL_IN_PROG);

    nixl_status_t status = NIXL_IN_PROG;
    const auto deadline = absl::Now() + cloudian_test_timeout;
    while (status == NIXL_IN_PROG && absl::Now() < deadline) {
        absl::SleepFor(absl::Milliseconds(50));
        status = localBackendEngine_->checkXfer(handle);
    }
    ASSERT_EQ(status, NIXL_SUCCESS);
    ASSERT_EQ(localBackendEngine_->releaseReqH(handle), NIXL_SUCCESS);

    std::fill(buffer.begin(), buffer.end(), 'Z');
    handle = nullptr;
    ASSERT_EQ(localBackendEngine_->prepXfer(
                  NIXL_READ, local_descs, remote_descs, cloudian_agent_name, handle, nullptr),
              NIXL_SUCCESS);
    ASSERT_EQ(localBackendEngine_->postXfer(
                  NIXL_READ, local_descs, remote_descs, cloudian_agent_name, handle, nullptr),
              NIXL_IN_PROG);

    status = NIXL_IN_PROG;
    const auto read_deadline = absl::Now() + cloudian_test_timeout;
    while (status == NIXL_IN_PROG && absl::Now() < read_deadline) {
        absl::SleepFor(absl::Milliseconds(50));
        status = localBackendEngine_->checkXfer(handle);
    }
    ASSERT_EQ(status, NIXL_SUCCESS);
    EXPECT_TRUE(std::all_of(buffer.begin() + slice_offset,
                            buffer.begin() + slice_offset + slice_length,
                            [](char value) { return value == 'A'; }));
    EXPECT_TRUE(std::all_of(
        buffer.begin(), buffer.begin() + slice_offset, [](char value) { return value == 'Z'; }));
    ASSERT_EQ(localBackendEngine_->releaseReqH(handle), NIXL_SUCCESS);
    ASSERT_EQ(localBackendEngine_->deregisterMem(local_metadata), NIXL_SUCCESS);
    ASSERT_EQ(localBackendEngine_->deregisterMem(remote_metadata), NIXL_SUCCESS);
}

TEST_P(setupObjCloudianTestFixture, CloudianMissingObjectReportsFailure) {
    const LogIgnoreGuard ignore_missing_object(
        "getObjectAsync: GetObjectRDMAAsync failed - NoSuchKey");
    std::vector<char> buffer(1024);
    nixlBlobDesc local_desc{reinterpret_cast<uintptr_t>(buffer.data()), buffer.size(), 1};
    nixlBlobDesc remote_desc;
    remote_desc.devId = 2;
    remote_desc.metaInfo = "nixl-cloudian-missing-object-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    nixlBackendMD *local_metadata = nullptr;
    nixlBackendMD *remote_metadata = nullptr;
    ASSERT_EQ(localBackendEngine_->registerMem(local_desc, DRAM_SEG, local_metadata), NIXL_SUCCESS);
    ASSERT_EQ(localBackendEngine_->registerMem(remote_desc, OBJ_SEG, remote_metadata),
              NIXL_SUCCESS);

    nixl_meta_dlist_t local_descs(DRAM_SEG);
    nixl_meta_dlist_t remote_descs(OBJ_SEG);
    local_descs.addDesc(
        nixlMetaDesc(local_desc.addr, local_desc.len, local_desc.devId, local_metadata));
    remote_descs.addDesc(nixlMetaDesc(0, local_desc.len, remote_desc.devId, remote_metadata));

    nixlBackendReqH *handle = nullptr;
    ASSERT_EQ(localBackendEngine_->prepXfer(
                  NIXL_READ, local_descs, remote_descs, cloudian_agent_name, handle, nullptr),
              NIXL_SUCCESS);
    ASSERT_EQ(localBackendEngine_->postXfer(
                  NIXL_READ, local_descs, remote_descs, cloudian_agent_name, handle, nullptr),
              NIXL_IN_PROG);

    nixl_status_t status = NIXL_IN_PROG;
    const auto deadline = absl::Now() + cloudian_test_timeout;
    while (status == NIXL_IN_PROG && absl::Now() < deadline) {
        absl::SleepFor(absl::Milliseconds(50));
        status = localBackendEngine_->checkXfer(handle);
    }
    EXPECT_EQ(status, NIXL_ERR_BACKEND);
    EXPECT_EQ(localBackendEngine_->releaseReqH(handle), NIXL_SUCCESS);
    EXPECT_EQ(localBackendEngine_->deregisterMem(local_metadata), NIXL_SUCCESS);
    EXPECT_EQ(localBackendEngine_->deregisterMem(remote_metadata), NIXL_SUCCESS);
}

TEST_P(setupObjCloudianTestFixture, CloudianRejectsInvalidDescriptors) {
    const LogIgnoreGuard ignore_zero_length("transfer length must be non-zero");
    const LogIgnoreGuard ignore_out_of_bounds("Descriptor \\[");
    const LogIgnoreGuard ignore_unknown_object("The object segment key 999 is not registered");
    std::vector<char> buffer(1024);
    nixlBlobDesc local_desc{reinterpret_cast<uintptr_t>(buffer.data()), buffer.size(), 1};
    nixlBlobDesc remote_desc;
    remote_desc.devId = 2;
    remote_desc.metaInfo = "nixl-cloudian-validation-test";

    nixlBackendMD *local_metadata = nullptr;
    nixlBackendMD *remote_metadata = nullptr;
    ASSERT_EQ(localBackendEngine_->registerMem(local_desc, DRAM_SEG, local_metadata), NIXL_SUCCESS);
    ASSERT_EQ(localBackendEngine_->registerMem(remote_desc, OBJ_SEG, remote_metadata),
              NIXL_SUCCESS);

    auto run_invalid_transfer = [&](uintptr_t address, size_t length, int object_id) {
        nixl_meta_dlist_t local_descs(DRAM_SEG);
        nixl_meta_dlist_t remote_descs(OBJ_SEG);
        local_descs.addDesc(nixlMetaDesc(address, length, local_desc.devId, local_metadata));
        remote_descs.addDesc(nixlMetaDesc(0, length, object_id, remote_metadata));

        nixlBackendReqH *handle = nullptr;
        EXPECT_EQ(localBackendEngine_->prepXfer(
                      NIXL_WRITE, local_descs, remote_descs, cloudian_agent_name, handle, nullptr),
                  NIXL_SUCCESS);
        EXPECT_EQ(localBackendEngine_->postXfer(
                      NIXL_WRITE, local_descs, remote_descs, cloudian_agent_name, handle, nullptr),
                  NIXL_ERR_INVALID_PARAM);
        EXPECT_EQ(localBackendEngine_->releaseReqH(handle), NIXL_SUCCESS);
    };

    run_invalid_transfer(local_desc.addr, 0, remote_desc.devId);
    run_invalid_transfer(local_desc.addr + buffer.size() - 1, 2, remote_desc.devId);
    run_invalid_transfer(local_desc.addr, local_desc.len, 999);

    EXPECT_EQ(localBackendEngine_->deregisterMem(local_metadata), NIXL_SUCCESS);
    EXPECT_EQ(localBackendEngine_->deregisterMem(remote_metadata), NIXL_SUCCESS);
}

#ifdef HAVE_CUDA
TEST_P(setupObjCloudianTestFixture, CloudianVramXferTest) {
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        GTEST_SKIP() << "No CUDA devices available, skipping VRAM test";
    }
    if (!Aws::S3::S3ClientRDMA::IsRDMAEnabled()) {
        GTEST_SKIP() << "RDMA is disabled, skipping VRAM test";
    }
    transferHandler<VRAM_SEG, OBJ_SEG> transfer(localBackendEngine_,
                                                localBackendEngine_,
                                                cloudian_agent_name,
                                                cloudian_agent_name,
                                                false,
                                                1);
    transfer.setLocalMem();
    transfer.testTransfer(NIXL_WRITE);
    transfer.resetLocalMem();
    transfer.testTransfer(NIXL_READ);
    transfer.checkLocalMem();
}
#endif

INSTANTIATE_TEST_SUITE_P(ObjCloudianTests,
                         setupObjCloudianTestFixture,
                         testing::Values(obj_cloudian_test_params));

} // namespace gtest::plugins::obj

#endif // HAVE_CLOUDIAN_RDMA_SDK