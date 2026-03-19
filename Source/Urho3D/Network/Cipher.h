// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#pragma once

#include "../Container/RefCounted.h"
#include "../Container/Vector.h"

namespace Urho3D
{

/// Abstract base class for network encryption ciphers.
/// Subclass to implement different encryption algorithms (e.g. libsodium, OpenSSL).
class URHO3D_API Cipher : public RefCounted
{
public:
    /// Destruct.
    ~Cipher() override = default;

    /// Generate a new key pair for key exchange. Returns true on success.
    virtual bool GenerateKeyPair() = 0;
    /// Return the public key bytes for sending to the remote side.
    virtual const Vector<unsigned char>& GetPublicKey() const = 0;
    /// Return the public key size in bytes.
    virtual unsigned GetPublicKeySize() const = 0;
    /// Derive session keys from our key pair and the remote's public key. isServer determines key direction.
    virtual bool DeriveSessionKeys(const unsigned char* remotePublicKey, unsigned remoteKeySize, bool isServer) = 0;
    /// Return true if session keys have been derived and cipher is ready for encrypt/decrypt.
    virtual bool IsReady() const = 0;
    /// Encrypt plaintext into ciphertext. Returns true on success.
    virtual bool Encrypt(const unsigned char* plaintext, unsigned plaintextSize, Vector<unsigned char>& ciphertext) = 0;
    /// Decrypt ciphertext into plaintext. Returns true on success (includes authentication check).
    virtual bool Decrypt(const unsigned char* ciphertext, unsigned ciphertextSize, Vector<unsigned char>& plaintext) = 0;
    /// Return the overhead in bytes added by encryption (MAC tag + nonce if prepended).
    virtual unsigned GetOverhead() const = 0;
    /// Return human-readable cipher name.
    virtual const char* GetName() const = 0;
    /// Mix a password hash into existing session keys (PAKE). Returns true on success.
    virtual bool MixPasswordIntoKeys(const unsigned char* passwordHash, unsigned hashSize) { return false; }
};

}
