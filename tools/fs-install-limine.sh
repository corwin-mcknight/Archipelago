
# Store PWD
TOOLS_DIR=$(pwd)

mkdir -p ../build/tools
cd ../build/tools
if [ ! -d "limine" ]; then   
    git clone https://github.com/limine-bootloader/limine.git --branch=v10.x-binary --depth=1
    make -C limine
fi

cd ..

mkdir -p sysroot
cp -r ../media/* ./sysroot/

cd tools/limine
cp limine-bios-cd.bin limine-uefi-cd.bin limine-bios.sys ../../sysroot/boot/
cd ../..

xorriso -as mkisofs -b boot/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        --quiet \
        sysroot -o image.iso

./tools/limine/limine bios-install image.iso


cd $TOOLS_DIR