# netexec 未来扩展计划

本文档列出 netexec 在两层 API 基础上的未来扩展方向，按优先级和建议实现顺序排列。

## 已完成的重大重构

- 两层 API 拆分完成：`netexec::net::ip::tcp::*` 与 `netexec::net::tls::*`（tls 层）。
- `ip::tcp::acceptor` 构造函数不再隐式执行 `bind + listen`；新增显式 `tcp::async_listen` CPO。
- tls 层 accept CPO 从 `async_accept_connection` 改名为 `async_accept`。
- `ip::tcp::async_accept` 保持原名。
- 删除旧 TAPS 骨架文件：`__detail/initiate.hpp`、`listen.hpp`、`preconnection.hpp`、`local_endpoint.hpp`、`remote_endpoint.hpp`、`rendezvous.hpp`。
- Windows Schannel TLS 后端已完整实现：握手、加解密、服务端自签名证书、PEM 证书/私钥/CA bundle 加载。

---

## 1. TLS 支持（已基本完成，OpenSSL 后端待完善）

### 目标
`netexec::net::ip::tcp::*` 提供普通 socket 操作；TLS 通过 `netexec::net::tls::*`（tls 层）显式启用。

### 设计原则
不同操作系统应优先使用原生 TLS 实现（减少依赖、符合平台规范），但上层 API 必须完全一致。实现上采用**平台抽象层 + 统一概念接口**，与 netexec 现有 `platform.hpp`（隐藏 Windows/POSIX 差异）和 `iocp_context` / `poll_context` / `uring_context`（隐藏 IO 后端差异）的抽象方式保持一致。

### 平台后端选择

| 平台 | 推荐原生实现 | 理由 |
|------|-------------|------|
| Windows | Schannel | 系统自带，支持 Windows 证书 store，无需额外依赖 |
| Linux | OpenSSL | 事实标准，发行版自带，生态最全 |
| macOS / iOS | SecureTransport / Network.framework | Apple 已废弃 OpenSSL，原生框架更符合审核要求 |
| Android | BoringSSL / OpenSSL | Android 系统层基于 BoringSSL，NDK 开发通常沿用 |
| 嵌入式 / 特殊场景 | mbedTLS | 体积小、可裁剪、许可友好 |

CMake 提供选项覆盖默认：
```cmake
set(NETEXEC_TLS_BACKEND "auto" CACHE STRING
    "TLS backend: auto, openssl, schannel, mbedtls, secure_transport")
```

### Windows 下的 OpenSSL 可选后端

Windows 默认使用 **Schannel**（系统原生、无额外依赖），但建议把 **OpenSSL 作为可选后端**保留，用于跨平台一致性测试、特定算法套件或统一证书文件场景。

通过 CMake 开关启用：
```bash
cmake -B build -G "Visual Studio 18 2026" -DNETEXEC_TLS_BACKEND=openssl
```

OpenSSL 采用与项目风格一致的 **CPM** 管理，自动下载到 `build/_deps/openssl-src/`，无需用户手动安装。CMake 逻辑示例：

```cmake
if(NETEXEC_TLS_BACKEND STREQUAL "openssl")
    if(WIN32)
        # Windows 下用 CPM 自动拉取并构建 OpenSSL
        include(cmake/Modules/CPM.cmake)
        CPMAddPackage(
            NAME openssl
            VERSION 3.2.1
            GITHUB_REPOSITORY openssl/openssl
            GIT_TAG openssl-3.2.1
            OPTIONS
                "OPENSSL_NO_TESTS ON"
                "OPENSSL_NO_DOCS ON"
        )
        # OpenSSL 源码位于 build/_deps/openssl-src/
        # 构建产物位于 build/_deps/openssl-build/
    else()
        # Linux / macOS 优先使用系统已安装的 OpenSSL
        find_package(OpenSSL REQUIRED)
    endif()
elseif(NETEXEC_TLS_BACKEND STREQUAL "schannel")
    # Schannel 为 Windows 原生 API，无需额外依赖
    target_link_libraries(netexec INTERFACE secur32 crypt32)
endif()
```

默认情况下，Windows 用户不启用 `-DNETEXEC_TLS_BACKEND=openssl` 时，项目完全零第三方 TLS 依赖。

