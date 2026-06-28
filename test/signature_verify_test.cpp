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
#include <unistd.h>

#include <ist_app.hpp>
#include <signature_verify.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

// Test helper: generate an ECDSA P-384 key pair, write the public key to a
// PEM file, and return the private key for signing.
struct TestKeyPair
{
    EVP_PKEY* pkey = nullptr;

    explicit TestKeyPair()
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        EXPECT_NE(ctx, nullptr);
        EXPECT_EQ(EVP_PKEY_keygen_init(ctx), 1);
        EXPECT_EQ(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_secp384r1),
                  1);
        EXPECT_EQ(EVP_PKEY_keygen(ctx, &pkey), 1);
        EVP_PKEY_CTX_free(ctx);
    }

    ~TestKeyPair()
    {
        EVP_PKEY_free(pkey);
    }

    TestKeyPair(const TestKeyPair&) = delete;
    TestKeyPair& operator=(const TestKeyPair&) = delete;

    void write_public_key(const fs::path& path) const
    {
        BIO* bio = BIO_new_file(path.c_str(), "w");
        ASSERT_NE(bio, nullptr);
        EXPECT_EQ(PEM_write_bio_PUBKEY(bio, pkey), 1);
        BIO_free(bio);
    }
};

// Sign a file with ECDSA-SHA384 and write the base64-encoded signature.
static void sign_file(const fs::path& data_file, const fs::path& sig_file,
                      EVP_PKEY* pkey)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    ASSERT_NE(ctx, nullptr);

    ASSERT_EQ(EVP_DigestSignInit(ctx, nullptr, EVP_sha384(), nullptr, pkey), 1);

    std::ifstream in(data_file, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    char buf[4096];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
    {
        ASSERT_EQ(
            EVP_DigestSignUpdate(ctx, buf, static_cast<size_t>(in.gcount())),
            1);
    }

    size_t sig_len = 0;
    ASSERT_EQ(EVP_DigestSignFinal(ctx, nullptr, &sig_len), 1);

    std::vector<uint8_t> sig(sig_len);
    ASSERT_EQ(EVP_DigestSignFinal(ctx, sig.data(), &sig_len), 1);
    sig.resize(sig_len);
    EVP_MD_CTX_free(ctx);

    // Base64-encode the signature (matching the production .sig format)
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, sig.data(), static_cast<int>(sig.size()));
    BIO_flush(b64);

    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);

    std::ofstream out(sig_file);
    out.write(bptr->data, static_cast<std::streamsize>(bptr->length));
    BIO_free_all(b64);
}

class SignatureVerifyTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        tmpDir_ = fs::temp_directory_path() /
                  ("sig_test_" + std::to_string(getpid()) + "_" +
                   std::to_string(counter++));
        fs::create_directories(tmpDir_ / "bin");
        fs::create_directories(tmpDir_ / "lib");

        keyPair_ = std::make_unique<TestKeyPair>();
        pubKeyPath_ = tmpDir_ / "pub.pem";
        keyPair_->write_public_key(pubKeyPath_);

        // Create a test binary
        binaryPath_ = tmpDir_ / "bin" / "kist_itm";
        std::ofstream(binaryPath_) << "test binary content for signing";

        // Sign it
        sign_file(binaryPath_, fs::path(binaryPath_).concat(".sig"),
                  keyPair_->pkey);
    }

    void create_signed_lib(const std::string& name, const std::string& content)
    {
        fs::path lib_path = tmpDir_ / "lib" / name;
        std::ofstream(lib_path) << content;
        sign_file(lib_path, fs::path(lib_path).concat(".sig"), keyPair_->pkey);
    }

    void create_lib_symlink(const std::string& link_name,
                            const fs::path& target)
    {
        fs::path link_path = tmpDir_ / "lib" / link_name;
        std::error_code ec;
        fs::create_symlink(target, link_path, ec);
        ASSERT_FALSE(ec) << "failed to create symlink " << link_path << ": "
                         << ec.message();
    }

    void TearDown() override
    {
        fs::remove_all(tmpDir_);
    }

    fs::path tmpDir_;
    fs::path pubKeyPath_;
    fs::path binaryPath_;
    std::unique_ptr<TestKeyPair> keyPair_;
    static inline int counter = 0;
};

// ----------------
// verifyFileSignature tests
// ----------------

TEST_F(SignatureVerifyTest, ValidSignature)
{
    fs::path sig_path = binaryPath_;
    sig_path += ".sig";
    EXPECT_TRUE(verifyFileSignature(binaryPath_, sig_path, pubKeyPath_));
}

TEST_F(SignatureVerifyTest, CorruptedSignature)
{
    fs::path sig_path = binaryPath_;
    sig_path += ".sig";

    // Corrupt the signature by overwriting with garbage base64
    std::ofstream(sig_path) << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                               "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                               "AAAAAAAAAA==";

    EXPECT_FALSE(verifyFileSignature(binaryPath_, sig_path, pubKeyPath_));
}

