# schannel TLS 客户端六个缺陷：握手后连接无法工作的问题与修复

## 摘要

netexec 的 schannel 封装（`netexec::net::tls`）在客户端场景下，握手可以完成，但**任何握手之后的通信都无法进行**：服务端发送 TLS 会话票据（NewSessionTicket）后连接陷入不可恢复的状态——数据无法解密、接收协程挂死、或被当作"永不完成的重协商"。经系统排查，共定位并修复六个真实缺陷；六个修复全部落地后，客户端行为与 Windows 自带 curl（同为 schannel 后端）完全一致：本地 TLS 测试床 HTTP 200，经 SOCKS5 代理访问 GitHub raw 下载 120/120 个文件与参考实现字节级一致。

涉及文件：

| 文件 | 缺陷 |
|---|---|
| `include/netexec/net/tls/__detail/schannel_tls.hpp` | #1 输入缓冲陈旧字节、#3 消费计量错误、#4 RENEGOTIATE 未处理、#6 凭据 API、insecure 属性 |
| `include/netexec/net/tls/operations.hpp` | #2 Finished 未发送、#5 接收循环 EOF 处理 |
| `include/netexec/net/tls/__detail/tls_session_base.hpp` | #5 `eof_received()` 虚接口 |
| `include/netexec/net/tls/context.hpp` / `preconnection.hpp` | `tls::insecure` 属性接线 |

六者环环相扣：每修复一层，下一层随即暴露。下文按因果链排序。

---

## 缺陷 #1（元凶）：`SEC_E_OK` 后握手输入缓冲从不清理陈旧字节

**位置**：`schannel_tls_session::client_handshake_step()` 的 `SEC_E_OK` 分支

### 症状与诊断

插桩打印显示：

```
[isc] st=0x0 in=1252 consumed_now=1252 out0=80
[isc] after-consume in=1252        ← ISC 声称成功，input_buffer_ 却一字未动
```

`InitializeSecurityContext` 返回 `SEC_E_OK`（0x0）且产出了 80B 输出（TLS 1.3 客户端 Finished），但 1252B 的服务端航班在 `input_buffer_` 里原封未动。

### 根因

SSPI 契约：ISC 返回 `SEC_E_OK` 且**未**在第二个输入缓冲报告 `SECBUFFER_EXTRA` 时，表示输入已全部被 schannel 消费。原 `consume_handshake_input()` 只在报告 EXTRA 时擦除已消费前缀，未报告 EXTRA 时**不做任何事**——陈旧字节永久滞留。后续任何 ISC 调用（尤其是会话票据触发的重协商泵）把这些已消费字节再次当作新的握手输入解析，schannel 于是产出一个约 217B 的伪 ClientHello；接收循环把它发到 socket，服务端视其为非法记录并断链。

这是"永不完成的重协商"表象的真正源头：**喂给 ISC 的输入本身已经是毒的**。

### 修复

```cpp
if (status == SEC_E_OK) {
    this->context_initialized_ = true;
    // SEC_E_OK without a reported SECBUFFER_EXTRA means schannel
    // consumed the entire input; keep the buffer in sync or stale
    // bytes poison every later call.
    if (inbuffers[1].BufferType != SECBUFFER_EXTRA) {
        this->input_buffer_.clear();
    }
    // （EXTRA 残余 = 与握手尾批同至的应用数据，转移到 decrypt_buffer_）
    ...
}
```

---

## 缺陷 #2：TLS 1.3 客户端 Finished 从未发送

**位置**：`operations.hpp` 中 `run_handshake()`

### 症状与诊断

```
[isc] st=0x0 in=1252 out0=80       ← 完成调用产出 80B
[hs] complete                       ← run_handshake 直接返回
```

服务器迟迟不响应——它在等握手的最后一帧。

### 根因

TLS 1.3 中客户端 Finished 是**最后一次 ISC 调用的输出**，随 `SEC_E_OK` 一起产出在 `output_buffer_`。`run_handshake()` 在 `handshake_step()` 返回 complete 后直接 `co_return`，这段输出从未写入 socket。

### 修复

```cpp
if (complete) {
    // TLS 1.3: the completing call also produces our Finished; send
    // it or the peer never sees the handshake as done.
    auto out2 = session.outgoing_data();
    while (!out2.empty()) {
        const auto* out_bytes = reinterpret_cast<const char*>(out2.data());
        std::size_t sent2 = 0;
        while (sent2 < out2.size()) {
            auto n2 = co_await ip::tcp::async_send(
                sock, net::buffer(out_bytes + sent2, out2.size() - sent2));
            sent2 += n2;
        }
        session.consume_outgoing(out2.size());
        out2 = session.outgoing_data();
    }
    co_return;
}
```

