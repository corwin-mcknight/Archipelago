# Archipelago
Archipelago is an operating system that is designed to be minimal and security focused, and use modern concepts.

## Goal
The eventual goal is to have an entire source tree with BSD Ports-like package management, and a minimal kernel that is designed to be secure and minimal.

## State
Currently, the kernel is under development. There is no package management, and the project is in a very early state.

Archipelago currently only supports x86_64, live cd, and Limine as a bootloader.

## Building
The easiest way to build the operating system currently is to use the provided Dockerfile, or devcontainer.

Instructions for setting up a proper build environemnt are a TODO item.

### Build Instructions
    
```bash
make clean   # Start with a fresh environment.
make build   # Build the operating system.
make install # Create's the system root and packages it into an iso
make run     # Runs the operating system in QEMU
```
