# AirportItlwm for macOS 15 Sequoia (Intel AX201), beta

This is a fork of [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm) 2.4.0
(upstream commit `53c51c2`). It runs `AirportItlwm` on macOS 15 against Apple's own
`IO80211Family` and `IOSkywalkFamily`. You do not need the legacy IO80211 kexts or root patches.
Wi-Fi shows up as a normal AirPort interface in the Wi-Fi menu and System Settings.

**Status: beta.** It has been tested on one machine only. Use it at your own risk and keep a
working EFI to fall back to.

## Tested on

| Item | Value |
|---|---|
| Wi-Fi card | Intel AX201 (iwx) |
| macOS | 15.7.7 (24G720), `IO80211Family` 1200.13.1 |
| Board / SMBIOS | Z490-I, iMac20,2 |
| Bootloader | OpenCore, `SecureBootModel` Disabled, SIP disabled |
| Bluetooth | IntelBluetoothFirmware + IntelBTPatcher + BlueToolFixup |

Other iwx cards (AX200, AX210, AX211) should take the same code path, but they have not been
tested. The older iwm and iwn cards have not been tested with this fork either.

## What is different from upstream

- A Sequoia Skywalk interface (`AirportItlwmV2`, `AirportItlwmSkywalkInterface`) that registers with
  the native `IO80211Family` V2 controller and data path.
- Native handlers for the ioctls that Sequoia's `airportd` calls. These include
  BSS info, beacon info, roaming, regulatory and the WCL (Wi-Fi connection layer) calls. They follow
  the behaviour of Apple's own drivers wherever the Intel hardware allows it.
- Faster and steadier reconnects. In our tests, a forced disconnect reconnected in about 0.5 s, 5 out of 5 times.
- Bluetooth coexistence: the radio stays in constant-awake mode (CAM), and 2.4 GHz MIMO uses static
  SMPS. Together these keep Bluetooth mouse and keyboard latency low.

## Known limits

- `APPLE80211_IOC_WCL_ARP_MODE` returns "unsupported" on purpose. The firmware has no ARP offload
  that maps onto it.
- Intel firmware has no per-connection retry or RTS knobs. The driver accepts these WCL calls but
  cannot apply them.
- Power save stays in CAM on purpose (see above). This costs some idle power.
- Some registry properties (`AIRITL_TT_WQ`, `AIRITL_WQ*`) come from a work-loop timer probe.
  They are harmless and will be removed in a later version.

## Install (OpenCore)

1. Copy `AirportItlwm.kext` to `EFI/OC/Kexts/`.
2. Add it under `Kernel > Add` with `MinKernel` `24.0.0` and `MaxKernel` `24.99.99`.
3. Disable `itlwm.kext` (HeliPort) if you have it. Only one of the two drivers may load.
4. Do not block or force-inject `IOSkywalkFamily` or `IO80211Family`. This driver needs Apple's
   stock versions.
5. Reboot and check with `kextstat | grep AirportItlwm`.

To roll back, disable the entry again, or put your previous kext back.

## Debug logging

By default the driver logs only errors and a few state changes. To turn on its detailed connect,
roam and WCL logs, add the boot-arg `-airitldbg`. They go to the kernel log, so read them with
`sudo dmesg`. Messages logged before the boot-arg is read (very early in `start`) do not appear,
even with `-airitldbg` set.

## Build

```sh
git clone https://github.com/acidanthera/MacKernelSDK
xcodebuild -project itlwm.xcodeproj -target AirportItlwm-Sonoma14.4 -configuration Release build
```

The `AirportItlwm-Sonoma14.4` target is the one built for macOS 15 here: it compiles with
`__IO80211_TARGET=__MAC_15_0`. Firmware is embedded at build time, as in upstream. The kext must be
signed before use. An ad-hoc signature is enough for OpenCore injection:
`codesign --force --sign - AirportItlwm.kext`.

## License and credits

GPL-2.0, the same as upstream (see `LICENSE`). All credit for itlwm, net80211 and the Intel
drivers goes to the upstream authors listed below. The Sequoia work in this fork builds on them.

---

## Upstream README

# itlwm

**An Intel Wi-Fi Adapter Kernel Extension for macOS, based on the OpenBSD Project.**

## Documentation

We highly recommend exploring our documentation before using this Kernel Extension:

- [Intro](https://OpenIntelWireless.github.io/itlwm)
- [Compatibility](https://openintelwireless.github.io/itlwm/Compat)
- [FAQ](https://openintelwireless.github.io/itlwm/FAQ)

## Download

[![Download from https://github.com/OpenIntelWireless/itlwm/releases](https://img.shields.io/github/v/release/OpenIntelWireless/itlwm?label=Download)](https://github.com/OpenIntelWireless/itlwm/releases)

## Questions and Issues

Check out our [FAQ Page](https://openintelwireless.github.io/itlwm/FAQ) for more info.

If you have other questions or feedback, feel free to [![Join the chat at https://gitter.im/OpenIntelWireless/itlwm](https://badges.gitter.im/OpenIntelWireless/itlwm.svg)](https://gitter.im/OpenIntelWireless/itlwm?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge).

We only accept bug reports in GitHub Issues, before opening an issue, you're recommended to reconfirm it with us on [Gitter](https://gitter.im/OpenIntelWireless/itlwm); once it's confirmed, please use the provided issue template.

## Credits

- [Acidanthera](https://github.com/acidanthera) for [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)
- [Apple](https://www.apple.com) for [macOS](https://www.apple.com/macos)
- [AppleIntelWiFi](https://github.com/AppleIntelWiFi) for [Black80211-Catalina](https://github.com/AppleIntelWiFi/Black80211-Catalina)
- [ErrorErrorError](https://github.com/ErrorErrorError) for UserClient bug fixes
- [Intel](https://www.intel.com) for [Wireless Adapter Firmwares](https://www.intel.com/content/www/us/en/support/articles/000005511/network-and-io/wireless.html) and [iwlwifi](https://wireless.wiki.kernel.org/en/users/drivers/iwlwifi)
- [Linux](https://www.kernel.org) for [iwlwifi](https://wireless.wiki.kernel.org/en/users/drivers/iwlwifi)
- [mercurysquad](https://github.com/mercurysquad) for [Voodoo80211](https://github.com/mercurysquad/Voodoo80211)
- [OpenBSD](https://openbsd.org) for [net80211, iwn, iwm, and iwx](https://github.com/openbsd/src)
- [pigworlds](https://github.com/OpenIntelWireless/itlwm/commits?author=pigworlds) for DVM devices support, MIRA bug fixes, and Tx aggregation for MVM Gen 1 devices
- [rpeshkov](https://github.com/rpeshkov) for [black80211](https://github.com/rpeshkov/black80211)
- [usr-sse2](https://github.com/usr-sse2) for implementing the usage of Apple RSN Supplicant and bug fixes
- [zxystd](https://github.com/zxystd) for developing [itlwm](https://github.com/OpenIntelWireless/itlwm)

## Acknowledgements

- [@penghubingzhou](https://github.com/startpenghubingzhou)
- [@Bat.bat](https://github.com/williambj1)
- [@iStarForever](https://github.com/XStar-Dev)
- [@stevezhengshiqi](https://github.com/stevezhengshiqi)
- [@DogAndPot](https://github.com/DogAndPot) for providing resources and help for system configuration
- [@Daliansky](https://github.com/Daliansky) for providing Wi-Fi cards
