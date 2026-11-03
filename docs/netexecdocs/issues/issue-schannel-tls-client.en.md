# Six defects in the schannel TLS client: post-handshake traffic never worked

## Summary

With netexec's schannel wrapper (`netexec::net::tls`), a client handshake completes — but **nothing after the handshake works**: once the server sends TLS session tickets (NewSessionTicket) the connection enters an unrecoverable state (data fails to decrypt, the receive coroutine stalls, or an "endless renegotiation" consumes the application data). Systematic debugging located six genuine defects; with all six fixed, the client behaves identically to Windows' bundled curl (also a schannel client): HTTP 200 against a local TLS testbed, and 120/120 byte-identical file downloads from GitHub raw through a SOCKS5 proxy against a reference implementation.

Files touched:

| File | Defects |
|---|---|
| `include/netexec/net/tls/__detail/schannel_tls.hpp` | #1 stale handshake input, #3 wrong consumed accounting, #4 unhandled RENEGOTIATE, #6 credentials API, insecure property |
| `include/netexec/net/tls/operations.hpp` | #2 Finished never sent, #5 receive-loop EOF handling |
| `include/netexec/net/tls/__detail/tls_session_base.hpp` | #5 `eof_received()` virtual |
| `include/netexec/net/tls/context.hpp` / `preconnection.hpp` | `tls::insecure` property wiring |

The six are layered: fixing each one exposed the next. Listed below in causal order.

---

## Defect #1 (root cause): handshake input buffer never cleared of stale bytes

**Location**: `schannel_tls_session::client_handshake_step()`, `SEC_E_OK` branch

### Symptom & diagnosis

Instrumentation showed:

```
[isc] st=0x0 in=1252 consumed_now=1252 out0=80
[isc] after-consume in=1252        ← ISC reports success; input_buffer_ untouched
```

`InitializeSecurityContext` returned `SEC_E_OK` (0x0) and produced 80B of output (the TLS 1.3 client Finished) — yet the 1252B server flight sat in `input_buffer_` completely intact.

### Root cause

SSPI contract: when ISC returns `SEC_E_OK` **without** reporting `SECBUFFER_EXTRA` in the second input buffer, schannel consumed the entire input. The original `consume_handshake_input()` only erased the consumed prefix when EXTRA was reported and did **nothing** otherwise — stale bytes stayed forever. Every later ISC call (in particular the renegotiation pump triggered by session tickets) re-fed those already-consumed bytes as fresh handshake input; schannel then produced a bogus ~217B ClientHello, the receive loop sent it, and the server killed the connection for an illegal record.

This was the true source of the "never-completing renegotiation" symptom: **the input being fed to ISC was already poisoned.**

### Fix

```cpp
if (status == SEC_E_OK) {
    this->context_initialized_ = true;
    // SEC_E_OK without a reported SECBUFFER_EXTRA means schannel
    // consumed the entire input; keep the buffer in sync or stale
    // bytes poison every later call.
    if (inbuffers[1].BufferType != SECBUFFER_EXTRA) {
        this->input_buffer_.clear();
    }
    // (EXTRA remainder = application data batched with the handshake
    // tail; moved to decrypt_buffer_ for the decrypt path)
    ...
}
```

---

## Defect #2: the TLS 1.3 client Finished was never sent

**Location**: `run_handshake()` in `operations.hpp`

### Symptom & diagnosis

```
[isc] st=0x0 in=1252 out0=80       ← completing call produces 80B
[hs] complete                       ← run_handshake returns immediately
```

The server never responds — it is waiting for the final handshake flight.

### Root cause

In TLS 1.3 the client Finished is the output of the **last** ISC call, produced alongside `SEC_E_OK` into `output_buffer_`. `run_handshake()` returned as soon as `handshake_step()` reported completion; that output never reached the socket.

### Fix

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

## Defect #3: DecryptMessage consumed accounting used `buffers[0]`

**Location**: `schannel_tls_session::decrypt()`, `SEC_E_OK` branch

### Symptom & diagnosis

```
[dmsg] st=0x0 in_buf=239           ← first record decrypts fine
[dmsg] st=0x80090318 in_buf=234    ← only 5 bytes consumed (= header length)
```

The 239B buffer held one complete application record (207B) + close_notify (32B); only 5 bytes advanced, then an INCOMPLETE_MESSAGE loop.

### Root cause

After `DecryptMessage`, schannel **replaces** `buffers[0]` (the input `SECBUFFER_DATA`) with `SECBUFFER_STREAM_HEADER` — whose `cbBuffer` is the header length (13 for TLS 1.2 GCM; 5 here), **not** the record length. The original code:

