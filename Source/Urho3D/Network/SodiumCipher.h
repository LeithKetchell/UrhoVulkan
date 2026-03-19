// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#pragma once

#include "Cipher.h"

namespace Urho3D
{

/// libsodium-based cipher using X25519 key exchange and XChaCha20-Poly1305 AEAD encryption.
class URHO3D_API SodiumCipher : public Cipher
{
public:
    /// Construct. Initializes libsodium if not already initialized.
    SodiumCipher();
    /// Destruct. Securely zeros key material.
    ~SodiumCipher() override;

    /// Generate X25519 key pair. Returns true on success.
    bool GenerateKeyPair() override;
    /// Return the 32-byte public key.
    const Vector<unsigned char>& GetPublicKey() const override { return publicKey_; }
    /// Return public key size (32 bytes for X25519).
    unsigned GetPublicKeySize() const override;
    /// Derive tx/rx session keys using X25519. isServer determines which side gets which key.
    bool DeriveSessionKeys(const unsigned char* remotePublicKey, unsigned remoteKeySize, bool isServer) override;
    /// Return true if session keys have been derived.
    bool IsReady() const override { return ready_; }
    /// Encrypt using XChaCha20-Poly1305. Nonce is prepended to ciphertext.
    bool Encrypt(const unsigned char* plaintext, unsigned plaintextSize, Vector<unsigned char>& ciphertext) override;
    /// Decrypt using XChaCha20-Poly1305. Expects nonce prepended to ciphertext.
    bool Decrypt(const unsigned char* ciphertext, unsigned ciphertextSize, Vector<unsigned char>& plaintext) override;
    /// Return overhead: 24-byte nonce + 16-byte MAC tag = 40 bytes.
    unsigned GetOverhead() const override;
    /// Return "XChaCha20-Poly1305".
    const char* GetName() const override { return "XChaCha20-Poly1305"; }
    /// Mix password hash into session keys using BLAKE2b(key || passwordHash). PAKE authentication.
    bool MixPasswordIntoKeys(const unsigned char* passwordHash, unsigned hashSize) override;

private:
    /// Our public key (32 bytes).
    Vector<unsigned char> publicKey_;
    /// Our secret key (32 bytes).
    Vector<unsigned char> secretKey_;
    /// Session transmit key (32 bytes).
    Vector<unsigned char> txKey_;
    /// Session receive key (32 bytes).
    Vector<unsigned char> rxKey_;
    /// Whether key pair has been generated.
    bool hasKeyPair_;
    /// Whether session keys have been derived.
    bool ready_;
};

}
