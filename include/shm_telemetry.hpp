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

#include <string>

// Shared memory telemetry maps this interface plus the "Functional" property
// on an inventory_software path to SoftwareInventory Status/State.  Publishing
// under any other interface routes the metric to a Chassis URI instead.
inline constexpr const char* k_op_status_iface =
    "xyz.openbmc_project.State.Decorator.OperationalStatus";

// Both are no-ops when built without nvidia-tal, and after a failed init.
void init_shm_telemetry();
void publish_functional_on_shm(const std::string& object_path, bool functional);
