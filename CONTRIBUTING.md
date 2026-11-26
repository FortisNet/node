# Contributing

Thank you for helping. The node holds wallet synchronization data, talks to
many outside services and is reachable from the internet, so changes are
reviewed with that in mind: small, explained and tested.

## Before you start

- For anything larger than a fix, open an issue first and say what you want
  to change and why. It saves work on both sides.
- Security problems do not go in public issues. See [SECURITY.md](SECURITY.md).

## Building and testing

You need Docker, a C++23 compiler, `make`, Python 3, Node.js 25.9 and npm.

```sh
npm ci --omit=optional
make verify
```

`make verify` is the gate. It builds with Clang and GCC with warnings as
errors, runs the Clang Static Analyzer, clang-tidy and Cppcheck, then every
unit test with both compilers and the HTTP and TLS replays. It writes
`build-verify/report.json`. A change is ready for review when every stage
passes and `inputs_unchanged` is true. `make lean-verify` is a quicker
subset while you work. [docs/verification.md](docs/verification.md) explains
each stage.

This repository has no hosted pipeline. Say in the pull request that the
gate passed and on which commit; a reviewer may run it again.

- Do not silence a finding to get through. Fix it, or explain in the pull
  request why the tool is wrong, next to the narrowest possible suppression.
  Known limits of the tools are in
  [docs/static-analysis-limitations.md](docs/static-analysis-limitations.md).
- A change in behaviour comes with a test that fails without it.
- Never change an expected value in `tests/corpus/` to make a test pass.
  If the expectation is wrong, say why in the pull request.

## Rules the code keeps

- **C++23**, formatted by `.clang-format`. Prefer standard facilities,
  ownership through objects and explicit borrowed lifetimes.
  [docs/cpp-standards.md](docs/cpp-standards.md) and
  [docs/object-model.md](docs/object-model.md) have the conventions.
- **[sources.mk](sources.mk) is the list of sources.** Add a file there, in
  its component. Dependencies between components go one way;
  `make check-layers` checks it.
- **The API is a contract.** When you change it, update the OpenAPI files
  and schemas and regenerate the outputs. Keep JSON and protobuf
  compatibility, quotas, durable acceptance and recovery after a restart.
- **Public diagnostics hold stable codes and public metadata only.** Never
  wallet contents, messages, credentials or raw upstream answers.
- **Test data is invented.** Inputs are made up, and expected values are
  the project's own. Do not add recorded responses from providers or
  publishers, or anything from a real user.

## Pull requests

- One subject per pull request.
- Say what changed, why, how you tested it and what is left undone.
- Commit messages describe the resulting behaviour.
- Never include provider keys, device credentials, tokens, stores, backups
  or data from a real host.

## License

By contributing you agree that your contribution is licensed under the
[Apache License 2.0](LICENSE), as the rest of the repository is.
