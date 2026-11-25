# GenICam

C11 library for controlling and implementing basic GigE Vision devices.
The library provides reusable GVCP and GVSP implementations. The demos add
synthetic camera registers, a grayscale image generator, and image file output.

## Build and run

CMake 3.16 or newer, Expat, and zlib are required. On Debian and Ubuntu the
development packages are `libexpat1-dev` and `zlib1g-dev`. On Windows, supply
Expat and zlib for the selected compiler through the CMake toolchain or
`CMAKE_PREFIX_PATH`. For a static Windows Expat build, use `XML_STATIC` if
the dependency target does not already define it.

```sh
cmake -S . -B build
cmake --build build --config Release
(cd build && ctest -C Release --output-on-failure)
```

Start the camera and controller in two terminals.

```sh
./build/genicam-server 127.0.0.1 3956
```

```sh
./build/genicam-control 127.0.0.1 3956 10 capture.pgm
```

Single-configuration builds put the executables directly in `build`.
Multi-configuration builds place them in the selected configuration directory,
such as `build/Release`. Use `Ctrl+C` to stop the camera. A non-loopback IPv4
argument advertises that address and enables broadcast discovery. The demo
advertises a /24 mask for LAN addresses. For a different subnet, set the
mask in the server configuration. A directed discovery uses the address in
the command above. To list cameras on a selected LAN interface, use
`genicam-control --discover <local-ip>`.

Install the static library and public headers with
`cmake --install build --prefix <prefix>`. An application using CMake can
use `find_package(genicam CONFIG REQUIRED)` and link `genicam::genicam`.
Set `GC_BUILD_DEMOS=OFF` and `BUILD_TESTING=OFF` for a library-only build.

## Public interfaces

| Header | Purpose |
| --- | --- |
| `genicam.h` | Device port, feature access, stream receive, frame metadata, and statistics |
| `gige.h` | Camera discovery, controller configuration, and reusable camera server |
| `sfnc.h` | Names for common standardized features |
| `pfnc.h` | Lookup and row-size calculation for supported pixel formats |

On the controller, open the device, load the node map, configure the camera,
and start the local stream before executing the remote acquisition command.
Pass an application-owned buffer to each receive call. Execute the remote stop
command and stop the local stream before closing it. Only one thread may
receive on a stream. Another thread may stop it while the receive is in
progress. Join receiving threads before closing the stream, then close the node
map and device.

On the device, pass the XML, register callbacks, and optional resend workspace
to the server. Call its poll and send functions serially. The server owns the
GigE Vision bootstrap registers and updates the control lease and stream
destination. The application owns the XML and its custom register state.

## Implementation scope

- GigE Vision over IPv4 and UDP. GVCP implements discovery, register and
  memory I/O, an optional monitor session, control ownership, and heartbeat.
- GVSP supports a single unicast image stream with normal or extended packet
  IDs on receive, out-of-order data after a leader, duplicate detection, and
  bounded resend requests when the camera advertises them. A server may retain
  its last frame in application-provided memory for resend.
- GenApi supports local XML and ZIP descriptions, integer and masked
  registers, float and string registers, integer and float features, booleans,
  enumerations, commands, access predicates, and a subset of Converter and
  SwissKnife formulas. Formula operators cover arithmetic, bitwise operations,
  shifts, and parentheses. Integer formulas are bounded to the exact integer
  range used by the evaluator. The node map reads current device values on
  demand.
- The current transport profile covers ordinary image payloads. USB3 Vision,
  the GenTL Producer, GenDC, Chunk/Multipart, device events, and GigE Vision
  3.0 RDMA are later work.

## OSAL and tests

`src/osal/osal.c` is the Linux and Windows platform implementation. It adapts
threading and clock primitives from the supplied IPC OSAL and adds IPv4
networking. For an RTOS port, implement the same internal `src/osal/osal.h`
interface and select your file with `GC_OSAL_SOURCE`. Use
`GC_OSAL_LIBRARIES` for the RTOS networking, threading, or C math libraries
required by the port. The port needs a TCP/IP stack that exposes UDP,
a monotonic clock, the specified synchronization primitives, and builds of
Expat and zlib.

The C integration test uses a local fake camera with a compressed XML
description. It covers control contention, feature access, frame assembly,
retransmission, malformed and duplicate packets, and stream shutdown.