> **注意**：OpenSSL 官方仓库并非 CMake 项目，上述 `CPMAddPackage` 示例表达的是“通过 CPM 拉取源码并构建”的意图。实际实现时，需配合 CMake wrapper（例如 `janbar/openssl-cmake`）或自定义 `ExternalProject_Add` / `execute_process` 调用 OpenSSL 的 `Configure` + `nmake` / `jom` 完成构建。

### Windows OpenSSL 后端实现（TODO / 大工程量）

在 Windows 上支持 OpenSSL 作为可选 TLS 后端，工程量远大于 Linux/macOS，因为需要额外处理依赖构建、Windows 证书 store 集成以及 DLL 分发等问题。下面是实现路径和具体步骤。

#### 1. 目标

- 保持与 Linux OpenSSL 后端代码最大程度复用。
- 通过 CMake 选项 `-DNETEXEC_TLS_BACKEND=openssl` 在 Windows 上启用。
- 默认仍使用 Schannel，确保普通用户零依赖。

#### 2. 依赖引入

核心依赖只有 **OpenSSL**（`libssl` + `libcrypto`），但 Windows 上构建方式需要额外处理：

| 引入方式 | 说明 |
|---|---|
| **vcpkg** | `vcpkg install openssl:x64-windows`，CMake 通过 `find_package(OpenSSL)` 定位 |
| **Conan** | 在 `conanfile.py` 里加 `openssl/[>=3.0]` |
| **CPM + CMake wrapper** | 与项目现有 CPM 风格一致，但 OpenSSL 官方不是 CMake 项目，需要 wrapper（如 `janbar/openssl-cmake`）或自定义 `ExternalProject_Add` |
| **预编译二进制** | 需要匹配 MSVC 版本和运行时库，分发和维护成本高 |

推荐做法：**vcpkg 或 Conan**，因为它们提供了稳定的 Windows 二进制包；CPM 自编译适合需要固定源码版本的场景，但会显著增加初次构建时间。

链接时除了 `OpenSSL::SSL` 和 `OpenSSL::Crypto`，通常还要加：
```cmake
ws2_32.lib
advapi32.lib
user32.lib
gdi32.lib
```

#### 3. 需要修改和新增的代码

##### 3.1 CMake 构建系统

- 在 CMake 中处理 `NETEXEC_TLS_BACKEND=openssl` 的 Windows 分支。
- 调用 `find_package(OpenSSL REQUIRED)` 或 CPM/ExternalProject 获取 OpenSSL。
- 把 OpenSSL 头文件目录和库链接到 `netexec` target。
- 定义编译宏 `NETEXEC_TLS_USE_OPENSSL`。
- 处理 OpenSSL DLL 的拷贝或安装（Windows 上运行时通常需要 `libssl-3-x64.dll` 和 `libcrypto-3-x64.dll`）。

##### 3.2 后端选择逻辑

当前编译期选择大致如下：
```cpp
#if defined(_WIN32)
    using context = schannel_tls_context;
#elif defined(__APPLE__)
    using context = secure_transport_tls_context;
#else
    using context = openssl_tls_context;
#endif
```

需要改成允许 OpenSSL 覆盖平台默认：
```cpp
#if defined(NETEXEC_TLS_USE_OPENSSL)
    using context = openssl_tls_context;
#elif defined(_WIN32)
    using context = schannel_tls_context;
#elif defined(__APPLE__)
    using context = secure_transport_tls_context;
#else
    using context = openssl_tls_context;
#endif
```

##### 3.3 完善 `openssl_tls_context`

如果 `include/netexec/net/tls/__detail/openssl_tls.hpp` 还是 stub，需要补全 `context_base` 的所有接口：

- `use_certificate_file`：调用 `SSL_CTX_use_certificate_file(ctx_, path, SSL_FILETYPE_PEM)`。
- `use_private_key_file`：调用 `SSL_CTX_use_PrivateKey_file` + `SSL_CTX_check_private_key`。
- `use_ca_bundle`：调用 `SSL_CTX_load_verify_locations`。
- `use_default_trust_store`：这是 Windows 下最麻烦的部分（见 3.7）。
- `create_client_session` / `create_server_session`：构造 `openssl_tls_session`。

##### 3.4 完善 `openssl_tls_session`

需要基于 **memory BIO** 实现异步 TLS：

```cpp
class openssl_tls_session : public session_base {
  private:
    SSL* ssl_;
    BIO* rbio_;  // OpenSSL 从这里读入 socket 收到的密文
    BIO* wbio_;  // OpenSSL 把要发送的密文写入这里
};
```

