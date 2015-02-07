Running android-x86 on Amazon ec2 - howto
-----------------------------------------


1. Configure your build environment.

2. Install the repo client.

3. Get the source code (details here). Use the official repository, as shown bellow; 
there is also a mirror at SourceForge but I ran into several errors when using it.

$ mkdir android-x86
$ cd android-x86
$ repo init -u http://git.android-x86.org/manifest -b ics-x86
$ repo sync

4. Download the xenvfb.c source file and add it to kernel/drivers/video directory.

5. Append the following line to kernel/drivers/video/Makefile.

obj-m += xenvfb.o

6. Download the kernel configuration file (this has also iptables builtin) and save it to kernel/arch/configs/android-x86_defconfig .

7. Download the modified init file and save it to bootable/newinstaller/initrd/ directory.

8. Download the modified 0-auto-detect and 2-mount files and save them to bootable/newinstaller/initrd/scripts/ directory.

9. Build the image as shown below(add a -j4 or -j8 option if you have a decent CPU):

$ . build/envsetup.sh
$ lunch generic_x86-eng
$ make iso_img TARGET_PRODUCT=generic_x86 USE_SQUASHFS=0

10. Once the build is completed, we'll have a generic_x86.iso image in the out/target/product/generic_x86/ directory. 
With this image you can create a bundle that boots successfully on ec2, however, it's not possible to connect to it 
using adb since, for some reason, the dhcpd daemon won't start by default. To fix this, we have to modify the init.rc 
file to always start the dhpc service. First, we mount the iso and extract the ramdisk.img file as shown bellow:

$ mkdir tmp
$ mount generic_x86.iso tmp
$ mkdir ramdisk
$ cd ramdisk
$ zcat ../tmp/ramdisk.img | cpio -idv

This should be the content of the ramdisk directory:

$ ls
data default.prop dev init init.rc proc sbin sys system ueventd.rc

11. Edit the init.rc file and add the dhcpd service after the dbus service, as shown bellow:

service dbus /system/bin/dbus-daemon --system --nofork
  class main
  socket dbus stream 660 bluetooth bluetooth
  user bluetooth
  group bluetooth net_bt_admin

# NEW - Required for Amazon ec2
service dhcpcd /system/bin/dhcpcd eth0
  class main
  oneshot

12. Now we can recreate the ramdisk.img:

$ cd ramdisk
$ find . | cpio -o -H newc | gzip > "../ramdisk.img"

13. Finally we can create the bundle and register the ami on ec2. First we create an ext3 filesystem for the bundle, 
and copy the content of the iso (replacing the ramdisk.img with the newly modified one):

$ dd if=/dev/zero of=android_x86-ics.img bs=1M count=300
$ mkfs.ext3 android_x86-ics.img
$ mkdir tmp-iso; mount generic_x86.iso tmp-iso
$ mkdir tmp-ext3; mount android_x86-ics.img tmp-ext3
$ cp -R tmp-iso/* tmp-ext3
$ cp ramdisk.img tmp-ext3

14. Create the boot directory and the menu.lst which is required by pv-grub:

$ mkdir -p tmp-ext3/boot/grub
$ touch tmp-ext3/boot/grub/menu.lst

15. Edit the tmp-ext3/boot/grub/menu.lst file and add the following content:

default 0
timeout 3

title Android
  root (hd0)
  kernel /kernel root=/dev/sda1 rw console=hcv0
  initrd /initrd.img

16. Create the bundle using ec2-bundle-vol (the aki-64695810 kernel works for the EU region).

17. Upload the bundle using ec2-upload-bundle.

18. Register a new ami from the bundle using ec2-register.
