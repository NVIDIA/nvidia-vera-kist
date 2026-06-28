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

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <ist_app.hpp>
#include <signature_verify.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// Per-turn read size for the asynchronous session.
static constexpr size_t k_verify_chunk_size = 65536;

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

// One file's verification, advanced one caller-provided block per pump() so it
// can run either in a tight loop or one block per io_context turn.
class FileVerifier
{
  public:
    FileVerifier() = default;

    bool init(const fs::path& file, const fs::path& sig_file,
              const fs::path& pub_key_path)
    {
        file_ = file;

        pubKey_ = load_public_key(pub_key_path);
        if (!pubKey_)
        {
            return false;
        }

        std::ifstream sig_stream(sig_file);
        if (!sig_stream)
        {
            std::cerr << "Failed to open signature file: " << sig_file << '\n';
            return false;
        }
        std::string sig_b64((std::istreambuf_iterator<char>(sig_stream)),
                            std::istreambuf_iterator<char>());
        if (!base64_decode(sig_b64, sigBytes_))
        {
            std::cerr << "Failed to base64-decode signature: " << sig_file
                      << '\n';
            return false;
        }

        ctx_.reset(EVP_MD_CTX_new());
        if (!ctx_)
        {
            std::cerr << "Failed to create EVP_MD_CTX\n";
            return false;
        }
        if (EVP_DigestVerifyInit(ctx_.get(), nullptr, EVP_sha384(), nullptr,
                                 pubKey_.get()) != 1)
        {
            std::cerr << "EVP_DigestVerifyInit failed for: " << file_ << '\n';
            return false;
        }

        dataStream_.open(file_, std::ios::binary);
        if (!dataStream_)
        {
            std::cerr << "Failed to open data file: " << file_ << '\n';
            return false;
        }
        return true;
    }

    enum class State
    {
        More,
        Done,
        Error,
    };

    State pump(std::span<char> buf)
    {
        dataStream_.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = dataStream_.gcount();
        if (dataStream_ || n > 0)
        {
            if (n > 0 && EVP_DigestVerifyUpdate(ctx_.get(), buf.data(),
                                                static_cast<size_t>(n)) != 1)
            {
                std::cerr << "EVP_DigestVerifyUpdate failed for: " << file_
                          << '\n';
                return State::Error;
            }
            return State::More;
        }

        int rc = EVP_DigestVerifyFinal(ctx_.get(), sigBytes_.data(),
                                       sigBytes_.size());
        if (rc != 1)
        {
            std::cerr << "Signature verification FAILED for: " << file_ << '\n';
            return State::Error;
        }
        std::cerr << "Signature verified OK: " << file_ << '\n';
        return State::Done;
    }

  private:
    fs::path file_;
    EvpPkeyPtr pubKey_;
    EvpMdCtxPtr ctx_;
    std::vector<uint8_t> sigBytes_;
    std::ifstream dataStream_;
};

bool verifyFileSignature(const fs::path& file, const fs::path& sig_file,
                         const fs::path& pub_key_path)
{
    FileVerifier verifier;
    if (!verifier.init(file, sig_file, pub_key_path))
    {
        return false;
    }

    std::array<char, 8192> buf{};
    FileVerifier::State st = FileVerifier::State::More;
    while ((st = verifier.pump(buf)) == FileVerifier::State::More)
    {
    }
    return st == FileVerifier::State::Done;
}

struct VerifyTarget
{
    fs::path file;
    fs::path sig;
};

// Resolve the binary and libraries to the (file, signature) pairs to verify,
// or nullopt if the key, a signature, or the lib directory is missing. Shared
// by the synchronous and asynchronous drivers.
static std::optional<std::vector<VerifyTarget>>
    collect_files_to_verify(const IstPlatformConfig& cfg)
{
    std::error_code ec;
    if (!fs::exists(cfg.signingKeyPath, ec) || ec)
    {
        std::cerr << "Verification key not found: " << cfg.signingKeyPath
                  << '\n';
        return std::nullopt;
    }

    std::vector<VerifyTarget> targets;
    std::set<fs::path> verified_real;

    fs::path binary_sig = cfg.itmBinaryPath;
    binary_sig += ".sig";
    if (!fs::exists(binary_sig, ec) || ec)
    {
        std::cerr << "Signature file missing for binary: " << binary_sig
                  << '\n';
        return std::nullopt;
    }
    targets.push_back({cfg.itmBinaryPath, binary_sig});
    fs::path binary_real = fs::canonical(cfg.itmBinaryPath, ec);
    if (!ec)
    {
        verified_real.insert(binary_real);
    }

    if (cfg.itmLibDir.empty())
    {
        return targets;
    }
    bool is_dir = fs::is_directory(cfg.itmLibDir, ec);
    if (ec)
    {
        std::cerr << "Cannot access lib directory '" << cfg.itmLibDir
                  << "': " << ec.message() << '\n';
        return std::nullopt;
    }
    if (!is_dir)
    {
        return targets;
    }

    std::vector<fs::path> symlinks;

    for (const auto& entry : fs::directory_iterator(cfg.itmLibDir, ec))
    {
        if (ec)
        {
            std::cerr << "Error iterating lib directory: " << ec.message()
                      << '\n';
            return std::nullopt;
        }

        const fs::path& path = entry.path();

        if (path.extension() == ".sig")
        {
            continue;
        }

        std::error_code entry_ec;
        if (entry.is_symlink(entry_ec))
        {
            symlinks.push_back(path);
            continue;
        }
        if (entry_ec)
        {
            std::cerr << "Failed to stat " << path << ": " << entry_ec.message()
                      << '\n';
            return std::nullopt;
        }

        if (!entry.is_regular_file(entry_ec))
        {
            if (entry_ec)
            {
                std::cerr << "Failed to stat " << path << ": "
                          << entry_ec.message() << '\n';
                return std::nullopt;
            }
            continue;
        }

        fs::path lib_sig = path;
        lib_sig += ".sig";
        if (!fs::exists(lib_sig, ec) || ec)
        {
            std::cerr << "Signature file missing for library: " << path << '\n';
            return std::nullopt;
        }

        fs::path lib_real = fs::canonical(path, ec);
        if (ec)
        {
            std::cerr << "Failed to resolve library path " << path << ": "
                      << ec.message() << '\n';
            return std::nullopt;
        }
        verified_real.insert(lib_real);
        targets.push_back({path, lib_sig});
    }
    if (ec)
    {
        std::cerr << "Error iterating lib directory: " << ec.message() << '\n';
        return std::nullopt;
    }

    for (const fs::path& link : symlinks)
    {
        fs::path link_real = fs::canonical(link, ec);
        if (ec)
        {
            std::cerr << "Symlink target could not be resolved: " << link
                      << ": " << ec.message() << '\n';
            return std::nullopt;
        }
        if (!verified_real.contains(link_real))
        {
            std::cerr << "Symlink resolves outside the verified set: " << link
                      << " -> " << link_real << '\n';
            return std::nullopt;
        }
    }

    return targets;
}