核心方法：
- `handshake_step`：调用 `SSL_do_handshake`，根据返回值判断完成或需要更多 IO。
- `encrypt`：调用 `SSL_write`，然后从 `wbio_` 取出密文。
- `decrypt`：把密文写入 `rbio_`，然后调用 `SSL_read`。
- `feed_incoming` / `consume_outgoing`：与 netexec 的异步 IO 调度器对接。
- `shutdown`：调用 `SSL_shutdown`。

##### 3.5 握手循环

典型实现：

```cpp
auto openssl_tls_session::handshake_step(std::error_code& ec) -> bool {
    ERR_clear_error();
    int ret = SSL_do_handshake(ssl_);
    int ssl_err = SSL_get_error(ssl_, ret);

    if (ret == 1) {
        ec = {};
        return true;  // 握手完成
    }

    if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
        ec = {};  // 需要更多 IO
        return false;
    }

    ec = make_openssl_error_code();
    return false;
}
```

netexec scheduler 根据 `WANT_READ` / `WANT_WRITE` 注册对应的可读/可写事件。

##### 3.6 错误码映射

需要把 OpenSSL 错误通过 `ERR_get_error()` 映射到统一的 `tls_errc`：

```cpp
auto make_openssl_error_code() -> std::error_code {
    unsigned long err = ERR_get_error();
    // 根据 ERR_GET_REASON(err) 映射
    if (ERR_GET_REASON(err) == SSL_R_CERTIFICATE_VERIFY_FAILED)
        return tls_errc::certificate_verify_failed;
    // ...
}
```

##### 3.7 Windows 默认信任 store 集成

OpenSSL 默认不会自动信任 Windows 系统证书 store。`use_default_trust_store()` 需要额外实现，可选方案：

- **方案 A**：在构建/安装时附带一个 `cacert.pem`，加载它作为默认信任锚。
- **方案 B**：运行时用 Windows API（`CertOpenStore` / `CertEnumCertificatesInStore`）枚举系统根证书，导出到临时 PEM 文件，再调用 `SSL_CTX_load_verify_locations`。
- **方案 C**：实现自定义 `X509_STORE` 和 verify callback，在回调里用 Windows 证书 API 验证证书链。

方案 B 最接近 Schannel 的行为，但实现较复杂；方案 A 最简单但维护成本高（需要定期更新 cacert.pem）。

##### 3.8 初始化与多线程

OpenSSL 1.1.1+ 通常不需要显式初始化，但如果要兼容旧版本，可以在程序启动时调用：
```cpp
SSL_library_init();
SSL_load_error_strings();
OpenSSL_add_all_algorithms();
```

多线程方面，OpenSSL 1.1.0+ 默认线程安全；1.0.2 需要设置锁回调。

##### 3.9 测试

需要新增参数化测试：
```cpp
TEST_CASE("openssl tls echo on Windows") {
    tls_echo_test<openssl_context_factory>();
}
```

并在 CI 矩阵里增加 Windows + OpenSSL 的构建配置。

#### 4. 与现有 Linux OpenSSL 后端的复用

如果 Linux 的 `openssl_tls_context` / `openssl_tls_session` 已经实现，Windows 上基本可以**直接复用同一份代码**，差异点主要在：

- CMake 如何找到/构建 OpenSSL。
- `use_default_trust_store()` 的平台特定实现。
- 运行时 DLL 的拷贝/安装。

因此 OpenSSL 后端的业务逻辑本身不需要为 Windows 重写，工程量主要在构建系统和平台集成。

#### 5. 优先级与风险

- **优先级：低到中**。没有明确需求时，Windows 默认用 Schannel 已经足够。
- **主要风险**：
  - 构建时间显著增加（OpenSSL 自编译较慢）。
  - DLL 分发复杂化。
  - Windows 系统证书 store 集成不如 Schannel 自然。
- **推荐做法**：保持 Schannel 为默认，OpenSSL 作为可选后端长期保留在 backlog 中。

### 抽象层接口

在 `netexec::net::tls::__detail` 中定义后端无关的 TLS 抽象。上下文负责加载证书、信任策略；会话负责一次具体连接的握手和加解密。

