# Xmac NTFS

FSKit module that mounts NTFS partitions read-write on macOS 15.4+, built on libntfs-3g.
Shipped inside Xmac; this module is free software under GPL-2.0-or-later (see LICENSE),
because it links libntfs-3g (tuxera/ntfs-3g, GPL-2.0-or-later).

## Layout
- `Sources/Module/Bridge/` — C engine (`xntfs.h`): thread-safe facade over libntfs-3g, device
  I/O over FSKit's `FSBlockDeviceResource` with sector alignment.
- `Sources/Module/*.swift` — FSKit `FSUnaryFileSystem` + `FSVolume`.
- `Sources/Host/` — container app that registers the module.
- `Tests/engine/` — engine tests on NTFS images (unit + randomised model test, ASan/UBSan).

## Build
```sh
./Scripts/build-vendor.sh      # static libntfs-3g (arm64 + x86_64) and ntfsprogs test tools
./Scripts/test-engine.sh       # engine tests; SEEDS="1 2 3" for more fuzz seeds
xcodegen generate && xcodebuild -scheme XmacNTFS -allowProvisioningUpdates build
```
Signing needs the `com.apple.developer.fskit.fsmodule` entitlement (paid team, provisioning profile).

## Enabling the module
System Settings › General › Login Items & Extensions. Switch the list to **By Category**,
open **File System Extensions** and turn on **Xmac NTFS**. The switch in the *By Application*
view does not work on macOS 26/27 (it is rejected with EPERM; see macfuse/macfuse#1194).
Afterwards, NTFS partitions mount read-write when plugged in: the module probes NTFS media
before macOS's own read-only driver.

## Safety
- Windows hibernated / Fast Startup or cached metadata → mounted read-only.
- A journal left by an unsafe eject is reset on mount (standard ntfs-3g `recover` behaviour).
- Names are normalised to NFC; lookups are case-insensitive, case-preserving.
- Extended attributes are stored as NTFS named streams, so macOS writes no `._` files.
- Windows reparse points other than symlinks (WOF/CompactOS compression, dedup) are not
  readable or writable (EOPNOTSUPP) rather than silently wrong.
