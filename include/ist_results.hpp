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

/**
 * Archive every result file in `resultsDir` into ist_results.tar.gz and delete
 * each original as it is added (to stay within XMC storage limits).
 *
 * This function is blocking (file I/O + gzip) and must be called from a worker
 * thread, not from the main io_context thread.
 *
 * @return true if all files were archived and the archive was closed cleanly.
 */
bool archiveResults(const std::filesystem::path& resultsDir);

/**
 * Chunked variant of archiveResults() that runs on the io_context one block
 * per turn instead of blocking. onComplete(ok) fires exactly once on the
 * io_context thread.
 */
void archiveResultsAsync(
    boost::asio::io_context& io, std::filesystem::path resultsDir,
    std::move_only_function<void(bool ok) const> onComplete);
