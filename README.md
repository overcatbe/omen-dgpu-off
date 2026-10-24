# omen-dgpu-off

A tiny Linux kernel module that fixes the **"laptop doesn't really turn off"**
problem on the **HP OMEN 16-ap0xxx** (board ID `8E35`, AMD Ryzen + Radeon 610M
iGPU + NVIDIA RTX 50-series Laptop dGPU).

## The symptom

You shut the laptop down under Linux. The screen goes black, the power LED goes
off, and it looks off. But the NVIDIA GPU is still powered:

- the chassis stays warm (sometimes hot, even in a closed bag),
- the battery keeps draining with the lid closed and the charger unplugged,
  until it is empty.

It happens on every distro and every kernel tested (6.12 up to 7.x), and kernel
parameters don't change it. Under Windows the same machine turns off properly.

## What the error is

This is a firmware (BIOS/ACPI) bug. Here is what the decompiled ACPI tables of
BIOS F.06 show:

1. The dGPU (`\_SB.PCI0.GPP0.PEGP`) is powered through an ACPI **power
   resource**, `\_SB.PCI0.GPP0.PG00` (in SSDT5). `PG00._ON` and `PG00._OFF`
   switch the GPU's power rail with an active-low GPIO named `PWEN`.
   `PG00._OFF` does it properly: it puts the PCIe link into L2/L3 (`DL23`), then
   drops the rail (`POFF`).
2. The firmware has a hook that runs when the machine enters a sleep state,
   including S5 (soft-off): `\_SB.PCI0.GPTS`. That is where the firmware should
   make sure the dGPU rail is off. **On this BIOS, `GPTS` is empty.**
3. The power switch for the rail is not cleared when the machine enters S5. The
   GPIO keeps whatever state it had last.
4. Late in shutdown, Linux wakes runtime-suspended PCI devices back up
   (`pci_device_shutdown()` calls `pm_runtime_resume()`). For the dGPU that
   means `PG00._ON` runs, the rail goes **on**, and nothing turns it off again
   before the machine powers down. So the GPU stays powered in S5.

Windows apparently leaves the GPU in D3cold when it shuts down, so the rail is
already off and the empty hook doesn't matter there. That would explain why
HP's testing never caught it.

### The false lead: `WQBZ`

Every boot, the kernel log on these laptops shows:

```
ACPI BIOS Error (bug): AE_AML_BUFFER_LIMIT ... Method: \_SB.WMID.WQBZ
ACPI Error: Aborting method \_SB.WMID.WQBE
```

Most reports online (including some sent to HP) blame this for the shutdown
problem. **It is unrelated.** `WQBZ` builds the "Boot Order" string for HP's
BIOS-settings WMI interface (read by `hp-bioscfg` a few seconds after boot). It
reads past the end of a 50-byte buffer that has no terminating zero byte. It's
a real bug, but a harmless one, and it has nothing to do with GPU power.

### Things that do *not* help

- `acpi_osi=!` plus `acpi_osi="Windows 20xx"`
- `acpi_sleep=s5`
- `acpi_rev_override=5`
- writing to `/sys/bus/pci/devices/.../power/control`
- NVIDIA driver options. Shutdown has already moved past the driver by the time
  the rail gets switched back on.

## How the fix works

The module does the job the empty `GPTS` should do:

- At load time it checks the DMI board vendor and name (`HP` / `8E35`) and looks
  up the `\_SB.PCI0.GPP0.PG00` ACPI handle. On any other machine it refuses to
  load (`-ENODEV`).
- It registers a **syscore shutdown** callback. Syscore callbacks run after
  `device_shutdown()`, so after the step that switched the dGPU back on, and
  right before the kernel hands control to the firmware for S5.
- In that callback it calls `PG00._OFF` once and logs the `_STA` value before
  and after.
- It only does this for a real poweroff (`system_state == SYSTEM_POWER_OFF`).
  Reboot, suspend and hibernate are left alone.

The whole module is about 80 lines. See [`omen-dgpu-off.c`](omen-dgpu-off.c).

## Install (DKMS)

```sh
git clone https://github.com/overcatbe/omen-dgpu-off.git
cd omen-dgpu-off
sudo cp -r . /usr/src/omen-dgpu-off-1.0
sudo dkms install omen-dgpu-off/1.0
sudo modprobe omen-dgpu-off
sudo dmesg | grep omen_dgpu_off        # expect: "armed, PG00._STA=..."
```

Then load it at every boot with whatever your distro uses:

- systemd distros: `echo omen-dgpu-off | sudo tee /etc/modules-load.d/omen-dgpu-off.conf`
- Slackware: add `/sbin/modprobe omen-dgpu-off` to `/etc/rc.d/rc.modules.local`

Without DKMS: `make` in this directory builds `omen-dgpu-off.ko` for the running
kernel (needs the kernel headers).

Remove it again with `sudo dkms remove omen-dgpu-off/1.0 --all` and then delete
the boot entry.

## Kernel upgrades

The module has to be built for every new kernel. Most distros' DKMS hooks do
this automatically when the kernel package is installed.

**Slackware:** there is no install-time hook. `rc.M` runs the DKMS
autoinstaller at boot, but that happens *after* `rc.S` has already run
`rc.modules.local`. So on the first boot of a new kernel the module is built
but not loaded, and that first poweroff leaves the GPU on. Build it by hand
before you reboot into the new kernel:

```sh
sudo dkms install omen-dgpu-off/1.0 -k <new-kernel-version>
ls /lib/modules/<new-kernel-version>/updates/omen-dgpu-off.ko
```

If you forgot, run `sudo modprobe omen-dgpu-off` before you shut down.

## Status

Tested on one OMEN 16-ap0xxx (BIOS F.06, Slackware-current, kernel 6.18.52,
NVIDIA 595 open kernel modules; it also builds against 7.3.0). Result after 23 minutes off and
unplugged: no measurable battery drop (`energy_now` unchanged). Without the
module the battery used to drain steadily until empty. Poweroff does not hang.

Limitations:

- One machine, one BIOS version. Reports from other 16-ap0xxx units and newer
  BIOS versions (F.12, F.13, ...) are very welcome. Please open an issue with
  your board ID (`cat /sys/class/dmi/id/board_name`), BIOS version and result.
- It uses the `struct syscore` / `register_syscore()` API from recent kernels.
  Older kernels need `register_syscore_ops()` instead.
- You won't find the module's log line from poweroff on disk, because syslog
  has already stopped by then. To check that it worked, look at the battery
  level after the machine has been off for a while, or at the chassis
  temperature.
- This is a workaround. The real fix belongs in HP's firmware: `GPTS` should
  power off the dGPU rail before S5.

**No warranty.** This calls an ACPI method from the firmware's own tables, on the
one board it was written for. Use it at your own risk.

## License

GPL-2.0, see [LICENSE](LICENSE).