TEST_F(SignatureVerifyTest, MissingSigFile)
{
    fs::path sig_path = tmpDir_ / "nonexistent.sig";
    EXPECT_FALSE(verifyFileSignature(binaryPath_, sig_path, pubKeyPath_));
}

TEST_F(SignatureVerifyTest, TamperedDataFile)
{
    fs::path sig_path = binaryPath_;
    sig_path += ".sig";

    // Modify the binary after signing
    std::ofstream(binaryPath_, std::ios::app) << "tampered";

    EXPECT_FALSE(verifyFileSignature(binaryPath_, sig_path, pubKeyPath_));
}

TEST_F(SignatureVerifyTest, InvalidBase64InSigFile)
{
    fs::path sig_path = binaryPath_;
    sig_path += ".sig";

    std::ofstream(sig_path) << "this is not valid base64!!!@#$%";

    EXPECT_FALSE(verifyFileSignature(binaryPath_, sig_path, pubKeyPath_));
}

TEST_F(SignatureVerifyTest, WrongKey)
{
    fs::path sig_path = binaryPath_;
    sig_path += ".sig";

    // Generate a different key pair
    TestKeyPair other_key;
    fs::path other_pub = tmpDir_ / "other_pub.pem";
    other_key.write_public_key(other_pub);

    EXPECT_FALSE(verifyFileSignature(binaryPath_, sig_path, other_pub));
}

TEST_F(SignatureVerifyTest, EmptySigFile)
{
    fs::path sig_path = binaryPath_;
    sig_path += ".sig";

    // Truncate the sig file to zero bytes
    std::ofstream ofs(sig_path, std::ios::trunc);
    ofs.close();

    EXPECT_FALSE(verifyFileSignature(binaryPath_, sig_path, pubKeyPath_));
}

// ----------------
// verifyItmSignatures tests
// ----------------

TEST_F(SignatureVerifyTest, VerifyAllPassesWithSignedBinaryAndLibs)
{
    create_signed_lib("libcrypto.so.3", "fake libcrypto content");
    create_signed_lib("libusb-1.0.so.0.4.0", "fake libusb content");

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_TRUE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyAllFailsWhenBinarySigMissing)
{
    fs::remove(fs::path(binaryPath_).concat(".sig"));

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_FALSE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyAllFailsWhenLibSigMissing)
{
    fs::path lib_path = tmpDir_ / "lib" / "libcrypto.so.3";
    std::ofstream(lib_path) << "lib content";

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_FALSE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyAllFailsWhenLibTampered)
{
    create_signed_lib("libcrypto.so.3", "original content");

    std::ofstream(tmpDir_ / "lib" / "libcrypto.so.3") << "tampered content";

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_FALSE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyFailsWhenKeyNotOnDisk)
{
    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";

    EXPECT_FALSE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyPassesWithEmptyLibDir)
{
    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = "";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_TRUE(verifyItmSignatures(cfg));
}

// ----------------
// symlink handling tests
// ----------------

TEST_F(SignatureVerifyTest, VerifyPassesWithSymlinkToSignedLib)
{
    create_signed_lib("libusb-1.0.so.0.5.0", "fake libusb content");
    create_lib_symlink("libusb-1.0.so.0", "libusb-1.0.so.0.5.0");

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_TRUE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyPassesWithSymlinkChain)
{
    create_signed_lib("libusb-1.0.so.0.5.0", "fake libusb content");
    create_lib_symlink("libusb-1.0.so.0", "libusb-1.0.so.0.5.0");
    create_lib_symlink("libusb-1.0.so", "libusb-1.0.so.0");

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_TRUE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyFailsWhenSymlinkResolvesOutsideVerifiedSet)
{
    fs::path outside = tmpDir_ / "unsigned_payload.so";
    std::ofstream(outside) << "unsigned content";
    create_lib_symlink("libusb-1.0.so.0", outside);

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_FALSE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyFailsWhenSymlinkDangling)
{
    create_lib_symlink("libusb-1.0.so.0", "does-not-exist.so.0.5.0");

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_FALSE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyPassesWhenSymlinkResolvesToBinary)
{
    create_lib_symlink("kist_itm.link", binaryPath_);

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_TRUE(verifyItmSignatures(cfg));
}

TEST_F(SignatureVerifyTest, VerifyPassesWithMixedRealAndSymlinkedLibs)
{
    create_signed_lib("libcrypto.so.3", "fake libcrypto content");
    create_lib_symlink("libcrypto.so", "libcrypto.so.3");
    create_signed_lib("libusb-1.0.so.0.5.0", "fake libusb content");
    create_lib_symlink("libusb-1.0.so.0", "libusb-1.0.so.0.5.0");
    create_lib_symlink("libusb-1.0.so", "libusb-1.0.so.0");

    IstPlatformConfig cfg;
    cfg.itmBinaryPath = binaryPath_;
    cfg.itmLibDir = tmpDir_ / "lib";
    cfg.signingKeyPath = pubKeyPath_;

    EXPECT_TRUE(verifyItmSignatures(cfg));
}
