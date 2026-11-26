# Governance

How decisions are made in this repository, and who makes them.

## Maintainers

The maintainers are the members of the
[FortisNet organisation](https://github.com/FortisNet) with write access to
this repository. They review and merge changes, make releases and answer
security reports.

## How a change gets in

- Every change arrives as a pull request with a passing report of the gate
  (`make verify`). This repository has no hosted pipeline, so the author
  runs the gate and a reviewer may run it again.
- A maintainer other than the author reviews it. Changes to authentication,
  storage, the HTTP and TLS code or the release build need particular care
  and are never merged on the author's word alone.
- Disagreements are settled by discussion on the pull request or issue. When
  that does not settle it, the maintainers decide.

## Releases

A release of the node is built from files with a passing gate report and is
signed and published by
[FortisNet Node OS](https://github.com/FortisNet/node-os/blob/main/docs/releasing.md),
after a maintainer has looked at what is about to be signed and approved it.

## Becoming a maintainer

Maintainers are added by the existing maintainers, from people whose
contributions have shown care with this kind of code. There is no formal
process yet; ask in an issue.

## Changing this document

By pull request, like everything else.
