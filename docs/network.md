# Networking

The initial networking surface provides portable IPv4 and IPv6 address and
socket-address values plus blocking hostname resolution. Blocking TCP and UDP
sockets remain later parts of the Stage 6 networking work.

## IPv4 addresses

Import the address and error modules from the standard library:

```flecha
@imports
    :: lib::std::net::address
    :: lib::std::net::errors;
```

`Ipv4Address` stores one IPv4 address in network byte order. Its fields are
private, and every representable value is valid. Consequently, the zero value
is the wildcard address `0.0.0.0`, not an invalid sentinel. Address equality is
portable structural equality over this representation.

The public value API is:

```flecha
Ipv4Address Ipv4Address::fromOctets(
    u8 octet1,
    u8 octet2,
    u8 octet3,
    u8 octet4
);
(Ipv4Address, error) Ipv4Address::parse(stringv text);
(string, error) Ipv4Address::toString(&const self);
string Ipv4Address::toString!(&const self);
[4]u8 Ipv4Address::octets(&const self);

Ipv4Address Ipv4Address::wildcard();
bool Ipv4Address::isWildcard(&const self);
Ipv4Address Ipv4Address::loopback();
bool Ipv4Address::isLoopback(&const self);
```

## Canonical parsing and formatting

`parse` accepts exactly four decimal components separated by dots. Each
component is between 0 and 255. Zero is written as `0`; a multi-digit component
cannot begin with zero. The parser does not trim whitespace and rejects signs,
letters, empty components, missing components, and trailing separators with
`NetworkError.InvalidAddress`.

`toString` emits the canonical decimal form without unnecessary leading
zeroes. Parsing a canonical string and formatting the result therefore
reproduces the input. Formatting allocates an owning string. Allocation failure
returns an empty string with `AllocationError.OutOfMemory`; partial output is
never exposed. `toString!()` returns the owning string directly and produces a
crash report on allocation failure.

## Wildcard and loopback

`wildcard()` returns `0.0.0.0`, the unspecified address used when binding all
local IPv4 interfaces. `isWildcard()` recognizes only that value.

`loopback()` returns `127.0.0.1`. `isLoopback()` recognizes the complete
`127.0.0.0/8` block, not only the address returned by `loopback()`.

## IPv6 addresses

`Ipv6Address` stores one IPv6 address as two private 64-bit values in network
order. Every bit pattern is a valid address, so its zero value is the wildcard
address `::`. Equality is portable structural equality over the two stored
halves.

The public value API is:

```flecha
export const usize MAX_IPV6_STRING_LENGTH = 39;

Ipv6Address Ipv6Address::fromSegments(
    u16 segment1,
    u16 segment2,
    u16 segment3,
    u16 segment4,
    u16 segment5,
    u16 segment6,
    u16 segment7,
    u16 segment8
);
(Ipv6Address, error) Ipv6Address::parse(stringv text);
(string, error) Ipv6Address::toString(&const self);
string Ipv6Address::toString!(&const self);
[8]u16 Ipv6Address::segments(&const self);

Ipv6Address Ipv6Address::wildcard();
bool Ipv6Address::isWildcard(&const self);
Ipv6Address Ipv6Address::loopback();
bool Ipv6Address::isLoopback(&const self);
```

### Parsing and canonical formatting

`parse` accepts eight hexadecimal segments or one `::` compression marker that
stands for one or more zero segments. Each explicit segment contains one to
four hexadecimal digits. Uppercase input and leading zeroes are accepted. The
parser does not trim whitespace and rejects signs, invalid hexadecimal digits,
empty uncompressed segments, repeated compression, and incorrect segment
counts with `NetworkError.InvalidAddress`.

Embedded dotted-decimal IPv4 notation such as `::ffff:192.0.2.1` is not
currently accepted. Construct the corresponding IPv6 segments explicitly when
that representation is needed.

`toString` emits lowercase hexadecimal without leading zeroes. It compresses
the longest run of at least two zero segments with `::`; the leftmost run wins
when lengths tie. A single zero segment is never compressed. These rules emit
`::` for the wildcard address and `::1` for loopback, and ensure every emitted
string parses back to the same address.

Formatting allocates an owning string with at most
`MAX_IPV6_STRING_LENGTH` bytes. Allocation failure returns an empty string with
`AllocationError.OutOfMemory`; partial output is never exposed. `toString!()`
returns the canonical owning string directly and produces a crash report on
allocation failure.