```cpp
// Original (wrong — comment and semantics):
// Buffer 0 holds the whole encrypted record that was processed.
const std::size_t consumed = buffers[0].cbBuffer;   // = header length!
```

Each record had only its header erased; the remaining ciphertext became garbage input for the next round.

### Fix

Consumed = total − `SECBUFFER_EXTRA` tail:

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

## Defect #4: `SEC_I_RENEGOTIATE` (session tickets) not handled at all

**Location**: `decrypt()` rewritten as a loop; new `pump_renegotiation()`

### Symptom

When the server sends session tickets, `DecryptMessage` returns `SEC_I_RENEGOTIATE`; the original code hit the `status != SEC_E_OK` branch and threw. Both TLS 1.3 tickets and TLS 1.2 NewSessionTicket trigger this path (the latter observed on the wire as a plaintext `16 03 03 ...` handshake record).

### Fix (three parts)

① New session state:

```cpp
bool renegotiating_ = false;                    // renegotiation in flight
std::vector<unsigned char> pending_plaintext_;  // application plaintext salvaged in the same call
```

② `decrypt()` becomes a `for(;;)` loop with a RENEGOTIATE branch following the documented schannel pattern:

- Salvage any `SECBUFFER_DATA` plaintext of the same call into `pending_plaintext_` (returned ahead of everything);
- Move the unconsumed `SECBUFFER_EXTRA` tail into the ISC input (**never** echo the token DecryptMessage returned — echoing kills the connection, verified empirically);
- Pump **immediately**, with only what is already buffered. This is essential: if the pump waits for the next network chunk, freshly arrived application data gets mixed in and consumed as handshake input (diagnostic on record: `[pump] consumed=934` swallowed an entire HTTP response);
- On completion `continue` — the loop naturally processes the remaining ciphertext, so consecutive tickets (servers commonly send two) are fine.

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
    /* erase consumed prefix; move remainder into input_buffer_ */
    this->renegotiating_ = true;
    const bool reneg_done = this->pump_renegotiation(ec);
    /* hand back pending_plaintext_ first; on reneg_done, continue with leftover ciphertext */
```

③ The pump reuses the existing `client_handshake_step()`:

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
                ++empty_retries;   // produced output: one more empty-input ISC
                                   // lets the state machine finalize locally
                continue;
            }
            ec.clear();
            return false;          // genuinely needs more wire data
        }
        empty_retries = 0;
    }
}
```

The `decrypt()` entry cooperates: while `renegotiating_` is set, incoming wire data goes to `input_buffer_` (not `decrypt_buffer_`); non-empty `pending_plaintext_` is handed out first.

### Incidental fix: the ISC call contract

`client_handshake_step()`'s `InitializeSecurityContext` call was also aligned field-by-field with curl's (`lib/vtls/schannel.c`):

- Output buffers are **TOKEN + ALERT + EMPTY** (three; originally one TOKEN);
- Continuation calls pass **NULL** as `phNewContext` (originally `&context_` again; the first call still passes `&context_`);
- Output collection scans all three buffers for TOKEN type.

---

## Defect #5: close_notify semantics broken — receive coroutine stalls forever on proxied sockets

**Location**: `decrypt()` CONTEXT_EXPIRED branch + `tls_session_base.hpp` + `receive_some()`

### Symptom & diagnosis

```
[dbg-rx] -> written=0 ec=The operation completed successfully.   (repeats)
[dmsg] st=0x80090317                                              (SEC_E_CONTEXT_EXPIRED)
```

The peer sent close_notify but the receive loop kept waiting until timeout. Windows SDK `winerror.h` confirms: `0x80090317 = SEC_E_CONTEXT_EXPIRED` — graceful EOF, not an error.

### Root cause

The original code returned `ec.clear()` + zero bytes for CONTEXT_EXPIRED — "no error, no data". The receive loop read that as "keep waiting". A **SOCKS5 tunnel does not forward the TCP FIN**, so `async_receive` never returns 0 and the coroutine hangs. EOF must be an explicit state, not a zero-byte convention.

### Fix (three cooperating pieces)

```cpp
// tls_session_base.hpp — new virtual on the session base
virtual auto eof_received() const -> bool { return false; }

// schannel_tls.hpp — decrypt()'s CONTEXT_EXPIRED branch
this->decrypt_buffer_.clear();
this->eof_received_ = true;      // new member bool eof_received_{false}
ec.clear();
return;
// ...
auto eof_received() const -> bool override { return this->eof_received_; }

// operations.hpp — after both decrypt calls in receive_some()
co_await __detail::flush_session_outgoing(strm);
if (session->eof_received()) {
    co_return 0;  // close_notify: graceful EOF (data preceding it is
                  // returned first as `written`)
}
```

