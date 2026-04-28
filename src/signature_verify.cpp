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
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <ist_app.hpp>
#include <signature_verify.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// RAII deleters for OpenSSL types
struct EvpPkeyDeleter
{
    void operator()(EVP_PKEY* p) const
    {
        EVP_PKEY_free(p);
    }
};

struct EvpMdCtxDeleter
{
    void operator()(EVP_MD_CTX* p) const
    {
        EVP_MD_CTX_free(p);
    }
};

struct BioDeleter
{
    void operator()(BIO* p) const
    {
        BIO_free_all(p);
    }
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

static EvpPkeyPtr load_public_key(const fs::path& path)
{
    BioPtr bio(BIO_new_file(path.c_str(), "r"));
    if (!bio)
    {
        std::cerr << "Failed to open public key file: " << path << '\n';
        return nullptr;
    }

    EVP_PKEY* raw = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
    if (raw == nullptr)
    {
        std::cerr << "Failed to parse PEM public key: " << path << '\n';
        return nullptr;
    }
    if (EVP_PKEY_bits(raw) != 384)
    {
        std::cerr << "Public key is not P-384: " << path << '\n';
        EVP_PKEY_free(raw);
        return nullptr;
    }
    return EvpPkeyPtr(raw);
}

static bool base64_decode(const std::string& input,
                          std::vector<uint8_t>& output)
{
    if (input.empty())
    {
        return false;
    }

    BioPtr b64(BIO_new(BIO_f_base64()));
    if (!b64)
    {
        return false;
    }
    // mem is intentionally a raw pointer: BIO_push transfers ownership
    // into the b64 chain, and BIO_free_all (via BioPtr) frees both.
    BIO* mem = BIO_new_mem_buf(input.data(), static_cast<int>(input.size()));
    if (!mem)
    {
        return false;
    }
    BIO_push(b64.get(), mem);
    BIO_set_flags(b64.get(), BIO_FLAGS_BASE64_NO_NL);

    output.resize(input.size());
    int len =
        BIO_read(b64.get(), output.data(), static_cast<int>(output.size()));
    if (len <= 0)
    {
        return false;
    }
    output.resize(static_cast<size_t>(len));
    return true;
}

bool verifyFileSignature(const fs::path& file, const fs::path& sig_file,
                         const fs::path& pub_key_path)
{
    EvpPkeyPtr pub_key = load_public_key(pub_key_path);
    if (!pub_key)
    {
        return false;
    }

    // Read the base64-encoded signature file
    std::ifstream sig_stream(sig_file);
    if (!sig_stream)
    {
        std::cerr << "Failed to open signature file: " << sig_file << '\n';
        return false;
    }
    std::string sig_b64((std::istreambuf_iterator<char>(sig_stream)),
                        std::istreambuf_iterator<char>());

    std::vector<uint8_t> sig_bytes;
    if (!base64_decode(sig_b64, sig_bytes))
    {
        std::cerr << "Failed to base64-decode signature: " << sig_file << '\n';
        return false;
    }

    // Set up the verification context: ECDSA + SHA-384
    EvpMdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx)
    {
        std::cerr << "Failed to create EVP_MD_CTX\n";
        return false;
    }

    if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha384(), nullptr,
                             pub_key.get()) != 1)
    {
        std::cerr << "EVP_DigestVerifyInit failed for: " << file << '\n';
        return false;
    }

    // Read and hash the data file in chunks
    std::ifstream data_stream(file, std::ios::binary);
    if (!data_stream)
    {
        std::cerr << "Failed to open data file: " << file << '\n';
        return false;
    }

    std::array<char, 8192> buf{};
    while (data_stream.read(buf.data(), buf.size()) || data_stream.gcount() > 0)
    {
        if (EVP_DigestVerifyUpdate(ctx.get(), buf.data(),
                                   static_cast<size_t>(data_stream.gcount())) !=
            1)
        {
            std::cerr << "EVP_DigestVerifyUpdate failed for: " << file << '\n';
            return false;
        }
    }

    int rc =
        EVP_DigestVerifyFinal(ctx.get(), sig_bytes.data(), sig_bytes.size());
    if (rc != 1)
    {
        std::cerr << "Signature verification FAILED for: " << file << '\n';
        return false;
    }

    std::cerr << "Signature verified OK: " << file << '\n';
    return true;
}

bool verifyItmSignatures(const IstPlatformConfig& cfg)
{
    std::error_code ec;
    if (!fs::exists(cfg.signingKeyPath, ec) || ec)
    {
        std::cerr << "Verification key not found: " << cfg.signingKeyPath
                  << '\n';
        return false;
    }

    // Verify the kist_itm binary
    fs::path binary_sig = cfg.itmBinaryPath;
    binary_sig += ".sig";
    if (!fs::exists(binary_sig, ec) || ec)
    {
        std::cerr << "Signature file missing for binary: " << binary_sig
                  << '\n';
        return false;
    }
    if (!verifyFileSignature(cfg.itmBinaryPath, binary_sig, cfg.signingKeyPath))
    {
        return false;
    }

    // Verify libraries in the lib directory
    if (cfg.itmLibDir.empty())
    {
        return true;
    }
    bool is_dir = fs::is_directory(cfg.itmLibDir, ec);
    if (ec)
    {
        std::cerr << "Cannot access lib directory '" << cfg.itmLibDir
                  << "': " << ec.message() << '\n';
        return false;
    }
    if (!is_dir)
    {
        return true;
    }

    for (const auto& entry : fs::directory_iterator(cfg.itmLibDir, ec))
    {
        if (ec)
        {
            std::cerr << "Error iterating lib directory: " << ec.message()
                      << '\n';
            return false;
        }

        const fs::path& path = entry.path();

        // Skip .sig files themselves
        if (path.extension() == ".sig")
        {
            continue;
        }

        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec))
        {
            if (entry_ec)
            {
                std::cerr << "Failed to stat " << path << ": "
                          << entry_ec.message() << '\n';
                return false;
            }
            continue;
        }

        fs::path lib_sig = path;
        lib_sig += ".sig";
        if (!fs::exists(lib_sig, ec) || ec)
        {
            std::cerr << "Signature file missing for library: " << path << '\n';
            return false;
        }

        if (!verifyFileSignature(path, lib_sig, cfg.signingKeyPath))
        {
            return false;
        }
    }
    if (ec)
    {
        std::cerr << "Error iterating lib directory: " << ec.message() << '\n';
        return false;
    }

    return true;
}
