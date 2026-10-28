# AES67 Virtual Sound Card (Windows, 2 in / 2 out)

A free, open-source AES67 virtual sound card for Windows. It appears in any ASIO-capable
application (DAWs, broadcast software, OBS via ASIO plugins, ...) as the device
**"AES67 Virtual Sound Card"** with 2 inputs and 2 outputs.

* **Receives** one AES67 / RAVENNA / Dante-AES67 stream: load the sender's **SDP file**
  (or pick a stream announced via SAP) and it arrives on inputs 1-2.
* **Transmits** outputs 1-2 as an AES67 multicast stream and **generates the SDP file**
  that receivers need to subscribe.
* **PTP (IEEE 1588-2008)** clock follower with a PI servo. If no grandmaster exists on the
  network it can act as master itself, so two PCs running this driver lock to each other.
* No licence key, no activation, no kernel driver, no driver signing: the ASIO driver is a
  user-mode DLL registered with `regsvr32`. MIT licensed.

Formats: L24 or L16 PCM, 44.1 / 48 / 96 kHz, 1 ms packets (AES67 default), any packet time
from 125 us to 4 ms on receive. Streams with more than 2 channels are accepted; you choose
which pair of stream channels feeds the two inputs.

## Install

1. Download the latest `aes67-vsc-<version>-win64.zip` from the
   [Releases page](https://github.com/John-Butler3/aes67-vsc/releases) (or build it, see
   below) and unpack it to a permanent folder, e.g. `C:\Program Files\AES67 VSC`.
2. Right-click `install.bat` and choose **Run as administrator**. It registers
   `aes67asio.dll` and adds a Windows Firewall rule for the UDP ports (319/320 PTP,
   5004 audio, 9875 SAP).
3. In your audio application select the ASIO device **AES67 Virtual Sound Card**.

`uninstall.bat` (as administrator) removes the registration again. Settings live in
`%APPDATA%\AES67VSC\config.ini`; a log is written to `%APPDATA%\AES67VSC\driver.log`.

Only 64-bit hosts are supported by the prebuilt package.

## Use

Open **aes67vsc-panel.exe** (or press the ASIO *Control Panel* button in your host):

* **Device**: pick the network interface that is on the AES67 network, the sample rate,
  the ASIO buffer size (16 to 4096 frames; multiples of the 1 ms packet keep timing tidiest), PTP mode/domain and the
  *link offset* (receive playout delay, default 4 ms; 1-2 ms works on a quiet switched LAN).
* **Receive**: *Load SDP file...* with the SDP exported by the sending device (Dante
  Controller > AES67 flows, RAVENNA/ANEMAN, another PC with this driver, ...). You can also
  drop an `.sdp` file onto the panel or onto `aes67vsc-panel.exe`. Streams announced via
  SAP appear in the *Discovered* list. For multichannel streams choose which stream
  channel feeds input 1.
* **Transmit**: set a stream name, multicast address and port, then **Save SDP file...**
  (or copy it to the clipboard) and give that file to the receiver. The stream is also
  announced via SAP so AES67 devices with SAP discovery list it automatically.
* **Status** shows the live state of the driver while a host has the device open:
  PTP lock and offset, received/lost packets, transmitted packets, engine timing.

Press **Apply** to save. The running driver picks up stream and clock changes on the fly;
changes to the interface, sample rate or buffer size make it ask the host to restart the
device.

### Command line

`aes67vsc.exe` does everything the panel does, for scripting:

```
aes67vsc import stream.sdp        set the receive stream from an SDP file
aes67vsc export mystream.sdp      write the SDP for the transmit stream
aes67vsc set tx.dest=239.69.1.2 tx.port=5004 general.interface=192.168.1.10
aes67vsc show                     configuration and live status
aes67vsc discover 5               list SAP-announced streams
aes67vsc interfaces               list IPv4 interfaces
aes67vsc selftest                 loop a tone through TX and RX on this machine
```

## How it works

```
 ASIO host  <->  aes67asio.dll (in the host process)
                   |- engine thread   PTP-paced period clock: delivers input blocks,
                   |                  runs the host callback, queues output, sends one
                   |                  RTP packet per packet time
                   |- rx thread       RTP receive -> timestamp-indexed ring buffer
                   |- ptp thread      IEEE 1588 slave (or master), PI servo
                   |- sap thread      SAP announcement of the transmit stream
                   '- housekeeping    status to shared memory, config file watch
 aes67vsc-panel.exe / aes67vsc.exe    read/write config.ini, read status, SAP listener
```

* Receive timing: packets are placed in a ring by RTP timestamp. The playout pointer is
  the local PTP sample clock plus the measured arrival offset minus the link offset, so
  with a PTP-locked sender the latency is exactly the link offset and no resampling is
  needed. Without PTP the arrival offset tracks slow drift and slips a few samples per
  second when necessary.
* Transmit timing: RTP timestamps are the PTP time in samples (`a=mediaclk:direct=0`),
  packets are sent at their nominal time from the engine thread.
* Latency: input = buffer + link offset; output = buffer + 1 packet (+ the receiver's own
  link offset).
* PTP: end-to-end delay mechanism, multicast 224.0.1.129, two-step and one-step masters,
  software timestamps (typically within a few tens of microseconds on a LAN). Default
  profile and AES67 media profile intervals are both accepted. In *Automatic* mode the
  driver follows any grandmaster it sees and becomes master (clockClass 248, priority 128)
  only if none appears within a few seconds; a real grandmaster always wins.

## Interoperability notes

* Dante devices need AES67 mode enabled in Dante Controller; create an AES67 multicast
  flow and export its SDP. Dante uses PTPv2 domain 0 when in AES67 mode.
* Ravenna / Merging: use the SDP from the device's web page or ANEMAN; L24/48k/1 ms flows
  are the common denominator.
* Sample rates must match between the stream and the device; the panel adjusts the device
  rate when you load an SDP with a different rate.
* AM824 / AES3-transparent streams are not supported, only linear PCM.
* Two instances of the driver on one PC (two hosts open at once) would both transmit to the
  same group; only open the device from one application at a time.

## Build from source

Windows (Visual Studio 2019+ or MinGW-w64, CMake 3.16+):

```
cmake -S . -B build -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix dist\aes67-vsc-win64
```

Cross-compile from Linux with MinGW-w64 (`apt install mingw-w64`):

```
./build.sh windows      # build/win64/
./build.sh test         # native unit tests + loopback self test (Linux)
```

The core (SDP, RTP, PTP, SAP, engine) is portable C++17 and also builds on Linux, where
`aes67vsc run` streams headlessly for testing. GitHub Actions builds a Windows package on
every push. No third-party code or SDKs are used; the ASIO COM interface is declared in
`src/asio/iasio.h` from the public ABI.

## Status and limitations

* Built and unit-tested; the RTP/PTP/SAP stack has been verified end to end on Linux
  (loopback and two-process master/slave). The Windows ASIO host integration and control
  panel compile cleanly but have not yet been exercised in a DAW: please report issues.
* Software PTP timestamps limit clock accuracy to the tens of microseconds; good enough for
  sample-accurate playout with a 1-2 ms link offset, not for hardware-grade phase alignment.
* One receive stream and one transmit stream, 2 channels each.
* IPv4 only. No RTCP, no SIP/RTSP, no NMOS (SDP file and SAP discovery only).

## License

MIT. See `LICENSE`.