---

## 缺陷 #3：`DecryptMessage` 消费计量误用 `buffers[0]`

**位置**：`schannel_tls_session::decrypt()` 的 `SEC_E_OK` 分支

### 症状与诊断

```
[dmsg] st=0x0 in_buf=239           ← 首个记录解密成功
[dmsg] st=0x80090318 in_buf=234    ← 只消费了 5 字节（= 记录头长度）
```

239B 缓冲含一条完整应用数据记录（207B）+ close_notify（32B），却只前进了 5 字节，随后 INCOMPLETE_MESSAGE 死循环。

### 根因

`DecryptMessage` 调用后，schannel 把 `buffers[0]`（原为输入 `SECBUFFER_DATA`）**替换为 `SECBUFFER_STREAM_HEADER`**——其 `cbBuffer` 是记录头长度（TLS 1.2 GCM 为 13，本例为 5），**不是**记录总长。原代码：

```cpp
// 原代码（错误——注释与语义均错）：
// Buffer 0 holds the whole encrypted record that was processed.
const std::size_t consumed = buffers[0].cbBuffer;   // = 记录头长度！
```

每条记录只擦掉头部，剩余密文成为下一轮的垃圾输入。

### 修复

消费量 = 缓冲总长 − `SECBUFFER_EXTRA` 尾部：

```cpp
std::size_t consumedAll = this->decrypt_buffer_.size();
for (const auto& buf : buffers) {
    if (buf.BufferType == SECBUFFER_EXTRA && buf.cbBuffer > 0) {
        consumedAll = this->decrypt_buffer_.size() - buf.cbBuffer;
        break;
    }
}
if (consumedAll >= this->decrypt_buffer_.size()) {
    this->decrypt_buffer_.clear();
} else {
    this->decrypt_buffer_.erase(
        this->decrypt_buffer_.begin(),
        this->decrypt_buffer_.begin() +
            static_cast<std::ptrdiff_t>(consumedAll));
}
```

---

## 缺陷 #4：`SEC_I_RENEGOTIATE`（会话票据）完全没有处理

**位置**：`decrypt()` 整体重写为循环结构；新增 `pump_renegotiation()`

### 症状

服务端发送会话票据时 `DecryptMessage` 返回 `SEC_I_RENEGOTIATE`，原代码落入 `status != SEC_E_OK` 分支直接抛错。TLS 1.3 会话票据与 TLS 1.2 NewSessionTicket 均触发此路径（对后者的连接实测记录类型为明文 `16 03 03 ...` 握手记录）。

### 修复（三个组成部分）

① 会话新增状态：

```cpp
bool renegotiating_ = false;                    // 重协商进行中
std::vector<unsigned char> pending_plaintext_;  // 同批抢救出的应用明文
```

② `decrypt()` 重写为 `for(;;)` 循环，新增 RENEGOTIATE 分支——遵循 schannel 文档的标准模式：

- 抢救同批 `SECBUFFER_DATA` 明文到 `pending_plaintext_`（优先于一切返回）；
- 未消费的 `SECBUFFER_EXTRA` 尾部移作 ISC 输入（**不是**回发 DecryptMessage 给的 token——实测回发会立即杀死连接）；
- **当场**只泵已缓冲的输入。这一步至关重要：若等下一批网络数据到达后再泵，后到的应用数据会被混入并当作握手输入消费掉（有诊断记录：`[pump] consumed=934` 把 HTTP 响应整块吃掉）；
- 泵完成后 `continue`，循环自然处理剩余密文——多张票据连续到达（实测服务端常发两张）亦无问题。

```cpp
if (status == SEC_I_RENEGOTIATE) {
    for (const auto& buf : buffers) {
        if (buf.BufferType == SECBUFFER_DATA && buf.cbBuffer > 0) {
            const auto* p = static_cast<const unsigned char*>(buf.pvBuffer);
            this->pending_plaintext_.insert(
                this->pending_plaintext_.end(), p, p + buf.cbBuffer);
        }
    }
    std::size_t consumed = this->decrypt_buffer_.size();
    for (const auto& buf : buffers) {
        if (buf.BufferType == SECBUFFER_EXTRA && buf.cbBuffer > 0) {
            consumed = this->decrypt_buffer_.size() - buf.cbBuffer;
            break;
        }
    }
    /* 擦除已消费前缀；余下移入 input_buffer_ */
    this->renegotiating_ = true;
    const bool reneg_done = this->pump_renegotiation(ec);
    /* 先交还 pending_plaintext_；reneg_done 则 continue 处理剩余密文 */
```

