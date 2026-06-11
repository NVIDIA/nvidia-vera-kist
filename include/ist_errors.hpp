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

#include <sdbusplus/exception.hpp>

namespace sdbusplus::error::com::nvidia::vera::ist
{

struct CollateralNotFound final :
    public sdbusplus::exception::generated_exception
{
    static constexpr auto errName =
        "com.nvidia.vera.ist.Error.CollateralNotFound";
    static constexpr auto errDesc = "IST test vectors are not installed.";
    static constexpr auto errWhat =
        "com.nvidia.vera.ist.Error.CollateralNotFound: "
        "IST test vectors are not installed.";

    const char* name() const noexcept override
    {
        return errName;
    }
    const char* description() const noexcept override
    {
        return errDesc;
    }
    const char* what() const noexcept override
    {
        return errWhat;
    }
};

struct InvalidParameter final : public sdbusplus::exception::generated_exception
{
    static constexpr auto errName =
        "com.nvidia.vera.ist.Error.InvalidParameter";
    static constexpr auto errDesc = "Invalid IST parameters were given.";
    static constexpr auto errWhat =
        "com.nvidia.vera.ist.Error.InvalidParameter: "
        "Invalid IST parameters were given.";

    const char* name() const noexcept override
    {
        return errName;
    }
    const char* description() const noexcept override
    {
        return errDesc;
    }
    const char* what() const noexcept override
    {
        return errWhat;
    }
};

struct ResultStorageError final :
    public sdbusplus::exception::generated_exception
{
    static constexpr auto errName =
        "com.nvidia.vera.ist.Error.ResultStorageError";
    static constexpr auto errDesc = "IST result storage is unavailable.";
    static constexpr auto errWhat =
        "com.nvidia.vera.ist.Error.ResultStorageError: "
        "IST result storage is unavailable.";

    const char* name() const noexcept override
    {
        return errName;
    }
    const char* description() const noexcept override
    {
        return errDesc;
    }
    const char* what() const noexcept override
    {
        return errWhat;
    }
};

struct HookNotFound final : public sdbusplus::exception::generated_exception
{
    static constexpr auto errName = "com.nvidia.vera.ist.Error.HookNotFound";
    static constexpr auto errDesc =
        "A required IST hook script is not installed.";
    static constexpr auto errWhat =
        "com.nvidia.vera.ist.Error.HookNotFound: "
        "A required IST hook script is not installed.";

    const char* name() const noexcept override
    {
        return errName;
    }
    const char* description() const noexcept override
    {
        return errDesc;
    }
    const char* what() const noexcept override
    {
        return errWhat;
    }
};

struct InvalidArgument final : public sdbusplus::exception::generated_exception
{
    static constexpr auto errName =
        "xyz.openbmc_project.Common.Error.InvalidArgument";
    static constexpr auto errDesc = "Invalid argument was given.";
    static constexpr auto errWhat =
        "xyz.openbmc_project.Common.Error.InvalidArgument: "
        "Invalid argument was given.";

    const char* name() const noexcept override
    {
        return errName;
    }
    const char* description() const noexcept override
    {
        return errDesc;
    }
    const char* what() const noexcept override
    {
        return errWhat;
    }
};

struct Unavailable final : public sdbusplus::exception::generated_exception
{
    static constexpr auto errName =
        "xyz.openbmc_project.Common.Error.Unavailable";
    static constexpr auto errDesc = "The service is temporarily unavailable.";
    static constexpr auto errWhat =
        "xyz.openbmc_project.Common.Error.Unavailable: "
        "The service is temporarily unavailable.";

    const char* name() const noexcept override
    {
        return errName;
    }
    const char* description() const noexcept override
    {
        return errDesc;
    }
    const char* what() const noexcept override
    {
        return errWhat;
    }
};

struct InternalFailure final : public sdbusplus::exception::generated_exception
{
    static constexpr auto errName =
        "xyz.openbmc_project.Common.Error.InternalFailure";
    static constexpr auto errDesc = "The operation failed internally.";
    static constexpr auto errWhat =
        "xyz.openbmc_project.Common.Error.InternalFailure: "
        "The operation failed internally.";

    const char* name() const noexcept override
    {
        return errName;
    }
    const char* description() const noexcept override
    {
        return errDesc;
    }
    const char* what() const noexcept override
    {
        return errWhat;
    }
};

struct ResourceNotFound final : public sdbusplus::exception::generated_exception
{
    static constexpr auto errName =
        "xyz.openbmc_project.Common.Error.ResourceNotFound";
    static constexpr auto errDesc = "The resource is not found.";
    static constexpr auto errWhat =
        "xyz.openbmc_project.Common.Error.ResourceNotFound: "
        "The resource is not found.";

    const char* name() const noexcept override
    {
        return errName;
    }
    const char* description() const noexcept override
    {
        return errDesc;
    }
    const char* what() const noexcept override
    {
        return errWhat;
    }
};

} // namespace sdbusplus::error::com::nvidia::vera::ist
