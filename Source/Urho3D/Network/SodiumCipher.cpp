// Copyright (c) 2008-2022 the Urho3D project
// License: MIT

#include "../Precompiled.h"

#include "SodiumCipher.h"
#include "../IO/Log.h"

#include <libsodium/sodium.h>

#include "../DebugNew.h"

namespace Urho3D
{

SodiumCipher::SodiumCipher() :
    hasKeyPair_(false),
    ready_(false)
{
    if (sodium_init() < 0)
        URHO3D_LOGERROR("SodiumCipher: sodium_init() failed");
}

SodiumCipher::~SodiumCipher()
{
    // Securely zero all key material
    if (secretKey_.Size())
        sodium_memzero(secretKey_.Buffer(), secretKey_.Size());
    if (txKey_.Size())
        sodium_memzero(txKey_.Buffer(), txKey_.Size());
    if (rxKey_.Size())
        sodium_memzero(rxKey_.Buffer(), rxKey_.Size());
}

bool SodiumCipher::GenerateKeyPair()
{
    publicKey_.Resize(crypto_kx_PUBLICKEYBYTES);
    secretKey_.Resize(crypto_kx_SECRETKEYBYTES);

    if (crypto_kx_keypair(publicKey_.Buffer(), secretKey_.Buffer()) != 0)
    {
        URHO3D_LOGERROR("SodiumCipher: crypto_kx_keypair() failed");
        hasKeyPair_ = false;
        return false;
    }

    hasKeyPair_ = true;
    URHO3D_LOGINFO("SodiumCipher: X25519 key pair generated");
    return true;
}

unsigned SodiumCipher::GetPublicKeySize() const
{
    return crypto_kx_PUBLICKEYBYTES;
}

bool SodiumCipher::DeriveSessionKeys(const unsigned char* remotePublicKey, unsigned remoteKeySize, bool isServer)
{
    if (!hasKeyPair_)
    {
        URHO3D_LOGERROR("SodiumCipher: Cannot derive session keys — no key pair generated");
        return false;
    }

    if (remoteKeySize != crypto_kx_PUBLICKEYBYTES)
    {
        URHO3D_LOGERROR("SodiumCipher: Remote public key size mismatch (expected " +
            String(crypto_kx_PUBLICKEYBYTES) + ", got " + String(remoteKeySize) + ")");
        return false;
    }

    rxKey_.Resize(crypto_kx_SESSIONKEYBYTES);
    txKey_.Resize(crypto_kx_SESSIONKEYBYTES);

    int result;
    if (isServer)
    {
        result = crypto_kx_server_session_keys(
            rxKey_.Buffer(), txKey_.Buffer(),
            publicKey_.Buffer(), secretKey_.Buffer(),
            remotePublicKey);
    }
    else
    {
        result = crypto_kx_client_session_keys(
            rxKey_.Buffer(), txKey_.Buffer(),
            publicKey_.Buffer(), secretKey_.Buffer(),
            remotePublicKey);
    }

    if (result != 0)
    {
        URHO3D_LOGERROR("SodiumCipher: Session key derivation failed");
        sodium_memzero(rxKey_.Buffer(), rxKey_.Size());
        sodium_memzero(txKey_.Buffer(), txKey_.Size());
        ready_ = false;
        return false;
    }

    ready_ = true;
    URHO3D_LOGINFO("SodiumCipher: Session keys derived (" + String(isServer ? "server" : "client") + " side)");
    return true;
}

bool SodiumCipher::Encrypt(const unsigned char* plaintext, unsigned plaintextSize, Vector<unsigned char>& ciphertext)
{
    if (!ready_)
    {
        URHO3D_LOGERROR("SodiumCipher: Cannot encrypt — session keys not derived");
        return false;
    }

    // Output format: [24-byte nonce][ciphertext + 16-byte MAC]
    const unsigned nonceSize = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    const unsigned macSize = crypto_aead_xchacha20poly1305_ietf_ABYTES;
    ciphertext.Resize(nonceSize + plaintextSize + macSize);

    // Generate random nonce
    unsigned char* nonce = ciphertext.Buffer();
    randombytes_buf(nonce, nonceSize);

    unsigned long long ciphertextLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
        ciphertext.Buffer() + nonceSize, &ciphertextLen,
        plaintext, plaintextSize,
        nullptr, 0,  // no additional authenticated data
        nullptr,     // nsec (unused)
        nonce,
        txKey_.Buffer()) != 0)
    {
        URHO3D_LOGERROR("SodiumCipher: Encryption failed");
        return false;
    }

    return true;
}

