# GenICam

C11 SDK for host applications and camera devices. Both roles share the
GVCP and GVSP codecs, image layout, frame queues, and platform services.
The demos add synthetic camera registers, a grayscale image source, and
image file output.

## Build and run

CMake 3.16 or newer is required. The Host SDK uses Expat and zlib. On Debian
and Ubuntu the development packages are `libexpat1-dev` and `zlib1g-dev`. On Windows, supply
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
such as `build/Release`. Use `Ctrl+C` to stop the camera.

### Camera XML and parameter reads

The server loads [demo/camera/camera.xml](demo/camera/camera.xml) at startup and serves it
through GVCP. Edit the XML and restart the server to publish your changes.
The register addresses and types must match the demo's register callbacks.
Use `genicam-server --xml path/to/camera.xml` to load a different file.
CMake also places a copy of `camera.xml` next to the executable for use
when the source tree is unavailable.

```sh
./build/genicam-control --list 127.0.0.1
./build/genicam-control --get 127.0.0.1 ExposureTime
./build/genicam-control --get 127.0.0.1 PixelFormat
```

`--list` shows supported node names and types from the remote XML.
`--get` reads the current value from the device in monitor mode.
Both commands accept a custom UDP port as the final argument.

### Discovery with Aravis

For a viewer on the server host, start `genicam-server` and refresh the
device list. The demo advertises `127.0.0.1` and answers discovery from
the loopback subnet. For a viewer on another computer, start the server
with the local IPv4 address on their shared subnet. The demo reads the
subnet mask from that interface.

```sh
./build/genicam-server 192.168.1.10 3956
```

Replace the example address with your server's LAN address. Keep the default
GVCP port `3956` for standard broadcast discovery. To check discovery with
Aravis on the controller host, run `arv-tool-0.8` without a command.
`genicam-control --discover <local-ip>` can also check a selected interface.

Install the static library and public headers with
`cmake --install build --prefix <prefix>`. Link the role you need.

| SDK | Build switch | Installed CMake target |
| --- | --- | --- |
| Host | `GC_BUILD_HOST` | `genicam::host` |
| Camera | `GC_BUILD_CAMERA` | `genicam::camera` |
| Shared | Included in either build | `genicam::common` |

Set `GC_BUILD_HOST=OFF` for a Camera-only build that uses neither Expat nor
zlib. Set `GC_BUILD_CAMERA=OFF` for a Host-only build. Use
`find_package(genicam CONFIG REQUIRED COMPONENTS camera)` for a Camera
application or replace `camera` with `host` for a Host application.
Existing applications can still link `genicam::genicam` or `libgenicam.a`.
Set `GC_BUILD_DEMOS=OFF` and `BUILD_TESTING=OFF` for a library-only build.

## Public interfaces

| Header | Purpose |
| --- | --- |
| `host.h`, `gige/host.h` | Device discovery, port access, GenApi, stream receive, and buffer queues |
| `camera.h`, `gige/camera.h` | Camera description and register callbacks, GigE device configuration, and frame submission |
| `frame.h`, `status.h` | Shared frame layout and SDK status codes |
| `sfnc.h` | Names for common standardized features |
| `pfnc.h` | Lookup and row-size calculation for supported pixel formats |
| `gendc.h` | GenDC container views and single-image container encoding |
| `genicam.h`, `gige.h` | Compatibility includes for existing applications |

The [Host demo](demo/host/control.c) opens a device, loads its node map,
configures the camera, and starts the local stream before executing the
remote acquisition command. Use `gc_stream_receive()` with one buffer,
or use the stream queue to submit and recycle several application buffers.
Queue receive returns a dequeued buffer even when acquisition fails.
Stop the camera and stream, join the host receiver, and close the queue
before closing the stream, node map, and device. Use `gc_device_get_xml()`
to retrieve raw device XML or ZIP bytes and `gc_nodemap_open_xml()` to
parse a local or saved description against the same device port.

The [Camera demo](demo/camera/server.c) opens a camera with its XML,
register callbacks, and optional resend workspace. Those resources live
until close. The GigE backend owns the bootstrap registers and the control
lease. Submit application buffers with `gc_camera_submit_frame()` and
service the camera from one task using `gc_camera_poll()`. A successful
submission borrows the payload until the completion callback. With no
destination, the callback receives `GC_STOPPED`. If a frame was accepted
but remains queued at close, its callback also receives `GC_STOPPED`.
With another task submitting frames, the poll timeout bounds the delay
before the service task checks the queue again.

## Implementation scope

- GigE Vision 1.2 IPv4/UDP camera profile. GVCP implements discovery, register
  and memory I/O, a monitor session, control ownership, and heartbeat. The
  Host also handles bounded pending acknowledgements.
- GVSP supports a single unicast image stream with normal or extended packet
  IDs, all-in image reception, out-of-order data after a leader, duplicate
  detection, and bounded resend requests. The Camera uses classic IDs and
  may keep its last frame in application storage for resend.
- GenApi 2.1.1 subset, tested with schema 1.1. Supports XML and ZIP descriptions, integer and masked
  registers, float and string registers, integer and float features, booleans,
  enumerations, commands, access predicates, and a subset of Converter and
  SwissKnife formulas. Formula operators cover arithmetic, bitwise operations,
  shifts, and parentheses. Integer formulas are bounded to the exact integer
  range used by the evaluator. The node map reads current device values on
  demand.
- SFNC 2.8 feature names and a PFNC 2.5 subset for Mono, Bayer, RGB, and BGR.
- GenDC 1.2 for final linear containers in memory. The parser exposes
  components, parts, and raw metadata. The encoder builds a single-image
  container. GenDC on the transport, the GenTL Producer, USB3 Vision,
  Chunk/Multipart, device events, and RDMA remain future work.

The sources identify their protocol and version. Refer to the
[EMVA GenICam documents](https://www.emva.org/standards-technology/genicam/genicam-downloads/)
and the [A3 GigE Vision specifications](https://www.automate.org/vision/vision-standards/download-the-gige-vision-standard).
The A3 full specifications require a download request. Current results describe
the implemented profile and interoperability exercised by the tests.

## OSAL and tests

`src/osal/osal.c` is the Linux and Windows platform implementation. It adapts
threading and clock primitives from the supplied IPC OSAL and adds IPv4
networking. For an RTOS port, implement the same internal `src/osal/osal.h`
interface in `osal.c`, or select a separate file with `GC_OSAL_SOURCE`. Use
`GC_OSAL_LIBRARIES` for the RTOS networking, threading, or C math libraries
required by the port. The port needs a TCP/IP stack that exposes UDP,
a monotonic clock, and the specified synchronization primitives.
The Host build also requires Expat and zlib.

Tests cover local camera and host integration, compressed XML and feature
access, control contention, pending acknowledgements, frame assembly and
resend, buffer ownership and shutdown, and GenDC encoding and parsing.
