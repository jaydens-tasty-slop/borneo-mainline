# Mainline Linux on the Motorola Moto G Power (2021)

Mainline Linux and postmarketOS with Plasma Mobile on the Moto G Power (2021), codename `borneo`.
The aim is a kernel that can go upstream in full. Status as of 2026-11-05.

- SoC: Qualcomm SM6115 (Snapdragon 662); PMICs PM6125 and PMI632
- Panel: DJN FT8009, 720x1600
- Kernel: v7.4-rc1 plus 144 commits
- Boot: unlocked Motorola bootloader, kernel sent with `fastboot boot`
- Root filesystem: an image file on the microSD card. Of the flash partitions, only `boot_a` and
  `dtbo_a` are written, and slot B keeps Android

**The kernel branch isn't public yet, and no patch has been sent upstream.** This repository holds
the tools and the postmarketOS configuration. No firmware is included: it comes from a backup of the
phone's own partitions.

Most of the patches, the tools and this README were written by Claude Code (Claude Opus 5.5), driving
the phone over USB, with a person at the phone for hands-on tests. A person still has to review and
sign off each patch before it goes anywhere.

## Status

Works = tested on this hardware. "Not tested" means no one has tried it, not that it's broken.

| Area | Status | Notes |
|---|---|---|
| **System** | | |
| Boot, CPUs, cpufreq, CPU idle | Works | no `clk_ignore_unused` or `pd_ignore_unused` |
| Thermal, ADCs | Works | |
| eMMC | Works | HS200; left write-protected on purpose |
| microSD | Works | SDR104 |
| Reboot | Works | warm reset; development images never power off |
| System sleep (s2idle) | Partial | wakes by RTC and power key; see [deep sleep](#deep-sleep) |
| **Display and input** | | |
| Display, backlight | Works | new panel driver, AW99703 backlight, PMI632 LCDB rails |
| GPU (Adreno 610) | Works | OpenGL ES 3.2 on freedreno, Vulkan on Turnip |
| Touchscreen | Works | FT8009 over SPI; reaches the corners in `evtest`, used by hand in Plasma |
| Power and volume keys | Works | |
| **Connectivity** | | |
| WiFi (WCN3990) | Works | WPA2 on 2.4 and 5 GHz; IPv6 is off in the image, see [known problems](#known-problems) |
| Bluetooth | Partial | LE scan works, pairing not tested; the address is generated, not the real one |
| USB device mode | Works | adb |
| USB host / OTG | Not tested | |
| Mobile data, SMS | Works | tested with a SIM; the modem needs Motorola's unit info in SMEM to go online |
| Voice calls | Not working | needs an IMS client on the AP and a voice driver; not close |
| GNSS | Not verified | |
| **Power** | | |
| Charging | Partial | about 225 mA from a 500 mA port; wall charger and end of charge not observed |
| Battery gauge | Partial | reports capacity and status; accuracy not judged |
| Flash LED, notification LED | Works | |
| Vibrator | Works | capped under feedbackd at about the vendor's 3 V |
| **Multimedia** | | |
| Earpiece, both microphones | Works | WCD9370 over SoundWire, through PipeWire |
| Loudspeaker | Not working | Awinic amp (ID 0x1852) powers up and gets clocks, but stays silent; cause not found |
| Headphones, headset mic, jack | Not tested | |
| Video decode (iris) | Partial | output matches software decode, but firmware reports an error when a session closes |
| Video encode (iris) | Not working | session error when streaming starts |
| Cameras | Partial | all four capture, see [cameras](#cameras) |
| **Sensors** | | |
| Accelerometer, gyro, magnetometer, compass, light, proximity | Works | through the ADSP's Sensor Core, `libssc` and `iio-sensor-proxy` |
| Auto-rotate | Partial | works; on this kernel, after a suspend, not yet confirmed by hand |
| Fingerprint, SAR sensor | Not started | |
| **Software** | | |
| postmarketOS with Plasma Mobile 6.8 | Works | from the microSD card; browser, app drawer, on-screen keyboard |
| Android LTS kernel | Not started | mainline came first |

### Cameras

Main (S5KGM1ST, tested at 4000x3000), front (S5K4H7YX), macro (OV02B10) and depth (OV02B1B, mono)
all deliver real images, and the main and macro lens motors work. libcamera lists and captures all
four through its software ISP, and plasma-camera finds them.

Limits:

- Macro and depth share a CSIPHY, so only one of them can stream at a time.
- Auto exposure is poor in the dark, as libcamera has no sensor helpers for these yet.
- A stream doesn't survive suspend, but restarting the capture works.
- The module's EEPROM calibration isn't read.
- plasma-camera's on-screen preview hasn't been checked by eye.
- The sensor drivers are on hold: where their register tables come from has to be settled first.

### Deep sleep

With a direct `echo mem`, the SoC reaches vdd-min for about 43 s of a 46 s sleep, thanks to two clock
fixes, in the display clock controller and CAMSS. Through `systemctl suspend`, one boot showed no new vdd-min time. That
boot may have been spoiled by a sensor check run just before it, so this needs a clean retest. Battery
drain hasn't been measured.

## Known problems

- **On-screen keyboard:** reported cut off in portrait on one boot, while an earlier portrait
  screenshot shows it whole. Not yet explained.
- **Text console on wake:** the kernel log can flash on screen. Probably the development command
  line's `no_console_suspend`, not tested without it.
- **WiFi on 5 GHz:** earlier runs missed broadcast and multicast frames. It didn't reproduce on
  2026-11-02 and the cause is unknown. IPv6 stays off in the image because of it.
- **DSP restarts:** sound comes back after an ADSP restart with the held `lpass-dsp-restart` series.
  A debugfs crash during a stream still resets the whole phone, and a real crash during a stream
  hasn't been tested.
- **Sensors after a DSP restart:** `iio-sensor-proxy` can't claim the accelerometer until a reboot, so
  auto-rotate stops.
- **FastRPC:** a DSP crash leaks each blocked call's context. Left alone on purpose, since freeing them
  may expose a use-after-free.
- **Boot flake:** seen once in about ten boots on one day: USB came up, the host never connected, and
  the phone reset itself. A retry works.
- **Lens click:** each lens power-up clicks, even at position 0. That's normal, but nothing should
  power-cycle a lens in a loop.

## Upstream state

By commit author, 101 of the 144 commits were written here and 43 are other people's work. `tools/series.list` splits
them into 48 series, each with its own maintainers. 16 of the series are on hold, each with its reason
in that file. The other 32 are waiting for a person's review.

On hold because the work is someone else's, or builds on it:

| Series | Author | Waiting for |
|---|---|---|
| `aw99703` backlight, `touch-edt` touchscreen | Luka Panio | their promised v2 |
| `lcdb` regulators | Noxwell | sign-off and agreement on a rewrite |
| `charger`, `flash-led` | László Gergely Lajosházi | contact with the author |
| `sm6115-audio` | Alexey Klimov, Konrad Dybcio | checking whether they are still on it |
| `aw882xx` amplifier | Filippo Falezza | a working speaker here |
| `fastrpc-interrupted` | based on Anandu Krishnan E's patch | reporting a crash to them first |
| `qrtr-carried` | Joonhoe Kim | nothing: already in netdev as 743b5c86276b, dropped on rebase |

On hold for other reasons:

| Series | Waiting for |
|---|---|
| `lpass-dsp-restart` | an RFC first, as it crosses several subsystems |
| `camss-shared-csiphy` | the CAMSS maintainers' view on modelling a shared CSIPHY |
| `borneo-sensors`, `dw9719-more-parts` | a decision on where the register tables come from |
| `moto-smem-board` | a decision on how to pitch a Motorola-only driver |
| `borneo`, `borneo-cameras` | the board itself, last |

The fixes that stand alone came out of this bring-up, in ASoC, Bluetooth, ath10k, QMI, serdev, FastRPC,
genpd, cpuidle, flash LEDs, V4L2, CAMSS, dw9719, IPA and others. Each has its own series in
`tools/series.list`. A commit message describes each problem and how it was tested. Some have only been
compile tested on the paths this phone doesn't use.

## Tools

| Tool | What it does |
|---|---|
| `tools/bh` | drives the phone: boot from RAM, wait for a mode, read crash logs. Writes only to `boot_a` and `dtbo_a` |
| `tools/bk` | builds a boot image from a kernel tree in `linux/` |
| `tools/mkfw` | lays out firmware from your own partition backups |
| `tools/mkroot`, `tools/mkpmos` | build the Alpine and postmarketOS images on the microSD card |
| `tools/mkseries` | cuts the kernel branch into series, with checkpatch and maintainer lists; signs and sends nothing |
| `tools/ftrace/` | function tracing into persistent RAM, for a phone that stops with nothing on the console |

The scripts need the phone's serial, so they can't reach another device on the host. Put it in
`build/serial`, which git ignores, or set `ANDROID_SERIAL`.

Boot, once the images exist:

```sh
adb reboot bootloader
tools/bh wait fastboot --timeout 90
tools/bh boot build/dev/borneo-pmos.img
```

The Plasma user is `user`, password `147147`. Change it if the phone stays on a network.

These tools were written for one phone. Reading them is more useful than running them, and until the
kernel branch is public they can't be used to build anything.

**If you adapt them:** never write to `modemst1`, `modemst2`, `fsg`, `fsc`, `persist`, `prodpersist`,
`utags`, `hw`, `xbl`, `tz` or `abl`. Some of those hold the IMEI and calibration data, and losing them
can't be undone. Run `rmtfs` only on copies held in RAM.
