# Object model

This document describes how the C++ code of the node is shaped: when to use
a class, a struct, a free function or an interface, how objects are
constructed and owned, how failures are reported, and which low-level idioms
are kept on purpose. Anyone who adds or restructures code under `src/` needs
it. The toolchain and comment rules are in [cpp-standards.md](cpp-standards.md),
the namespace rules in [namespaces.md](namespaces.md), and the exception
policy per boundary in [exception-guarantees.md](exception-guarantees.md).

## Principles

The node is designed for a small, fixed memory budget (512 MiB). What keeps
it fast and predictable there is allocation discipline:

- bump arenas for documents;
- fixed-capacity tables whose size is checked by `static_assert`;
- caller-provided output buffers;
- allocation-free tokenizers.

Classes cost nothing against that discipline. A non-virtual member function
compiles to the same code as a free function. `private:` does not change
size or layout. A virtual call and a call through a function pointer with a
`void*` context are both one indirect call. RAII destructors make the same
release calls as hand-written cleanup, on every path.

The rules below therefore ask for typed, owned, encapsulated state, and keep
the allocation discipline unchanged:

1. State has an owner with private fields and invariants.
2. An object is constructed complete, or not at all.
3. Seams and callbacks are typed.
4. Nothing on a hot path allocates.

## Class, struct, free function or interface

| Use | When | Examples |
|---|---|---|
| **struct** (aggregate, public fields, no invariants) | Wire DTOs, configuration slices, value results, request and response records | `SpotProviderOwnerResult`, `KvMutation`, `FacetResult`, `SourceResult` |
| **class** (private state, invariants, behaviour) | Anything with a lifecycle, a lock, a thread, a resource, a cache, a queue or a state machine, or with fields that must change together | services, owners, `KvStore`, `BoundedChainIdSet`, `RepairCandidates` |
| **free function** | Pure transforms with no state: codecs, parsers, formatters | `EvmTxStatusOf`, the `proto_wire` writer |
| **interface** (abstract class) | Only at a real seam: production has one implementation and tests need a fake, or production has two or more | `Clock`, `ChainDriver`, `TokenSpotReader`, `CompositionHooks` |

The test for "class": if two fields must be updated together (`array` and
`count`, `mutex` and `data`, `started` and `thread`), they belong in a class
whose methods keep them consistent.

Rules for classes:

- **Data members are private** in every class with behaviour.
- **Rule of zero.** Members are RAII types (`fortis::base::Mutex`,
  `fortis::base::Thread`, `std::unique_ptr`, `fortis::base::Buffer`,
  `std::vector`), so a class declares a destructor only when it must stop a
  thread first. A resource owner deletes its copy operations.
- **`const` methods are thread-safe, or documented as not.**
- **One responsibility per class.** A class of more than about 400 lines, or
  with more than about 15 data members, is a candidate for decomposition.

Wire DTOs, configuration rows and result structs stay aggregates. Giving
`KvMutation` getters adds nothing. Pure transforms stay free functions in a
namespace; they are the most testable code in the tree.

## Construction

**Constructors do not throw for anticipated failures.** A class whose
construction can fail has a static factory that returns
`fortis::base::Result`:

```cpp
class TokenService {
 public:
  static base::Result<TokenService> Create(const config::ProviderRuntimeConfig& config,
                                           TokenServiceDeps deps, ...);
  ...
};
```

- The service planes (`TokenService`, `AddressService`, `NewsService`,
  `DiscoveryService`, `PriceSpotService`, `PriceHistoryService`,
  `AiConversationService`, `CrossChainService`, `RobinhoodService`,
  `DashPayService`), `SpotProviderOwner` and `ProviderExecutionAdapter` are
  created this way.
- A class that holds its state behind a `std::unique_ptr<Impl>` returns the
  movable handle **by value**. That keeps one allocation per object. It is
  safe because threads capture the `Impl*`, which is stable across a move of
  the handle. A moved-from handle may only be destroyed.
- A class without a pimpl returns `Result<std::unique_ptr<T, ...>>`.
  `fortis::base::MakeOwned` (`src/base/factory.h`) places a factory's value
  into separately owned storage where a parent needs that.
- The factory reports allocation failure as a `Status`
  (`kResourceExhausted`); it does not throw. Startup therefore refuses with a
  specific log line.
- Dependencies arrive in one `…Deps` struct: required ones as references,
  optional ones as nullable pointers. Each module receives only its own slice
  of the configuration (`src/app/module_configs.h`), not the whole
  `NativeConfig`.
