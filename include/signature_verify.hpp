/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <boost/asio/io_context.hpp>

#include <filesystem>
#include <functional>

struct IstPlatformConfig;

/**
 * Verify ECDSA-P384/SHA-384 signature of a single file.
 *
 * @param file       Path to the data file.
 * @param sigFile    Path to the base64-encoded DER signature (.sig).
 * @param pubKeyPath Path to the PEM public key.
 * @return true if the signature is valid.
 */
bool verify_file_signature(const std::filesystem::path& file,
                           const std::filesystem::path& sig_file,
                           const std::filesystem::path& pub_key_path);

/**
 * Verify signatures of the kist_itm binary and all libraries in its lib dir.
 *
 * Fails if the verification key (cfg.signingKeyPath) does not exist
 * on disk.
 *
 * This function is blocking (file I/O + crypto). Production code uses
 * verify_itm_signatures_async(); this synchronous form is the building block it
 * shares and is exercised directly by the unit tests.
 *
 * @param cfg Platform configuration (provides itmBinaryPath, itmLibDir,
 *            signingKeyPath).
 * @return true if all signatures are valid.
 */
bool verify_itm_signatures(const IstPlatformConfig& cfg);

/**
 * Chunked variant of verify_itm_signatures() that runs on the io_context one
 * block per turn instead of blocking. on_complete(ok) fires exactly once on the
 * io_context thread.
 */
void verify_itm_signatures_async(
    boost::asio::io_context& io, IstPlatformConfig cfg,
    std::move_only_function<void(bool ok) const> on_complete);