③ 新增泵方法，复用既有 `client_handshake_step()`：

```cpp
auto pump_renegotiation(std::error_code& ec) -> bool {
    int empty_retries = 0;
    while (true) {
        std::error_code hec;
        const auto out_before = this->output_buffer_.size();
        const bool complete = this->client_handshake_step(hec);
        if (hec) { ec = hec; return true; }
        if (complete) { this->renegotiating_ = false; return true; }
        if (this->input_buffer_.empty()) {
            const bool produced = this->output_buffer_.size() > out_before;
            if (produced && empty_retries < 4) {
                ++empty_retries;   // 有产出时再调一次空输入 ISC 令状态机收尾
                continue;
            }
            ec.clear();
            return false;          // 真正需要更多网络数据
        }
        empty_retries = 0;
    }
}
```

`decrypt()` 入口同步配合：`renegotiating_` 为真时，新到的网络数据进入 `input_buffer_`（而非 `decrypt_buffer_`）；`pending_plaintext_` 非空时优先交还。

### 附带修正：ISC 调用契约

`client_handshake_step()` 的 `InitializeSecurityContext` 调用同步修正为与 curl（`lib/vtls/schannel.c`）逐字段一致的 SSPI 契约：

- 输出缓冲 **TOKEN + ALERT + EMPTY 三个**（原仅一个 TOKEN）；
- 续呼时 `phNewContext` 传 **NULL**（原重复传 `&context_`；首次调用仍传 `&context_`）；
- 输出收集扫描全部三个缓冲中的 TOKEN 类型。

---

## 缺陷 #5：close_notify 语义错位，代理隧道上接收协程永久挂死

**位置**：`decrypt()` 的 `CONTEXT_EXPIRED` 分支 + `tls_session_base.hpp` + `receive_some()`

### 症状与诊断

```
[dbg-rx] -> written=0 ec=The operation completed successfully.   （反复出现）
[dmsg] st=0x80090317                                               （SEC_E_CONTEXT_EXPIRED）
```

对端已发 close_notify，但接收循环继续等待下一批数据直到超时。经查 Windows SDK `winerror.h`：`0x80090317 = SEC_E_CONTEXT_EXPIRED`（优雅 EOF，非错误）。

### 根因

原代码对 `CONTEXT_EXPIRED` 返回 `ec.clear()` + 0 字节——"无错误、无数据"。接收循环将其理解为"继续等"。而 **SOCKS5 隧道不转发 TCP FIN**，`async_receive` 永不返回 0，协程挂死。EOF 必须成为显式状态而非"0 字节"约定。

### 修复（三处配合）

```cpp
// tls_session_base.hpp —— 会话基类新增虚接口
virtual auto eof_received() const -> bool { return false; }

// schannel_tls.hpp —— decrypt() 的 CONTEXT_EXPIRED 分支
this->decrypt_buffer_.clear();
this->eof_received_ = true;      // 新增成员 bool eof_received_{false}
ec.clear();
return;
// ...
auto eof_received() const -> bool override { return this->eof_received_; }

// operations.hpp —— receive_some() 两处 decrypt 调用之后
co_await __detail::flush_session_outgoing(strm);
if (session->eof_received()) {
    co_return 0;  // close_notify: graceful EOF（对 close_notify 前最后一批
                  // 数据则先交还 written 再返回 0）
}
```

---

## 缺陷 #6：凭据 API 使用旧式 `SCHANNEL_CRED`，票据记录处理有缺陷

**位置**：`schannel_tls_context::ensure_client_credentials()`

### 证据链

修复前五项后，票据仍触发多轮无法本地完成的重协商；而 Windows 自带 curl（schannel 后端）对**同一服务器**的 `--trace-config all` 转储显示：`renegotiating → renegotiated` 在同一毫秒内完成，**零网络收发**。逐字段对比 curl 的 `schannel.c` 后，唯一未复刻的差异是凭据结构：curl 在现代 Windows 使用 `SCH_CREDENTIALS`，netexec 使用旧式 `SCHANNEL_CRED`。切换后（配合前五项修复）票据经本地 ISC 即刻消化，行为与 curl 完全一致。

### 修复