```cpp
namespace netexec::net::tls::__detail {

class context_base {
  public:
    virtual ~context_base() = default;

    virtual auto use_certificate_file(std::string_view path) -> std::error_code = 0;
    virtual auto use_private_key_file(std::string_view path) -> std::error_code = 0;
    virtual auto use_ca_bundle(std::string_view path) -> std::error_code = 0;
    virtual auto use_default_trust_store() -> std::error_code = 0;

    virtual auto set_hostname(std::string_view name) -> void {}
    virtual auto create_client_session() -> std::unique_ptr<session_base> = 0;
    virtual auto create_server_session() -> std::unique_ptr<session_base> = 0;
};

class session_base {
  public:
    virtual ~session_base() = default;

    virtual auto handshake_step(std::error_code& ec) -> bool = 0;
    virtual auto outgoing_data() -> std::span<const std::byte> = 0;
    virtual auto consume_outgoing(std::size_t n) -> void = 0;
    virtual auto feed_incoming(std::span<const std::byte> data,
                               std::size_t& consumed,
                               std::error_code& ec) -> void = 0;

    virtual auto shutdown(std::error_code& ec) -> void = 0;
    virtual auto max_message_size() const noexcept -> std::size_t { return 0; }

    virtual auto encrypt(
        const void* input, std::size_t input_size,
        void* output, std::size_t output_size,
        std::size_t& output_written,
        std::error_code& ec) -> void = 0;

    virtual auto decrypt(
        const void* input, std::size_t input_size,
        void* output, std::size_t output_size,
        std::size_t& output_written,
        std::error_code& ec) -> void = 0;
};

} // namespace netexec::net::tls::__detail
```

各后端提供具体实现：
- `include/netexec/net/tls/__detail/openssl_tls.hpp`
- `include/netexec/net/tls/__detail/schannel_tls.hpp`
- `include/netexec/net/tls/__detail/mbedtls_tls.hpp`
- `include/netexec/net/tls/__detail/secure_transport_tls.hpp`

编译期根据平台和 CMake 选项选择默认后端：
```cpp
namespace netexec::net::tls::__detail {

#if defined(NETEXEC_TLS_USE_MBEDTLS)
    using context = mbedtls_tls_context;
#elif defined(_WIN32)
    using context = schannel_tls_context;
#elif defined(__APPLE__)
    using context = secure_transport_tls_context;
#else
    using context = openssl_tls_context;
#endif

} // namespace netexec::net::tls::__detail
```

### 证书与信任策略配置

证书路径和信任策略属于**运行时配置**，由 `net::tls::preconnection` 的属性环境传入；CMake 只负责选择 TLS 后端，不负责指定证书文件。

#### 客户端属性

```cpp
namespace netexec::net::tls {

struct ca_bundle : prop<ca_bundle_t, std::string> {
    explicit ca_bundle(std::string pem_file_path);
};

struct use_system_trust_store : prop<use_system_trust_store_t, bool> {
    explicit use_system_trust_store(bool value = true);
};

} // namespace netexec::net
```

使用示例：

```cpp
// 默认：信任系统/平台预置 CA（如 Windows 证书 store、OpenSSL 默认路径）
auto pre = ex::env{
    net::hostname("example.com"),
    net::port(443)
};

// 自定义 CA，不信任系统 store（适合自签名证书或内网 CA）
auto pre_custom = ex::env{
    net::hostname("internal.example.com"),
    net::port(443),
    net::tls::ca_bundle("/path/to/company-ca.pem"),
    net::tls::use_system_trust_store(false)
};
```

#### 服务端属性

```cpp
namespace netexec::net::tls {

struct certificate : prop<certificate_t, std::string> {
    explicit certificate(std::string pem_file_path);
};

struct private_key : prop<private_key_t, std::string> {
    explicit private_key(std::string pem_file_path);
};

} // namespace netexec::net
```

使用示例：

```cpp
auto server_pre = ex::env{
    net::port(443),
    net::tls::certificate("/etc/ssl/server.crt"),
    net::tls::private_key("/etc/ssl/server.key")
};
```

#### 各后端映射

公共 API 统一使用 PEM 文件路径，每个后端在内部转换为原生格式：

| 后端 | 证书加载方式 |
|------|-------------|
| OpenSSL | `SSL_CTX_use_certificate_file` / `SSL_CTX_use_PrivateKey_file` / `SSL_CTX_load_verify_locations` |
| Schannel | 读取 PEM → 转换为 DER → `CertCreateCertificateContext`，私钥通过 CryptoAPI 导入 |
| SecureTransport | `SecCertificateCreateWithData` + `SecIdentityCreateWithCertificate` |
| mbedTLS | `mbedtls_x509_crt_parse_file` + `mbedtls_pk_parse_keyfile` |

