# libicsneo (with FreeWili 2 support)

This fork adds **FreeWili 2** as a native `icsneo::Device`, so it can send and
receive CAN / CAN FD through the standard libicsneo C++ and Python APIs — no
FreeWili-specific application code required.

- **Start here:** [FW2_PORT.md](FW2_PORT.md) — what was added, status table, build
  instructions.
- **Python test scripts:** [python-scripts/](python-scripts/) — ready-to-run examples
  for sending/receiving CAN and CAN FD frames, including a two-device loopback test.

---

libicsneo is the [Intrepid Control Systems](https://intrepidcs.com/) device
communication library. Installation and usage documentation can be found within
each of the respective APIs.

## Documentation

- [C++](https://libicsneo.readthedocs.io/en/latest/icsneocpp/)
- [Python](https://libicsneo.readthedocs.io/en/latest/icsneopy/)
- [C](https://libicsneo.readthedocs.io/en/latest/icsneoc/)

## Hardware Support

- EtherBADGE
- neoVI Connect
- neoVI FIRE
- neoVI FIRE 2
- neoVI FIRE 3
- neoVI ION
- neoVI PLASMA
- neoVI RED 2
- RAD-A2B
- RAD-Comet 2
- RAD-Comet 3
- RAD-Epsilon
- RAD-EpsilonXL
- RAD-Galaxy
- RAD-Galaxy 2
- RAD-Gemini
- RAD-Gigastar
- RAD-Gigastar 2
- RAD-Moon 2
- RAD-Moon 3
- RAD-Moon T1S
- RAD-Pluto
- RAD-Star 2
- RAD-SuperMoon
- RADComet
- ValueCAN 3
- ValueCAN 4

## License

libicsneo is licensed as BSD-3 with an extra clause, see [LICENSE](LICENSE)
for more details.
