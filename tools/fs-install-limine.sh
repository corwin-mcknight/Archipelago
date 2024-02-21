
# Store PWD
TOOLS_DIR=$(pwd)

mkdir -p ../build/tools
cd ../build/tools
if [ ! -d "limine" ]; then   
    git clone https://github.com/limine-bootloader/limine.git --branch=v4.x-branch-binary --depth=1
    make -C limine
fi

cd ..

mkdir -p sysroot
cd tools/limine
cp limine.sys limine-cd.bin limine-cd-efi.bin ../../sysroot/
cd ../..

cp -r ../media/* ./sysroot/

xorriso -as mkisofs -b limine-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot limine-cd-efi.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        sysroot -o image.iso

./tools/limine/limine-deploy image.iso


cd $TOOLS_DIR