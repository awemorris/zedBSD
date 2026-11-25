# mikuterm-idd (UMDF 2 / IddCx 1.4 Virtual Display Driver)

`mikuterm-idd` is a user-mode Indirect Display Driver (IddCx 1.4) that creates a virtual monitor for `mikuterm.exe` Extended Display mode.

## Building

Requires Visual Studio 2022 with the Windows Driver Kit (WDK) installed:

```powershell
msbuild drivers\win32\mikuterm-idd\mikuterm_idd.vcxproj /p:Configuration=Release /p:Platform=x64
```

## Installing a Test-Signed Build

1. Enable test-signing on the target Windows machine (Administrator Command Prompt):
   ```cmd
   bcdedit /set testsigning on
   ```
   Reboot Windows.
2. Import the test certificate (`mikuterm_idd_test.cer`) into `LocalMachine\Root` and `LocalMachine\TrustedPublisher`:
   ```cmd
   certutil -addstore Root mikuterm_idd_test.cer
   certutil -addstore TrustedPublisher mikuterm_idd_test.cer
   ```
3. Install the driver package:
   ```cmd
   pnputil /add-driver mikuterm_idd.inf /install
   ```
