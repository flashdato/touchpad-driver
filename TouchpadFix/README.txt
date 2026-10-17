TouchpadFix - driver for the Hantick 0911:5288 I2C touchpad
(Infinix INBook X3 XL422; Windows shows it as "I2C HID Device", ACPI\SYNA360)
================================================================================

Why: Windows' own driver (hidi2c.sys) lets this chip drop off the I2C bus right
after start-up, so it never sends input. Linux works because it waits after a
reset instead of waiting for an interrupt the chip never sends, retries while
the chip wakes up, and never puts it to sleep. This driver does the same on
Windows, and recovers the chip by itself if it ever stops answering.

The driver is test-signed (signed with its own certificate, not by Microsoft),
so Windows must be in test-signing mode, which needs Secure Boot off.

Steps (PowerShell as administrator, in this folder)
---------------------------------------------------
  Set-ExecutionPolicy -Scope Process Bypass -Force

  1. .\tpctl.ps1 prepare
       Tells you if Secure Boot must be turned off in the BIOS, then turns on
       test signing. Restart when it says so, and run prepare once more.
  2. .\tpctl.ps1 install
       Installs the driver on the touchpad, then shut down completely.
  3. Power on, wait a minute, use the touchpad, then:
     .\tpctl.ps1 status
       Shows what the driver sees and what to do next.

If the pointer does not move
----------------------------
  .\tpctl.ps1 status          read "What to do next"
  .\tpctl.ps1 test            25-second input test
  .\tpctl.ps1 mode mouse      plain mouse instead of precision touchpad
  .\tpctl.ps1 polling on      if no touchpad interrupts arrive
  .\tpctl.ps1 collect         saves a full report to Desktop\TouchpadFix

Undo
----
  .\tpctl.ps1 uninstall       Windows goes back to its own driver
  bcdedit /set testsigning off   then restart (turn Secure Boot back on if you like)

If Windows ever fails to start after installing
-----------------------------------------------
  Let it reach "Automatic Repair" > Advanced options > Command Prompt, and run:
    bcdedit /set {default} testsigning off
  then Continue. Without test signing the driver cannot load, and Windows starts
  normally (touchpad back to the old state).

Keep test signing on while you use this driver: if it is turned off, the
touchpad stops working again.
