// include/beman/net/detail/tls/tls_error.hpp                         -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS_ERROR
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS_ERROR

#include <string>
#include <system_error>

// ----------------------------------------------------------------------------

namespace netexec::net::tls::__detail {

// Backend-agnostic TLS error codes.  Each platform backend (Schannel, OpenSSL,
// mbedTLS, SecureTransport) maps its native errors into these values so that
// callers get a consistent interface.
enum class tls_errc {
    success = 0,
    handshake_failed,
    certificate_verify_failed,
    certificate_untrusted_root,
    certificate_name_mismatch,
    certificate_expired,
    certificate_revoked,
    unsupported_operation,
    invalid_argument,
    no_buffer_space,
    message_too_large,
    unexpected_eof,
    decryption_failed,
    encryption_failed,
};

inline auto tls_category() noexcept -> const std::error_category& {
    struct category : std::error_category {
        auto name() const noexcept -> const char* override { return "tls"; }

        auto message(int value) const -> std::string override {
            switch (static_cast<tls_errc>(value)) {
            case tls_errc::success:
                return "success";
            case tls_errc::handshake_failed:
                return "TLS handshake failed";
            case tls_errc::certificate_verify_failed:
                return "TLS certificate verification failed";
            case tls_errc::certificate_untrusted_root:
                return "TLS certificate has an untrusted root";
            case tls_errc::certificate_name_mismatch:
                return "TLS certificate name mismatch";
            case tls_errc::certificate_expired:
                return "TLS certificate expired";
            case tls_errc::certificate_revoked:
                return "TLS certificate revoked";
            case tls_errc::unsupported_operation:
                return "TLS operation not supported";
            case tls_errc::invalid_argument:
                return "TLS invalid argument";
            case tls_errc::no_buffer_space:
                return "TLS output buffer too small";
            case tls_errc::message_too_large:
                return "TLS plaintext message too large";
            case tls_errc::unexpected_eof:
                return "TLS unexpected end of stream";
            case tls_errc::decryption_failed:
                return "TLS decryption failed";
            case tls_errc::encryption_failed:
                return "TLS encryption failed";
            }
            return "unknown TLS error";
        }
    };
    static category cat{};
    return cat;
}

inline auto make_error_code(tls_errc errc) noexcept -> std::error_code {
    return std::error_code{static_cast<int>(errc), tls_category()};
}

// Map common Schannel / Windows certificate errors to the unified enum.
inline auto map_schannel_error(long status) -> std::error_code {
    switch (static_cast<unsigned long>(status)) {
    case 0x80090325ul: // SEC_E_UNTRUSTED_ROOT
        return make_error_code(tls_errc::certificate_untrusted_root);
    case 0x80090328ul: // SEC_E_CERT_EXPIRED ( CRYPT_E_NO_REVOCATION_CHECK? verify)
        return make_error_code(tls_errc::certificate_expired);
    case 0x80090322ul: // SEC_E_WRONG_PRINCIPAL
        return make_error_code(tls_errc::certificate_name_mismatch);
    case 0x800B0109ul: // CERT_E_UNTRUSTEDROOT
        return make_error_code(tls_errc::certificate_untrusted_root);
    case 0x800B010Ful: // CERT_E_CN_NO_MATCH
        return make_error_code(tls_errc::certificate_name_mismatch);
    case 0x800B0101ul: // CERT_E_EXPIRED
        return make_error_code(tls_errc::certificate_expired);
    case 0x800B010Cul: // CERT_E_REVOKED
        return make_error_code(tls_errc::certificate_revoked);
    case 0x800B010Aul: // CERT_E_CHAINING
        return make_error_code(tls_errc::certificate_verify_failed);
    case 0x80096004ul: // TRUST_E_CERT_SIGNATURE
        return make_error_code(tls_errc::certificate_verify_failed);
    default:
        return std::error_code{static_cast<int>(status), std::system_category()};
    }
}

} // namespace netexec::net::tls::__detail

namespace std {

template <>
struct is_error_code_enum<netexec::net::tls::__detail::tls_errc> : std::true_type {};

} // namespace std

// ----------------------------------------------------------------------------

#endif