bool verifyItmSignatures(const IstPlatformConfig& cfg)
{
    std::optional<std::vector<VerifyTarget>> targets =
        collect_files_to_verify(cfg);
    if (!targets)
    {
        return false;
    }
    for (const VerifyTarget& t : *targets)
    {
        if (!verifyFileSignature(t.file, t.sig, cfg.signingKeyPath))
        {
            return false;
        }
    }
    return true;
}

// Kept alive by the strong `self` capture in the post chain; self-destructs
// once finish() fires the callback, so no IstService member is needed.
class SignatureVerifySession :
    public std::enable_shared_from_this<SignatureVerifySession>
{
  public:
    SignatureVerifySession(
        boost::asio::io_context& io, IstPlatformConfig cfg,
        std::move_only_function<void(bool ok) const> on_complete) :
        io_(io), cfg_(std::move(cfg)), onComplete_(std::move(on_complete))
    {}

    SignatureVerifySession(const SignatureVerifySession&) = delete;
    SignatureVerifySession& operator=(const SignatureVerifySession&) = delete;

    void start()
    {
        post_step();
    }

  private:
    // post() re-arms step() on the next io_context turn: deferred execution,
    // not recursion.
    // NOLINTNEXTLINE(misc-no-recursion)
    void post_step()
    {
        // NOLINTNEXTLINE(misc-no-recursion)
        boost::asio::post(io_, [self = shared_from_this()]() { self->step(); });
    }

    // NOLINTNEXTLINE(misc-no-recursion)
    void step()
    {
        if (finished_)
        {
            return;
        }

        // Resolve the file list on the first turn so completion is always
        // delivered asynchronously, never inline from start().
        if (!enumerated_)
        {
            std::optional<std::vector<VerifyTarget>> targets =
                collect_files_to_verify(cfg_);
            if (!targets)
            {
                finish(false);
                return;
            }
            targets_ = std::move(*targets);
            enumerated_ = true;
            post_step();
            return;
        }

        if (!verifierActive_)
        {
            if (index_ >= targets_.size())
            {
                finish(true);
                return;
            }
            const VerifyTarget& t = targets_[index_];
            if (!verifier_.init(t.file, t.sig, cfg_.signingKeyPath))
            {
                finish(false);
                return;
            }
            verifierActive_ = true;
            post_step();
            return;
        }

        FileVerifier::State st = verifier_.pump(buf_);
        if (st == FileVerifier::State::Error)
        {
            finish(false);
            return;
        }
        if (st == FileVerifier::State::More)
        {
            post_step();
            return;
        }

        verifier_ = FileVerifier();
        verifierActive_ = false;
        ++index_;
        post_step();
    }

    void finish(bool ok)
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;
        auto cb = std::move(onComplete_);
        if (cb)
        {
            cb(ok);
        }
    }

    boost::asio::io_context& io_;
    IstPlatformConfig cfg_;
    std::move_only_function<void(bool ok) const> onComplete_;
    std::vector<VerifyTarget> targets_;
    FileVerifier verifier_;
    std::array<char, k_verify_chunk_size> buf_{};
    size_t index_ = 0;
    bool enumerated_ = false;
    bool verifierActive_ = false;
    bool finished_ = false;
};

void verifyItmSignaturesAsync(
    boost::asio::io_context& io, IstPlatformConfig cfg,
    std::move_only_function<void(bool ok) const> on_complete)
{
    std::make_shared<SignatureVerifySession>(io, std::move(cfg),
                                             std::move(on_complete))
        ->start();
}
