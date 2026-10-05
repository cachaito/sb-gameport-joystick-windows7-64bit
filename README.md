# Driver for Sound Blaster Live! Gameport Joystick under Windows 7 64-bit

This project was created because it turned out that Creative never released a driver for the gameport input available on the Sound Blaster Live! 5.1(0100) card for Windows 7 64-bit. Furthermore, Windows 7 itself does not have implemented HID device support for this port.

## Limitations:
- Since Windows 7 certification system has been defunct for many years, no new certificate will sign the driver. To use this driver, you must enable the option in `CMD`: `bcdedit.exe -set TESTSIGNING ON`
- I created this driver specifically for my Saitek ST50 joystick. It has three axes with a throttle and two fire buttons. I'm providing the source code because it can easily be used to create a more generic driver or a driver tailored to your joystick. 
- The driver was compiled in `WinDDK 7600.16385.1` and it's runtime environment is Windows 7 64bit, but upgrading the code for newer systems shouldn't be a problem.

## Installation
1) If Windows doesn't recognize the new gameport after installing the Sound Blaster card in the PCI slot, you need to install the audio driver for the card first: https://github.com/kxproject/kX-Audio-driver-binaries
2) Install the joystick driver from the GameportHidPkg folder
3) Run `CMD+R` for `joy.cpl` for the calibration.
    
## Authors
- I created this driver using [Vibe](https://chat.mistral.ai/).


## License
[MIT](https://choosealicense.com/licenses/mit/)
