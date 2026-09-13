# Architecture Decision Records

One file per significant decision. Each record states the decision, why, what
was rejected, what it costs, and — most importantly — a **tripwire**: the
observation that would justify revisiting it.

A decision without a falsification condition becomes dogma nobody dares touch.

| # | Decision | One-way door? |
|---|---|---|
| [0001](0001-pluggable-clock.md) | Clock is a policy type; virtual time exists from day one | **yes** |
| [0002](0002-proactor-canonical-model.md) | Proactor is the canonical I/O model; readiness backends emulate it | **yes** |
| [0003](0003-generation-checked-handles.md) | Objects are named by generation-checked handles, not pointers | **yes** |
| [0004](0004-no-blocking-itc.md) | No blocking cross-EventManager calls | **yes** |
| [0005](0005-callback-core-coroutine-layer.md) | Callback core, coroutines as an opt-in layer | no |
| [0006](0006-compile-time-protocol.md) | Protocol/framing is a compile-time concept | no |
| [0007](0007-shared-nothing-no-work-stealing.md) | Shared-nothing shards, no work stealing in I/O loops | no |
| [0008](0008-context-and-deadline-propagation.md) | Context and deadline propagation are part of the core | **yes** |
| [0009](0009-compile-time-affinity-annotations.md) | Affinity is compile-time checked where the toolchain allows | **yes** |

Template:

```markdown
# ADR-NNNN: Title

**Status:** proposed | accepted | superseded by ADR-MMMM
**Date:** YYYY-MM-DD
**One-way door:** yes/no

## Context
## Decision
## Alternatives rejected
## Consequences
## Tripwire
```
