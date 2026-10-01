# Requirements

1. https://github.com/cloudian/aws-sdk-cpp (a patched AWS S3 SDK exposing
   `{Get,Put}ObjectRDMA(Async)` on `Aws::S3::S3Client`) installed in the default `/usr/local`
   directory or discoverable via pkg-config.
2. Nvidia CUDA Toolkit 13.1.1 (or later) with the `cuobjclient` library

The build (`src/plugins/obj/meson.build`) detects the presence of these two dependencies. The
Cloudian accelerated engine is only compiled when both are found; if either is missing, it's
skipped and the `cloudian` OBJ plugin will not be available.

# Building

Build and install https://github.com/cloudian/aws-sdk-cpp with s3-crt support and with
transparent RDMA disabled:

```sh
git clone --recurse-submodules --depth 1 --shallow-submodules --branch 1.11.893+rdma https://github.com/cloudian/aws-sdk-cpp.git 
mkdir aws-sdk-cpp/build
cd aws-sdk-cpp/build
cmake .. -DBUILD_ONLY='s3;s3-crt' \
    -DCMAKE_BUILD_TYPE=Release \
    -DS3_CLIENT_DISABLE_TRANSPARENT_RDMA=true \
    -DENABLE_TESTING=OFF
make -j$(nproc)
sudo make install
```

Then build NIXL as normal; `meson setup` should report:

```
Found CUObjClient Library. Enabling S3 Accelerated engines
Found Cloudian RDMA-enabled AWS C++ SDK. Enabling Cloudian Accelerated engine
```

# Running

At runtime, the following environment variables must be set:

* AWS_DEFAULT_BUCKET
* AWS_ENDPOINT_URL
* AWS_REGION
* AWS_ACCESS_KEY_ID
* AWS_SECRET_ACCESS_KEY

To test on systems without RDMA support, export:

* S3RDMA_CLIENT_ALWAYS_USE_TCP=true

The RDMA transport threshold can be tuned with:

* S3RDMA_THRESHOLD_BYTES - minimum data size (in bytes) to use RDMA rather than the HTTP body for
  transfers. Defaults to 1048576 (1MiB). `GetObject` always uses RDMA regardless of this setting,
  since the object size isn't known up front.

See https://github.com/cloudian/aws-sdk-cpp/blob/release/1.11.893%2Brdma/S3RDMA.md for additional
supported environment variables.
