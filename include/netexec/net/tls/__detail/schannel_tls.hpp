// include/beman/net/detail/tls/schannel_tls.hpp                       -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_SCHANNEL_TLS
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_SCHANNEL_TLS

#include <netexec/net/tls/__detail/tls_context_base.hpp>
#include <netexec/net/tls/__detail/tls_session_base.hpp>

#if !defined(NETEXEC_TLS_BACKEND_SCHANNEL)
#  error "schannel_tls.hpp should only be included when NETEXEC_TLS_BACKEND_SCHANNEL is defined"
#endif

// ----------------------------------------------------------------------------

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef SECURITY_WIN32
#  define SECURITY_WIN32
#endif
#include <WinSock2.h>
#include <Windows.h>
#include <Schannel.h>
#include <Security.h>
#include <wincrypt.h>
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

// The following types are required for ALPN support with Schannel but are not
// always present in older Windows SDK headers.
#ifndef _SCH_UNICODE_STRING_DEFINED
#  define _SCH_UNICODE_STRING_DEFINED
typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
#endif

#ifndef _SCH_TLS_PARAMETERS_DEFINED
#  define _SCH_TLS_PARAMETERS_DEFINED
typedef struct _TLS_PARAMETERS {
    DWORD           cAlpnIds;
    PUNICODE_STRING rgstrAlpnIds;
    DWORD           grbitDisabledProtocols;
    DWORD           cDisabledCrypto;
    void*           pDisabledCrypto;
    DWORD           dwFlags;
} TLS_PARAMETERS, *PTLS_PARAMETERS;
#endif

#ifndef _SCH_CREDENTIALS_DEFINED
#  define _SCH_CREDENTIALS_DEFINED
#  define SCH_CREDENTIALS_VERSION 0x00000005
struct _HMAPPER;
typedef struct _SCH_CREDENTIALS {
    DWORD            dwVersion;
    DWORD            dwCredFormat;
    DWORD            cCreds;
    PCCERT_CONTEXT*  paCred;
    HCERTSTORE       hRootStore;
    DWORD            cMappers;
    struct _HMAPPER** aphMappers;
    DWORD            dwSessionLifespan;
    DWORD            dwFlags;
    DWORD            cTlsParameters;
    PTLS_PARAMETERS  pTlsParameters;
} SCH_CREDENTIALS, *PSCH_CREDENTIALS;
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// ----------------------------------------------------------------------------

