# Power and battery behavior

Use `CLAUDE.md` and current source for measured values; current depends on image,
BLE state, SD state, and the physical measurement point.

## Required distinctions

- `AT+POWEROFF` calls the Clip power-off event and enters PMIC ship mode. It is
  not a substitute for testing nRF SYSTEM OFF or peripheral leakage.
- Debug UART console is a material idle-current contributor. The `production`
  snippet disables console/UART log backend; use it for power validation.
- SD low power is an explicit lifecycle: FATFS unmount, disk deinit, SPI4
  runtime suspend, CS low, then LDO2 off. FS logging can keep SD active.
- Main/radio DCDC and `CONFIG_NRF70_QSPI_LOW_POWER` are part of the low-power
  configuration. Do not regress them while changing unrelated Kconfig.

## Battery model

`battery.c` uses the nRF fuel-gauge library, a 240 mAh model, and a persisted
state record in settings. The display percent is monotonic within a charge or
discharge phase to avoid 1% visual bounce; charging and discharging have
different monotonic directions. Persist fuel-gauge state before reboot/power
events and whenever the displayed SoC changes.

## The cable changes the experiment

`clip_sd_busy()` returns true while `usb_cdc_is_enabled()`, and that stays true
until **10 minutes after VBUS goes away**. So with a cable attached the SD card
never idle-powers-off and no SD-idle behaviour — power or correctness — can be
observed at all. Anything in that area has to be tested **on battery**, with the
device read from the heartbeat rather than the AT channel.

Recording is also refused while USB is up and VBUS is present
(`clip_event.c:557`, `CONFIG_CLIP_USB_MSC`), because the host could be writing
the card over MSC. The gate reads `battery_vbus_present()` from the NPM1300, not
the USB controller's flag, which reports phantom `VBUS_REMOVED` when the WiFi
radio powers up.

And note `AT+USB=off` disables CDC as well as MSC: it removes the AT channel and
only a physical replug brings it back. To hand the card back to the device,
unmount on the host instead.

## Validate changes

1. Test debug and production separately.
2. Confirm `AT+LOG=off` before idle-current checks.
3. Wait beyond `CONFIG_CLIP_SD_IDLE_DELAY_MS` and confirm SD/LDO2/SPI4 state.
4. Check BLE advertising, Wi-Fi off, USB detached, and OLED state explicitly.
5. Record rail, voltage, fixture, firmware image, and elapsed idle time with
   every power number.