#### 默认行为

- **客户端**：`use_system_trust_store(true)`，不指定 `ca_bundle` 时使用平台默认信任 store。
- **服务端**：显式提供 `certificate` 和 `private_key` 时使用该证书。
  - **Windows Schannel**：未提供证书时，服务端会自动生成一张内存中的自签名证书，使本地 HTTPS 可以立刻工作（浏览器会提示证书不受信任，但 TLS 通道本身是正常的）。

> 本地开发证书警告的消除方案与 `async_shutdown` 的设计说明已移至 [feature.md](feature.md)。

### 与现有 CPO 的集成

- `net::tls::async_initiate` 在 TCP 连接成功后驱动 `tls_session->handshake_step()` 直到完成。
- `net::tls::async_send` 先调用 `tls_session->encrypt()`，再调用 `tcp::async_send()`。
- `net::tls::async_receive` 读到 EOF 或连接关闭后，批量调用 `tls_session->decrypt()`。
- 新增 `net::tls::async_receive_some`：解密并返回当前可用的部分明文，供 HTTP 等协议按请求边界读取。
- `net::tls::async_listen` 创建携带 TLS 上下文的 acceptor，`async_accept` 为新连接创建 `tls_session` 并执行服务端握手。
- `netexec::net::ip::tcp::*` 继续使用裸 socket；TLS 语义只在 tls 层生效。

### 错误码统一

不同 TLS 库的错误码千差万别，统一映射到 `std::error_code`。已实现 `netexec::net::tls::__detail::tls_category()` 与 `tls_errc`：

- 验证失败统一通过 `std::error_code` / `std::system_category` 报告（后续可引入专用的 `tls_category()`）。

```cpp
namespace netexec::net::tls::__detail {

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

auto tls_category() noexcept -> const std::error_category&;
auto make_error_code(tls_errc) noexcept -> std::error_code;

} // namespace netexec::net::tls::__detail
```

Schannel 后端会把 `CERT_E_UNTRUSTEDROOT`、`CERT_E_CN_NO_MATCH`、`CERT_E_EXPIRED`、`SEC_E_UNTRUSTED_ROOT` 等 Windows 错误映射到上述统一错误码；未被识别的原生错误仍保留在 `std::system_category()` 中。

### 测试策略

用模板参数化测试用例，同一套逻辑覆盖所有后端：

```cpp
template <typename ContextFactory>
void tls_echo_test() { /* ... */ }

TEST_CASE("openssl tls echo") { tls_echo_test<openssl_context_factory>(); }
TEST_CASE("schannel tls echo") { tls_echo_test<schannel_context_factory>(); }
```

### 实现阶段

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | 抽象接口与属性 API | ✅ 完成 |
| Phase 2 | Windows Schannel 后端：真实握手、`EncryptMessage` / `DecryptMessage`、服务端自签名证书、PEM 证书/私钥/CA bundle 加载 | ✅ 完成 |
| Phase 3 | OpenSSL 后端完善（跨平台一致性） | 🚧 已接入 CMake，接口为 stub |
| Phase 4 | 添加 mbedTLS 后端 | 🚧 stub |
| Phase 5 | 添加 macOS SecureTransport / Network.framework 后端 | 🚧 stub |

当前 `examples/netexec/https-server.cpp` 可在 Windows 上直接运行：`https://localhost:8443/` 能被 curl（`-k`）和浏览器打开（自签名证书会触发浏览器安全警告）。

若提供 `net::tls::certificate(...)` 与 `net::tls::private_key(...)`，则 https-server 会加载对应 PEM 文件并使用真实证书；客户端可通过 `net::tls::ca_bundle(...)` 指定自定义 CA 进行校验。

### 已知限制 / TODO

- **私钥格式**：当前 Schannel 后端仅支持 RSA 私钥（`BEGIN PRIVATE KEY` PKCS#8 与 `BEGIN RSA PRIVATE KEY`），暂不支持加密私钥、ECDSA 及其他非对称算法。
- **服务端默认证书**：未提供 `certificate` / `private_key` 时，Schannel 服务端会临时生成内存中的自签名证书以便本地开发。后续应改为：生产环境默认报错，仅在显式开启的开发模式下允许自动生成。

### 使用 C++ 生成开发证书（低优先级 / 待调研）

当前 `examples/netexec/generate_dev_certs.py` 用 Python `cryptography` 库生成根 CA + 叶子证书链。后续可考虑提供一个可选的 C++ 证书生成工具，作为 Python 脚本的替代方案。

