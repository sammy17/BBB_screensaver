# BBB_screensaver
Screensaver kernel module for Beagle Bone Black

This is a screensaver kernel module developed for Beagle Bone Black running Linux. This project was done as a requirement for EC535 course at Boston University. 

## How to run:
Set the KERNELDIR variable and CROSS variable to kernel build directory and cross compilation prefix respectively in km/Makefile. 
Then compile the kernel module.

``` make ```

Copy the myscreensaver.ko file to the Beagle Bone SD card root folder along with root/images folder.

``` cp -r km/myscreensaver.ko root/images <SDcard_mount_path>/root/```

Use the following command to insert the module:

```mknod /dev/myscreensaver c 61 0```

```insmod myscreensaver.ko```

LCD touch screen should be connected to BBB when the module is inserted. 
If everything goes well, you well obser the screensaver on the screen after 15 seconds of inactivity. 

Good luck!
