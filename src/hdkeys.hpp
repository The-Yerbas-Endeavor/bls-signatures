// Copyright 2020 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SRC_BLSHDKEYS_HPP_
#define SRC_BLSHDKEYS_HPP_

#include <math.h>

#include "hkdf.hpp"
#include "privatekey.hpp"
#include "util.hpp"

namespace bls {

class HDKeys {
    /**
     * Implements HD keys as specified in EIP2333.
     **/
public:
    static const uint8_t HASH_LEN = 32;

    static PrivateKey KeyGen(const std::vector<uint8_t>& seed)
    {
        return KeyGen(Bytes(seed));
    }

    static PrivateKey KeyGen(const Bytes& seed)
    {
        // KeyGen
        // 1. PRK = HKDF-Extract("BLS-SIG-KEYGEN-SALT-", IKM || I2OSP(0, 1))
        // 2. OKM = HKDF-Expand(PRK, keyInfo || I2OSP(L, 2), L)
        // 3. SK = OS2IP(OKM) mod r
        // 4. return SK

        const uint8_t info[1] = {0};
        const size_t infoLen = 0;

        // Required by the ietf spec to be at least 32 bytes
        if (seed.size() < 32) {
            throw std::invalid_argument("Seed size must be at least 32 bytes");
        }

        // std::cout << "seed: "<< Util::HexStr(seed.begin(),seed.size()) <<
        // std::endl;

        blst_scalar* skBn = Util::SecAlloc<blst_scalar>(1);
        blst_keygen_v3(skBn, seed.begin(), seed.size(), info, infoLen);
        uint8_t* skBytes = Util::SecAlloc<uint8_t>(32);
        blst_bendian_from_scalar(skBytes, skBn);

        // std::cout << "skBytes: "<< Util::HexStr(skBytes,32) << std::endl;

        PrivateKey k = PrivateKey::FromBytes(Bytes(skBytes, 32));

        Util::SecFree(skBn);
        Util::SecFree(skBytes);

        return k;
    }

    static void IKMToLamportSk(
        uint8_t* outputLamportSk,
        const uint8_t* ikm,
        size_t ikmLen,
        const uint8_t* salt,
        size_t saltLen)
    {
        // Expands the ikm to 255*HASH_LEN bytes for the lamport sk
        const uint8_t info[1] = {0};
        HKDF256::ExtractExpand(
            outputLamportSk,
            HASH_LEN * 255,
            ikm,
            ikmLen,
            salt,
            saltLen,
            info,
            0);
    }

    static void ParentSkToLamportPK(
        uint8_t* outputLamportPk,
        const PrivateKey& parentSk,
        uint32_t index)
    {
        uint8_t* salt = Util::SecAlloc<uint8_t>(4);
        uint8_t* ikm = Util::SecAlloc<uint8_t>(HASH_LEN);
        uint8_t* notIkm = Util::SecAlloc<uint8_t>(HASH_LEN);
        uint8_t* lamport0 = Util::SecAlloc<uint8_t>(HASH_LEN * 255);
        uint8_t* lamport1 = Util::SecAlloc<uint8_t>(HASH_LEN * 255);

        Util::IntToFourBytes(salt, index);
        parentSk.Serialize(ikm);

        for (size_t i = 0; i < HASH_LEN; i++) {  // Flips the bits
            notIkm[i] = ikm[i] ^ 0xff;
        }

        HDKeys::IKMToLamportSk(lamport0, ikm, HASH_LEN, salt, 4);
        HDKeys::IKMToLamportSk(lamport1, notIkm, HASH_LEN, salt, 4);

        uint8_t* lamportPk = Util::SecAlloc<uint8_t>(HASH_LEN * 255 * 2);

        for (size_t i = 0; i < 255; i++) {
            Util::Hash256(
                lamportPk + i * HASH_LEN, lamport0 + i * HASH_LEN, HASH_LEN);
        }

        for (size_t i = 0; i < 255; i++) {
            Util::Hash256(
                lamportPk + 255 * HASH_LEN + i * HASH_LEN,
                lamport1 + i * HASH_LEN,
                HASH_LEN);
        }
        Util::Hash256(outputLamportPk, lamportPk, HASH_LEN * 255 * 2);

        Util::SecFree(salt);
        Util::SecFree(ikm);
        Util::SecFree(notIkm);
        Util::SecFree(lamport0);
        Util::SecFree(lamport1);
        Util::SecFree(lamportPk);
    }

    static PrivateKey DeriveChildSk(const PrivateKey& parentSk, uint32_t index)
    {
        uint8_t* lamportPk = Util::SecAlloc<uint8_t>(HASH_LEN);
        HDKeys::ParentSkToLamportPK(lamportPk, parentSk, index);
        std::vector<uint8_t> lamportPkVector(lamportPk, lamportPk + HASH_LEN);
        PrivateKey child = HDKeys::KeyGen(lamportPkVector);
        Util::SecFree(lamportPk);
        return child;
    }

    static PrivateKey DeriveChildSkUnhardened(
        const PrivateKey& parentSk,
        uint32_t index)
    {
        uint8_t* buf = Util::SecAlloc<uint8_t>(G1Element::SIZE + 4);
        uint8_t* digest = Util::SecAlloc<uint8_t>(HASH_LEN);
        memcpy(
            buf, parentSk.GetG1Element().Serialize().data(), G1Element::SIZE);
        Util::IntToFourBytes(buf + G1Element::SIZE, index);
        Util::Hash256(digest, buf, G1Element::SIZE + 4);

        PrivateKey ret = PrivateKey::Aggregate(
            {parentSk, PrivateKey::FromBytes(Bytes(digest, HASH_LEN), true)});

        Util::SecFree(buf);
        Util::SecFree(digest);
        return ret;
    }

    static G1Element DeriveChildG1Unhardened(
        const G1Element& pk,
        uint32_t index)
    {
        uint8_t* buf = Util::SecAlloc<uint8_t>(G1Element::SIZE + 4);
        uint8_t* digest = Util::SecAlloc<uint8_t>(HASH_LEN);
        memcpy(buf, pk.Serialize().data(), G1Element::SIZE);

        Util::IntToFourBytes(buf + G1Element::SIZE, index);
        Util::Hash256(digest, buf, G1Element::SIZE + 4);

        blst_scalar nonce, zro;
        blst_scalar_from_bendian(&nonce, digest);
        memset(&zro, 0x00, sizeof(blst_scalar));
        blst_sk_add_n_check(&nonce, &nonce, &zro);

        Util::SecFree(buf);
        Util::SecFree(digest);

        G1Element gen = G1Element::Generator();
        return pk + gen * nonce;
    }

    static G2Element DeriveChildG2Unhardened(
        const G2Element& pk,
        uint32_t index)
    {
        uint8_t* buf = Util::SecAlloc<uint8_t>(G2Element::SIZE + 4);
        uint8_t* digest = Util::SecAlloc<uint8_t>(HASH_LEN);
        memcpy(buf, pk.Serialize().data(), G2Element::SIZE);
        Util::IntToFourBytes(buf + G2Element::SIZE, index);
        Util::Hash256(digest, buf, G2Element::SIZE + 4);

        blst_scalar nonce, zro;
        blst_scalar_from_bendian(&nonce, digest);
        memset(&zro, 0x00, sizeof(blst_scalar));
        blst_sk_add_n_check(&nonce, &nonce, &zro);

        Util::SecFree(buf);
        Util::SecFree(digest);

        G2Element gen = G2Element::Generator();
        return pk + gen * nonce;
    }
};
}  // end namespace bls
#endif  // SRC_BLSHDKEYS_HPP_
