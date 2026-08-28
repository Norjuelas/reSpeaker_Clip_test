# 0000 — Fork, attribution, and full-history import

**Status:** accepted · 2026-08-28
**Decides:** how `firmware-bpin-core` is created from `reSpeaker_Clip_test`, and what to do
about the key material already in this repository's history.

---

## Context

`reSpeaker_Clip_test` is a fork of Seeed Technology's firmware line for the reSpeaker Clip.
The B·Pin work — WiFi station mode, HTTPS upload with mTLS, AES-GCM at-rest audio encryption,
the health panel, the security hardening — is 69 commits on top of that line, on `feat/https`
(now continued on `clean-repo`). It is being migrated to a private repository,
`firmware-bpin-core`.

Two questions had to be settled before creating that repository:

1. **Import the full history, or start from a squashed import commit?**
2. **What about key material already committed?**

An earlier internal note claimed there were no secrets in the history, on the grounds that
WiFi credentials and test keys live outside the repo. That claim is false, and a full scan
was run to find out how false.

### What the scan found

Method: every one of the **3,404 unique blobs** in the object database was read and inspected,
across all 6 refs and 400 commits. This is exhaustive at the blob level rather than a
per-commit sample.

**Exactly three private keys exist in the history**, deduplicated by content:

| Path | Blob | Identity |
|---|---|---|
| `boards/seeed/clip/sysbuild/root-rsa-2048.pem` | `78c0c341` | **MCUboot's published example key.** md5 `a4c04fd911912d8faa721b0b595efa80`. Currently the MCUboot signing key. |
| `boards/seeed/clip/sysbuild/b0-ecdsa-p256.pem` | `2fba4b9e` | **Real EC P-256 private key.** The project's own NSIB/b0n network-core secure-boot key. |
| `bootloader/root-rsa-2048.pem` | `7b833f36` | **A third RSA key**, distinct modulus. Present 2026-03-27 → 2026-07-02, removed in the sysbuild refactor, in no current branch. |

Other apparent occurrences are not additional keys: `applications/clip/sysbuild/` and
`samples/_mcuboot/sysbuild/` hold the same blob as `boards/`, and ten `samples/*/sysbuild/`
entries are `mode=120000` symlinks to a 53-byte relative path.

Everything else is clean. No AWS access keys, no GitHub or Slack tokens, no SSH keys, no
`.env` files, no real WiFi credentials (all 18 `AT+STACFG` hits are documentation
placeholders such as `"ssid"` and `"<red>"`), and no `AT+KEYCFG` audio keys.
`mobile/app/android/key.properties.example` is genuinely an example — **no Android keystore
was ever committed**, which was the specific risk attached to deleting the `mobile/` tree.

### What each key means

**`root-rsa-2048.pem` — a trust problem, not a leak.** It is MCUboot's published example key,
so publishing it changes nothing; anyone can already obtain it. The problem is that shipped
bootloaders verify against it, so signature verification currently protects nothing: anyone
with a cable can install firmware these devices accept. Rotation is possible but irreversible
for fielded units, and is the subject of its own decision record.

**`b0-ecdsa-p256.pem` — a real secret that cannot be rotated.** It was generated deliberately
to replace NCS's per-build throwaway key, precisely so that network-core OTA would keep working
across builds. Its public-key hash is therefore burned into immutable boot on every shipped
unit. Consequence: **anyone with read access to this repository can sign a network-core image
that every existing device will accept, and there is no remediation for those devices.** New
production units can use a new key; existing ones cannot.

**`bootloader/root-rsa-2048.pem` — never used, possibly not ours.** No configuration ever
referenced it, and Seeed's own documentation warned against it: *"Do NOT use
`bootloader/root-rsa-2048.pem` — it is a different key."* It arrived in a Seeed commit ("Add
bootloader keys and external dependency patches"). Whether it is another published example or
a real Seeed key is unknown.

---

## Decision

**Import the full history of `feat/https` into `firmware-bpin-core`, unfiltered, into a
private repository.**

Specifically:

1. Full-history import. No `git filter-repo`, no squash.
2. The repository is **private**, and stays private.
3. Seeed copyright headers are preserved byte-identical; attribution goes in `NOTICE`.
4. All three keys travel with the history. New production keys are generated later and
   **never enter git** — they live in the org secret manager and are injected in CI.
5. The orphaned `bootloader/root-rsa-2048.pem` is raised with Seeed, or at minimum recorded
   here as possibly theirs.

### Why full history

The 69 commit messages are the project's diagnostic record. They carry the hardware evidence —
hashes, byte counts, measured stack high-water marks — that the design documents cite as
proof. Squashing makes `git log -S` and `git bisect` useless on exactly the subsystems most
likely to regress: TLS, the heap, and thread stacks, each of which has already caused a field
failure.

### Why not filter the keys out

Rewriting history does not un-leak anything. Both keys are already on developer machines, in
the old remote, and in any clone taken to date. Filtering would rewrite every SHA from March
2026 onward, breaking the commit references in the design documents, in exchange for no actual
reduction in exposure.

Filtering would only be worth it if the repository were ever to be made public. It is not.

---

## Consequences

**Accepted, and permanent:**

- Anyone with read access to `firmware-bpin-core` can sign a network-core image that any
  currently-shipped unit will accept. This cannot be fixed for those units.
- Repository access is therefore a security control, not just a convenience. Treat the read
  list as the list of people who can sign netcore firmware for the fleet.

**Required as a result:**

- Restrict repository access to people who need it, and review the list.
- If the repository is ever considered for public release, this decision must be revisited
  **before** that happens — and it will require key rotation for any device still in service,
  not just history rewriting.
- New signing keys are generated outside git and injected in CI.
- A secret scanner belongs in CI. This scan is a snapshot; nothing prevents the next secret.

**Notes for whoever reads this later:**

- The scan targeted known credential patterns. A secret in an unusual shape — a bare hex key
  in a config file, a password in a comment — would not have matched. The result is "no
  secrets of the kinds that matter here", not a proof of absence.
- To reproduce the blob-level scan:
  ```sh
  git cat-file --batch-all-objects --batch-check='%(objectname) %(objecttype)' \
    | awk '$2=="blob"{print $1}' \
    | while read b; do
        git cat-file blob $b | head -c 200 | grep -q "BEGIN .*PRIVATE KEY" && echo $b
      done
  ```

---

## Related

- Key rotation for the MCUboot signing key — its own decision record; irreversible for
  fielded units and needs a staged plan (dev board → bridge image signed with the old key →
  sacrificial unit → production).
- APPROTECT — its own decision record; also irreversible.
- Licence and attribution: Apache-2.0, with Seeed Technology credited in `NOTICE`. Vendored
  libraries keep their own licences (opus BSD, speexdsp BSD).
