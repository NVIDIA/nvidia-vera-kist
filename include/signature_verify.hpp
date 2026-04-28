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

#include <filesystem>

struct IstPlatformConfig;

/**
 * Verify ECDSA-P384/SHA-384 signature of a single file.
 *
 * @param file       Path to the data file.
 * @param sigFile    Path to the base64-encoded DER signature (.sig).
 * @param pubKeyPath Path to the PEM public key.
 * @return true if the signature is valid.
 */
bool verifyFileSignature(const std::filesystem::path& file,
                         const std::filesystem::path& sigFile,
                         const std::filesystem::path& pubKeyPath);

/**
 * Verify signatures of the kist_itm binary and all libraries in its lib dir.
 *
 * Fails if the verification key (cfg.signingKeyPath) does not exist
 * on disk.
 *
 * This function is blocking (file I/O + crypto) and must be called from a
 * worker thread, not from the main io_context thread.
 *
 * @param cfg Platform configuration (provides itmBinaryPath, itmLibDir,
 *            signingKeyPath).
 * @return true if all signatures are valid.
 */
bool verifyItmSignatures(const IstPlatformConfig& cfg);
