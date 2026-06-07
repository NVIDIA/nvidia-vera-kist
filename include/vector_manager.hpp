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
#include <ist_app.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ----------------
// VectorManager
//
// Owns the test-vector store: mounting the on-disk vectors, publishing their
// version, and applying an update (receiving a PLDM image over an fd,
// validating its header/CRC, persisting the payload, remounting).  IstService
// owns one of these and gates entry (service initialized, no run active) before
// delegating here.
//
// Must be created via VectorManager::create() so async session callbacks can
// safely capture weak_from_this().  The io_context, StatePublisher and
// IstPlatformConfig are owned by IstService and outlive this object.
// ----------------

class VectorManager : public std::enable_shared_from_this<VectorManager>
{
  public:
    static std::shared_ptr<VectorManager> create(boost::asio::io_context& io,
                                                 StatePublisher& publisher,
                                                 const IstPlatformConfig& cfg);

    bool inProgress() const
    {
        return updateInProgress_;
    }

    // Mount any existing on-disk vectors at service startup and publish the
    // version/activation state.
    void mountVectorsOnStartup();

    // Mount existing vectors if they are not already mounted (before an IST
    // run).
    void ensureMounted();

  private:
    friend class IstService;

    std::string startUpdate(UniqueFd image, std::string_view applyTime);

    VectorManager(boost::asio::io_context& io, StatePublisher& publisher,
                  const IstPlatformConfig& cfg);

    void onTransferComplete(bool ok);
    void onHeaderPeekComplete(bool ok, std::vector<uint8_t> prefix,
                              UniqueFd readFd, PldmComponentInfo comp,
                              const std::filesystem::path& imagePath);
    void onTeardownComplete(bool ok, UniqueFd readFd,
                            std::vector<uint8_t> prefix, PldmComponentInfo comp,
                            const std::filesystem::path& imagePath);
    void onMountComplete(bool ok);
    bool mountImages();
    bool teardownMounts();
    void readAndPublishVersion();
    void finishUpdate(bool ok);

    boost::asio::io_context& io_;
    StatePublisher& publisher_;
    const IstPlatformConfig& platformCfg_;

    bool updateInProgress_{false};
    std::shared_ptr<TransferSession> activeTransfer_;
    std::shared_ptr<PldmHeaderPeekSession> activeHeaderPeek_;
};
