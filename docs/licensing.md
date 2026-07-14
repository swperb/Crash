# Crash — Licensing & Commerce Design

How Crash sells and unlocks Pro. Companion to
[`../crash_design_doc.md`](../crash_design_doc.md) §2 (business model) and §6.6 /
§10 (open-core boundary). This document is the plan for replacing the demo gate
in [`../src/License.cpp`](../src/License.cpp) with a real system.

> Status: **design**. Nothing here is implemented yet except the app-side stub.

---

## 1. Principles

1. **Offline-first validation.** The app must verify a license *without* calling
   home. Design §2.3 says the app validates "at launch"; a file manager that
   won't unlock without a network round-trip is a non-starter (network drives,
   air-gapped machines, planes). We achieve this with **signed license files**:
   the app ships a public key and verifies a signature locally.
2. **Honest-user friction, not DRM.** Crash is MIT open core (§2.3). A determined
   user can recompile the core with the check removed — and that's *fine*
   (design §7: "execution and continued investment are the moat, not the header
   files"). The goal is a frictionless *legitimate* purchase and a check that
   stops casual sharing, not uncrackable protection. **No invasive DRM** — for a
   tool that gets full filesystem access, anti-tamper rootkittery would poison
   the exact trust the product is built on.
3. **Minimal compliance & ops burden.** A solo/small team should not be running
   PCI infrastructure or filing VAT in 40 jurisdictions. Push that to a
   merchant-of-record and keep our own service tiny and stateless-ish.
4. **Keep the platform's cut, not literally 100%.** Design §2.2 says "keep 100%."
   To be precise: self-hosted commerce avoids the **Microsoft Store's 15%**, but
   every payment path costs **~3–6%** (card fees + processor). "100%" means *no
   store tax*, not *no fees*. Budget ~5%.

---

## 2. Architecture at a glance

```
  ┌────────────┐   hosted checkout    ┌──────────────────────┐
  │  Buyer     │ ───────────────────▶ │ Merchant of Record   │
  │ (browser)  │ ◀─── receipt ─────── │ (Lemon Squeezy /     │
  └────────────┘                      │  Paddle): cards, tax │
        ▲  downloads Crash.lic        │  VAT, chargebacks    │
        │                             └──────────┬───────────┘
        │                                        │ webhook: order.created
        │ license file (email + page)            ▼
        │                             ┌──────────────────────┐
        │                             │ License Service      │
        │                             │ (Cloudflare Worker)  │
        │  activation (optional)      │  - verify webhook    │
        │◀──────────────────────────▶ │  - issue signed .lic │  ── D1 (licenses)
        │                             │  - activation limit  │  ── KV (fingerprints)
        │                             │  - revoke on refund  │  ── KMS (Ed25519 priv)
        │                             └──────────────────────┘
        ▼
  ┌────────────┐   verify Ed25519 sig with embedded PUBLIC key (offline)
  │ Crash app  │   → unlock Pro; load crash_pro.dll (closed source)
  └────────────┘
```

---

## 3. Commerce layer — merchant of record

**Recommendation: a merchant-of-record (MoR) processor — [Lemon Squeezy] or
[Paddle].** They are the legal seller: they collect and remit **global sales
tax / VAT / GST**, own **chargeback/fraud** handling, and issue invoices. For a
solo dev selling a $15–25 app worldwide, the tax compliance they absorb is worth
far more than the fee delta.

| Option | Fee (approx) | Who handles tax | Notes |
|---|---|---|---|
| **Lemon Squeezy** (rec.) | ~5% + 50¢ | ✅ MoR — they file | Simplest DX, built for indie software + license keys. |
| **Paddle** | ~5% + 50¢ | ✅ MoR — they file | Mature, similar; slightly heavier onboarding. |
| **Stripe** | ~2.9% + 30¢ | ❌ *you* file (Stripe Tax assists) | Cheapest, most control — but you register/collect/remit VAT yourself. |
| **MS Store IAP** | 15% | ✅ MS | Simplest distribution, but ties unlock to the Store account and re-adds the platform cut §2.2 wants to avoid. |

**Decision to confirm:** MoR (Lemon Squeezy/Paddle) vs self-managed tax (Stripe).
Everything downstream is processor-agnostic — we only depend on a signed
*purchase webhook*, so switching later is cheap.

The **product**: one SKU, *Crash Pro*, one-time purchase, perpetual license.
(Optionally "perpetual + 1 year of updates" JetBrains-style later; the token
format below already carries an `updatesUntil` field to enable that without a
breaking change.)

[Lemon Squeezy]: https://www.lemonsqueezy.com
[Paddle]: https://www.paddle.com

---

## 4. The license token (offline-verifiable)

A license is a small **signed blob** the app verifies locally. Signing:
**Ed25519** — 32-byte keys, 64-byte signatures, fast, no parameter foot-guns.

### Format

A UTF-8 JSON payload, canonicalized, then `payload || Ed25519_sig(payload)`,
base64url-encoded into a `Crash.lic` file:

```jsonc
{
  "v": 1,
  "lid": "CRX-7F3A-9K2M-...",     // license id (also the human/support key)
  "prod": "crash",
  "ed": "pro",                     // edition
  "email": "buyer@example.com",    // or a salted hash, for privacy
  "iss": 1793664000,               // issued (unix)
  "updatesUntil": 0,               // 0 = perpetual for all versions
  "maxSeats": 3,                   // machine activations allowed
  "order": "ls_ord_abc123"         // processor order id (support/refunds)
}
```

The file is ~400 bytes. Users **download `Crash.lic`** after purchase (and get it
by email); in-app they click **Import license…** (Crash already has the entry UI
— it just reads a file instead of matching a hardcoded key). `lid` doubles as a
short support key for the re-download page.

### App-side verification (replaces the demo gate)

- Embed the **32-byte Ed25519 public key** as a constant in the app. The
  **private key lives only in the License Service (KMS)** — never in the repo.
- On launch: read `%LOCALAPPDATA%\Crash\Crash.lic`, split payload/sig, verify sig
  with the public key, then check `prod=="crash"`, `ed=="pro"`, and (if we ship
  update-gated perpetual) that the running build date ≤ `updatesUntil` or
  `updatesUntil==0`.
- Crypto lib: **Monocypher** (single-file, public-domain, audited) or
  `ed25519-donna`. Windows CNG's Ed25519 support is too version-dependent to
  rely on. This is one vendored `.c` file — small, no dependency risk.

Because verification is a pure signature check, it works **fully offline**,
satisfying Principle 1.

---

## 5. License Service (backend)

Small, cheap, mostly stateless. **Recommended stack: Cloudflare Workers + D1 +
KV**, because it's global, serverless, near-free at this volume, and can sign
Ed25519 in-Worker via WebCrypto. (A tiny Fly.io/Render container with SQLite is a
fine alternative.)

### Endpoints

1. **`POST /webhook/{processor}`** — the only trusted writer.
   - Verify the processor's webhook signature (HMAC / signing secret).
   - On `order.created`: insert a license row, **sign** a `Crash.lic`, store it,
     email the buyer a download link + attach the file.
   - On `order.refunded` / `chargeback`: set `revoked=1`.
2. **`GET /license?order=…&email=…`** — re-download page/endpoint (rate-limited).
3. **`POST /activate`** *(optional, enables seat limits + revocation)* —
   `{lid, fingerprint}` → check `revoked`, count activations in KV, allow up to
   `maxSeats`, return `{ok}` (and could return a fresh short-lived token). The
   app calls this **once per machine, best-effort**; if offline, it falls back to
   the offline signature check. This keeps the app usable offline while still
   letting us enforce seat counts and kill refunded/leaked keys when online.

### Data

`licenses` (D1/SQLite): `lid, order, email, edition, maxSeats, issued, updatesUntil, revoked`.
`activations` (KV): `lid → [fingerprint,…]`. Tiny; a JSON column works too.

### Machine fingerprint

Stable, privacy-respecting, non-PII: hash of `MachineGuid`
(`HKLM\SOFTWARE\Microsoft\Cryptography`) + volume serial. Never sent in the
clear; only its hash leaves the machine.

### Key custody

Ed25519 **private key in a KMS / secret store** (Cloudflare Secrets, AWS KMS).
Rotate by shipping a new *public* key in an app update and keeping the old one
valid (multi-key verification) so old licenses still work.

---

## 6. Open-core split (the real gate)

Today the Pro check is an `if (g_pro)` in the MIT source — trivially patchable
and, worse, the Pro *logic* is public. Design §2.3/§10 calls for Pro features in
a **separate closed-source module**. Target:

- Public repo (MIT): core + a `crash_plugin.h` ABI + a stub. **No Pro logic.**
- **`crash_pro.dll`** (closed source, shipped by the installer, not in the repo):
  command palette, recursive/indexed search, future scripting.
- Boot: core verifies the license, computes a capability set, and only then
  `LoadLibrary("crash_pro.dll")` + calls `CrashPro_Init(host, license)`. No
  license → the DLL is never loaded; the features simply don't exist in-process.

This makes the gate "don't load real code we never published" rather than "flip a
bool in open source" — a materially higher bar, and it keeps Pro IP private
while the engine stays genuinely open. Still crackable (a forged DLL); that's an
accepted trade-off (Principle 2).

**Migration:** move `src/main.cpp`'s command-palette + `Enumerator::Search` into
the Pro module behind the ABI; the free build keeps in-folder filter only.

---

## 7. Flows

**Purchase → unlock**
1. In-app **Upgrade to Pro** opens the hosted checkout in the browser (the app
   never touches card data — Principle 3, and never handles payment fields).
2. MoR processes payment, fires the webhook.
3. Service issues + emails `Crash.lic` and shows a download link.
4. User clicks **Import license…** in Crash (or drops the file in). App verifies
   offline → Pro unlocks → loads `crash_pro.dll`.
5. (Optional) App calls `/activate` in the background to claim a seat.

**Refund/chargeback** → MoR webhook → `revoked=1`. Offline apps keep working
until their next `/activate` (soft-enforcement). For a $20 one-time app we
**accept** that fully-offline refunded copies may linger — chasing them isn't
worth the ops or the honest-user friction.

**Support / lost key** → re-download page (`/license?order&email`) or resend from
the processor dashboard.

---

## 8. Threat model — explicitly accepted

| Risk | Stance |
|---|---|
| Recompiling the MIT core without the check | Accepted (§7). Pro IP is in the closed DLL, so this yields only the free tier anyway. |
| Cracking `crash_pro.dll` / forging a license | Accepted. No invasive DRM. Signed keys + closed module raise the bar; that's the ceiling we want for a trust-sensitive tool. |
| Key/file sharing | Bounded by optional `/activate` seat limits + revocation; tolerated when offline. |
| Refund abuse | MoR handles chargebacks; we revoke; soft-enforce. |
| Private key leak | KMS custody + key rotation with multi-key verification. |
| Store-vs-GitHub "is it free?" confusion (§2.2) | One consistent story: core free everywhere; Pro is one SKU, one checkout, linked identically from the Store listing, the site, and the repo. |

---

## 9. Build order

1. **App-side verification** — vendor Monocypher, embed a public key, make
   `License.cpp` verify a signed `Crash.lic` (+ **Import license…** UI). Generate
   test licenses with a local CLI holding the dev private key. *No backend needed
   to develop/test this.*
2. **Open-core split** — extract Pro into `crash_pro.dll` behind the ABI.
3. **License Service** — Cloudflare Worker: webhook → sign → email; re-download.
4. **Processor** — wire Lemon Squeezy/Paddle checkout + webhook; go live.
5. **Activation** (optional, later) — seat limits + revocation.
6. **Store listing** — free core on the MS Store; **Upgrade to Pro** deep-links
   to the same checkout.

Step 1 is independently shippable and unblocks everything: once the app verifies
real signed licenses, the backend can be built and swapped in without touching
the client.