#### 如何用 C++ 生成

实现路径有三种，按项目依赖策略选择其一：

1. **基于 OpenSSL C API**
   - 使用 `EVP_PKEY`、`RSA_generate_key_ex`、`X509`、`X509V3_EXT` 等 API 生成 RSA 密钥对、组装 DN/SAN、签名、导出 PEM。
   - 代码相对成熟，文档和示例丰富。
   - 示例流程：
     ```cpp
     EVP_PKEY* pkey = EVP_RSA_gen(2048);
     X509* cert = X509_new();
     X509_set_version(cert, 2); // v3
     ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
     X509_gmtime_adj(X509_get_notBefore(cert), 0);
     X509_gmtime_adj(X509_get_notAfter(cert), 31536000L);
     X509_set_pubkey(cert, pkey);
     X509_NAME_add_entry_by_txt(/* ... */);
     X509_add_ext(cert, X509V3_EXT_conf_nid(/* ... */), -1);
     X509_sign(cert, pkey, EVP_sha256());
     PEM_write_bio_PrivateKey(/* ... */);
     PEM_write_bio_X509(/* ... */);
     ```

2. **基于 Windows CNG / CryptoAPI**
   - 使用 `NCryptCreateKey` 生成 RSA 密钥，`CertCreateSelfSignCertificate` 创建自签证书，`CertStrToName` 编码 DN。
   - 纯 Windows 原生，不引入 OpenSSL。
   - 但只适用于 Windows 平台，跨平台需求下需要为 Linux/macOS 另写实现。

3. **基于 mbedTLS**
   - 使用 `mbedtls_pk_setup`、`mbedtls_rsa_gen_key`、`mbedtls_x509write_crt` 等 API。
   - 体积小、可裁剪，适合嵌入式或需要最小依赖的场景。
   - API 比 OpenSSL 更底层，代码量更大。

#### 利弊

| 方面 | C++ 生成 | Python `cryptography` |
|---|---|---|
| 代码量 | 大（几百行，需处理 ASN.1/X.509/PEM 细节） | 小（几十行） |
| 可读性/维护性 | 低，容易在扩展、编码、内存管理上出错 | 高，声明式 API |
| 跨平台一致性 | 需要处理平台差异或引入第三方库 | 同一份脚本跨平台 |
| 项目依赖 | 若编进项目，会增加库的链接依赖 | 仅开发时临时需要 Python |
| 与服务器耦合 | 容易诱使把生成逻辑放进服务器启动流程 | 生成与运行完全解耦 |
| 运行时生成 | 可行，但密钥管理风险高 | 不适合运行时生成 |

#### 意义 / 适用场景

- **无 Python 环境**：嵌入式设备或某些 CI 流水线只提供 C++ 工具链。
- **运行时自动初始化**：示例服务器启动时发现没有证书，自动造一张，实现“双击运行”。但生产环境不应如此，密钥应妥善保管。
- **完整 C++ 教学示例**：展示从密钥生成到 TLS 握手的完整链路。
- **构建系统集成**：把证书生成作为 CMake 自定义命令的一部分，不依赖外部脚本。

#### 推荐做法

如果未来要实现，建议：

1. **做成独立小工具**，例如 `tools/netexec-gen-dev-certs.cpp`，不要编进 `netexec` 库本体。
2. **不要运行时生成服务器证书**，保留“启动时加载 PEM 文件”的接口。
3. **保留 Python 脚本作为首选方案**，C++ 工具作为无 Python 环境下的备选。
4. **优先基于 OpenSSL 实现**，因为 OpenSSL 已经是计划中的 Linux 默认 TLS 后端，可以复用依赖；Windows 下若不想引入 OpenSSL，可单独用 CNG 实现，但维护成本更高。

优先级：**低**。当前 Python 脚本已能满足本地开发需求。

### API 影响
```cpp
net::tls::preconnection pre(ex::env{
    net::hostname("example.com"),
    net::port(443)
    // secure 默认 true
});
auto conn = co_await net::tls::async_initiate(pre, ctx); // TCP 连接 + TLS 握手
```

---

## 2. Hostname 解析（高优先级）

### 目标
`net::tls::preconnection` 支持 `hostname`，`net::tls::async_initiate` 自动解析地址。

