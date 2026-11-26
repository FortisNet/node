# Security

## Reporting a vulnerability

Please do not open a public issue for a security problem.

Use GitHub's private vulnerability reporting for this repository:
<https://github.com/FortisNet/node/security/advisories/new> (the
**Security** tab, then **Report a vulnerability**). Say what is affected, how
to reproduce it and what an attacker gains. Do not include provider keys,
device credentials, wallet data or messages from a real user.

## What happens next

- We aim to acknowledge a report within 5 working days.
- We tell you whether we can reproduce it and what we plan to do, and keep
  you informed until it is fixed.
- Fixes are made on the `main` branch and reach hosts as a new node release.
  The problem is then described in a
  [security advisory](https://github.com/FortisNet/node/security/advisories),
  with credit to you unless you prefer otherwise.
- Please give us up to 90 days before publishing details yourself.

Only the newest release is supported with fixes.

## What is in scope

- The node: its HTTP and TLS code, device authentication and attestation,
  storage, the handling of what providers answer, and its public pages.
- The release build.

The images, the orchestrator and the signing of releases are in
[FortisNet Node OS](https://github.com/FortisNet/node-os); report problems
with those there.

## What the design assumes

- The operator of a host is trusted, and so is whoever holds the release
  signing key.
- Clients, the network and every outside provider are not trusted. What a
  provider answers is bounded and validated before it is used.
- The node runs in development mode today: device attestation can be
  switched off, and production attestation has not been validated on real
  devices. Do not treat an enrolled device as a verified one yet.
- If a credential is exposed, rotate it at once. Provider key files and
  backups that contain them need the same protection as the keys.
