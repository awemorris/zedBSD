# zedBSD network configuration format version 1

Status: frozen by `ws011-p001`

## Lexical grammar

- Input is UTF-8-compatible text, but version 1 keys and unquoted scalar values
  are restricted to ASCII.
- A physical line is at most 511 bytes including indentation and content.
- LF and CRLF line endings are accepted.
- Indentation is exactly two ASCII spaces per level. Tabs are forbidden
  anywhere.
- Blank lines and comments beginning with `#` are ignored. An inline comment
  begins at `#` only when preceded by a space.
- Mapping entries are `key: value` or `key:`. There is exactly one space after
  the colon when a value is present.
- Sequence entries are `- value` at the schema-defined indentation.
- Quoting, escapes, flow collections, anchors, aliases, tags, document markers,
  multiline scalars, and implicit YAML types are not supported.
- Keys are unique within their mapping. Unknown keys are errors.
- Scalars are schema-defined bounded ASCII strings, decimal unsigned integers,
  or the exact booleans `true` and `false`.
- Empty collections are represented by omitting their optional key. `[]` and
  `{}` are deliberately not accepted in version 1.

This is a zedBSD format with YAML-like visual structure, not a YAML language
profile. A general YAML parser is neither required nor sufficient to validate
the schema.

## Schema

The required top-level keys are `version: 1` and `interfaces:`. `routes:` is
optional. `dns:` is required by semantic validation and contains `mode:` plus
an optional `servers:` sequence.

Interface names are unique, 1–15 characters, begin with an ASCII alphanumeric,
and then contain only alphanumerics, `_`, or `-`. At most 16 interfaces exist.
Each interface requires:

```text
type: loopback | ethernet | vlan | bridge
enabled: true | false
```

An optional `ipv4:` mapping contains `dhcp`, `dhcp-timeout`, and `addresses`.
DHCP and static addresses are mutually exclusive. A timeout is valid only when
DHCP is true and ranges from 1 to 3600 seconds. Each of at most eight addresses
is written as:

```yaml
        - address: 192.0.2.1
          prefix-length: 24
```

IPv4 octets are decimal 0–255 and prefix length is 0–32.

A VLAN additionally requires `parent` naming an existing interface and
`vlan-id` from 1 through 4094. No other type accepts these keys. A bridge may
have a `members:` sequence of unique existing interface names. No other type
accepts members. Parent/member dependency cycles are rejected.

Each of at most 16 routes requires a `destination` of `default` or IPv4/prefix
and an IPv4 `gateway`:

```yaml
routes:
  - destination: 192.0.2.0/24
    gateway: 10.0.0.1
```

DNS mode is `dhcp`, `static`, or `merge`. `static` requires at least one server;
at most eight unique IPv4 servers are accepted.

## Canonical form

The writer emits version, interfaces in model order, routes in model order,
and DNS. It uses LF, two-space indentation, lowercase type/mode/boolean values,
decimal integers without leading decoration, no comments, and one trailing
newline. Parsing canonical output must reproduce an equivalent model.