namespace netexec::net::tls::__detail {

namespace schannel {

inline auto make_error_code(SECURITY_STATUS status) -> std::error_code {
    return map_schannel_error(status);
}

inline auto utf8_to_wide(std::string_view str) -> std::wstring {
    if (str.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::wstring result(size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), size);
    return result;
}

inline auto encode_x500_name(std::wstring_view name) -> std::vector<unsigned char> {
    CERT_NAME_BLOB blob{};
    std::wstring   with_prefix = std::wstring(name);
    if (!::CertStrToNameW(
            X509_ASN_ENCODING,
            with_prefix.c_str(),
            CERT_X500_NAME_STR,
            nullptr,
            nullptr,
            &blob.cbData,
            nullptr)) {
        return {};
    }
    std::vector<unsigned char> encoded(blob.cbData);
    blob.pbData = encoded.data();
    if (!::CertStrToNameW(
            X509_ASN_ENCODING,
            with_prefix.c_str(),
            CERT_X500_NAME_STR,
            nullptr,
            encoded.data(),
            &blob.cbData,
            nullptr)) {
        return {};
    }
    encoded.resize(blob.cbData);
    return encoded;
}

inline auto generate_self_signed_certificate(std::string_view common_name) -> PCCERT_CONTEXT {
    std::wstring wide_cn = utf8_to_wide(common_name);
    if (wide_cn.empty()) {
        wide_cn = L"localhost";
    }
    std::wstring subject = L"CN=" + wide_cn;
    auto         encoded = encode_x500_name(subject);
    if (encoded.empty()) {
        return nullptr;
    }

    // Create a new key container and generate a 2048-bit RSA key pair.
    // The default key length used by CertCreateSelfSignCertificate is too weak
    // for modern browsers, so we explicitly create a stronger key first.
    // Use a unique container name: multiple preconnection contexts may be
    // created concurrently (e.g. IPv4 + IPv6 acceptors), and a shared name
    // would cause CryptAcquireContext to fall back to a different key store.
    static std::atomic<unsigned> counter{};
    std::wstring container = L"netexec-" + std::to_wstring(::GetTickCount64())
                           + L"-" + std::to_wstring(counter.fetch_add(1));
    HCRYPTPROV          hProv     = 0;
    constexpr DWORD     key_flags = (2048u << 16) | CRYPT_EXPORTABLE;
    if (!::CryptAcquireContextW(
            &hProv,
            container.c_str(),
            MS_ENHANCED_PROV_W,
            PROV_RSA_FULL,
            CRYPT_NEWKEYSET)
        && !::CryptAcquireContextW(
            &hProv,
            container.c_str(),
            MS_ENHANCED_PROV_W,
            PROV_RSA_FULL,
            CRYPT_NEWKEYSET | CRYPT_MACHINE_KEYSET)) {
        return nullptr;
    }

    HCRYPTKEY hKey = 0;
    if (!::CryptGenKey(hProv, AT_KEYEXCHANGE, key_flags, &hKey)) {
        ::CryptReleaseContext(hProv, 0);
        ::CryptAcquireContextW(&hProv, container.c_str(), MS_ENHANCED_PROV_W, PROV_RSA_FULL, CRYPT_DELETEKEYSET);
        return nullptr;
    }
    ::CryptDestroyKey(hKey);
    ::CryptReleaseContext(hProv, 0);

    CRYPT_KEY_PROV_INFO key_info{};
    key_info.pwszContainerName = container.data();
    key_info.pwszProvName      = const_cast<LPWSTR>(MS_ENHANCED_PROV_W);
    key_info.dwProvType        = PROV_RSA_FULL;
    key_info.dwKeySpec         = AT_KEYEXCHANGE;

    CERT_NAME_BLOB name_blob{};
    name_blob.pbData = encoded.data();
    name_blob.cbData = static_cast<DWORD>(encoded.size());

    // Helper to encode a single certificate extension.
    auto encode_extension = [](LPCSTR oid, const void* info) -> std::vector<BYTE> {
        std::vector<BYTE> der;
        DWORD             size = 0;
        if (::CryptEncodeObjectEx(
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                oid,
                info,
                0,
                nullptr,
                nullptr,
                &size)) {
            der.resize(size);
            if (!::CryptEncodeObjectEx(
                    X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                    oid,
                    info,
                    0,
                    nullptr,
                    der.data(),
                    &size)) {
                der.clear();
            } else {
                der.resize(size);
            }
        }
        return der;
    };

    // Build a Subject Alternative Name extension covering the common local
    // access patterns: DNS localhost, IPv4 loopback, and IPv6 loopback.
    // This lets browsers that resolve 'localhost' to either 127.0.0.1 or ::1
    // pass name validation (modulo the self-signed issuer warning).
    std::vector<CERT_ALT_NAME_ENTRY> alt_entries;

    CERT_ALT_NAME_ENTRY dns_entry{};
    dns_entry.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
    dns_entry.pwszDNSName     = const_cast<LPWSTR>(L"localhost");
    alt_entries.push_back(dns_entry);

    static constexpr BYTE            ipv4_loopback[] = {127, 0, 0, 1};
    CERT_ALT_NAME_ENTRY              ipv4_entry{};
    ipv4_entry.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
    ipv4_entry.IPAddress.cbData = static_cast<DWORD>(sizeof(ipv4_loopback));
    ipv4_entry.IPAddress.pbData = const_cast<BYTE*>(ipv4_loopback);
    alt_entries.push_back(ipv4_entry);

    static constexpr BYTE            ipv6_loopback[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CERT_ALT_NAME_ENTRY              ipv6_entry{};
    ipv6_entry.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
    ipv6_entry.IPAddress.cbData = static_cast<DWORD>(sizeof(ipv6_loopback));
    ipv6_entry.IPAddress.pbData = const_cast<BYTE*>(ipv6_loopback);
    alt_entries.push_back(ipv6_entry);

    CERT_ALT_NAME_INFO alt_info{};
    alt_info.cAltEntry  = static_cast<DWORD>(alt_entries.size());
    alt_info.rgAltEntry = alt_entries.data();

    auto san_der = encode_extension(szOID_SUBJECT_ALT_NAME2, &alt_info);

    // Add the Server Authentication EKU so browsers/Schannel treat this as a
    // valid TLS server certificate.
    LPCSTR               server_auth = szOID_PKIX_KP_SERVER_AUTH;
    CERT_ENHKEY_USAGE    eku{};
    eku.cUsageIdentifier             = 1;
    eku.rgpszUsageIdentifier         = const_cast<LPSTR*>(&server_auth);

    auto eku_der = encode_extension(szOID_ENHANCED_KEY_USAGE, &eku);

    // Add Key Usage: digitalSignature + keyEncipherment for TLS server certs.
    CRYPT_BIT_BLOB key_usage_blob{};
    BYTE           key_usage_bits = CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE;
    key_usage_blob.cbData         = 1;
    key_usage_blob.pbData         = &key_usage_bits;

    auto key_usage_der = encode_extension(szOID_KEY_USAGE, &key_usage_blob);

    // Add Basic Constraints v2: CA = FALSE.
    CERT_BASIC_CONSTRAINTS2_INFO basic_constraints{};
    basic_constraints.fCA                 = FALSE;
    basic_constraints.fPathLenConstraint  = FALSE;
    basic_constraints.dwPathLenConstraint = 0;

    auto basic_constraints_der = encode_extension(szOID_BASIC_CONSTRAINTS2, &basic_constraints);

    CERT_EXTENSION  ext_san{};
    CERT_EXTENSION  ext_eku{};
    CERT_EXTENSION  ext_key_usage{};
    CERT_EXTENSION  ext_basic_constraints{};
    CERT_EXTENSIONS cert_exts{};
    std::vector<CERT_EXTENSION> ext_list;
    if (!san_der.empty()) {
        ext_san.pszObjId     = const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2);
        ext_san.fCritical    = FALSE;
        ext_san.Value.cbData = static_cast<DWORD>(san_der.size());
        ext_san.Value.pbData = san_der.data();
        ext_list.push_back(ext_san);
    }
    if (!eku_der.empty()) {
        ext_eku.pszObjId     = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
        ext_eku.fCritical    = FALSE;
        ext_eku.Value.cbData = static_cast<DWORD>(eku_der.size());
        ext_eku.Value.pbData = eku_der.data();
        ext_list.push_back(ext_eku);
    }
    if (!key_usage_der.empty()) {
        ext_key_usage.pszObjId     = const_cast<LPSTR>(szOID_KEY_USAGE);
        ext_key_usage.fCritical    = TRUE;
        ext_key_usage.Value.cbData = static_cast<DWORD>(key_usage_der.size());
        ext_key_usage.Value.pbData = key_usage_der.data();
        ext_list.push_back(ext_key_usage);
    }
    if (!basic_constraints_der.empty()) {
        ext_basic_constraints.pszObjId     = const_cast<LPSTR>(szOID_BASIC_CONSTRAINTS2);
        ext_basic_constraints.fCritical    = TRUE;
        ext_basic_constraints.Value.cbData = static_cast<DWORD>(basic_constraints_der.size());
        ext_basic_constraints.Value.pbData = basic_constraints_der.data();
        ext_list.push_back(ext_basic_constraints);
    }
    if (!ext_list.empty()) {
        cert_exts.cExtension  = static_cast<DWORD>(ext_list.size());
        cert_exts.rgExtension = ext_list.data();
    }

    // Use SHA-256 instead of the default SHA-1 for the signature.
    CRYPT_ALGORITHM_IDENTIFIER sig_alg{};
    sig_alg.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    auto* cert = ::CertCreateSelfSignCertificate(
        0,
        &name_blob,
        0,
        &key_info,
        &sig_alg,
        nullptr,
        nullptr,
        ext_list.empty() ? nullptr : &cert_exts);
    return cert;
}

// ----------------------------------------------------------------------------
// PEM helpers.
// ----------------------------------------------------------------------------

inline auto read_file(std::string_view path) -> std::string {
    std::ifstream file(std::string{path}, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

inline auto split_pem_blocks(const std::string& content) -> std::vector<std::string> {
    std::vector<std::string> blocks;
    std::size_t             pos = 0;
    while (true) {
        const auto begin = content.find("-----BEGIN ", pos);
        if (begin == std::string::npos) {
            break;
        }
        auto end = content.find("-----END ", begin);
        if (end == std::string::npos) {
            break;
        }
        end = content.find("-----", end + 9);
        if (end == std::string::npos) {
            break;
        }
        end += std::string("-----").size();

        // Swallow trailing whitespace belonging to this PEM block.
        const auto next = content.find_first_not_of("\r\n", end);
        if (next == std::string::npos) {
            end = content.size();
        } else {
            end = next;
        }

        blocks.push_back(content.substr(begin, end - begin));
        pos = end;
    }
    return blocks;
}

inline auto decode_pem_block(const std::string& block) -> std::vector<unsigned char> {
    std::vector<unsigned char> decoded;
    DWORD                      size = 0;
    if (!::CryptStringToBinaryA(
            block.data(),
            static_cast<DWORD>(block.size()),
            CRYPT_STRING_BASE64HEADER,
            nullptr,
            &size,
            nullptr,
            nullptr)) {
        return {};
    }
    decoded.resize(size);
    if (!::CryptStringToBinaryA(
            block.data(),
            static_cast<DWORD>(block.size()),
            CRYPT_STRING_BASE64HEADER,
            decoded.data(),
            &size,
            nullptr,
            nullptr)) {
        return {};
    }
    decoded.resize(size);
    return decoded;
}

inline auto make_alpn_protocols_buffer(std::string_view protocol) -> std::vector<unsigned char> {
    std::vector<unsigned char> buffer;
    const unsigned short       protocol_size = static_cast<unsigned short>(protocol.size());
    const unsigned long        protocol_list_size =
        static_cast<unsigned long>(offsetof(SEC_APPLICATION_PROTOCOL_LIST, ProtocolList) + 1u + protocol_size);
    const unsigned long total_size =
        static_cast<unsigned long>(offsetof(SEC_APPLICATION_PROTOCOLS, ProtocolLists) + protocol_list_size);

    buffer.resize(total_size);
    auto* apps = reinterpret_cast<SEC_APPLICATION_PROTOCOLS*>(buffer.data());
    apps->ProtocolListsSize = protocol_list_size;

    auto* list = &apps->ProtocolLists[0];
    list->ProtoNegoExt = SecApplicationProtocolNegotiationExt_ALPN;
    list->ProtocolListSize = 1u + protocol_size;
    list->ProtocolList[0] = static_cast<unsigned char>(protocol_size);
    if (protocol_size != 0) {
        std::memcpy(&list->ProtocolList[1], protocol.data(), protocol_size);
    }
    return buffer;
}

inline auto import_private_key(
    const std::vector<unsigned char>& der,
    bool                              is_rsa_traditional,
    HCRYPTPROV&                       out_prov,
    std::wstring&                     out_container) -> std::error_code {
    std::wstring container = L"netexec-key-" + std::to_wstring(::GetTickCount64());
    HCRYPTPROV   hProv     = 0;
    if (!::CryptAcquireContextW(
            &hProv,
            container.c_str(),
            MS_ENHANCED_PROV_W,
            PROV_RSA_FULL,
            CRYPT_NEWKEYSET)) {
        return map_schannel_error(static_cast<long>(::GetLastError()));
    }

    // Convert whatever PEM format we have into a CAPI RSA private-key blob
    // and import it into the acquired container.
    std::vector<unsigned char> key_blob;
    {
        const std::vector<unsigned char>* rsa_der = &der;
        std::vector<unsigned char>          pkcs8_rsa_der;

        if (!is_rsa_traditional) {
            // PKCS#8: decode the wrapper and extract the inner RSA private key.
            DWORD pki_size = 0;
            if (!::CryptDecodeObjectEx(
                    X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                    PKCS_PRIVATE_KEY_INFO,
                    der.data(),
                    static_cast<DWORD>(der.size()),
                    0,
                    nullptr,
                    nullptr,
                    &pki_size)) {
                ::CryptReleaseContext(hProv, 0);
                return map_schannel_error(static_cast<long>(::GetLastError()));
            }
            std::vector<unsigned char> pki_buf(pki_size);
            if (!::CryptDecodeObjectEx(
                    X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                    PKCS_PRIVATE_KEY_INFO,
                    der.data(),
                    static_cast<DWORD>(der.size()),
                    0,
                    nullptr,
                    pki_buf.data(),
                    &pki_size)) {
                ::CryptReleaseContext(hProv, 0);
                return map_schannel_error(static_cast<long>(::GetLastError()));
            }
            const auto* pki = reinterpret_cast<PCRYPT_PRIVATE_KEY_INFO>(pki_buf.data());
            pkcs8_rsa_der.assign(pki->PrivateKey.pbData, pki->PrivateKey.pbData + pki->PrivateKey.cbData);
            rsa_der = &pkcs8_rsa_der;
        }

        DWORD blob_size = 0;
        if (!::CryptDecodeObjectEx(
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                PKCS_RSA_PRIVATE_KEY,
                rsa_der->data(),
                static_cast<DWORD>(rsa_der->size()),
                0,
                nullptr,
                nullptr,
                &blob_size)) {
            ::CryptReleaseContext(hProv, 0);
            return map_schannel_error(static_cast<long>(::GetLastError()));
        }
        key_blob.resize(blob_size);
        if (!::CryptDecodeObjectEx(
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                PKCS_RSA_PRIVATE_KEY,
                rsa_der->data(),
                static_cast<DWORD>(rsa_der->size()),
                0,
                nullptr,
                key_blob.data(),
                &blob_size)) {
            ::CryptReleaseContext(hProv, 0);
            return map_schannel_error(static_cast<long>(::GetLastError()));
        }
        key_blob.resize(blob_size);
    }

    HCRYPTKEY hKey   = 0;
    BOOL      imported = ::CryptImportKey(
        hProv,
        key_blob.data(),
        static_cast<DWORD>(key_blob.size()),
        0,
        0,
        &hKey);
    if (imported && hKey != 0) {
        ::CryptDestroyKey(hKey);
    }

    if (!imported) {
        ::CryptReleaseContext(hProv, 0);
        return map_schannel_error(static_cast<long>(::GetLastError()));
    }

    out_prov      = hProv;
    out_container = std::move(container);
    return {};
}

} // namespace schannel

// ----------------------------------------------------------------------------
// Schannel TLS session.
//
// The session is driven by the higher-level stream:
//   1. Call handshake_step() until it returns true.
//   2. Between calls, send outgoing_data() to the peer and feed incoming
//      ciphertext with feed_incoming().
//   3. After the handshake, use encrypt() / decrypt() for application data.
// ----------------------------------------------------------------------------
class schannel_tls_session : public session_base {
  public:
    // Client-side constructor.
    schannel_tls_session(CredHandle* cred, std::string target_name, HCERTSTORE ca_store = nullptr)
        : cred_(cred)
        , target_name_(std::move(target_name))
        , ca_store_(ca_store)
        , manual_validation_(ca_store != nullptr)
        , is_client_(true) {}

    // Server-side constructor.
    explicit schannel_tls_session(CredHandle* cred, std::vector<unsigned char> alpn = {})
        : cred_(cred)
        , alpn_buffer_(std::move(alpn))
        , is_client_(false) {}

    ~schannel_tls_session() {
        if (this->context_initialized_) {
            ::DeleteSecurityContext(&this->context_);
        }
    }

    schannel_tls_session(const schannel_tls_session&)            = delete;
    schannel_tls_session& operator=(const schannel_tls_session&) = delete;

    auto handshake_step(std::error_code& ec) -> bool override {
        if (this->handshake_complete_) {
            ec.clear();
            return true;
        }

        if (this->is_client_) {
            return this->client_handshake_step(ec);
        }
        return this->server_handshake_step(ec);
    }

    auto outgoing_data() -> std::span<const std::byte> override {
        if (this->output_buffer_.empty()) {
            return {};
        }
        return std::span{reinterpret_cast<const std::byte*>(this->output_buffer_.data()),
                         this->output_buffer_.size()};
    }

    auto consume_outgoing(std::size_t n) -> void override {
        if (n >= this->output_buffer_.size()) {
            this->output_buffer_.clear();
        } else {
            this->output_buffer_.erase(this->output_buffer_.begin(),
                                       this->output_buffer_.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }

    auto feed_incoming(std::span<const std::byte> data, std::size_t& consumed, std::error_code& ec)
        -> void override {
        consumed = 0;
        if (data.empty()) {
            ec.clear();
            return;
        }

        // Append new data to the input buffer.  For Schannel the actual
        // consumption happens inside InitializeSecurityContext / AcceptSecurityContext,
        // so we report 0 bytes consumed here.
        const auto old_size = this->input_buffer_.size();
        this->input_buffer_.resize(old_size + data.size());
        std::memcpy(this->input_buffer_.data() + old_size, data.data(), data.size());
        ec.clear();
    }

    auto shutdown(std::error_code& ec) -> void override {
        if (!this->context_initialized_) {
            ec.clear();
            return;
        }
        if (!this->output_buffer_.empty()) {
            // A close-notify token was already generated; caller should send it.
            ec.clear();
            return;
        }

        // Tell Schannel we want to initiate a TLS shutdown.
        DWORD       shutdown_type = SCHANNEL_SHUTDOWN;
        SecBuffer   token{};
        token.BufferType = SECBUFFER_TOKEN;
        token.cbBuffer   = sizeof(shutdown_type);
        token.pvBuffer   = &shutdown_type;
        SecBufferDesc token_desc{};
        token_desc.ulVersion = SECBUFFER_VERSION;
        token_desc.cBuffers  = 1;
        token_desc.pBuffers  = &token;

        auto status = ::ApplyControlToken(&this->context_, &token_desc);
        if (status != SEC_E_OK) {
            ec = schannel::make_error_code(status);
            return;
        }

        // Generate the outbound close-notify token.
        SecBuffer   out{};
        out.BufferType = SECBUFFER_TOKEN;
        out.cbBuffer   = 0;
        out.pvBuffer   = nullptr;
        SecBufferDesc out_desc{};
        out_desc.ulVersion = SECBUFFER_VERSION;
        out_desc.cBuffers  = 1;
        out_desc.pBuffers  = &out;

        DWORD     context_attr = 0;
        TimeStamp expiry{};
        if (this->is_client_) {
            status = ::InitializeSecurityContextA(
                this->cred_,
                &this->context_,
                nullptr,
                ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
                    | ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
                0,
                SECURITY_NATIVE_DREP,
                nullptr,
                0,
                nullptr,
                &out_desc,
                &context_attr,
                &expiry);
        } else {
            status = ::AcceptSecurityContext(
                this->cred_,
                &this->context_,
                nullptr,
                ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY
                    | ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM,
                SECURITY_NATIVE_DREP,
                nullptr,
                &out_desc,
                &context_attr,
                &expiry);
        }

        if (status == SEC_E_OK || status == SEC_I_CONTEXT_EXPIRED) {
            if (out.cbBuffer > 0 && out.pvBuffer != nullptr) {
                const auto* bytes = static_cast<const unsigned char*>(out.pvBuffer);
                this->output_buffer_.assign(bytes, bytes + out.cbBuffer);
                ::FreeContextBuffer(out.pvBuffer);
            }
            ec.clear();
        } else {
            ec = schannel::make_error_code(status);
        }
    }

    auto max_message_size() const noexcept -> std::size_t override { return this->max_message_size_; }

    auto encrypt(
        const void* input,
        std::size_t input_size,
        void* output,
        std::size_t output_size,
        std::size_t& output_written,
        std::error_code& ec) -> void override {
        output_written = 0;

        if (!this->handshake_complete_) {
            ec = make_error_code(tls_errc::handshake_failed);
            return;
        }

        if (input_size > this->max_message_size_) {
            ec = make_error_code(tls_errc::message_too_large);
            return;
        }

        const std::size_t total_size = this->header_size_ + input_size + this->trailer_size_;
        if (output_size < total_size) {
            ec = make_error_code(tls_errc::no_buffer_space);
            return;
        }

        auto* out_bytes = static_cast<unsigned char*>(output);

        SecBuffer buffers[4]{};
        buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        buffers[0].pvBuffer   = out_bytes;
        buffers[0].cbBuffer   = static_cast<unsigned long>(this->header_size_);

        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer   = out_bytes + this->header_size_;
        buffers[1].cbBuffer   = static_cast<unsigned long>(input_size);
        std::memcpy(buffers[1].pvBuffer, input, input_size);

        buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        buffers[2].pvBuffer   = out_bytes + this->header_size_ + input_size;
        buffers[2].cbBuffer   = static_cast<unsigned long>(this->trailer_size_);

        buffers[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc{};
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers  = 4;
        desc.pBuffers  = buffers;

        const auto status = ::EncryptMessage(&this->context_, 0, &desc, 0);
        if (status != SEC_E_OK) {
            ec = schannel::make_error_code(status);
            return;
        }

        output_written = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
        ec.clear();
    }

    auto decrypt(
        const void* input,
        std::size_t input_size,
        void* output,
        std::size_t output_size,
        std::size_t& output_written,
        std::error_code& ec) -> void override {
        output_written = 0;

        if (!this->handshake_complete_) {
            ec = make_error_code(tls_errc::handshake_failed);
            return;
        }

        // Append new ciphertext to the decryption buffer.
        if (input_size != 0) {
            const auto old_size = this->decrypt_buffer_.size();
            this->decrypt_buffer_.resize(old_size + input_size);
            std::memcpy(this->decrypt_buffer_.data() + old_size, input, input_size);
        }

        if (this->decrypt_buffer_.size() < this->header_size_) {
            ec = std::make_error_code(std::errc::resource_unavailable_try_again);
            return;
        }

        SecBuffer buffers[4]{};
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].pvBuffer   = this->decrypt_buffer_.data();
        buffers[0].cbBuffer   = static_cast<unsigned long>(this->decrypt_buffer_.size());

        buffers[1].BufferType = SECBUFFER_EMPTY;
        buffers[2].BufferType = SECBUFFER_EMPTY;
        buffers[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc{};
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers  = 4;
        desc.pBuffers  = buffers;

        const auto status = ::DecryptMessage(&this->context_, &desc, 0, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            ec = std::make_error_code(std::errc::resource_unavailable_try_again);
            return;
        }
        if (status == SEC_I_CONTEXT_EXPIRED || status == SEC_E_CONTEXT_EXPIRED) {
            // Peer sent a TLS close_notify; treat this as a graceful EOF.
            ec.clear();
            return;
        }
        if (status != SEC_E_OK) {
            ec = schannel::make_error_code(status);
            return;
        }

        // Find the decrypted data buffer.
        unsigned char* plaintext = nullptr;
        std::size_t    plaintext_size = 0;
        for (const auto& buf : buffers) {
            if (buf.BufferType == SECBUFFER_DATA) {
                plaintext      = static_cast<unsigned char*>(buf.pvBuffer);
                plaintext_size = buf.cbBuffer;
                break;
            }
        }

        if (plaintext_size > output_size) {
            ec = make_error_code(tls_errc::no_buffer_space);
            return;
        }

        if (plaintext_size > 0) {
            std::memcpy(output, plaintext, plaintext_size);
            output_written = plaintext_size;
        }

        // Remove the consumed record from decrypt_buffer_.  Buffer 0 holds the
        // whole encrypted record that was processed.
        const std::size_t consumed = buffers[0].cbBuffer;
        if (consumed >= this->decrypt_buffer_.size()) {
            this->decrypt_buffer_.clear();
        } else {
            this->decrypt_buffer_.erase(this->decrypt_buffer_.begin(),
                                        this->decrypt_buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
        }

        ec.clear();
    }

  private:
    auto client_handshake_step(std::error_code& ec) -> bool {
        SecBuffer outbuffer{};
        outbuffer.BufferType = SECBUFFER_TOKEN;

        SecBufferDesc outdesc{};
        outdesc.ulVersion = SECBUFFER_VERSION;
        outdesc.cBuffers  = 1;
        outdesc.pBuffers  = &outbuffer;

        SecBuffer inbuffers[2]{};
        inbuffers[0].BufferType = SECBUFFER_TOKEN;
        inbuffers[0].pvBuffer   = this->input_buffer_.data();
        inbuffers[0].cbBuffer   = static_cast<unsigned long>(this->input_buffer_.size());
        inbuffers[1].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc indesc{};
        indesc.ulVersion = SECBUFFER_VERSION;
        indesc.cBuffers  = 2;
        indesc.pBuffers  = inbuffers;

        DWORD context_attr = 0;
        const auto* target = this->target_name_.empty() ? nullptr : this->target_name_.c_str();

        const auto status = ::InitializeSecurityContextA(
            this->cred_,
            this->context_initialized_ ? &this->context_ : nullptr,
            const_cast<SEC_CHAR*>(target),
            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
                | ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
            0,
            SECURITY_NATIVE_DREP,
            this->context_initialized_ ? &indesc : nullptr,
            0,
            &this->context_,
            &outdesc,
            &context_attr,
            nullptr);

        this->consume_handshake_input(inbuffers[1], status);
        this->copy_handshake_output(outbuffer);

        if (status == SEC_E_OK) {
            this->context_initialized_ = true;
            if (this->manual_validation_) {
                if (!this->validate_remote_certificate(ec)) {
                    return false;
                }
            }
            this->handshake_complete_ = true;
            this->query_stream_sizes(ec);
            return true;
        }

        if (status == SEC_I_CONTINUE_NEEDED) {
            this->context_initialized_ = true;
            ec.clear();
            return false;
        }

        if (status == SEC_I_COMPLETE_NEEDED) {
            // TLS 1.3 post-handshake completion step.
            const auto complete_status = ::CompleteAuthToken(&this->context_, &indesc);
            if (complete_status != SEC_E_OK) {
                ec = schannel::make_error_code(complete_status);
                return false;
            }
            this->context_initialized_ = true;
            ec.clear();
            return this->input_buffer_.empty();
        }

        if (status == SEC_I_INCOMPLETE_CREDENTIALS) {
            // The server asked for a client certificate but we do not have one.
            // Keep the handshake alive; the server may decide to continue anyway.
            this->context_initialized_ = true;
            ec.clear();
            return false;
        }

        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            ec.clear();
            return false;
        }

        ec = schannel::make_error_code(status);
        return false;
    }

    auto validate_remote_certificate(std::error_code& ec) -> bool {
        PCCERT_CONTEXT remote_cert = nullptr;
        const auto     query_status =
            ::QueryContextAttributesA(&this->context_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote_cert);
        if (query_status != SEC_E_OK) {
            ec = schannel::make_error_code(query_status);
            return false;
        }

        CERT_CHAIN_PARA chain_para{};
        chain_para.cbSize = sizeof(chain_para);
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (!::CertGetCertificateChain(
                nullptr,
                remote_cert,
                nullptr,
                this->ca_store_,
                &chain_para,
                0,
                nullptr,
                &chain)) {
            ::CertFreeCertificateContext(remote_cert);
            ec = map_schannel_error(static_cast<long>(::GetLastError()));
            return false;
        }

        CERT_CHAIN_POLICY_PARA policy_para{};
        policy_para.cbSize            = sizeof(policy_para);
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_para{};
        ssl_para.cbSize               = sizeof(ssl_para);
        auto                           wide_target = schannel::utf8_to_wide(this->target_name_);
        ssl_para.pwszServerName       = const_cast<LPWSTR>(wide_target.c_str());
        policy_para.pvExtraPolicyPara = &ssl_para;

        CERT_CHAIN_POLICY_STATUS policy_status{};
        policy_status.cbSize = sizeof(policy_status);
        if (!::CertVerifyCertificateChainPolicy(
                CERT_CHAIN_POLICY_SSL,
                chain,
                &policy_para,
                &policy_status)) {
            ::CertFreeCertificateChain(chain);
            ::CertFreeCertificateContext(remote_cert);
            ec = map_schannel_error(static_cast<long>(::GetLastError()));
            return false;
        }

        ::CertFreeCertificateChain(chain);
        ::CertFreeCertificateContext(remote_cert);

        if (policy_status.dwError != 0) {
            ec = map_schannel_error(static_cast<long>(policy_status.dwError));
            return false;
        }

        ec.clear();
        return true;
    }

    auto server_handshake_step(std::error_code& ec) -> bool {
        SecBuffer outbuffer{};
        outbuffer.BufferType = SECBUFFER_TOKEN;

        SecBufferDesc outdesc{};
        outdesc.ulVersion = SECBUFFER_VERSION;
        outdesc.cBuffers  = 1;
        outdesc.pBuffers  = &outbuffer;

        const bool have_alpn = !this->alpn_buffer_.empty();
        SecBuffer  inbuffers[3]{};
        inbuffers[0].BufferType = SECBUFFER_TOKEN;
        inbuffers[0].pvBuffer   = this->input_buffer_.data();
        inbuffers[0].cbBuffer   = static_cast<unsigned long>(this->input_buffer_.size());
        inbuffers[1].BufferType = SECBUFFER_EMPTY;
        if (have_alpn) {
            inbuffers[2].BufferType = SECBUFFER_APPLICATION_PROTOCOLS;
            inbuffers[2].pvBuffer   = this->alpn_buffer_.data();
            inbuffers[2].cbBuffer   = static_cast<unsigned long>(this->alpn_buffer_.size());
        }

        SecBufferDesc indesc{};
        indesc.ulVersion = SECBUFFER_VERSION;
        indesc.cBuffers  = have_alpn ? 3u : 2u;
        indesc.pBuffers  = inbuffers;

        DWORD context_attr = 0;
        const auto status  = ::AcceptSecurityContext(
            this->cred_,
            this->context_initialized_ ? &this->context_ : nullptr,
            &indesc,
            ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY
                | ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM,
            SECURITY_NATIVE_DREP,
            &this->context_,
            &outdesc,
            &context_attr,
            nullptr);

        this->consume_handshake_input(inbuffers[1], status);
        this->copy_handshake_output(outbuffer);

        if (status == SEC_E_OK) {
            this->context_initialized_ = true;
            this->handshake_complete_ = true;
            this->query_stream_sizes(ec);
            return true;
        }

        if (status == SEC_I_CONTINUE_NEEDED) {
            this->context_initialized_ = true;
            ec.clear();
            return false;
        }

        if (status == SEC_I_COMPLETE_NEEDED) {
            // TLS 1.3 post-handshake authentication / completion step.
            const auto complete_status = ::CompleteAuthToken(&this->context_, &indesc);
            if (complete_status != SEC_E_OK) {
                ec = schannel::make_error_code(complete_status);
                return false;
            }
            this->context_initialized_ = true;
            ec.clear();
            // Treat as not yet complete; caller will drive another step if
            // there is more handshake data pending.
            return this->input_buffer_.empty();
        }

        if (status == SEC_I_INCOMPLETE_CREDENTIALS) {
            // Server-side this is unusual, but keep the handshake alive and
            // wait for the client to send more data.
            this->context_initialized_ = true;
            ec.clear();
            return false;
        }

        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            ec.clear();
            return false;
        }

        ec = schannel::make_error_code(status);
        return false;
    }

    auto consume_handshake_input(const SecBuffer& extra_buffer, SECURITY_STATUS status) -> void {
        if (extra_buffer.BufferType == SECBUFFER_EXTRA) {
            // Some data was not consumed by this handshake step. Keep the
            // trailing bytes in the input buffer; they may be part of the next
            // handshake flight or, in TLS 1.3, early data / post-handshake
            // records that follow the Finished message.
            const auto consumed = this->input_buffer_.size() - extra_buffer.cbBuffer;
            this->input_buffer_.erase(this->input_buffer_.begin(),
                                      this->input_buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
        } else if (status == SEC_I_CONTINUE_NEEDED || status == SEC_I_INCOMPLETE_CREDENTIALS) {
            // Schannel consumed the whole input for this step but needs another
            // round. The buffer is logically empty for the next step.
            this->input_buffer_.clear();
        } else if (status == SEC_E_OK) {
            // Handshake finished. Do NOT clear the buffer: TLS 1.3 peers may
            // send application data immediately after the Finished message,
            // sometimes in the same TCP segment. The remaining bytes must be
            // available to the first decrypt() call.
        }
    }

    auto copy_handshake_output(const SecBuffer& buffer) -> void {
        if (buffer.pvBuffer != nullptr && buffer.cbBuffer > 0) {
            const auto* bytes = static_cast<const unsigned char*>(buffer.pvBuffer);
            this->output_buffer_.insert(this->output_buffer_.end(), bytes, bytes + buffer.cbBuffer);
            ::FreeContextBuffer(buffer.pvBuffer);
        }
    }

    auto query_stream_sizes(std::error_code& ec) -> void {
        SecPkgContext_StreamSizes sizes{};
        const auto status = ::QueryContextAttributesA(&this->context_, SECPKG_ATTR_STREAM_SIZES, &sizes);
        if (status != SEC_E_OK) {
            ec = schannel::make_error_code(status);
            return;
        }

        this->header_size_      = sizes.cbHeader;
        this->trailer_size_     = sizes.cbTrailer;
        this->max_message_size_ = sizes.cbMaximumMessage;
        ec.clear();
    }

    CredHandle*  cred_{nullptr};
    std::string  target_name_;
    HCERTSTORE   ca_store_{nullptr};
    bool         manual_validation_{false};
    bool         is_client_{false};

    std::vector<unsigned char> alpn_buffer_;

    CtxtHandle  context_{};
    bool        context_initialized_{false};
    bool        handshake_complete_{false};

    std::size_t header_size_{0};
    std::size_t trailer_size_{0};
    std::size_t max_message_size_{0};

    std::vector<unsigned char> input_buffer_;     // ciphertext received from peer (handshake)
    std::vector<unsigned char> output_buffer_;    // ciphertext to send to peer (handshake)
    std::vector<unsigned char> decrypt_buffer_;   // ciphertext waiting to be decrypted (application data)
};

class schannel_tls_context : public context_base {
  public:
    schannel_tls_context() = default;

    ~schannel_tls_context() {
        if (this->client_cred_valid_) {
            ::FreeCredentialsHandle(&this->client_cred_);
        }
        if (this->server_cred_valid_) {
            ::FreeCredentialsHandle(&this->server_cred_);
        }
        if (this->server_cert_ != nullptr) {
            ::CertFreeCertificateContext(this->server_cert_);
        }
        if (this->key_prov_ != 0) {
            ::CryptReleaseContext(this->key_prov_, 0);
        }
        if (this->ca_store_ != nullptr) {
            ::CertCloseStore(this->ca_store_, 0);
        }
    }

    schannel_tls_context(const schannel_tls_context&)            = delete;
    schannel_tls_context& operator=(const schannel_tls_context&) = delete;

    auto use_certificate_file(std::string_view path) -> std::error_code override {
        const auto content = schannel::read_file(path);
        if (content.empty()) {
            return std::make_error_code(std::errc::no_such_file_or_directory);
        }

        const auto blocks = schannel::split_pem_blocks(content);
        for (const auto& block : blocks) {
            if (block.find("CERTIFICATE") == std::string::npos) {
                continue;
            }
            const auto der = schannel::decode_pem_block(block);
            if (der.empty()) {
                return make_error_code(tls_errc::invalid_argument);
            }

            auto* cert = ::CertCreateCertificateContext(
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                der.data(),
                static_cast<DWORD>(der.size()));
            if (cert == nullptr) {
                return map_schannel_error(static_cast<long>(::GetLastError()));
            }

            if (this->server_cert_ != nullptr) {
                ::CertFreeCertificateContext(this->server_cert_);
            }
            this->server_cert_       = cert;
            this->server_cert_owned_ = true;

            // If a private key was already loaded, associate it with the new cert.
            return this->apply_private_key_to_certificate();
        }

        return make_error_code(tls_errc::invalid_argument);
    }

    auto use_private_key_file(std::string_view path) -> std::error_code override {
        const auto content = schannel::read_file(path);
        if (content.empty()) {
            return std::make_error_code(std::errc::no_such_file_or_directory);
        }

        const auto blocks = schannel::split_pem_blocks(content);
        for (const auto& block : blocks) {
            if (block.find("PRIVATE KEY") == std::string::npos) {
                continue;
            }

            const auto der = schannel::decode_pem_block(block);
            if (der.empty()) {
                return make_error_code(tls_errc::invalid_argument);
            }

            const bool is_rsa_traditional = block.find("RSA PRIVATE KEY") != std::string::npos;

            HCRYPTPROV   new_prov      = 0;
            std::wstring new_container;
            auto         ec = schannel::import_private_key(der, is_rsa_traditional, new_prov, new_container);
            if (ec) {
                return ec;
            }

            if (this->key_prov_ != 0) {
                ::CryptReleaseContext(this->key_prov_, 0);
            }
            this->key_prov_      = new_prov;
            this->key_container_ = std::move(new_container);

            return this->apply_private_key_to_certificate();
        }

        return make_error_code(tls_errc::invalid_argument);
    }

    auto use_ca_bundle(std::string_view path) -> std::error_code override {
        const auto content = schannel::read_file(path);
        if (content.empty()) {
            return std::make_error_code(std::errc::no_such_file_or_directory);
        }

        if (this->ca_store_ != nullptr) {
            ::CertCloseStore(this->ca_store_, 0);
        }
        this->ca_store_ = ::CertOpenStore(
            CERT_STORE_PROV_MEMORY,
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            0,
            CERT_STORE_CREATE_NEW_FLAG,
            nullptr);
        if (this->ca_store_ == nullptr) {
            return map_schannel_error(static_cast<long>(::GetLastError()));
        }

        const auto blocks = schannel::split_pem_blocks(content);
        for (const auto& block : blocks) {
            if (block.find("CERTIFICATE") == std::string::npos) {
                continue;
            }
            const auto der = schannel::decode_pem_block(block);
            if (der.empty()) {
                ::CertCloseStore(this->ca_store_, 0);
                this->ca_store_ = nullptr;
                return make_error_code(tls_errc::invalid_argument);
            }
            if (!::CertAddEncodedCertificateToStore(
                    this->ca_store_,
                    X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                    der.data(),
                    static_cast<DWORD>(der.size()),
                    CERT_STORE_ADD_USE_EXISTING,
                    nullptr)) {
                // Ignore duplicates; treat other errors as fatal.
                if (::GetLastError() != ERROR_ALREADY_EXISTS && ::GetLastError() != CRYPT_E_EXISTS) {
                    ::CertCloseStore(this->ca_store_, 0);
                    this->ca_store_ = nullptr;
                    return map_schannel_error(static_cast<long>(::GetLastError()));
                }
            }
        }

        return {};
    }

    auto use_default_trust_store() -> std::error_code override {
        // Schannel uses the Windows certificate store by default; nothing to do.
        return {};
    }

    auto create_client_session() -> std::unique_ptr<session_base> override {
        auto ec = this->ensure_client_credentials();
        if (ec) {
            return nullptr;
        }
        return std::make_unique<schannel_tls_session>(
            &this->client_cred_, this->target_name_, this->ca_store_);
    }

    auto create_server_session() -> std::unique_ptr<session_base> override {
        auto ec = this->ensure_server_credentials();
        if (ec) {
            return nullptr;
        }
        return std::make_unique<schannel_tls_session>(
            &this->server_cred_,
            schannel::make_alpn_protocols_buffer("http/1.1"));
    }

    auto set_hostname(std::string_view name) -> void override { this->target_name_ = std::string{name}; }

  private:
    auto apply_private_key_to_certificate() -> std::error_code {
        if (this->server_cert_ == nullptr || this->key_container_.empty() || this->key_prov_ == 0) {
            return {};
        }

        CRYPT_KEY_PROV_INFO info{};
        info.pwszContainerName = this->key_container_.data();
        info.pwszProvName      = const_cast<LPWSTR>(MS_ENHANCED_PROV_W);
        info.dwProvType        = PROV_RSA_FULL;
        info.dwKeySpec         = AT_KEYEXCHANGE;

        if (!::CertSetCertificateContextProperty(
                this->server_cert_,
                CERT_KEY_PROV_INFO_PROP_ID,
                0,
                &info)) {
            return map_schannel_error(static_cast<long>(::GetLastError()));
        }
        return {};
    }


    auto ensure_client_credentials() -> std::error_code {
        if (this->client_cred_valid_) {
            return {};
        }

        SCHANNEL_CRED cred{};
        cred.dwVersion = SCHANNEL_CRED_VERSION;
        // Use system default protocols (TLS 1.2+).
        cred.grbitEnabledProtocols = 0;

        // When a custom CA bundle is provided, disable Schannel's automatic
        // validation and perform explicit chain verification after the handshake.
        if (this->ca_store_ != nullptr) {
            cred.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION;
        }

        const auto status = ::AcquireCredentialsHandleA(
            nullptr,
            const_cast<SEC_CHAR*>(UNISP_NAME_A),
            SECPKG_CRED_OUTBOUND,
            nullptr,
            &cred,
            nullptr,
            nullptr,
            &this->client_cred_,
            nullptr);

        if (status != SEC_E_OK) {
            return schannel::make_error_code(status);
        }

        this->client_cred_valid_ = true;
        return {};
    }

    auto ensure_server_credentials() -> std::error_code {
        if (this->server_cred_valid_) {
            return {};
        }

        if (this->server_cert_ == nullptr) {
            // Generate a self-signed certificate on demand so that local HTTPS
            // endpoints work out of the box.  Browsers will warn about the
            // untrusted issuer, but the TLS channel will be functional.
            this->server_cert_ = schannel::generate_self_signed_certificate(this->target_name_);
            if (this->server_cert_ == nullptr) {
                return make_error_code(tls_errc::handshake_failed);
            }
            this->server_cert_owned_ = true;
        }

        // Allow the platform default TLS versions (TLS 1.2+ on modern Windows).
        // The handshake loop below handles SEC_I_CONTINUE_NEEDED and the
        // TLS 1.3 post-handshake states SEC_I_COMPLETE_NEEDED /
        // SEC_I_INCOMPLETE_CREDENTIALS.
        SCH_CREDENTIALS cred{};
        cred.dwVersion      = SCH_CREDENTIALS_VERSION;
        cred.cCreds         = 1;
        cred.paCred         = &this->server_cert_;
        cred.dwFlags        = SCH_CRED_NO_SYSTEM_MAPPER
                            | SCH_CRED_DISABLE_RECONNECTS
                            | SCH_CRED_IGNORE_NO_REVOCATION_CHECK
                            | SCH_CRED_IGNORE_REVOCATION_OFFLINE;
        cred.cTlsParameters = 0;
        cred.pTlsParameters = nullptr;

        const auto status = ::AcquireCredentialsHandleA(
            nullptr,
            const_cast<SEC_CHAR*>(UNISP_NAME_A),
            SECPKG_CRED_INBOUND,
            nullptr,
            &cred,
            nullptr,
            nullptr,
            &this->server_cred_,
            nullptr);

        if (status != SEC_E_OK) {
            return schannel::make_error_code(status);
        }

        this->server_cred_valid_ = true;
        return {};
    }

    CredHandle      client_cred_{};
    bool            client_cred_valid_{false};
    CredHandle      server_cred_{};
    bool            server_cred_valid_{false};
    PCCERT_CONTEXT  server_cert_{nullptr};
    bool            server_cert_owned_{false};
    HCRYPTPROV      key_prov_{0};
    std::wstring    key_container_;
    HCERTSTORE      ca_store_{nullptr};
    std::string     target_name_;
};

} // namespace netexec::net::tls::__detail

// ----------------------------------------------------------------------------

#endif
