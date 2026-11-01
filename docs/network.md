# Networking

The initial networking surface provides a portable IPv4 address value. IPv6,
socket addresses, hostname resolution, and blocking sockets remain later parts
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