### Wildcard and loopback

`wildcard()` returns `::`, the unspecified address used when binding all local
IPv6 interfaces. `isWildcard()` recognizes only that value.

`loopback()` returns `::1`. Unlike IPv4 loopback classification,
`isLoopback()` recognizes only that exact IPv6 address.

## Socket addresses

`Ipv4SocketAddress` and `Ipv6SocketAddress` pair an address value with an
explicit `u16` port. Their private representation prevents invalid ports, and
their zero values are the wildcard address on port zero. Construction and
access do not allocate:

```flecha
Ipv4SocketAddress Ipv4SocketAddress::fromParts(
    Ipv4Address address,
    u16 port
);
Ipv4Address Ipv4SocketAddress::address(&const self);
u16 Ipv4SocketAddress::port(&const self);
(Ipv4SocketAddress, error) Ipv4SocketAddress::parse(stringv text);
(string, error) Ipv4SocketAddress::toString(&const self);
string Ipv4SocketAddress::toString!(&const self);

Ipv6SocketAddress Ipv6SocketAddress::fromParts(
    Ipv6Address address,
    u16 port
);
Ipv6Address Ipv6SocketAddress::address(&const self);
u16 Ipv6SocketAddress::port(&const self);
(Ipv6SocketAddress, error) Ipv6SocketAddress::parse(stringv text);
(string, error) Ipv6SocketAddress::toString(&const self);
string Ipv6SocketAddress::toString!(&const self);
```

IPv4 text uses `address:port`, such as `192.0.2.1:8080`. IPv6 text requires
brackets around the address so its colons remain distinct from the port:
`[2001:db8::1]:443`. Formatting always uses these canonical forms, including
`0.0.0.0:0`, `[::]:0`, and `[::1]:65535`.

The port parser accepts decimal values from `0` through `65535`. It rejects an
empty port, signs, non-decimal bytes, overflow, and leading zeroes on a
multi-digit value with `NetworkError.InvalidPort`. IPv4 delegates the address
prefix to `Ipv4Address::parse`. IPv6 requires an opening bracket, a closing
bracket, and a colon immediately after that bracket before delegating the
interior to `Ipv6Address::parse`. A malformed address reports
`NetworkError.InvalidAddress`; a missing or malformed port reports
`NetworkError.InvalidPort`.

`toString()` allocates one complete owning string and propagates
`AllocationError.OutOfMemory` without exposing partial output. `toString!()`
returns the same canonical result directly and emits the standard crash report
if either address or socket formatting cannot allocate. Choose the recoverable
or crashable spelling deliberately; the bang is part of the method identity.

## Portable socket addresses

`SocketAddress` carries either family-specific socket-address value without
exposing a platform `sockaddr` structure. `AddressFamily`, `isIpv4`, `isIpv6`,
`ipv4`, and `ipv6` distinguish the active representation. The family-specific
accessors return the value and `true`, or the zero value and `false` when the
other family is active. `toString` and `toString!` delegate to the active
representation.

## Hostname resolution

Import `std::net::dns` and resolve a hostname with the port to attach to every
result:

```flecha
results, err := dns::resolve("localhost", 8080);
if err != none {
    return err;
}

loop usize index = 0; index < results.len(); index++ {
    endpoint, present, getError := results.get(index);
    if getError != none or !present {
        return getError;
    }

    // Try endpoint with the TCP or UDP API.
}
```

`resolve` is blocking. It returns an owning `Resolution` containing every
unique IPv4 and IPv6 result supported by the portable address model, in the
order supplied by the host resolver. The requested port is stored with every
returned `SocketAddress`. Destroying or moving `Resolution` releases its
runtime result set exactly once. `get` returns `present == false` without an
error when its index is outside the result set.

Names use ASCII hostname syntax. International names must be supplied in their
ASCII-compatible punycode form. Names are limited to 253 bytes; labels are
limited to 63 bytes, cannot be empty, and cannot begin or end with a hyphen. A
single trailing root dot is accepted. Empty names, repeated dots, unsupported
bytes, and malformed labels return `NetworkError.InvalidName` before calling
the host resolver.

Stable resolution failures are `NameNotFound`, `TemporaryFailure`,
`UnsupportedFamily`, and `ResolutionFailed`. Native allocation failure maps to
`AllocationError.OutOfMemory`. Results contain only portable numeric address
values and never expose native resolver records, pointers, or structure
layouts.
