# OneWili — vendored

The C client library for the FreeWili 2 (FW2) OneWili protocol. Used by the
`FreeWili2` device to talk to FW2 hardware.

| | |
|---|---|
| Upstream | https://github.com/freewili/onewili |
| Commit | `a857db25f7fb49bd32fbf60ddcedc0d59ea0cff7` |
| Date | 2026-08-21 |
| Subtree | `c/include` and `c/src`, copied verbatim |

## Why vendored rather than fetched

`onewili` is generated code — `onewili.c` and `onewili.h` are ~400 KB emitted from
the firmware's command table — so it changes shape whenever the firmware does.
Pinning it here means an FW2 firmware release cannot silently break a libicsneo
build, and it keeps `LIBICSNEO_ENABLE_ONEWILI` builds free of a network fetch.

## Updating

Copy `c/include` and `c/src` from the upstream checkout again and update the commit
above. **Do not edit these files** — everything we need to change lives in
`communication/fw2communication.cpp`. In particular, we deliberately do *not* patch
`ow__call`'s blocking round-trip; the pipelined TX path reuses OneWili's command
formatting and does its own writes.

## License

Upstream carries no `LICENSE` file at commit `a857db2`. **Resolve this with the
FreeWili team before any public release of `libicsneo-fw2`** — libicsneo itself is
MIT, and shipping vendored code of unstated license alongside it is not something
we should do silently. Until then this is fine for internal work only.
