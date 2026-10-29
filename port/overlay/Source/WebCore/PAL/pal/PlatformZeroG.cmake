# PAL for the ZeroG port: the generic pieces (same set as the PlayStation port),
# digests from BoringSSL through the OpenSSL API.
list(APPEND PAL_SOURCES
    crypto/openssl/CryptoDigestOpenSSL.cpp

    system/ClockGeneric.cpp
    system/Sound.cpp

    text/KillRing.cpp
)

list(APPEND PAL_PRIVATE_LIBRARIES
    OpenSSL::Crypto
)