```cpp
auto ensure_client_credentials() -> std::error_code {
    if (this->client_creds_) return {};

    // Modern credential API (SCH_CREDENTIALS), matching curl's schannel
    // backend: with the legacy SCHANNEL_CRED structure, schannel
    // surfaces TLS session-ticket records as SEC_I_RENEGOTIATE
    // renegotiations that never complete; SCH_CREDENTIALS processes
    // them locally.
    SCH_CREDENTIALS credentials{};
    credentials.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials.dwFlags   = SCH_CRED_NO_DEFAULT_CREDS |
                          SCH_CRED_IGNORE_NO_REVOCATION_CHECK |
                          SCH_CRED_IGNORE_REVOCATION_OFFLINE;
    if (this->ca_store_ != nullptr || this->insecure_) {
        credentials.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION;
    }
    if (this->insecure_) {
        credentials.dwFlags |= SCH_CRED_NO_SERVERNAME_CHECK;
    }
    auto creds = std::make_shared<credential_set>();
    const auto status = ::AcquireCredentialsHandleA(
        nullptr, const_cast<SEC_CHAR*>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND,
        nullptr, &credentials, nullptr, nullptr, &creds->handle, nullptr);
    if (status != SEC_E_OK) return schannel::make_error_code(status);
    this->client_creds_ = std::move(creds);
    return {};
}
```

---

## 附送：`tls::insecure` 属性（curl `-k` 等价物）

自签证书本地测试床必需，亦是各 TLS 库的标配逃生门。五处接线：

```cpp
// context.hpp —— 属性定义
struct insecure : stdexec::prop<security_props::insecure_t, bool> {
    explicit insecure(bool value = true)
        : prop{security_props::insecure_tag, value} {}
};
```

`preconnection.hpp` 读取并入 `make_context()`（经 `context_base::set_insecure()` 虚设值），`schannel_tls.hpp` 在两处生效：凭据加 `SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_SERVERNAME_CHECK`；`client_handshake_step` 的显式链校验改为 `if (this->manual_validation_ && !this->insecure_)` 跳过。

---

## 排查方法论

1. **降变量**：GitHub + SOCKS5 代理变量过多，搭建本地 Python TLS 测试床（自签证书，`ssl.PROTOCOL_TLS_SERVER`），迭代周期从约 5 分钟压缩到 90 秒且行为完全可复现。
2. **找权威参照**：Windows 自带 curl 即 schannel 客户端且在本环境完全正常。`curl -v` 证明"重协商可以零往返瞬间完成"；`curl --trace-config all` 证明期间**零 TCP 收发**；再取 curl 的 `schannel.c` 源码逐字段对比 ISC 调用契约与凭据构造。
3. **分层插桩**：在 `decrypt()`、`client_handshake_step()`、接收循环三层打印 HRESULT 十六进制与 SecBuffer 布局。如 `[dbg-ren] type=7 size=13 / type=1 size=0 / type=6 size=229 / type=5 size=242 head=16 3 3 0 ba` 这一条转储同时暴露了 STREAM_HEADER 语义（缺陷 #3）与 EXTRA=全部未消费的事实（缺陷 #1/#4 的输入组成）。
4. **查 SDK 头文件定案**：`winerror.h` 查证 `0x80090317`；`sspi.h` 查证 ISC 标志位真值（排除一个"误开 MUTUAL_AUTH"假说）。

## 验证

| 项目 | 结果 |
|---|---|
| 本地 TLS 测试床（自签证书，`tls::insecure`） | 3/3 次运行 HTTP 200 + 响应体 |
| GitHub raw 经 SOCKS5 下载（对照参考缓存逐字节比对） | **120/120 一致，0 失败** |
| 连续多票据 / 响应与票据合批 / close_notify 收尾 | 全部正常 |

## 遗留（未修，已规避）

`async_resolve` 的 blocking-pool 调度对 IP 字面量存在间歇性挂起（getaddrinfo 被排入 blocking scheduler 后协程永不恢复；系统层 `getaddrinfo("127.0.0.1")` 本身即时返回）。调用方（PokeB2DX importer）目前对数字主机直接构造 endpoint 绕开。建议后续单独排查 `io_context` 的 blocking scheduler 路径。

## 参考资料

- Microsoft Docs — [DecryptMessage (Schannel)](https://learn.microsoft.com/windows/win32/api/sspi/nf-sspi-decryptmessage)：`SEC_I_RENEGOTIATE` 处理模式与输出缓冲语义
- Microsoft Docs — [InitializeSecurityContext (Schannel)](https://learn.microsoft.com/windows/win32/api/sspi/nf-sspi-initializesecuritycontexta)：输入消费 / SECBUFFER_EXTRA 语义
- curl 源码 — [`lib/vtls/schannel.c`](https://github.com/curl/curl/blob/master/lib/vtls/schannel.c)：renegotiation 驱动（`schannel_recv_renegotiate`）、`SCH_CREDENTIALS` 凭据构造、ISC 调用契约
- Windows SDK — `winerror.h` / `sspi.h`：HRESULT 与标志位定义
