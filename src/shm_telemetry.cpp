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
#include <shm_telemetry.hpp>
#ifdef NVIDIA_SHMEM
#include <tal.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>
#endif

#ifdef NVIDIA_SHMEM
namespace
{
bool shm_ready = false;
}
#endif

void init_shm_telemetry()
{
#ifdef NVIDIA_SHMEM
    shm_ready = tal::TelemetryAggregator::namespaceInit(
        tal::ProcessType::Producer, "nvidia-vera-kist");
    if (!shm_ready)
    {
        std::cerr << "Failed to init shared memory telemetry; IST vector "
                     "state will be absent from metric reports\n";
    }
#endif
}

void publish_functional_on_shm([[maybe_unused]] const std::string& object_path,
                               [[maybe_unused]] bool functional)
{
#ifdef NVIDIA_SHMEM
    if (!shm_ready)
    {
        return;
    }

    std::vector<uint8_t> smbus_data;
    nv::sensor_aggregation::DbusVariantType value{functional};
    uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    tal::TelemetryAggregator::updateTelemetry(object_path, k_op_status_iface,
                                              "Functional", smbus_data,
                                              timestamp, 0, value);
#endif
}