- Route registration is a separate step after every plane exists, because
  the composition order in `src/app/service_wiring.cpp` is part of the
  startup contract.
- Objects that are constant-initialized globals (`Mutex`, the system clock)
  keep `constexpr` constructors.

Why factories and not throwing constructors:

1. **C frames.** Construction can happen inside a `pthread_once` initializer
   or a callback from a C library. C code may have no unwind tables, so an
   exception that escapes there ends the process. A status return cannot
   escape.
2. **Startup refusals are specific.** The composition logs a fixed message
   per refused plane. A `Status` carries a static message at no allocation.
3. **Memory.** The usual failures are memory and thread exhaustion. Under
   memory exhaustion, returning a status needs nothing.

Only `std::bad_alloc` and `std::length_error` may be thrown, by standard
containers and `new` in code that uses them. They are fatal for the request,
never control flow, and are caught only at the boundaries described under
[Error handling](#error-handling).

## Ownership

`src/base/ownership.h` holds the allocation helpers. Production code does
not call `malloc` or `free` directly; `make check-cpp-contracts` runs
`scripts/check_memory_ownership.py`, which enforces the C-allocation
boundaries.

| Helper | Use |
|---|---|
| `std::unique_ptr`, `std::make_unique` | The default owner |
| `AllocateStorage<T>` + `StorageDeleter` | Unconstructed byte storage under a standard owner. Returns null on failure; it does not throw |
| `New<T>` / `MakeUnique` / `ObjectDeleter<T>` | Construct an object in owned storage. Null on allocation failure; a throwing constructor releases the storage and propagates |
| `NewDefaultInit<T>` | As `New`, but default-initialized, for large scratch objects whose zero-fill would be wasted work |
| `ResizeStorage` | Grow or shrink raw, trivially copyable storage. On failure the old owner is retained |
| `fortis::base::Buffer` | Move-only bounded byte owner. Contents are uninitialized unless zeroed, which avoids the zero-fill of `std::vector<uint8_t>(n)` for large scratch and response buffers |
| `fortis::base::FileDescriptor` (`file_descriptor.h`) | Owner of a file descriptor |

Rules:

- The composition root (`src/app/service_wiring.cpp`) owns every plane.
  Planes borrow each other by reference. The shutdown order is load-bearing,
  so shared ownership must not hide it.
- `std::shared_ptr` is used only where a worker or connection must be able
  to outlive the object that started it (outbound and incoming HTTPS, the
  Robinhood catalog). Do not build ownership graphs with it.
- A borrowed pointer, span or view never extends a lifetime. Copy to owned
  storage before queueing asynchronous work.

## Value types

- **`std::string_view`** for a non-owning string parameter. At a C boundary
  use `%.*s`, `memcmp` or a bounded copy. Never call `.data()` on a view and
  assume a NUL terminator.
- **`fortis::base::FixedString<N>`** (`src/base/fixed_string.h`) for a
  bounded string member. `sizeof(FixedString<N>) == N`, so record layouts are
  unchanged. `Assign` truncates at N−1 bytes, always terminates, and returns
  false on truncation, so truncation is a named and testable decision.
  `CopyCString` and `ConcatCString` do the same for plain `char` arrays.
  `tests/fixed_string_test.cpp` compares the truncation with `snprintf`.
- **`std::optional<T>`** where a function returns a value or "absent", and
  absence is not a failure.
- **`chains::ChainId`** (`src/chains/chain_id_set.h`) wraps a `uint64_t`: EVM
  chain ids are not bounded by 2^32. `BoundedChainIdSet` is a bounded,
  thread-safe set of them that drops inserts past its capacity. Each service
  keeps its own instance with its own capacity; the sets are not merged,
  because each service learning on its own is the intended behaviour.
- Fixed `char name[N]` members and `T items[kN]` arrays with a count are
  correct for bounded slot tables. Wrap the pair in a class when it has
  invariants.

## Interfaces and seams

An interface is an abstract class with a **protected non-virtual
destructor**: objects are never deleted through the interface, and the owner
holds the concrete type.

RTTI is disabled, which rules out `dynamic_cast`, `typeid` and `std::any`.
An interface must therefore expose everything its users need. An optional
capability is advertised by an explicit method (for example
`ChainDriver::HasInsight()`). If code wants to downcast, the seam is in the
wrong place. Test fakes are injected through `Create`, never discovered.

| Interface | Where | Production | Test fake |
|---|---|---|---|
| `Clock` | `src/base/clock.h` | `SystemClock()` | `ManualClock`, `PointerClock` (`tests/fake_seams.h`) |
| `JitterSource`, `RandomSource`, `IdSource`, `Starter<T>` | `src/base/seams.h` | CSPRNG, the dependency's own `Start()` | `FixedJitter` (`tests/fake_seams.h`), fixed sources under `tests/server/` |
| `ErrorReporter` | `src/base/error_reporter.h` | the error board | - |
| `ChainDriver`, `NftReader`, `TransferSource`, `TransactionView` | `src/chains/chain_driver.h` | one per chain family | - |
| `TokenFactExecutor`, `TokenSpotReader`, `TokenSeriesReader`, `TokenRevalidationQueue` | `src/tokens/token_facts.h` | the token service and its provider owner | - |
| `CompositionHooks` | `src/app/composition_hooks.h` | `NoCompositionHooks` | the replay binaries under `tests/server/` |

Two details of the base seams:

- `Clock` and the seams in `seams.h` have **no pure methods**. The base
  `Clock` is the system clock, so a fake overrides only what it controls and
  keeps real behaviour for the rest (for example a frozen wall clock with
  real sleeps). The base body of the other seams answers "no implementation".
  A fake that forgets to override a method silently gets the base behaviour.
- A `Clock` has two roles: the wall clock (`NowMs`, for cache ages and
  timestamps) and the monotonic clock (`MonotonicMs`, for deadlines and
  intervals). Where both are injected, they are separate options
  (`wall_clock`, `monotonic_clock`).

`KvStore` is deliberately **not** an interface. Tests use a real `KvStore`
on a temporary directory: it is fast and deterministic, and it has failure
injection behind `FORTIS_KV_TEST_FAILURES`. A fake store would drift from
the real recovery and atomicity semantics.

## Callbacks

| Kind | Mechanism |
|---|---|
| Synchronous visitor or matcher | `fortis::base::FunctionRef<Sig>` (`src/base/function_ref.h`): a non-owning callable reference, two words, no allocation. Examples: `KvVisitor`, `KvRecordVisitor`. A `FunctionRef` must not be stored or returned; it is not assignable |
| Route handler | `HttpRouter::Add<&T::Method>(method, path, target)` binds a member function with type checking. The route table row stays a function pointer and a context, so the table keeps its fixed size (96 routes) |
| Thread entry | `fortis::base::Thread::Start(callable, stack_bytes)`. The capture is stored inline in the thread object and must be small and trivially destructible |
| Function handed to C (`pthread_once`, comparators, TLS callbacks) | `fortis::base::FatalCallback<Fn>` (`src/base/exception_boundary.h`), a `noexcept` frame |
| Callback registered once at composition | An interface. `std::function` is not used |

A new route handler takes `(const HttpRequest&, HttpReply&)`. The older
handler signature that receives the client socket is kept as a migration
adapter (`HttpRouteHandler` in `src/http/http_server.h`); do not write new
routes against it.

## Standard library policy

| Facility | Policy |
|---|---|
| `std::string_view`, `std::array`, `std::optional`, `std::span`, `std::pair`, `<algorithm>`, `<charconv>`, `std::unique_ptr`, `std::atomic` | Use freely. They do not allocate |
| `std::vector`, `std::string` | Use off the hot paths, and `reserve()` the exact size |
| `std::function` | Do not use. Prefer `FunctionRef` or an interface |
| `std::shared_ptr` | Only as described under [Ownership](#ownership) |
| `std::mutex`, `std::condition_variable`, `std::thread` | Not in production code; see [Concurrency primitives](#concurrency-primitives). Test code may use `std::thread` |
| `std::expected`, `std::print` | Do not use; see [cpp-standards.md](cpp-standards.md) |
| `<regex>`, `<filesystem>`, iostreams, `std::random_device`, `<future>` | Do not use. They allocate, or are not verified on every target |

The helpers in `src/base/` exist where the standard type does not fit:

| Helper | Why it exists |
|---|---|
| `Status`, `Result<T>` (`status.h`) | The project's error contract. A `Status` is a code and a static message: two words, trivially copyable. Detail that varies per failure goes to the log line |
| `Mutex`, `ConditionVariable` (`threading.h`) | Constant-initialized, trivially destructible wrappers over pthreads. A global needs no dynamic initializer and has no exit-time destructor, so it stays usable during late shutdown |
| `Thread`, `ThreadHandle` | `std::thread` cannot set a stack size, and the node sizes every stack explicitly |
| `Buffer` | No zero-fill |
| `FunctionRef` | A non-owning callable reference without allocation |
| `FixedString<N>` | A bounded string with an explicit truncation result |

## Error handling

| Situation | Mechanism |
|---|---|
| Expected failure that the immediate caller acts on (I/O, bad input, provider down, quota, not found) | A **domain enum** when callers branch on several outcomes; otherwise `fortis::base::Status` / `Result<T>` |
| Fallible construction or start | A `Create` factory that returns `Result` |
| Optional value, no error | `std::optional<T>` |
| Programming error or broken invariant | `abort()` (as `Result::value()` does on an error) |
| Resource exhaustion inside standard containers | `std::bad_alloc`, caught **only** at the boundaries |

A domain outcome is not a `Status`. An enum whose values the caller treats as
different results (found, missing, stale; an HTTP mapping; a provider
verdict) stays its own enum, with its own name. No domain type is called
`Status` or `Result`, so those names always mean the base types.

Exception boundaries (`src/base/exception_boundary.h`):

- **Request dispatch.** `HttpRouter::Dispatch` maps `std::bad_alloc` to 503
  and other standard exceptions to 500, and discards a captured but unsent
  response. A handler that has already written to the socket cannot be turned
  into a 500, so the connection is closed.
- **The AI reply worker** recovers per job: the job is settled as failed and
  the worker continues. Other workers treat an escaping exception as fatal.
- **Thread entries** use `FatalBoundary`: log, then `std::terminate()`, for
  every exception. A dead owner thread inside a live object would leave
  requests hanging on it, which is worse than a restart.
- **C callbacks** use `FatalCallback`, so a stray throw terminates
  deterministically and never unwinds through C frames.

A boundary logs a trusted static context and the caught class category
only. It never logs `what()`, which can contain credentials, wallet contents
or provider responses.

[exception-guarantees.md](exception-guarantees.md) has the guarantee for each
boundary.

## Concurrency primitives

`src/base/threading.h` wraps pthreads without dynamic allocation.

- `Mutex` is a default-attribute mutex. It also satisfies BasicLockable
  (`lock`, `unlock`, `try_lock`). No recursive or error-checking mutex
  exists; locking one twice from the same thread deadlocks.
- `LockGuard` holds a mutex for one scope. `UniqueLock` can be released and
  re-taken inside its scope: use it for a critical section that ends before a
  call-out or an early return, and for condition-variable waits. Do not
  write manual lock and unlock pairs.
- `ConditionVariable` timed waits take an absolute `CLOCK_REALTIME`
  deadline. A wait can wake spuriously; re-test the predicate.
- `Thread` owns a joinable thread by composition. `ThreadHandle` is the
  trivially destructible form for components with static storage, which need
  an explicit shutdown. Stop and wake a component before it is joined or
  destroyed. A thread object must not be destroyed on its own thread.
- Shared flags and counters are `std::atomic<T>`. An aggregate that holds an
  atomic cannot be reset with `x = X{}`; give it an explicit reset method.

Thread stacks are sized explicitly: 128 KiB (HTTP response writers, the
provider coordinator), 256 KiB (owners, listeners) or 512 KiB (token
workers).

## Resource constraints

Any change must keep these properties.

1. **No hidden allocation on hot paths.** `std::string`, `std::vector`
   growth and `std::shared_ptr` control blocks each add allocator calls, and
   each can throw `std::bad_alloc` where the current code returns an error.
   These paths stay allocation-free:
   - HTTP dispatch and parsing;
   - `bounded_json` and `json_lex` tokenizing;
   - `proto_wire` writing;
   - `KvStore::Get` and `Scan`;
   - the spot owner request path;
   - price series reads.
2. **Allocator rounding.** The arena block size is 1 MiB − 64
   (`kArenaBlockBytes` in `src/codec/token_json.h`), so that on a buddy
   allocator with a block header one block takes exactly one order. By the
   same reasoning, geometric `std::vector` growth across such a boundary can
   double the footprint. Always `reserve()` exact sizes, and never let a
   vector grow incrementally past a few hundred KiB.
3. **Stack.** With the stack sizes above, an object with large fixed arrays
   must not become a local variable by accident. Factories return heap
   objects.
4. **No virtual call in a per-byte or per-token loop.** Sinks are template
   parameters (`proto_wire`'s `Append`). Do not convert them to an interface.

## What stays as it is

These idioms look low-level and are deliberate.

1. **Arenas.** `tj::Arena` with `MarkOf` / `Rewind` and the growing blocks,
   and the news arena (`src/news/news_text.h`). The arena-allocated `Value` DOM costs one bump per
   node and is freed in one call. Do not convert DOM strings to
   `std::string`, or nodes to `std::vector` or `shared_ptr`.
2. **Wire codecs.** `proto_wire` stays free functions over a templated sink.
   `bounded_json` (allocation-free, caller-owned tokens), `json_lex`,
   `js_json`, nanopb, `price_history_batch_codec` and `crash_report_codec`
   stay as they are; their golden tests are the contract
   ([codec-semantics.md](codec-semantics.md)). The codecs use pointer and
   length internally on purpose.
3. **Fixed-capacity tables** in owners and services (request slots, shared
   flights, jobs, the router's route table) and their `static_assert` memory
   budgets. These bounds are the admission-control design: a full table
   answers "busy"; it does not grow. Wrap a table in a class; never replace
   it with a growing container.
4. **`HttpRouter` internals.** The route row of method, path, handler and
   context, and the fixed table. The typed binder sits on top.
5. **`KvStore`.** The on-disk format, framing, checksums and lock order, and
   the caller-provided buffer of `Get`, which is allocation-free by design.
   Do not return `std::vector<uint8_t>` from it.
6. **`Status` stays two words with a static message.** Do not add an owned
   string.
7. **C code and C callbacks.** blst, BLAKE3, the Mbed TLS glue and the guest
   shims stay C. Callbacks handed to C stay C-ABI functions behind
   `FatalCallback`.
8. **Domain enums.** Callers branch on their values. They are results, not
   errors.
9. **Measured algorithms.** The news clustering thresholds and the coin
   popularity bucket tree were tuned by measurement. Change their shape only
   with a differential test.

## Older idioms still in the tree

The code was first written in a C-like style, when its original target had
no C++ runtime. Most of it follows the rules above; some does not yet. Do not
copy these patterns into new code. Convert them when the surrounding code is
changed for another reason, with the module's replay tests as the check.

- **Function pointer and `void*` context seams.** Examples: `FacetDeps`
  (`news_feed`, `enqueue`, `ingest_async`) in `src/wallet/address_facets.h`,
  `SwrFetcher` in `src/wallet/address_facets_internal.h`, the completion
  callbacks of the provider owners, and the `allow_address` test redirects.
  The target is an interface or `FunctionRef`.
- **Two-phase objects.** `KvStore` is constructed and then opened with
  `Open(directory)`. A few classes still expose `valid()` (the provider
  execution coordinator, provider lanes, the history backfill owner).
  `SpotProviderOwner` still has setters that must be called before use
  (`SetClock`, `SetCoinPopularity`, `SetRobinhoodCatalog`). The target is a
  `Create` factory with an options struct.
- **Public-field `Impl` structs.** A pimpl hides fields from the header, but
  inside the module every function can reach every field.
  `SpotProviderOwner::Impl` is the largest. The target is a set of small
  classes that each own one table or state machine.
- **Hand-written owner start-up.** Several provider owners set up thread
  attributes and roll back coordinator registration by hand, and two of them
  choose between a private adapter and the shared coordinator in each method.
- **Routes on the socket-style handler signature** (see
  [Callbacks](#callbacks)).
- **`bool` plus out-parameter** signatures inside modules, and pointer plus
  length pairs outside the codecs.

## Testing

- Unit binaries run in milliseconds without sockets. Each one is registered
  in the Makefile test recipe, so `make test`, `make verify` and the
  sanitizer targets ([sanitizers.md](sanitizers.md)) run it.
- A test builds an object through its `Create` factory with fakes for its
  seams: `ManualClock`, `PointerClock` and `FixedJitter` from
  `tests/fake_seams.h`, a detached `HttpReply`, and a real `KvStore` in a
  temporary directory.
- A handler bound through the typed router binder is a member function. A
  test can call it directly with a built `HttpRequest` and a detached
  `HttpReply`.
- The factory tests (`tests/*_factory_test.cpp`,
  `tests/factory_contract_test.cpp`) check that a failed construction returns
  a `Status`, returns no object and releases every resource it had acquired.
- `tests/exception_boundary_test.cpp` checks the boundary policy, with death
  tests for the fatal boundaries.
- The Python replays under `tests/` compare whole HTTP responses and remain
  the behaviour check for a structural change. Add a unit test in the same
  change as the structure that makes it possible.