---

## Defect #6: credentials used the legacy SCHANNEL_CRED API, which mishandles ticket records

**Location**: `schannel_tls_context::ensure_client_credentials()`

### Evidence chain

With the first five fixed, tickets still drove multi-round renegotiations that could not complete locally; Windows' bundled curl (schannel backend) against the **same server** showed — via `--trace-config all` — `renegotiating → renegotiated` within the same millisecond, **zero network I/O**. Field-by-field comparison against curl's `schannel.c` left exactly one unreplicated difference: curl uses `SCH_CREDENTIALS` on modern Windows; netexec used the 1998-era `SCHANNEL_CRED`. After the switch (with the five fixes in place) tickets are digested by a purely local ISC call — identical to curl.

### Fix

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

## Bonus: the `tls::insecure` property (curl `-k` equivalent)

Required for self-signed local testbeds and a standard escape hatch in any TLS library. Five wiring points:

```cpp
// context.hpp — the property
struct insecure : stdexec::prop<security_props::insecure_t, bool> {
    explicit insecure(bool value = true)
        : prop{security_props::insecure_tag, value} {}
};
```

`preconnection.hpp` queries it and `make_context()` applies it via `context_base::set_insecure()`; `schannel_tls.hpp` honors it in two places: credentials gain `SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_SERVERNAME_CHECK`, and the explicit chain validation in `client_handshake_step` becomes `if (this->manual_validation_ && !this->insecure_)`.

---

## Debugging methodology

1. **Reduce variables**: GitHub + SOCKS5 carried too many unknowns; a local Python TLS testbed (self-signed cert, `ssl.PROTOCOL_TLS_SERVER`) cut iteration from ~5 minutes to 90 seconds with fully reproducible behavior.
2. **Find the authoritative reference**: Windows' bundled curl is a schannel client that works perfectly in this environment. `curl -v` proved "renegotiation can complete instantly with zero round trips"; `curl --trace-config all` proved **zero TCP I/O** during it; curl's `schannel.c` source provided the ISC call contract and credential construction to diff against.
3. **Layered instrumentation**: HRESULTs (hex) and SecBuffer layouts printed at three levels (`decrypt()`, `client_handshake_step()`, receive loop). One dump — `[dbg-ren] type=7 size=13 / type=1 size=0 / type=6 size=229 / type=5 size=242 head=16 3 3 0 ba` — simultaneously exposed the STREAM_HEADER semantics (#3) and the EXTRA=everything-unconsumed fact (input composition for #1/#4).
4. **Settle questions with SDK headers**: `winerror.h` for `0x80090317`; `sspi.h` for ISC flag values (which ruled out a "accidental MUTUAL_AUTH" hypothesis).

## Validation

| Check | Result |
|---|---|
| Local TLS testbed (self-signed, `tls::insecure`) | 3/3 runs HTTP 200 + body |
| GitHub raw via SOCKS5, byte-compared against reference cache | **120/120 identical, 0 failures** |
| Consecutive tickets / response batched with tickets / close_notify close | all correct |

## Remaining (unfixed, worked around)

`async_resolve`'s blocking-pool scheduling intermittently hangs for IP literals (getaddrinfo is dispatched to the blocking scheduler and the coroutine never resumes; the system-level `getaddrinfo("127.0.0.1")` itself returns instantly). The consumer (PokeB2DX importer) bypasses it by constructing the endpoint directly for numeric hosts. Worth a dedicated investigation of the io_context blocking-scheduler path.

## References

- Microsoft Docs — [DecryptMessage (Schannel)](https://learn.microsoft.com/windows/win32/api/sspi/nf-sspi-decryptmessage): the `SEC_I_RENEGOTIATE` handling pattern and output buffer semantics
- Microsoft Docs — [InitializeSecurityContext (Schannel)](https://learn.microsoft.com/windows/win32/api/sspi/nf-sspi-initializesecuritycontexta): input consumption / SECBUFFER_EXTRA semantics
- curl source — [`lib/vtls/schannel.c`](https://github.com/curl/curl/blob/master/lib/vtls/schannel.c): renegotiation driving (`schannel_recv_renegotiate`), `SCH_CREDENTIALS` construction, the ISC call contract
- Windows SDK — `winerror.h` / `sspi.h`: HRESULT and flag definitions