### 方案
- `net::ip::tcp::async_resolve(io_context&, const std::string& hostname, std::uint16_t port)` 提供异步解析
- 内部使用系统 `getaddrinfo` 或自定义异步 DNS
- `net::tls::async_initiate` 内部先 `co_await net::ip::tcp::async_resolve(ctx, hostname, port)`，再连接

### API 影响
```cpp
net::tls::preconnection pre(ex::env{
    net::hostname("example.com"),
    net::port(443)
});
auto endpoints = co_await net::ip::tcp::async_resolve(ctx, "example.com", 443);
auto conn = co_await net::tls::async_initiate(pre, ctx);
```

---

## 3. Message 边界与 Framer（高优先级）

### 目标
`net::tls::async_receive` 按 message 边界返回，而不是读到 EOF。

### 方案
- 在 `net::tls::preconnection` 中配置 framer 属性
- 常见 framer：
  - 固定长度（fixed length）
  - 长度前缀（length-prefixed）
  - 行分隔（newline-delimited）
  - HTTP/1.1、HTTP/2 等应用层 framer
- `async_send` 在发送前加帧头
- `async_receive` 循环 `tcp::async_receive` 直到一个完整 frame

### API 影响
```cpp
net::tls::preconnection pre(ex::env{
    net::hostname("example.com"),
    net::port(12345),
    net::framer(net::framer_type::length_prefixed)
});
```

---

## 4. UDP 支持（中优先级）

### 目标
支持 datagram 传输。

### 方案
- 新增 `netexec::net::ip::udp::socket`
- 新增 CPO：`async_send_to`, `async_receive_from`
- UDP socket 复用底层 `context_base::send_operation` / `receive_operation`
- tls 层 API 对 UDP 透明：`async_send` / `async_receive` 自动选择 datagram 语义

### API 影响
```cpp
namespace udp = netexec::net::ip::udp;
udp::socket sock(ctx);
co_await udp::async_send_to(sock, buf, ep);
auto [n, sender] = co_await udp::async_receive_from(sock, buf);
```

---

## 5. Acceptor 与 Sequence Sender（中优先级）

### 目标
提供更自然的 accept loop 抽象。

### 方案
- 当前：手动 `while (true) { co_await tcp::async_accept(acc); }`
- 未来：考虑 `net::tls::async_accept(acc)` 返回 sequence sender，可配合 `stdexec::sequence` 使用
- 或提供 `net::tls::async_listen(pre, ctx) | stdexec::transform_each(handler)`

### 参考
Dietmar Kühl 提到 TAPS 中 `initiate` / `listen` 未来可能演化为 sequence sender。

---

## 6. io_uring Backend 完善（中优先级）

### 目标
Linux 下默认使用 io_uring 替代 poll。

### 方案
- 完善现有 `uring_context` 实现
- 支持多 buffer receive、registered buffers、SQE polling 等优化
- CMake 选项切换 backend

---

## 7. 性能优化（中优先级）

### 方向
- 减少 operation state 的内存分配
- 支持 registered buffers / buffer rings
- 减少跨线程同步（io_context 线程亲和性）
- sender pipeline 内联优化

---

## 8. Cancellation 与错误处理增强（中优先级）

### 方向
- 统一的 `std::error_code` 路径
- 更细粒度的 cancel 语义
- per-operation timeout（结合 timer CPO）

---

## 9. 与 stdexec 更深集成（低优先级）

### 方向
- 利用 `stdexec::sequence` 处理 accept streams
- 利用 `stdexec::let_async_scope` / `counting_scope` 作为推荐并发模型
- 探索 sender 与 coroutine 的最佳实践

---

## 10. 文档与测试（持续推进）

### 方向
- 为每个 tls 层 CPO 增加单元测试
- 跨平台 CI（Windows IOCP / Linux poll / Linux io_uring）
- 更多示例：HTTP client/server、TLS echo、UDP echo
- API reference 文档自动生成

---

## 11. Rendezvous / P2P 支持（低优先级）

### 目标
实现 P3482R1 中 TAPS 的第三种连接方式 `rendezvous`，用于点对点连接。

### TAPS 语义
与 `initiate`（客户端主动连接）和 `listen`（服务端被动接受）并列，`rendezvous` 用于双方对等协商建立连接。典型场景：

- WebRTC / ICE
- NAT 穿透（STUN/TURN）
- 对等节点直接通信
- 同时打开（simultaneous open）

### 当前状态
netexec 中 `rendezvous` 是 stub，调用直接抛 `not implemented`。

