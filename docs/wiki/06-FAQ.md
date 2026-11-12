<!-- Source: https://docs.waveshare.com/ESP32-S3-ePaper-3.97/FAQ (fetched 2026-11-12) -->

# FAQ

 Q: After downloading the module program, if I try to download it again, sometimes I encounter issues such as inability to connect to the serial port or programming failure?

- You can press and hold the BOOT button to power on again, and the module can enter the download mode at this time, which can solve most of the problems that cannot be downloaded.

 Q: Failed to set up the VSCode environment?

- First consider the network issue, try switching to another network.

 Q: Error when compiling an Arduino program?

- Check if the Arduino IDE -> Tools is properly configured.

 Q: The first program compilation is extremely slow. How to handle it?

- It's normal for the first compilation to be slow, just wait patiently.

 Q: What is the standby time under lithium battery power supply?

- In clock mode, a 1500mAh battery can operate for more than 15 days.

 Q: The example program is successfully flashed but won't run?

- First, check if the example version matches the product version.

 Q: How to handle not finding the AppData folder?

- Some AppData folders are hidden by default. You can set your system to show them.
- English System: File Explorer -> View -> Check "Hidden items”
- Chinese System: File Explorer -> View -> Show -> Check "Hidden items”

 Q: How to check which COM port I am using?

- Windows System:

  - ①Check via Device Manager: Press the `Windows`+ `R` keys to open the "Run" dialog box; input `devmgmt.msc` and press Enter to open the Device Manager; expand the "Ports (COM and LPT)" section, where all COM ports and their current statuses will be listed.
  - ②Check using Command Prompt: Open the Command Prompt (CMD), enter the `mode` command, which will display status information for all COM ports.
  - ③Check Hardware Connection: If an external device is already connected to a COM port, the device typically occupies a port number. You can identify the port by checking the connected hardware.
- Linux System:

  - ①Use the `dmesg` command: Open the terminal.
  - ②Use the `ls` command: Enter `ls /dev/ttyS*` or `ls /dev/ttyUSB*` to list all serial port devices.
  - ③Use the `setserial` command: Enter `setserial -g /dev/ttyS*` to view the configuration information for all serial port devices.

[Give Feedback](https://docs.google.com/forms/d/e/1FAIpQLSfayJEZ5J-dp-3Wq_dkWsVRhNuQ6C79_GNV62mYuFW8Mj6U8Q/viewform)
