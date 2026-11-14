# Flecha 0.3 standard library

This is the public API index for the standard library shipped with Flecha
0.3.0. Import names, declarations, and examples below are release contracts.
The declaration appendix is generated directly from `std/src`; CI rejects a
changed public surface until this checked-in reference and its module metadata
are updated together.

Applications import these modules through the `std` dependency alias:

```flecha
@imports
    :: lib::std::containers::collections
    :: lib::std::fmt;
```

The installed, runnable [`standard_library`](../examples/standard_library/README.md)
example imports every module in this index. The topic guides carry the full
ownership, allocation, error, blocking, determinism, and platform contracts.
The [library API rules](library_api_style.md) govern naming and compatibility.

## Module index

| Module | Purpose | Operational contract | Reference | Focused example |
| --- | --- | --- | --- | --- |
| `std::bytes::buffer` | Growable owning byte storage | Allocates explicitly; mutation can fail without corrupting the initialized prefix; deterministic and nonblocking. | [guide](bytes.md#buffer) | [example/test](../std/src/bytes/buffer_test.fl) |
| `std::bytes::sequence` | Byte equality and lexicographic comparison | Borrows both inputs, performs no allocation, is deterministic, and does not block. | [guide](bytes.md#sequence) | [example/test](../std/src/bytes/sequence_test.fl) |
| `std::containers::collections` | Owning collections and collection algorithms | Ownership transfer and allocation failure are explicit; ordering is deterministic only where the selected container or policy promises it. | [guide](collections.md) | [example/test](../std/src/containers/array_test.fl) |
| `std::containers::errors` | Collection error families | Stable typed failures for bounds, capacity, and empty-container operations; no allocation or blocking. | [guide](collections.md#failure-and-cleanup-guarantees) | [example/test](../std/src/containers/deque_test.fl) |
| `std::containers::raw_array` | Low-level compatibility owning array | Owns its allocation and exposes explicit failure; prefer the safe collection layer for new APIs. | [guide](collections.md) | [example/test](../std/src/containers/raw_array_test.fl) |
| `std::cryptography::hmac` | Streaming and one-shot HMAC-SHA-2 | Borrows key/message bytes, returns fixed-size tags, is deterministic and nonblocking, and reports invalid state or input length. | [guide](cryptography.md#hmac) | [example/test](../std/src/cryptography/hmac_test.fl) |
| `std::cryptography::sha2` | Streaming and one-shot SHA-256/SHA-512 | Borrows input and returns fixed-size digests; deterministic, nonblocking, and independent of host byte order. | [guide](cryptography.md) | [example/test](../std/src/cryptography/sha2_test.fl) |
| `std::encoding::base64` | Canonical padded Base64 encoding | Owning results allocate; decoding rejects malformed or noncanonical input; output is deterministic across platforms. | [guide](encoding.md#base64) | [example/test](../std/src/encoding/base64_test.fl) |
| `std::encoding::binary` | Endian primitives, bounded readers/writers, and varints | Explicitly bounded and endian-stable; growable buffers may allocate; malformed/truncated input returns typed errors without overread. | [guide](encoding.md#binary) | [example/test](../std/src/encoding/binary_reader_test.fl) |
| `std::encoding::hex` | Canonical hexadecimal encoding | Owning results allocate; decoding validates complete byte pairs; deterministic and nonblocking. | [guide](encoding.md#hexadecimal) | [example/test](../std/src/encoding/hex_test.fl) |
| `std::fmt` | Typed formatting and console output facade | String formatting allocates and returns errors; console writes are blocking host I/O and preserve argument order. | [guide](formatting.md) | [example/test](../std/src/fmt/fmt_test.fl) |
| `std::hashing::hashing` | Structural non-cryptographic hashing | Borrows values, performs no allocation or blocking, and is deterministic only for the documented toolchain representation; never use for cryptography. | [guide](hashing.md) | [example/test](../std/src/hashing/hashing_test.fl) |
| `std::io::buffered` | Buffered reader and writer adapters | Owns internal buffers, may allocate, and inherits blocking and error behavior from the wrapped stream. | [guide](filesystem.md#buffered-streams) | [example/test](../std/src/io/buffered/buffered_test.fl) |
| `std::io::errors` | Portable I/O error family | Normalizes host failures into stable variants while preserving platform-dependent operation availability. | [guide](filesystem.md) | [example/test](../std/src/io/file/file_test.fl) |
| `std::io::file` | Files, directories, and temporary resources | Owns native handles, blocks on host I/O, requires explicit close where exposed, and reports portable I/O errors. | [guide](filesystem.md#file-streams) | [example/test](../std/src/io/file/temporary_file_test.fl) |
| `std::io::filesystem` | Whole-file and filesystem operations | Reads allocate owning results; operations block and can vary with host permissions/filesystem semantics; errors are explicit. | [guide](filesystem.md) | [example/test](../std/src/io/filesystem/filesystem_test.fl) |
| `std::io::path` | Lexical POSIX and Windows path manipulation | Lexical operations do not touch the filesystem; owning results may allocate; flavor selection makes platform rules explicit. | [guide](path.md) | [example/test](../std/src/io/path/path_test.fl) |
| `std::io::stdio` | Standard input, output, and error streams | Borrows written data, may allocate for line reads, blocks on host streams, and returns I/O or parse failures. | [guide](filesystem.md#standard-output-and-error) | [example/test](../std/src/io/stdio/stdio_test.fl) |
| `std::io::stream` | Readable/writable interfaces, adapters, and stream copying | Interface references borrow endpoint storage; adapters preserve borrow lifetimes, and copying may block according to its endpoints. | [guide](filesystem.md#file-streams) | [example/test](../std/src/io/stream/stream_test.fl) |
| `std::math::bits` | Unsigned bit counting, rotation, reversal, and rounding | Pure, nonallocating, nonblocking, deterministic operations with checked overflow where a result may be unrepresentable. | [guide](math.md#bit-operations) | [example/test](../std/src/math/bits_test.fl) |
| `std::math::errors` | Arithmetic error family | Stable overflow and invalid-arithmetic variants; no allocation, blocking, or platform state. | [guide](math.md#checked-arithmetic) | [example/test](../std/src/math/integer_test.fl) |
| `std::math::float` | Floating-point classification | Pure and nonallocating; follows IEEE classification for supported target floats and is deterministic for the same bit pattern. | [guide](math.md#floating-point-classification) | [example/test](../std/src/math/float_test.fl) |
| `std::math::integer` | Integer predicates, powers, divisors, and overflow modes | Pure and nonallocating; checked, saturating, and wrapping behavior is named explicitly and target-width behavior follows the integer type. | [guide](math.md#integer-operations) | [example/test](../std/src/math/integer_test.fl) |
| `std::math::math` | Ordered numeric minimum, maximum, clamp, absolute, and sign | Pure, deterministic, nonallocating operations; caller must provide an ordered clamp range. | [guide](math.md#numeric-operations) | [example/test](../std/src/math/numeric_test.fl) |
| `std::memory::errors` | Safe slice operation errors | Stable bounds/range failures with no allocation, blocking, or partial mutation. | [guide](memory.md#slice-errors) | [example/test](../std/src/memory/slices_test.fl) |
| `std::memory::slices` | Typed slice copying, filling, and swapping | Borrows storage, does not allocate or block, preserves overlap semantics, and validates before mutation. | [guide](memory.md) | [example/test](../std/src/memory/slices_test.fl) |
| `std::memory::storage` | Initialized-prefix raw storage for owning containers | Owns allocation and explicit live-element state; unsafe internals are wrapped by lifecycle-preserving operations and allocation errors. | [guide](collections.md#initialized-prefix-storage) | [example/test](../std/src/memory/raw_storage_test.fl) |
| `std::net::address` | IPv4, IPv6, and portable socket addresses | Parsing/formatting is deterministic; owning text may allocate; no network access occurs and address-family errors are explicit. | [guide](network.md) | [example/test](../std/src/net/address_test.fl) |
| `std::net::dns` | Hostname and service resolution | Returns owning address storage, may allocate and block on host resolution, and exposes platform/network failures. | [guide](network.md#hostname-resolution) | [example/test](../cmake/release_smoke.cmake) |
| `std::net::errors` | Portable networking error family | Normalizes host socket and resolver failures while retaining explicit unsupported-family and address errors. | [guide](network.md) | [example/test](../cmake/release_smoke.cmake) |
| `std::net::tcp` | Blocking TCP streams and listeners | Owns native sockets; connect, accept, read, and write block; close/shutdown and all host failures are explicit. | [guide](network.md#blocking-tcp) | [example/test](../cmake/release_smoke.cmake) |
| `std::net::udp` | Blocking UDP sockets and datagrams | Owns a native socket; send/receive can block, datagram bounds are checked, and host behavior/errors are exposed portably. | [guide](network.md#blocking-udp) | [example/test](../cmake/release_smoke.cmake) |
| `std::os::arguments` | Process command-line argument capture | Allocates an owning string array, snapshots host arguments deterministically for the process, and reports allocation/conversion errors. | [guide](os.md) | [example/test](../std/src/os/arguments_test.fl) |
| `std::os::environment` | Process environment access and mutation | Reads allocate owning strings; operations affect process-global host state, may be platform-sensitive, and return explicit errors. | [guide](os.md#environment-variables) | [example/test](../std/src/os/environment_test.fl) |
| `std::os::errors` | Operating-system context errors | Stable argument/environment failures with platform details normalized at the runtime boundary. | [guide](os.md) | [example/test](../std/src/os/environment_test.fl) |
| `std::process::command` | Child-process commands, pipes, and exit status | Command values own arguments; spawn creates native resources, wait and pipe I/O block, and termination/platform failures are explicit. | [guide](process.md) | [example/test](../examples/workspace_guard/src/commands.fl) |
| `std::process::errors` | Child-process error family | Normalizes spawn, pipe, wait, and unsupported-host failures without hiding abnormal termination status. | [guide](process.md) | [example/test](../examples/workspace_guard/src/commands.fl) |
| `std::random::errors` | Randomness error family | Separates entropy-source and range failures; deterministic generator methods do not report host entropy errors. | [guide](random.md#errors) | [example/test](../std/src/random/secure_test.fl) |
| `std::random::generator` | Seeded deterministic pseudorandom generator | Owns only its small state, does not allocate or block for generation, and reproduces the documented sequence across supported targets. | [guide](random.md#deterministic-generator) | [example/test](../std/src/random/generator_test.fl) |
| `std::random::secure` | Host cryptographic entropy | Fills caller storage without allocation; may block at the OS boundary and reports unavailable/host entropy failures. | [guide](random.md#secure-bytes) | [example/test](../std/src/random/secure_test.fl) |
| `std::tests::asserts` | Typed test assertions | Borrows compared values, records failures in the test context, may allocate diagnostic text, and does not terminate the test by default. | [guide](testing.md) | [example/test](../std/src/tests/asserts_test.fl) |
| `std::tests::benchmark` | Deterministic benchmark sampling and summaries | Runs supplied operations repeatedly, allocates sample storage, uses the monotonic clock, and returns configuration/operation errors. | [guide](testing.md#deterministic-benchmarks) | [example/test](../std/src/tests/benchmark_test.fl) |
| `std::tests::fixtures` | Temporary resources and seeded test data | Temporary fixtures own cleanup responsibility and perform blocking host I/O; seeded data is deterministic; failures are explicit. | [guide](testing.md#temporary-fixtures-and-deterministic-data) | [example/test](../std/src/tests/fixtures_test.fl) |
| `std::tests::golden` | Golden text and byte assertions | Reads/writes files synchronously, makes update mode explicit, allocates diagnostics, and returns I/O or mismatch failures. | [guide](testing.md#golden-files) | [example/test](../std/src/tests/golden_test.fl) |
| `std::tests::testing` | Unit-test state and failure accounting | Owns test name/state, records deterministic counts, may allocate diagnostics, and exposes failures without hidden process exit. | [guide](testing.md) | [example/test](../std/src/tests/asserts_test.fl) |
| `std::text::ascii` | ASCII classification and case conversion | Pure byte-oriented operations with no allocation, errors, blocking, locale, or platform dependence. | [guide](text.md) | [example/test](../std/src/text/ascii_test.fl) |
| `std::text::errors` | Text parse, format, and UTF-8 errors | Stable typed failures for conversion and validation; declaring/propagating them does not allocate or block. | [guide](text.md) | [example/test](../std/src/text/utf8_test.fl) |
| `std::text::strings` | String building, search, trim, transform, replace, parse, and format | Views borrow input; owning transformations allocate; byte/ASCII semantics are explicit; parsing and formatting return errors. | [guide](text.md) | [example/test](../std/src/text/strings_test.fl) |
| `std::text::utf8` | UTF-8 validation, decoding, counting, and encoding | Borrows byte-oriented strings, does not allocate, is deterministic, and reports exact malformed/truncated input errors. | [guide](text.md) | [example/test](../std/src/text/utf8_test.fl) |
| `std::time::clock` | Monotonic instants, wall timestamps, and sleep | Clock reads use host state; sleep blocks; elapsed monotonic durations are stable while wall time and precision are platform-dependent. | [guide](time.md) | [example/test](../std/src/time/clock_test.fl) |
| `std::time::duration` | Normalized signed time durations | Value operations do not allocate or block; normalization and checked arithmetic are deterministic across targets. | [guide](time.md#duration) | [example/test](../std/src/time/duration_test.fl) |
| `std::time::errors` | Clock, duration, and sleep errors | Stable overflow, range, and host-clock failures with no hidden fallback behavior. | [guide](time.md#errors) | [example/test](../std/src/time/clock_test.fl) |

## Reading the declaration appendix

`export` declarations are listed exactly as public top-level declarations.
Public methods belonging to exported types follow them. Function bodies are
intentionally omitted; enum variants and public structure fields remain visible.
Private implementation details and native runtime symbols are not public API.

## Declaration appendix

<a id="std-bytes-buffer"></a>
### `std::bytes::buffer`

Growable owning byte storage.

Sources: [`std/src/bytes/buffer.fl`](../std/src/bytes/buffer.fl)

```flecha
export error BufferError {
    // The requested operation requires at least one stored byte.
    Empty,
}

// Owns a growable sequence of initialized bytes.
export structure Buffer {
};

// Public methods
(Buffer, error) Buffer::withCapacity(usize capacity);
usize Buffer::len(&const self);
usize Buffer::capacity(&const self);
error Buffer::reserve(&self, usize capacity);
bool Buffer::empty(&const self);
[]const u8 Buffer::bytes(&const self) <-self;
void Buffer::clear(&self);
void Buffer::truncate(&self, usize length);
error Buffer::resize(&self, usize length, u8 fill);
(u8, BufferError) Buffer::popByte(&self);
error Buffer::write(&self, []const u8 source);
error Buffer::writeByte(&self, u8 value);
error Buffer::writeString(&self, stringv value);
```

<a id="std-bytes-sequence"></a>
### `std::bytes::sequence`

Byte equality and lexicographic comparison.

Sources: [`std/src/bytes/sequence.fl`](../std/src/bytes/sequence.fl)

```flecha
export bool equal([]const u8 left, []const u8 right);
export i32 compare_lexicographic([]const u8 left, []const u8 right);
export bool starts_with([]const u8 value, []const u8 prefix);
export bool ends_with([]const u8 value, []const u8 suffix);
export (usize, bool) index_of_byte([]const u8 value, u8 target);
export (usize, bool) index_of([]const u8 haystack, []const u8 needle);
export bool constant_time_equal([]const u8 left, []const u8 right);
export void zero_bytes([]u8 value);
```

<a id="std-containers-collections"></a>
### `std::containers::collections`

Owning collections and collection algorithms.

Sources: [`std/src/containers/algorithms.fl`](../std/src/containers/algorithms.fl), [`std/src/containers/array.fl`](../std/src/containers/array.fl), [`std/src/containers/binary_heap.fl`](../std/src/containers/binary_heap.fl), [`std/src/containers/deque.fl`](../std/src/containers/deque.fl), [`std/src/containers/hash_map.fl`](../std/src/containers/hash_map.fl), [`std/src/containers/hash_set.fl`](../std/src/containers/hash_set.fl), [`std/src/containers/queue.fl`](../std/src/containers/queue.fl), [`std/src/containers/stack.fl`](../std/src/containers/stack.fl)

```flecha
export func Action<T>(&const T) => ();
export func Less<T>(&const T left, &const T right) => (bool);
export func Predicate<T>(&const T value) => (bool);
export func Folder<T, A>(A accumulator, &const T value) => (A);
export bool any_of<T>([]const T values, Predicate<T> predicate);
export bool all_of<T>([]const T values, Predicate<T> predicate);
export usize count_if<T>([]const T values, Predicate<T> predicate);
export A fold<T, A>([]const T values, A initial, Folder<T, A> folder);
export (usize, bool) find_if<T>([]const T values, Predicate<T> predicate);
export usize lower_bound_by<T>([]const T values, &const T needle, Less<T> less,);
export usize upper_bound_by<T>([]const T values, &const T needle, Less<T> less,);
export (usize, bool) binary_search_by<T>([]const T values, &const T needle, Less<T> less,);
export usize lower_bound<T>([]const T values, &const T needle);
export usize upper_bound<T>([]const T values, &const T needle);
export (usize, bool) binary_search<T>([]const T values, &const T needle,);
export (usize, bool) index_of<T>([]const T values, T needle);
export bool contains<T>([]const T values, T needle);
export void reverse<T>([]T values);
export void fill<T>([]T values, T value);
export void for_each<T>([]const T values, Action<T> action);
export usize partition_by<T>([]T values, Predicate<T> predicate);
export usize dedup_by<T>([]T values, Equal<T> equal);
export usize dedup<T>([]T values);
export void sort_by<T>([]T values, Less<T> less);
export void sort<T>([]T values);
export structure Array<T> {
};
export structure BinaryHeap<T> {
};
export structure Deque<T> {
};
export func Hash<K>(&const K key) => (usize);
export func Equal<K>(&const K left, &const K right) => (bool);
export func HashMapAction<K, V>(&const K key, &const V value) => ();
export structure HashMap<K, V> {
};
export func HashSetAction<T>(&const T value) => ();
export structure HashSet<T> {
};
export structure Queue<T> {
};
export structure Stack<T> {
};

// Public methods
(Array<T>, error) Array<T>::fromSlice([]const T source);
usize Array<T>::len(&const self);
usize Array<T>::capacity(&const self);
error Array<T>::reserve(&self, usize capacity);
error Array<T>::push(&self, T value);
(usize, bool) Array<T>::indexOf(&const self, T value);
bool Array<T>::contains(&const self, T value);
void Array<T>::reverse(&self);
void Array<T>::fill(&self, T value);
error Array<T>::append(&self, []const T source);
(Array<T>, error) Array<T>::clone(&const self);
(T, error) Array<T>::get(&const self, usize index);
error Array<T>::set(&self, usize index, T value);
(T, error) Array<T>::remove(&self, usize index);
(T, error) Array<T>::swapRemove(&self, usize index);
(T, error) Array<T>::pop(&self);
[]const T Array<T>::view(&const self) <-self;
[]T Array<T>::viewRef(&self) <-self;
bool Array<T>::empty(&const self);
(T, error) Array<T>::first(&const self);
(T, error) Array<T>::last(&const self);
void Array<T>::clear(&self);
void Array<T>::sortBy(&self, Less<T> less);
void Array<T>::sort(&self);
usize Array<T>::partitionBy(&self, Predicate<T> predicate);
usize Array<T>::dedupBy(&self, Equal<T> equal);
usize Array<T>::dedup(&self);
void Array<T>::forEach(&const self, Action<T> action);
(usize, bool) Array<T>::findIf(&const self, Predicate<T> predicate);
usize Array<T>::lowerBoundBy(&const self, &const T needle, Less<T> less);
usize Array<T>::upperBoundBy(&const self, &const T needle, Less<T> less);
(usize, bool) Array<T>::binarySearchBy(&const self, &const T needle, Less<T> less,);
usize Array<T>::lowerBound(&const self, &const T needle);
usize Array<T>::upperBound(&const self, &const T needle);
(usize, bool) Array<T>::binarySearch(&const self, &const T needle);
bool Array<T>::anyOf(&const self, Predicate<T> predicate);
bool Array<T>::allOf(&const self, Predicate<T> predicate);
usize Array<T>::countIf(&const self, Predicate<T> predicate);
BinaryHeap<T> BinaryHeap<T>::zero() override;
BinaryHeap<T> BinaryHeap<T>::init(Less<T> less) <-less override;
void BinaryHeap<T>::tear(&self) override;
error BinaryHeap<T>::reserve(&self, usize capacity);
error BinaryHeap<T>::push(&self, T value);
(T, error) BinaryHeap<T>::top(&const self);
(T, error) BinaryHeap<T>::pop(&self);
void BinaryHeap<T>::clear(&self);
usize BinaryHeap<T>::len(&const self);
usize BinaryHeap<T>::capacity(&const self);
bool BinaryHeap<T>::empty(&const self);
Deque<T> Deque<T>::init() override;
Deque<T> Deque<T>::zero() override;
void Deque<T>::tear(&self) override;
error Deque<T>::reserve(&self, usize capacity);
(T, error) Deque<T>::get(&const self, usize index);
error Deque<T>::pushBack(&self, T value);
error Deque<T>::pushFront(&self, T value);
(T, error) Deque<T>::popBack(&self);
(T, error) Deque<T>::popFront(&self);
void Deque<T>::clear(&self);
(T, error) Deque<T>::front(&const self);
(T, error) Deque<T>::back(&const self);
error Deque<T>::set(&self, usize index, T value);
usize Deque<T>::len(&const self);
usize Deque<T>::capacity(&const self);
bool Deque<T>::empty(&const self);
HashMap<K, V> HashMap<K, V>::zero() override;
HashMap<K, V> HashMap<K, V>::init() override;
HashMap<K, V> HashMap<K, V>::withHash(self, Hash<K> hash) <-...args;
HashMap<K, V> HashMap<K, V>::withEqual(self, Equal<K> equal) <-...args;
unsafe void HashMap<K, V>::replaceHashPolicy(&self, Hash<K> hash,);
unsafe void HashMap<K, V>::replaceEqualPolicy(&self, Equal<K> equal,);
void HashMap<K, V>::tear(&self) override;
usize HashMap<K, V>::len(&const self);
usize HashMap<K, V>::capacity(&const self);
bool HashMap<K, V>::empty(&const self);
void HashMap<K, V>::forEach(&const self, HashMapAction<K, V> action);
error HashMap<K, V>::reserve(&self, usize capacity);
error HashMap<K, V>::set(&self, K key, V value);
(V, error) HashMap<K, V>::get(&const self, &const K key);
bool HashMap<K, V>::containsKey(&const self, &const K key);
(V, error) HashMap<K, V>::remove(&self, &const K key);
void HashMap<K, V>::clear(&self);
HashSet<T> HashSet<T>::zero() override;
HashSet<T> HashSet<T>::init() override;
HashSet<T> HashSet<T>::withHash(self, Hash<T> hash) <-...args;
HashSet<T> HashSet<T>::withEqual(self, Equal<T> equal) <-...args;
usize HashSet<T>::len(&const self);
usize HashSet<T>::capacity(&const self);
bool HashSet<T>::empty(&const self);
void HashSet<T>::forEach(&const self, HashSetAction<T> action);
error HashSet<T>::reserve(&self, usize capacity);
error HashSet<T>::insert(&self, T value);
bool HashSet<T>::contains(&const self, &const T value);
error HashSet<T>::erase(&self, &const T value);
void HashSet<T>::clear(&self);
error Queue<T>::enqueue(&self, T value);
(T, error) Queue<T>::dequeue(&self);
(T, error) Queue<T>::front(&const self);
void Queue<T>::clear(&self);
usize Queue<T>::len(&const self);
usize Queue<T>::capacity(&const self);
bool Queue<T>::empty(&const self);
error Queue<T>::reserve(&self, usize capacity);
error Stack<T>::push(&self, T value);
(T, error) Stack<T>::pop(&self);
(T, error) Stack<T>::top(&const self);
void Stack<T>::clear(&self);
usize Stack<T>::len(&const self);
usize Stack<T>::capacity(&const self);
bool Stack<T>::empty(&const self);
error Stack<T>::reserve(&self, usize capacity);
```

<a id="std-containers-errors"></a>
### `std::containers::errors`

Collection error families.

Sources: [`std/src/containers/errors.fl`](../std/src/containers/errors.fl)

```flecha
export error ArrayError {
    Empty,
    IndexOutOfBounds
}
export error DequeError {
    Empty,
    IndexOutOfBounds
}
export error StackError {
    Empty
}
export error QueueError {
    Empty
}
export error HeapError {
    Empty
}

// Reports failures from hash-map key lookup and removal.
export error HashMapError {
    // No entry has a key equal to the requested key.
    KeyNotFound
}
```

<a id="std-containers-raw_array"></a>
### `std::containers::raw_array`

Low-level compatibility owning array.

Sources: [`std/src/containers/raw_array.fl`](../std/src/containers/raw_array.fl)

```flecha
export structure RawArray<T> {
};

// Public methods
RawArray<T> RawArray<T>::init() override;
void RawArray<T>::tear(&self) override;
usize RawArray<T>::len(&const self);
usize RawArray<T>::capacity(&const self);
unsafe error RawArray<T>::reserve(&self, usize capacity);
unsafe error RawArray<T>::push(&self, T value);
unsafe (T, error) RawArray<T>::get(&const self, usize index);
unsafe error RawArray<T>::set(&self, usize index, T value);
unsafe (T, error) RawArray<T>::pop(&self);
unsafe []const T RawArray<T>::view(&const self) <-self;
unsafe []T RawArray<T>::viewRef(&self) <-self;
unsafe (T, error) RawArray<T>::remove(&self, usize index);
unsafe (T, error) RawArray<T>::swapRemove(&self, usize index);
void RawArray<T>::clear(&self);
void RawArray<T>::reverse(&self);
unsafe void RawArray<T>::swap(&self, usize left, usize right);
unsafe []const T RawArray<T>::viewAt(&const self, usize index) <-self;
unsafe void RawArray<T>::destroyAt(&self, usize index);
unsafe void RawArray<T>::relocate(&self, usize destination, usize source,);
unsafe void RawArray<T>::commitLength(&self, usize length);
```

<a id="std-cryptography-hmac"></a>
### `std::cryptography::hmac`

Streaming and one-shot HMAC-SHA-2.

Sources: [`std/src/cryptography/hmac.fl`](../std/src/cryptography/hmac.fl)

```flecha
export error HmacError {
    NotInitialized
}
export structure HmacSha256Tag {
};
export structure HmacSha512Tag {
};
export structure HmacSha256 {
};
export structure HmacSha512 {
};
export (HmacSha256Tag, error) sha256([]const u8 key, []const u8 message);
export (HmacSha512Tag, error) sha512([]const u8 key, []const u8 message);
```

<a id="std-cryptography-sha2"></a>
### `std::cryptography::sha2`

Streaming and one-shot SHA-256/SHA-512.

Sources: [`std/src/cryptography/sha2.fl`](../std/src/cryptography/sha2.fl)

```flecha
export error Sha2Error {
    AlreadyFinalized,
    MessageTooLong
}
export structure Sha256Digest {
};
export structure Sha256 {
};
export structure Sha512Digest {
};
export structure Sha512 {
};
export (Sha256Digest, Sha2Error) sha256([]const u8 input);
export (Sha256Digest, Sha2Error) sha256(stringv input);
export (Sha512Digest, Sha2Error) sha512([]const u8 input);
export (Sha512Digest, Sha2Error) sha512(stringv input);

// Public methods
Sha256Digest Sha256Digest::init([32]u8 value) override;
[]const u8 Sha256Digest::bytes(&const self) <-self;
Sha512Digest Sha512Digest::init([64]u8 value) override;
[]const u8 Sha512Digest::bytes(&const self) <-self;
```

<a id="std-encoding-base64"></a>
### `std::encoding::base64`

Canonical padded Base64 encoding.

Sources: [`std/src/encoding/base64.fl`](../std/src/encoding/base64.fl)

```flecha
export error Base64Error {
    // The required output length cannot be represented by usize.
    Overflow,

    // The source is not canonical padded RFC 4648 Base64.
    InvalidEncoding,
}

// Returns the padded RFC 4648 character count, or Base64Error.Overflow.
export (usize, Base64Error) encoded_length(usize byteCount);
export (usize, Base64Error) decoded_length([]const u8 source);
export (string, error) encode(stringv source);
export (string, error) encode_bytes([]const u8 source);
export (own []u8, error) encode_byte_to_byte([]const u8 source);
export (own []u8, error) encode_to_bytes(stringv source);
export (string, error) decode(stringv source);
export (string, error) decode_from_bytes([]const u8 source);
export (own []u8, error) decode_byte_to_byte([]const u8 source);
export (own []u8, error) decode_to_bytes(stringv source);
```

<a id="std-encoding-binary"></a>
### `std::encoding::binary`

Endian primitives, bounded readers/writers, and varints.

Sources: [`std/src/encoding/binary.fl`](../std/src/encoding/binary.fl), [`std/src/encoding/binary_buffer.fl`](../std/src/encoding/binary_buffer.fl), [`std/src/encoding/binary_reader.fl`](../std/src/encoding/binary_reader.fl), [`std/src/encoding/binary_varint.fl`](../std/src/encoding/binary_varint.fl), [`std/src/encoding/binary_writer.fl`](../std/src/encoding/binary_writer.fl)

```flecha
export error BinaryError {
    // The source ended before a complete value was available.
    UnexpectedEnd,

    // The destination cannot hold the complete encoded value.
    InsufficientSpace,

    // The decoded or calculated value cannot be represented by its type.
    Overflow,

    // The input is structurally valid bytes but not a canonical encoding.
    InvalidEncoding,
}

// Contains fixed-width unsigned types supported by binary encoders.
export typeset fixed_unsigned_integer {
    u8,
    u16,
    u32,
    u64,
};
export (T, BinaryError) read_little<T: fixed_unsigned_integer>([]const u8 source);
export (T, BinaryError) read_big<T: fixed_unsigned_integer>([]const u8 source);
export BinaryError write_little<T: fixed_unsigned_integer>([]u8 destination, T value);
export BinaryError write_big<T: fixed_unsigned_integer>([]u8 destination, T value);
export error append_little<T: fixed_unsigned_integer>(&buffer::Buffer destination, T value);
export error append_big<T: fixed_unsigned_integer>(&buffer::Buffer destination, T value);
export error append_varint<T: fixed_unsigned_integer>(&buffer::Buffer destination, T value);
export structure Reader {
};
export usize varint_length<T: fixed_unsigned_integer>(T value);
export (usize, BinaryError) write_varint<T: fixed_unsigned_integer >([]u8 destination, T value);
export (T, usize, BinaryError) read_varint<T: fixed_unsigned_integer >([]const u8 source);
export structure Writer {
};

// Public methods
Reader Reader::init([]const u8 source) <-source override;
usize Reader::position(&const self);
usize Reader::len(&const self);
usize Reader::remaining(&const self);
bool Reader::empty(&const self);
(u8, BinaryError) Reader::readByte(&self);
([]const u8, BinaryError) Reader::read(&self, usize count) <-self;
(u8, BinaryError) Reader::peekByte(&const self);
BinaryError Reader::skip(&self, usize count);
[]const u8 Reader::remainingBytes(&const self) <-self;
([]const u8, BinaryError) Reader::readRemaining(&self) <-self;
void Reader::reset(&self);
Writer Writer::init([]u8 destination) <-destination override;
usize Writer::position(&const self);
usize Writer::len(&const self);
usize Writer::remaining(&const self);
bool Writer::full(&const self);
[]const u8 Writer::writtenBytes(&const self) <-self;
BinaryError Writer::skip(&self, usize count);
void Writer::reset(&self);
BinaryError Writer::writeByte(&self, u8 value);
BinaryError Writer::writeString(&self, stringv value);
BinaryError Writer::write(&self, []const u8 source);
```

<a id="std-encoding-hex"></a>
### `std::encoding::hex`

Canonical hexadecimal encoding.

Sources: [`std/src/encoding/hex.fl`](../std/src/encoding/hex.fl)

```flecha
export error HexError {
    // The required output length cannot be represented by usize.
    Overflow,

    // The source has odd length or contains a non-hexadecimal byte.
    InvalidEncoding,
}

// Returns twice byteCount, or HexError.Overflow.
export (usize, HexError) encoded_length(usize byteCount);
export (usize, HexError) decoded_length([]const u8 source);
export (string, error) encode(stringv source);
export (string, error) encode_bytes([]const u8 source);
export (own []u8, error) encode_byte_to_byte([]const u8 source);
export (own []u8, error) encode_to_bytes(stringv source);
export (string, error) encode_upper(stringv source);
export (string, error) encode_upper_bytes([]const u8 source);
export (own []u8, error) encode_upper_byte_to_byte([]const u8 source);
export (own []u8, error) encode_upper_to_bytes(stringv source);
export (string, error) decode(stringv source);
export (string, error) decode_from_bytes([]const u8 source);
export (own []u8, error) decode_byte_to_byte([]const u8 source);
export (own []u8, error) decode_to_bytes(stringv source);
```

<a id="std-fmt"></a>
### `std::fmt`

Typed formatting and console output facade.

Sources: [`std/src/fmt/fmt.fl`](../std/src/fmt/fmt.fl)

```flecha
export void print(values...);
export void println(values...);
export void printf(stringv format, values...);
export void eprint(values...);
export void eprintln(values...);
export void eprintf(stringv format, values...);
export (string, error) sprintf(stringv format, values...);
export string sprintf!(stringv format, values...);
```

<a id="std-hashing-hashing"></a>
### `std::hashing::hashing`

Structural non-cryptographic hashing.

Sources: [`std/src/hashing/hashing.fl`](../std/src/hashing/hashing.fl)

```flecha
export usize hash<T>(&const T value);
export usize hash_string(stringv value);
```

<a id="std-io-buffered"></a>
### `std::io::buffered`

Buffered reader and writer adapters.

Sources: [`std/src/io/buffered/reader.fl`](../std/src/io/buffered/reader.fl), [`std/src/io/buffered/writer.fl`](../std/src/io/buffered/writer.fl)

```flecha
export structure BufferedReader << stream::Readable {
};
export structure BufferedWriter << stream::Writable {
};

// Public methods
(BufferedReader, error) BufferedReader::withCapacity(&stream::Readable source, usize capacity,) <-source;
(usize, error) BufferedReader::read(&self, []u8 destination) override;
(BufferedWriter, error) BufferedWriter::withCapacity(&stream::Writable sink, usize capacity,) <-sink;
(usize, error) BufferedWriter::write(&self, []const u8 source) override;
error BufferedWriter::flush(&self);
```

<a id="std-io-errors"></a>
### `std::io::errors`

Portable I/O error family.

Sources: [`std/src/io/errors.fl`](../std/src/io/errors.fl)

```flecha
export error IoError {
    // The requested path does not exist.
    NotFound,

    // The operating system denied the requested access.
    PermissionDenied,

    // Creation required an absent path, but an entry already exists there.
    AlreadyExists,

    // The supplied path cannot be used by the platform.
    InvalidPath,

    // A combination of operation options is inconsistent.
    InvalidOptions,

    // The operation requires an open resource.
    Closed,

    // A file could not be opened for an otherwise unclassified reason.
    OpenFailed,

    // A file could not be closed cleanly.
    CloseFailed,

    // A read operation failed.
    ReadFailed,

    // A write operation failed.
    WriteFailed,

    // A seek operation failed.
    SeekFailed,

    // Buffered data could not be flushed.
    FlushFailed,

    // A size cannot be represented or supported by the operation.
    TooLarge,

    // A complete requested value could not be read before end of input.
    UnexpectedEof,

    // Path existence could not be determined.
    ExistsFailed,

    // A file could not be removed.
    RemoveFailed,

    // A path could not be renamed.
    RenameFailed,

    // A directory could not be created.
    CreateDirectoryFailed,

    // A directory could not be removed.
    RemoveDirectoryFailed,

    // A nonempty directory was passed to a nonrecursive removal.
    DirectoryNotEmpty,

    // Path metadata could not be read.
    MetadataFailed,

    // The encountered filesystem entry kind is unsupported by the operation.
    UnsupportedFileType,
}
```

<a id="std-io-file"></a>
### `std::io::file`

Files, directories, and temporary resources.

Sources: [`std/src/io/file/directory.fl`](../std/src/io/file/directory.fl), [`std/src/io/file/file.fl`](../std/src/io/file/file.fl), [`std/src/io/file/temporary_directory.fl`](../std/src/io/file/temporary_directory.fl), [`std/src/io/file/temporary_file.fl`](../std/src/io/file/temporary_file.fl)

```flecha
export enum EntryType: u8 {
    // Represents a filesystem entry of an unsupported or unknown kind.
    default Other,

    // Represents a regular file.
    File,

    // Represents a directory.
    Directory,

    // Represents a POSIX symbolic link or Windows reparse-point link.
    SymbolicLink,
}
export structure DirectoryEntry {
    // The owned entry name.
    string entryName;

    // The entry classification captured during iteration.
    EntryType entryKind;
};
export structure Directory {
};
export enum SeekFrom: u8 {
    // Measures the offset from the beginning of the file.
    default Start,

    // Measures the offset from the current file position.
    Current,

    // Measures the offset from the end of the file.
    End
}

// Returns the runtime representation of this seek origin.
export structure OpenOptions {
    // Permits read operations.
    bool read;

    // Permits write operations.
    bool write;

    // Directs writes to the end of the file.
    bool append;

    // Truncates an existing file when it is opened.
    bool truncate;

    // Creates the file when it does not exist.
    bool create;

    // Creates a new file and fails if the path already exists.
    bool createNew;
};
export structure File << stream::Readable, stream::Writable {
};
export structure TemporaryDirectory {
};
export structure TemporaryFile {
};

// Public methods
EntryType EntryType::fromNumber(u8 number);
DirectoryEntry DirectoryEntry::zero() override;
stringv DirectoryEntry::name(&const self) <-self;
EntryType DirectoryEntry::kind(&const self);
void Directory::tear(&self) override;
Directory Directory::zero() override;
(Directory, error) Directory::open(stringv path);
bool Directory::isOpen(&const self);
(DirectoryEntry, bool, error) Directory::next(&self);
error Directory::close(&self);
u8 SeekFrom::number(&const self);
OpenOptions OpenOptions::readOnly();
OpenOptions OpenOptions::writeOnly();
OpenOptions OpenOptions::appendOnly();
void File::tear(&self) override;
File File::zero() override;
File File::fromHandle(*void handle);
(File, error) File::open(stringv path, OpenOptions opts);
bool File::isOpen(&const self);
(usize, error) File::read(&self, []u8 destination) override;
&stream::Readable File::reader(&self) <-self;
(usize, error) File::write(&self, []const u8 source) override;
&stream::Writable File::writer(&self) <-self;
(usize, error) File::writeString(&self, stringv source);
error File::writeAllString(&self, stringv source);
(u64, error) File::seek(&self, i64 offset, SeekFrom origin);
error File::flush(&self);
error File::close(&self);
void TemporaryDirectory::tear(&self) override;
(TemporaryDirectory, error) TemporaryDirectory::create();
stringv TemporaryDirectory::path(&const self) <-self;
error TemporaryDirectory::remove(&self);
void TemporaryFile::tear(&self) override;
TemporaryFile TemporaryFile::zero() override;
(TemporaryFile, error) TemporaryFile::create();
&File TemporaryFile::file(&self) <-self;
stringv TemporaryFile::path(&const self) <-self;
bool TemporaryFile::isOpen(&const self);
error TemporaryFile::remove(&self);
```

<a id="std-io-filesystem"></a>
### `std::io::filesystem`

Whole-file and filesystem operations.

Sources: [`std/src/io/filesystem/filesystem.fl`](../std/src/io/filesystem/filesystem.fl)

```flecha
export structure Metadata {
    // The classified entry kind.
    file::EntryType type;

    // The regular-file byte length, or zero for other entry kinds.
    u64 size;
};
export (string, error) read_file(stringv path);
export error write_file(stringv path, stringv contents);
export error write_bytes(stringv path, []const u8 contents);
export (bool, error) exists(stringv path);
export (Metadata, error) metadata(stringv path);
export error remove_file(stringv path);
export error rename(stringv source, stringv destination);
export error create_directory(stringv path);
export error create_directories(stringv value);
export error copy_file(stringv source, stringv destination);
export error copy_directory(stringv source, stringv destination);
export error remove_directory(stringv path, bool force);
```

<a id="std-io-path"></a>
### `std::io::path`

Lexical POSIX and Windows path manipulation.

Sources: [`std/src/io/path/path.fl`](../std/src/io/path/path.fl)

```flecha
export (string, error) join(stringv base, stringv child);
export (string, error) normalize(stringv value);
export stringv file_name(stringv value) <-value;
export stringv parent(stringv value) <-value;
export stringv extension(stringv value) <-value;
```

<a id="std-io-stdio"></a>
### `std::io::stdio`

Standard input, output, and error streams.

Sources: [`std/src/io/stdio/stdio.fl`](../std/src/io/stdio/stdio.fl)

```flecha
export structure StandardOutput {};
export structure StandardError {};
export structure StandardInput {};
export StandardOutput standard_output();
export StandardError standard_error();
export StandardInput standard_input();

// Public methods
(usize, error) StandardInput::read(&const self, []u8 destination);
stream::Reader StandardInput::reader(&const self);
(usize, error) StandardOutput::write(&const self, stringv source);
error StandardOutput::writeAll(&const self, stringv source);
(usize, error) StandardOutput::writeBytes(&const self, []const u8 source);
error StandardOutput::writeAllBytes(&const self, []const u8 source);
error StandardOutput::flush(&const self);
stream::Writer StandardOutput::writer(&const self);
(usize, error) StandardError::write(&const self, stringv source);
error StandardError::writeAll(&const self, stringv source);
(usize, error) StandardError::writeBytes(&const self, []const u8 source);
error StandardError::writeAllBytes(&const self, []const u8 source);
error StandardError::flush(&const self);
stream::Writer StandardError::writer(&const self);
```

<a id="std-io-stream"></a>
### `std::io::stream`

Readable/writable interfaces, adapters, and stream copying.

Sources: [`std/src/io/stream/readable.fl`](../std/src/io/stream/readable.fl), [`std/src/io/stream/reader.fl`](../std/src/io/stream/reader.fl), [`std/src/io/stream/stream.fl`](../std/src/io/stream/stream.fl), [`std/src/io/stream/writable.fl`](../std/src/io/stream/writable.fl), [`std/src/io/stream/writer.fl`](../std/src/io/stream/writer.fl)

```flecha
export interface Readable {
    (usize, error) read(&self, []u8 destination) virtual;
    error readExact(&self, []u8 destination) default;
};
export func Read([]u8 destination) => (usize, error);
export structure Reader << Readable {
};
export (usize, error) copy(&Readable reader, &Writable writer, []u8 buffer);
export interface Writable {
    (usize, error) write(&self, []const u8 source) virtual;
    error writeAll(&self, []const u8 source) default;
};
export func Write([]const u8 source) => (usize, error);
export structure Writer << Writable {
};

// Public methods
error Readable::readExact(&self, []u8 destination);
Reader Reader::init(Read operation) <- operation override;
Reader Reader::zero() override;
(usize, error) Reader::read(&self, []u8 destination) override;
error Writable::writeAll(&self, []const u8 source);
Writer Writer::init(Write operation) <- operation override;
Writer Writer::zero() override;
(usize, error) Writer::write(&self, []const u8 source) override;
```

<a id="std-math-bits"></a>
### `std::math::bits`

Unsigned bit counting, rotation, reversal, and rounding.

Sources: [`std/src/math/bits.fl`](../std/src/math/bits.fl)

```flecha
export usize width<T: unsigned_integer>();
export usize count_ones<T: unsigned_integer>(T value);
export usize count_zeros<T: unsigned_integer>(T value);
export usize leading_zeros<T: unsigned_integer>(T value);
export usize trailing_zeros<T: unsigned_integer>(T value);
export T rotate_left<T: unsigned_integer>(T value, usize count);
export T rotate_right<T: unsigned_integer>(T value, usize count);
export T swap_bytes<T: unsigned_integer>(T value);
export T reverse<T: unsigned_integer>(T value);
export bool is_power_of_two<T: unsigned_integer>(T value);
export T highest_one<T: unsigned_integer>(T value);
export T lowest_one<T: unsigned_integer>(T value);
export T bit_floor<T: unsigned_integer>(T value);
export (T, errors::ArithmeticError) checked_bit_ceil<T: unsigned_integer>(T value);
```

<a id="std-math-errors"></a>
### `std::math::errors`

Arithmetic error family.

Sources: [`std/src/math/errors.fl`](../std/src/math/errors.fl)

```flecha
export error ArithmeticError {
    // The mathematical result lies outside the destination integer type.
    Overflow,

    // Integer division was requested with a zero divisor.
    DivisionByZero,
}
```

<a id="std-math-float"></a>
### `std::math::float`

Floating-point classification.

Sources: [`std/src/math/float.fl`](../std/src/math/float.fl)

```flecha
export bool is_nan<T: floating_point>(T value);
export bool is_infinite<T: floating_point>(T value);
export bool is_finite<T: floating_point>(T value);
export bool is_normal<T: floating_point>(T value);
export bool is_subnormal<T: floating_point>(T value);
```

<a id="std-math-integer"></a>
### `std::math::integer`

Integer predicates, powers, divisors, and overflow modes.

Sources: [`std/src/math/integer.fl`](../std/src/math/integer.fl)

```flecha
export bool is_even<T: integer>(T value);
export bool is_odd<T: integer>(T value);
export bool is_multiple_of<T: integer>(T value, T divisor);
export T pow<T: integer>(T base, usize exponent);
export T gcd<T: integer>(T left, T right);
export T lcm<T: integer>(T left, T right);
export (T, errors::ArithmeticError) checked_add<T: integer>(T left, T right);
export (T, errors::ArithmeticError) checked_sub<T: integer>(T left, T right);
export (T, errors::ArithmeticError) checked_mul<T: integer>(T left, T right);
export (T, errors::ArithmeticError) checked_div<T: integer>(T left, T right);
export T saturating_add<T: integer>(T left, T right);
export T saturating_sub<T: integer>(T left, T right);
export T saturating_mul<T: integer>(T left, T right);
export T wrapping_add<T: integer>(T left, T right);
export T wrapping_sub<T: integer>(T left, T right);
export T wrapping_mul<T: integer>(T left, T right);
```

<a id="std-math-math"></a>
### `std::math::math`

Ordered numeric minimum, maximum, clamp, absolute, and sign.

Sources: [`std/src/math/numeric.fl`](../std/src/math/numeric.fl)

```flecha
export T min<T: numeric>(T value, T other);
export T max<T: numeric>(T value, T other);
export (T, T) min_max<T: numeric>(T value, T other);
export T clamp<T: numeric>(T value, T low, T high);
export T abs<T: numeric>(T value);
export T sign<T: numeric>(T value);
```

<a id="std-memory-errors"></a>
### `std::memory::errors`

Safe slice operation errors.

Sources: [`std/src/memory/errors.fl`](../std/src/memory/errors.fl)

```flecha
export error SliceError {
    // An index or resulting range lies outside its slice.
    IndexOutOfBounds,

    // A half-open range has its start after its end.
    InvalidRange,
}
```

<a id="std-memory-slices"></a>
### `std::memory::slices`

Typed slice copying, filling, and swapping.

Sources: [`std/src/memory/slices.fl`](../std/src/memory/slices.fl)

```flecha
export usize copy_elements<T>([]T destination, []const T source);
export mem_errors::SliceError copy_within<T>([]T values, usize sourceStart, usize sourceEnd, usize destinationStart,);
export void fill<T>([]T values, T value);
export mem_errors::SliceError swap<T>([]T values, usize left, usize right);
```

<a id="std-memory-storage"></a>
### `std::memory::storage`

Initialized-prefix raw storage for owning containers.

Sources: [`std/src/memory/raw_storage.fl`](../std/src/memory/raw_storage.fl)

```flecha
export structure RawStorage<T> {
};

// Public methods
RawStorage<T> RawStorage<T>::init() override;
RawStorage<T> RawStorage<T>::zero() override;
void RawStorage<T>::tear(&self) override;
unsafe (RawStorage<T>, error) RawStorage<T>::withCapacity(usize capacity);
usize RawStorage<T>::capacity(&const self);
unsafe void RawStorage<T>::initialize(&self, usize index, T value);
unsafe void RawStorage<T>::relocate(&self, usize destination, usize source,);
unsafe void RawStorage<T>::relocateFrom(&self, usize destination, &RawStorage<T> source, usize sourceIndex,);
unsafe T RawStorage<T>::extract(&self, usize index);
unsafe void RawStorage<T>::swap(&self, usize left, usize right);
unsafe void RawStorage<T>::destroy(&self, usize index);
unsafe T RawStorage<T>::get(&const self, usize index);
unsafe []const T RawStorage<T>::view(&const self, usize start, usize end,) <-self;
unsafe []T RawStorage<T>::viewRef(&self, usize start, usize end,) <-self;
```

<a id="std-net-address"></a>
### `std::net::address`

IPv4, IPv6, and portable socket addresses.

Sources: [`std/src/net/ipv4.fl`](../std/src/net/ipv4.fl), [`std/src/net/ipv6.fl`](../std/src/net/ipv6.fl), [`std/src/net/socket_address.fl`](../std/src/net/socket_address.fl)

```flecha
export structure Ipv4Address {
};
export const usize MAX_IPV6_STRING_LENGTH = 39;
export structure Ipv6Address {
};
export structure Ipv4SocketAddress {
};
export structure Ipv6SocketAddress {
};
export enum AddressFamily: u8 {
    default Ipv4,
    Ipv6,
}

// Stores either a portable IPv4 or IPv6 socket address without exposing a
// platform-native socket structure.
export structure SocketAddress {
};

// Public methods
Ipv4Address Ipv4Address::loopback();
Ipv4Address Ipv4Address::wildcard();
Ipv4Address Ipv4Address::fromOctets(u8 octet1, u8 octet2, u8 octet3, u8 octet4);
(Ipv4Address, error) Ipv4Address::parse(stringv text);
(string, error) Ipv4Address::toString(&const self);
string Ipv4Address::toString!(&const self);
[4]u8 Ipv4Address::octets(&const self);
bool Ipv4Address::isWildcard(&const self);
bool Ipv4Address::isLoopback(&const self);
Ipv6Address Ipv6Address::wildcard();
Ipv6Address Ipv6Address::loopback();
Ipv6Address Ipv6Address::fromSegments(u16 segment1, u16 segment2, u16 segment3, u16 segment4, u16 segment5, u16 segment6, u16 segment7, u16 segment8);
(Ipv6Address, error) Ipv6Address::parse(stringv text);
(string, error) Ipv6Address::toString(&const self);
string Ipv6Address::toString!(&const self);
[8]u16 Ipv6Address::segments(&const self);
bool Ipv6Address::isWildcard(&const self);
bool Ipv6Address::isLoopback(&const self);
SocketAddress SocketAddress::zero() override;
SocketAddress SocketAddress::fromIpv4(Ipv4SocketAddress value);
SocketAddress SocketAddress::fromIpv6(Ipv6SocketAddress value);
AddressFamily SocketAddress::family(&const self);
bool SocketAddress::isIpv4(&const self);
bool SocketAddress::isIpv6(&const self);
(Ipv4SocketAddress, bool) SocketAddress::ipv4(&const self);
(Ipv6SocketAddress, bool) SocketAddress::ipv6(&const self);
(string, error) SocketAddress::toString(&const self);
string SocketAddress::toString!(&const self);
Ipv4SocketAddress Ipv4SocketAddress::fromParts(Ipv4Address address, u16 port);
Ipv4Address Ipv4SocketAddress::address(&const self);
u16 Ipv4SocketAddress::port(&const self);
(Ipv4SocketAddress, error) Ipv4SocketAddress::parse(stringv text);
(string, error) Ipv4SocketAddress::toString(&const self);
string Ipv4SocketAddress::toString!(&const self);
Ipv6SocketAddress Ipv6SocketAddress::fromParts(Ipv6Address address, u16 port);
Ipv6Address Ipv6SocketAddress::address(&const self);
u16 Ipv6SocketAddress::port(&const self);
(Ipv6SocketAddress, error) Ipv6SocketAddress::parse(stringv text);
(string, error) Ipv6SocketAddress::toString(&const self);
string Ipv6SocketAddress::toString!(&const self);
```

<a id="std-net-dns"></a>
### `std::net::dns`

Hostname and service resolution.

Sources: [`std/src/net/dns.fl`](../std/src/net/dns.fl)

```flecha
export (own []address::SocketAddress, error) resolve(stringv name, u16 port);
```

<a id="std-net-errors"></a>
### `std::net::errors`

Portable networking error family.

Sources: [`std/src/net/errors.fl`](../std/src/net/errors.fl)

```flecha
export error NetworkError {
    InvalidAddress,
    InvalidPort,
    InvalidName,
    NameNotFound,
    TemporaryFailure,
    UnsupportedFamily,
    ResolutionFailed,
    Closed,
    TimedOut,
    ConnectionRefused,
    ConnectionReset,
    NetworkUnreachable,
    AddressInUse,
    AddressNotAvailable,
    BrokenPipe,
    ConnectFailed,
    BindFailed,
    AcceptFailed,
    ReadFailed,
    WriteFailed,
    ReceiveFailed,
    SendFailed,
    MessageTooLarge,
    ShutdownFailed,
    CloseFailed,
    AddressFailed,
    TimeoutConfigurationFailed,
}
```

<a id="std-net-tcp"></a>
### `std::net::tcp`

Blocking TCP streams and listeners.

Sources: [`std/src/net/tcp.fl`](../std/src/net/tcp.fl), [`std/src/net/tcp_listener.fl`](../std/src/net/tcp_listener.fl)

```flecha
export enum Shutdown: u8 {
    default Read,
    Write,
    ReadWrite,
}
export structure Stream << stream::Readable, stream::Writable {
};
export structure Listener {
};

// Public methods
u8 Shutdown::number(&const self);
Stream Stream::zero() override;
void Stream::tear(&self) override;
Stream Stream::fromHandle(*void handle);
(Stream, error) Stream::connect(address::SocketAddress host);
(Stream, error) Stream::connectWithTimeout(address::SocketAddress host, duration::Duration timeout);
bool Stream::isOpen(&const self);
(usize, error) Stream::read(&self, []u8 destination) override;
&stream::Readable Stream::reader(&self) <-self;
(usize, error) Stream::write(&self, []const u8 source) override;
&stream::Writable Stream::writer(&self) <-self;
error Stream::setReadTimeout(&self, duration::Duration timeout);
error Stream::clearReadTimeout(&self);
error Stream::setWriteTimeout(&self, duration::Duration timeout);
error Stream::clearWriteTimeout(&self);
error Stream::shutdown(&self, Shutdown direction);
(address::SocketAddress, error) Stream::localAddress(&const self);
(address::SocketAddress, error) Stream::peerAddress(&const self);
error Stream::close(&self);
Listener Listener::zero() override;
void Listener::tear(&self) override;
(Listener, error) Listener::bind(address::SocketAddress host, u32 backlog);
bool Listener::isOpen(&const self);
(Stream, address::SocketAddress, error) Listener::accept(&self);
(address::SocketAddress, error) Listener::localAddress(&const self);
error Listener::close(&self);
```

<a id="std-net-udp"></a>
### `std::net::udp`

Blocking UDP sockets and datagrams.

Sources: [`std/src/net/udp.fl`](../std/src/net/udp.fl)

```flecha
export const usize MAX_DATAGRAM_SIZE = 65507;
export structure Socket {
};

// Public methods
Socket Socket::zero() override;
void Socket::tear(&self) override;
(Socket, error) Socket::bind(address::SocketAddress local);
bool Socket::isOpen(&const self);
(usize, error) Socket::sendTo(&self, []const u8 source, address::SocketAddress destination);
(usize, address::SocketAddress, error) Socket::receiveFrom(&self, []u8 destination);
error Socket::setReceiveTimeout(&self, duration::Duration timeout);
error Socket::clearReceiveTimeout(&self);
error Socket::setSendTimeout(&self, duration::Duration timeout);
error Socket::clearSendTimeout(&self);
(address::SocketAddress, error) Socket::localAddress(&const self);
error Socket::close(&self);
error Socket::connect(&self, address::SocketAddress peer);
(usize, error) Socket::send(&self, []const u8 source);
(usize, error) Socket::receive(&self, []u8 destination);
(address::SocketAddress, error) Socket::peerAddress(&const self);
```

<a id="std-os-arguments"></a>
### `std::os::arguments`

Process command-line argument capture.

Sources: [`std/src/os/arguments.fl`](../std/src/os/arguments.fl)

```flecha
export (own []string, error) capture();
```

<a id="std-os-environment"></a>
### `std::os::environment`

Process environment access and mutation.

Sources: [`std/src/os/environments.fl`](../std/src/os/environments.fl)

```flecha
export (string, bool, error) get(stringv name);
export error set(stringv name, stringv value);
export error remove(stringv name);
```

<a id="std-os-errors"></a>
### `std::os::errors`

Operating-system context errors.

Sources: [`std/src/os/errors.fl`](../std/src/os/errors.fl)

```flecha
export error EnvironmentError {
    InvalidName,
    InvalidValue,
    PermissionDenied,
    ReadFailed,
    WriteFailed,
}
```

<a id="std-process-command"></a>
### `std::process::command`

Child-process commands, pipes, and exit status.

Sources: [`std/src/process/child.fl`](../std/src/process/child.fl), [`std/src/process/command.fl`](../std/src/process/command.fl), [`std/src/process/exit_status.fl`](../std/src/process/exit_status.fl), [`std/src/process/pipe.fl`](../std/src/process/pipe.fl)

```flecha
export structure ChildProcess {
};
export structure Command {
};
export enum Stdio: u8 {
    // Uses the corresponding parent standard stream
    default Inherit,

    // Connects the child stream to the platform null device
    Null,

    // Creates a parent-owned anon pipe
    Piped,
}
export enum Termination: u8 {
    default Exited,
    Signaled,
    Abnormal,
}

// Describes a completed child process
export structure ExitStatus {
};
export structure PipeReader << stream::Readable {
};
export structure PipeWriter << stream::Writable {
};

// Public methods
void ChildProcess::tear(&self) override;
ChildProcess ChildProcess::fromHandles(*void handle, *void inputHandle, *void outputHandle, *void errorHandle);
bool ChildProcess::isOpen(&const self);
bool ChildProcess::hasStdin(&const self);
bool ChildProcess::hasStdout(&const self);
bool ChildProcess::hasStderr(&const self);
&PipeWriter ChildProcess::standardIn(&self) <-self;
&PipeReader ChildProcess::standardOut(&self) <-self;
&PipeReader ChildProcess::standardErr(&self) <-self;
PipeWriter ChildProcess::takeStdin(&self);
PipeReader ChildProcess::takeStdout(&self);
PipeReader ChildProcess::takeStderr(&self);
(ExitStatus, error) ChildProcess::wait(&self);
error ChildProcess::close(&self);
Command Command::init(string executable) override;
error Command::arg(&self, string argument);
void Command::setStdin(&self, Stdio mode);
void Command::setStdout(&self, Stdio mode);
void Command::setStderr(&self, Stdio mode);
(ChildProcess, error) Command::spawn(&const self);
u8 Stdio::number(&const self);
ExitStatus ExitStatus::zero() override;
ExitStatus ExitStatus::fromRuntime(u8 kind, u32 detail);
Termination ExitStatus::termination(&const self);
u32 ExitStatus::detail(&const self);
bool ExitStatus::success(&const self);
bool ExitStatus::exited(&const self);
bool ExitStatus::signaled(&const self);
bool ExitStatus::abnormal(&const self);
PipeReader PipeReader::zero() override;
void PipeReader::tear(&self) override;
PipeReader PipeReader::fromHandle(*void handle);
bool PipeReader::isOpen(&const self);
(usize, error) PipeReader::read(&self, []u8 destination) override;
PipeReader PipeReader::take(&self);
&stream::Readable PipeReader::reader(&self) <-self;
error PipeReader::close(&self);
PipeWriter PipeWriter::zero() override;
void PipeWriter::tear(&self) override;
PipeWriter PipeWriter::fromHandle(*void handle);
bool PipeWriter::isOpen(&const self);
(usize, error) PipeWriter::write(&self, []const u8 source) override;
&stream::Writable PipeWriter::writer(&self) <-self;
PipeWriter PipeWriter::take(&self);
error PipeWriter::close(&self);
```

<a id="std-process-errors"></a>
### `std::process::errors`

Child-process error family.

Sources: [`std/src/process/errors.fl`](../std/src/process/errors.fl)

```flecha
export error ProcessError {
    ExecutableNotFound,
    PermissionDenied,
    InvalidArgument,
    BrokenPipe,
    Closed,
    SpawnFailed,
    WaitFailed,
    ReadFailed,
    WriteFailed,
    CloseFailed,
}
```

<a id="std-random-errors"></a>
### `std::random::errors`

Randomness error family.

Sources: [`std/src/random/errors.fl`](../std/src/random/errors.fl)

```flecha
export error RandomError {
    // The host cannot provide a cryptographically secure random source.
    Unavailable,

    // The host random source failed while servicing the request.
    GenerationFailed,

    // A requested interval is empty or reversed.
    InvalidRange,
}
```

<a id="std-random-generator"></a>
### `std::random::generator`

Seeded deterministic pseudorandom generator.

Sources: [`std/src/random/generator.fl`](../std/src/random/generator.fl)

```flecha
export structure Generator {
};

// Public methods
Generator Generator::fromSeed(u64 seed);
(Generator, errors::RandomError) Generator::fromSecure();
bool Generator::boolean(&self);
void Generator::fill(&self, []u8 destination);
```

<a id="std-random-secure"></a>
### `std::random::secure`

Host cryptographic entropy.

Sources: [`std/src/random/secure.fl`](../std/src/random/secure.fl)

```flecha
export errors::RandomError fill([]u8 destination);
```

<a id="std-tests-asserts"></a>
### `std::tests::asserts`

Typed test assertions.

Sources: [`std/src/tests/asserts.fl`](../std/src/tests/asserts.fl)

```flecha
export void is_true(&testing::UnitTest ut, bool actual, stringv message);
export void is_false(&testing::UnitTest ut, bool actual, stringv message);
export void equal<T>(&testing::UnitTest ut, &const T expected, &const T actual, stringv message);
export void not_equal<T>(&testing::UnitTest ut, &const T unexpected, &const T actual, stringv message);
export void type_equal<A, B>(&testing::UnitTest ut, &const A expected, &const B actual, stringv message);
export void type_not_equal<A, B>(&testing::UnitTest ut, &const A unexpected, &const B actual, stringv message);
export void is_none<T>(&testing::UnitTest ut, &const T actual, stringv message);
export void is_null<T>(&testing::UnitTest ut, &const T actual, stringv message);
export void has_error(&testing::UnitTest ut, error actual, stringv message);
export void has_error_family<E>(&testing::UnitTest ut, E actual, stringv message);
export void less<T: numeric>(&testing::UnitTest ut, &const T actual, &const T expected, stringv message);
export void less_or_equal<T: numeric>(&testing::UnitTest ut, &const T actual, &const T expected, stringv message);
export void greater<T: numeric>(&testing::UnitTest ut, &const T actual, &const T expected, stringv message);
export void greater_or_equal<T: numeric>(&testing::UnitTest ut, &const T actual, &const T expected, stringv message);
export void numeric_equal<T: numeric>(&testing::UnitTest ut, &const T actual, &const T expected, stringv message);
export void numeric_not_equal<T: numeric>(&testing::UnitTest ut, &const T actual, &const T unexpected, stringv message);
export void has_error_variant(&testing::UnitTest ut, error actual, error expected, stringv message);
export void string_equal(&testing::UnitTest ut, stringv actual, stringv expected, stringv message);
export void string_not_equal(&testing::UnitTest ut, stringv actual, stringv unexpected, stringv message);
export void bytes_equal(&testing::UnitTest ut, []const u8 actual, []const u8 expected, stringv message);
export void bytes_not_equal(&testing::UnitTest ut, []const u8 actual, []const u8 unexpected, stringv message);
```

<a id="std-tests-benchmark"></a>
### `std::tests::benchmark`

Deterministic benchmark sampling and summaries.

Sources: [`std/src/tests/benchmark.fl`](../std/src/tests/benchmark.fl)

```flecha
export error BenchmarkError {
    // The benchmark name is empty or contains an unsupported byte.
    InvalidName,

    // A timed sample would execute zero workload iterations.
    InvalidIterations,

    // A run would record zero timed samples.
    InvalidSamples,
}

// Performs one benchmark workload iteration. Returning an error stops the run.
export func Operation() => (error);
export func Hook() => (error);
export structure Config {
};
export structure Result {
};
export (Result, error) run(stringv name, Config config, Operation operation);
export (Result, error) run_with_hooks(stringv name, Config config, Operation operation, Hook setup, Hook teardown);
export (Result, error) summarize(stringv name, Config config, []const u64 sampleNanoseconds);

// Public methods
(Config, error) Config::create(usize warmupIterations, usize iterationsPerSample, usize sampleCount);
usize Config::warmupIterations(&const self);
usize Config::iterationsPerSample(&const self);
usize Config::sampleCount(&const self);
stringv Result::name(&const self) <-self;
usize Result::warmupIterations(&const self);
usize Result::iterationsPerSample(&const self);
usize Result::sampleCount(&const self);
duration::Duration Result::minimumDuration(&const self);
duration::Duration Result::medianDuration(&const self);
duration::Duration Result::meanDuration(&const self);
duration::Duration Result::maximumDuration(&const self);
(string, error) Result::summary(&const self);
(string, error) Result::machineLine(&const self);
Result Result::fromSummary(stringv name, Config config, u64 minimumNanoseconds, u64 medianNanoseconds, u64 meanNanoseconds, u64 maximumNanoseconds);
```

<a id="std-tests-fixtures"></a>
### `std::tests::fixtures`

Temporary resources and seeded test data.

Sources: [`std/src/tests/fixtures.fl`](../std/src/tests/fixtures.fl)

```flecha
export structure TemporaryDirectory {
};
export structure RandomData {
};

// Public methods
(TemporaryDirectory, error) TemporaryDirectory::create();
stringv TemporaryDirectory::path(&const self) <-self;
(string, error) TemporaryDirectory::childPath(&const self, stringv relative);
error TemporaryDirectory::remove(&self);
RandomData RandomData::fromSeed(u64 seed);
u64 RandomData::seed(&const self);
error RandomData::logSeed(&const self, &testing::UnitTest ut);
bool RandomData::boolean(&self);
void RandomData::fill(&self, []u8 destination);
```

<a id="std-tests-golden"></a>
### `std::tests::golden`

Golden text and byte assertions.

Sources: [`std/src/tests/golden.fl`](../std/src/tests/golden.fl)

```flecha
export enum Mode: u8 {
    // Reads and compares without modifying the golden file.
    default Verify,

    // Creates or replaces the golden file before recording the assertion.
    Update,
}

// Verifies actual against the byte-oriented text stored at goldenPath. Update
// mode must be passed explicitly and replaces the file. Every outcome records
// exactly one assertion; an I/O failure is also returned to the caller.
export error text(&testing::UnitTest ut, stringv actual, stringv goldenPath, stringv message, Mode mode = Mode.Verify,);
export error bytes(&testing::UnitTest ut, []const u8 actual, stringv goldenPath, stringv message, Mode mode = Mode.Verify,);
```

<a id="std-tests-testing"></a>
### `std::tests::testing`

Unit-test state and failure accounting.

Sources: [`std/src/tests/testing.fl`](../std/src/tests/testing.fl)

```flecha
export structure UnitTest {
};

// Public methods
UnitTest UnitTest::init(stringv name, bool verbose) override;
stringv UnitTest::name(&const self) <-self;
usize UnitTest::assertionCount(&const self);
usize UnitTest::failureCount(&const self);
bool UnitTest::failed(&const self);
void UnitTest::fail(&self);
void UnitTest::reportError(&self, stringv message);
void UnitTest::recordAssertion(&self, bool passed, stringv assertion, stringv message);
void UnitTest::printOutput(&const self);
void UnitTest::log(&self, stringv message);
```

<a id="std-text-ascii"></a>
### `std::text::ascii`

ASCII classification and case conversion.

Sources: [`std/src/text/ascii.fl`](../std/src/text/ascii.fl)

```flecha
export bool is_ascii(char value);
export bool is_ascii_alphabetic(char value);
export bool is_ascii_alphanumeric(char value);
export bool is_ascii_digit(char value);
export bool is_ascii_hex_digit(char value);
export bool is_ascii_whitespace(char value);
export bool is_ascii_control(char value);
export bool is_ascii_printable(char value);
export char to_ascii_lowercase(char value);
export char to_ascii_uppercase(char value);
```

<a id="std-text-errors"></a>
### `std::text::errors`

Text parse, format, and UTF-8 errors.

Sources: [`std/src/text/errors.fl`](../std/src/text/errors.fl)

```flecha
export error ParseError {
    Empty,
    InvalidSyntax,
    InvalidBase,
    Overflow,
}
export error FormatError {
    InvalidBase,
    InsufficientSpace,
}
export error Utf8Error {
    InvalidEncoding,
    Truncated,
    InvalidOffset,
    InvalidCodePoint,
}
```

<a id="std-text-strings"></a>
### `std::text::strings`

String building, search, trim, transform, replace, parse, and format.

Sources: [`std/src/text/format.fl`](../std/src/text/format.fl), [`std/src/text/parsing.fl`](../std/src/text/parsing.fl), [`std/src/text/replace.fl`](../std/src/text/replace.fl), [`std/src/text/search.fl`](../std/src/text/search.fl), [`std/src/text/strings.fl`](../std/src/text/strings.fl), [`std/src/text/transform.fl`](../std/src/text/transform.fl), [`std/src/text/trim.fl`](../std/src/text/trim.fl)

```flecha
export (string, error) int_to_string<T: integer>(T value, u8 base = 10);
export (string, error) float_to_string<T: floating_point>(T value);
export (usize, error) format_float<T: floating_point>([]u8 destination, T value,);
export (usize, error) format_integer<T: integer>([]u8 destination, T value, u8 base = 10,);
export (T, error) stringv_to_int<T: integer>(stringv value);
export (T, error) stringv_to_float<T: floating_point>(stringv value);
export (string, error) replace(stringv value, stringv target, stringv replacement,);
export bool starts_with(stringv value, stringv prefix);
export bool ends_with(stringv value, stringv suffix);
export bool contains(stringv value, stringv needle);
export (usize, bool) find(stringv value, stringv needle);
export (usize, bool) find_char(stringv value, char needle);
export structure StringBuilder {
};
export stringv stringv_from_bytes([]const u8 value) <-value;
export (string, error) string_from_bytes([]const u8 value);
export bool constant_time_equal(stringv left, stringv right);
export (string, error) to_ascii_lowercase(stringv value);
export (string, error) to_ascii_uppercase(stringv value);
export stringv trim_ascii(stringv value) <-value;
export stringv trim_ascii_start(stringv value) <-value;
export stringv trim_ascii_end(stringv value) <-value;

// Public methods
(StringBuilder, error) StringBuilder::withCapacity(usize capacity);
usize StringBuilder::len(&const self);
usize StringBuilder::capacity(&const self);
bool StringBuilder::empty(&const self);
error StringBuilder::reserve(&self, usize capacity);
error StringBuilder::append(&self, stringv value);
error StringBuilder::appendChar(&self, char value);
error StringBuilder::appendBytes(&self, []const u8 value);
error StringBuilder::appendBool(&self, bool value);
error StringBuilder::appendRepeated(&self, char value, usize times);
error StringBuilder::appendCodePoint(&self, u32 codePoint);
stringv StringBuilder::view(&const self) <-self;
(string, error) StringBuilder::toString(&const self);
string StringBuilder::toString!(&const self);
error StringBuilder::appendLine(&self, stringv line);
void StringBuilder::clear(&self);
[]const u8 StringBuilder::bytes(&const self) <-self;
```

<a id="std-text-utf8"></a>
### `std::text::utf8`

UTF-8 validation, decoding, counting, and encoding.

Sources: [`std/src/text/utf8.fl`](../std/src/text/utf8.fl)

```flecha
export error validate(stringv value);
export (u32, usize, error) decode(stringv value, usize offset);
export (usize, error) count_code_points(stringv value);
export ([4]u8, usize, error) encode(u32 codePoint);
```

<a id="std-time-clock"></a>
### `std::time::clock`

Monotonic instants, wall timestamps, and sleep.

Sources: [`std/src/time/clock.fl`](../std/src/time/clock.fl)

```flecha
export structure Instant {
};
export structure Timestamp {
};
export error sleep(duration::Duration value);

// Public methods
(Instant, error) Instant::now();
bool Instant::isValid(&const self);
(duration::Duration, error) Instant::durationSince(&const self, Instant earlier);
(duration::Duration, error) Instant::elapsed(&const self);
(Instant, error) Instant::checkedAdd(&const self, duration::Duration value);
(Instant, error) Instant::checkedSub(&const self, duration::Duration value);
(Timestamp, error) Timestamp::fromUnixParts(i64 seconds, u32 nanoseconds);
(Timestamp, error) Timestamp::now();
i64 Timestamp::unixSeconds(&const self);
u32 Timestamp::subsecondNanoseconds(&const self);
(duration::Duration, error) Timestamp::durationSince(&const self, Timestamp earlier);
(Timestamp, error) Timestamp::checkedAdd(&const self, duration::Duration value);
(Timestamp, error) Timestamp::checkedSub(&const self, duration::Duration value);
```

<a id="std-time-duration"></a>
### `std::time::duration`

Normalized signed time durations.

Sources: [`std/src/time/duration.fl`](../std/src/time/duration.fl)

```flecha
export structure Duration {
};

// Public methods
Duration Duration::zero() override;
(Duration, error) Duration::fromParts(u64 seconds, u32 nanoseconds);
Duration Duration::fromSeconds(u64 value);
Duration Duration::fromMilliseconds(u64 value);
Duration Duration::fromMicroseconds(u64 value);
Duration Duration::fromNanoseconds(u64 value);
u64 Duration::wholeSeconds(&const self);
u32 Duration::subsecondNanoseconds(&const self);
(u64, error) Duration::totalMilliseconds(&const self);
(u64, error) Duration::totalMicroseconds(&const self);
(u64, error) Duration::totalNanoseconds(&const self);
bool Duration::isZero(&const self);
bool Duration::lessThan(&const self, Duration other);
bool Duration::lessOrEqual(&const self, Duration other);
bool Duration::greaterThan(&const self, Duration other);
bool Duration::greaterOrEqual(&const self, Duration other);
(Duration, error) Duration::checkedAdd(&const self, Duration other);
(Duration, error) Duration::checkedSub(&const self, Duration other);
(Duration, error) Duration::checkedMul(&const self, u64 factor);
(u64, error) Duration::totalUnits(&const self, u64 unitsPerSecond, u64 nanosecondsPerUnit);
```

<a id="std-time-errors"></a>
### `std::time::errors`

Clock, duration, and sleep errors.

Sources: [`std/src/time/errors.fl`](../std/src/time/errors.fl)

```flecha
export error TimeError {
    // A seconds-plus-nanoseconds value is not normalized.
    InvalidNanoseconds,

    // A portable conversion or arithmetic result is not representable.
    Overflow,

    // Subtraction would produce a negative duration or instant.
    NegativeResult,

    // An instant was not produced by the monotonic clock in this execution.
    InvalidInstant,

    // The requested host clock could not provide an observation.
    ClockUnavailable,

    // A wall-clock result cannot be represented by Timestamp.
    TimestampOutOfRange,

    // A valid Duration cannot be represented by the host sleep primitive.
    DurationOutOfRange,

    // The host wait failed for another reason.
    SleepFailed,
}
```