bool SodiumCipher::Decrypt(const unsigned char* ciphertext, unsigned ciphertextSize, Vector<unsigned char>& plaintext)
{
    if (!ready_)
    {
        URHO3D_LOGERROR("SodiumCipher: Cannot decrypt — session keys not derived");
        return false;
    }

    const unsigned nonceSize = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    const unsigned macSize = crypto_aead_xchacha20poly1305_ietf_ABYTES;

    if (ciphertextSize < nonceSize + macSize)
    {
        URHO3D_LOGERROR("SodiumCipher: Ciphertext too short");
        return false;
    }

    const unsigned char* nonce = ciphertext;
    const unsigned char* encrypted = ciphertext + nonceSize;
    const unsigned encryptedSize = ciphertextSize - nonceSize;

    plaintext.Resize(encryptedSize - macSize);

    unsigned long long plaintextLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
        plaintext.Buffer(), &plaintextLen,
        nullptr,     // nsec (unused)
        encrypted, encryptedSize,
        nullptr, 0,  // no additional authenticated data
        nonce,
        rxKey_.Buffer()) != 0)
    {
        URHO3D_LOGERROR("SodiumCipher: Decryption failed — authentication check failed (tampered or wrong key)");
        return false;
    }

    return true;
}

unsigned SodiumCipher::GetOverhead() const
{
    // 24-byte nonce prepended + 16-byte Poly1305 MAC appended
    return crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES;
}

bool SodiumCipher::MixPasswordIntoKeys(const unsigned char* passwordHash, unsigned hashSize)
{
    if (!ready_ || !passwordHash || hashSize == 0)
    {
        URHO3D_LOGERROR("SodiumCipher: Cannot mix password — keys not ready or invalid hash");
        return false;
    }

    // Derive new tx key: BLAKE2b(txKey || passwordHash)
    {
        crypto_generichash_state state;
        unsigned char newKey[crypto_kx_SESSIONKEYBYTES];
        crypto_generichash_init(&state, nullptr, 0, crypto_kx_SESSIONKEYBYTES);
        crypto_generichash_update(&state, txKey_.Buffer(), txKey_.Size());
        crypto_generichash_update(&state, passwordHash, hashSize);
        crypto_generichash_final(&state, newKey, crypto_kx_SESSIONKEYBYTES);
        memcpy(txKey_.Buffer(), newKey, crypto_kx_SESSIONKEYBYTES);
        sodium_memzero(newKey, sizeof(newKey));
    }

    // Derive new rx key: BLAKE2b(rxKey || passwordHash)
    {
        crypto_generichash_state state;
        unsigned char newKey[crypto_kx_SESSIONKEYBYTES];
        crypto_generichash_init(&state, nullptr, 0, crypto_kx_SESSIONKEYBYTES);
        crypto_generichash_update(&state, rxKey_.Buffer(), rxKey_.Size());
        crypto_generichash_update(&state, passwordHash, hashSize);
        crypto_generichash_final(&state, newKey, crypto_kx_SESSIONKEYBYTES);
        memcpy(rxKey_.Buffer(), newKey, crypto_kx_SESSIONKEYBYTES);
        sodium_memzero(newKey, sizeof(newKey));
    }

    URHO3D_LOGINFO("SodiumCipher: Password mixed into session keys (PAKE)");
    return true;
}

}