### 实现挑战
1. **候选端点收集**：需要同时解析本地和远程端点，收集所有可能的 IP/port 候选。
2. **信令协调**：P2P 双方通常需要第三方信令通道交换候选地址。
3. **连通性检查**：ICE 需要对候选对进行 STUN binding request 测试。
4. **传输基础**：ICE 通常基于 UDP，所以 rendezvous 很可能依赖 UDP 支持先完成。

### 推荐方案
长期保持 stub，不作为近期目标。如果要实现，建议顺序：

1. 先实现 UDP 支持。
2. 设计 `rendezvous` 的 `net::tls::preconnection` 语义（local endpoint + remote endpoint + STUN/TURN server）。
3. 实现最小 ICE-lite 客户端/服务器。
4. 在 UDP socket 上封装 DTLS（如果需要安全 P2P）。

### API 形态（远期）
```cpp
auto pre = ex::env{
    net::hostname("peer.example.com"),
    net::port(0),                    // 本地临时端口
    net::stun_server("stun.example.com"),
    net::turn_server("turn.example.com")
};
auto conn = co_await net::tls::async_rendezvous(pre, ctx);
```

---

## 12. Transport 属性支持（低优先级）

### 目标
实现 P3482R1 中定义的大量传输属性，如 `reliability`、`preserve_msg_boundaries`、`congestion_control`、`multipath`、`direction` 等。

### 当前状态
netexec 当前 `net::tls::preconnection` 主要关注端点属性（`hostname`、`port`、`ip_address`）和安全属性（`net::tls::secure`、`net::tls::certificate`、`net::tls::private_key`、`net::tls::ca_bundle`），几乎没有传输属性。

### 为什么优先级低
netexec 目前只支持 TCP。而 TCP 的传输语义基本上是固定的：

| 属性 | TCP 实际值 |
|---|---|
| `reliability` | require |
| `preserve_order` | require |
| `preserve_msg_boundaries` | none |
| `congestion_control` | require |
| `direction` | bidirectional |
| `multipath` | disabled |
| `zero_rtt_msg` | prohibit |

在只有 TCP 时提供这些属性没有实际选择空间，只会变成文档上的摆设。

### 未来方向
等多传输后端（UDP、QUIC、SCTP 等）出现后，这些属性才有意义：

1. 在 `net::tls::preconnection` 中增加 `transport_props` 查询接口。
2. 根据属性选择合适的传输实现。
3. 与 framer 结合：不同传输默认使用不同 framer。

### 涉及属性清单
P3482R1 中定义的传输属性包括：

- `interface_preference`
- `reliability`
- `preserve_msg_boundaries`
- `per_msg_reliability`
- `preserve_order`
- `zero_rtt_msg`
- `multistreaming`
- `full_checksum_send` / `full_checksum_recv`
- `congestion_control`
- `keep_alive`
- `use_temp_local_address`
- `multipath`
- `advertises_alt_addr`
- `direction`
- `soft_error_notify`
- `active_read_before_send`

这些属性统一用 `transport_preference` 枚举表示：`require`、`prefer`、`none`、`avoid`、`prohibit`。

### API 形态（远期）
```cpp
auto pre = ex::env{
    net::hostname("example.com"),
    net::port(443),
    net::reliability(net::transport_preference::require),
    net::preserve_msg_boundaries(net::transport_preference::prefer),
    net::congestion_control(net::transport_preference::require)
};
```

---

## 建议实现顺序

1. **Phase A**：hostname 解析 + 完善 `async_initiate`/`async_listen`
2. **Phase B**：framer 基础框架（fixed / length-prefixed）+ 改进 `async_receive`
3. **Phase C**：OpenSSL 后端完善（跨平台 TLS 一致性）
4. **Phase D**：UDP 支持
5. **Phase E**：sequence sender / accept loop 抽象
6. **Phase F**：io_uring 完善 + 性能优化
7. **Phase G（待调研/低优先级）**：可选的 C++ 开发证书生成工具，作为 Python 脚本的备选方案
8. **Phase H（大工程量/低优先级）**：Windows 下 OpenSSL 可选 TLS 后端（依赖构建、Windows 证书 store 集成、DLL 分发）
9. **Phase I（低优先级）**：Rendezvous / P2P 支持（依赖 UDP、ICE/STUN/TURN）
10. **Phase J（低优先级）**：Transport 属性支持（等 UDP/QUIC 等多传输后端就位后才有意义）
