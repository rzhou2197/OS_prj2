For the testfile
type ndk-build in /test/jni
adb push testFile data/misc
For the kernel
make clean
make -j4
open the avd 
emulator -avd OsPrj-516021910576 -kernel /home/rong321/Desktop/O/goldfish/arch/arm/boot/zImage -show-kernel
adb shell
cd data/misc
ps -P|grep processtest
./testFile