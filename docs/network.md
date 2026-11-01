# Networking

The initial networking surface provides portable IPv4 and IPv6 address values.
Socket addresses, hostname resolution, and blocking sockets remain later parts
of the Stage 6 networking work.

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
never exposed.

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
`AllocationError.OutOfMemory`; partial output is never exposed.

### Wildcard and loopback

`wildcard()` returns `::`, the unspecified address used when binding all local
IPv6 interfaces. `isWildcard()` recognizes only that value.

`loopback()` returns `::1`. Unlike IPv4 loopback classification,
`isLoopback()` recognizes only that exact IPv6 address.
