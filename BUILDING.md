# Building Archipelago


Archipelago uses a simple Makefile based build system. However, to better manage dependencies and ensure a consistent build environment, we recommend using the provided development container.

The development container is defined in the `.devcontainer` directory and can be used with Visual Studio Code or Docker directly.

## Using the Development Container
To build and run Archipelago using the development container, follow these steps:

1. **Open in VS Code**: If you're using Visual Studio Code, simply open the project folder and click on the prompt to reopen in the container. This will set up the environment with all necessary dependencies.

2. **Using Docker**: If you prefer using Docker directly, you can build and run the container with the following commands:

   ```bash
   docker build -t archipelago-dev -f .devcontainer/Dockerfile .
   docker run -it --rm -v $(pwd):/workspace archipelago-dev
   ```

   This mounts the current directory into the container at `/workspace`.

## Build Commands
Once inside the development container, you can use the following commands to build and run Archipelago:

- **Clean the build**: This removes all previously compiled files.
  ```bash
  make clean
  ```

- **Build the project**: This compiles the kernel and other necessary components.
  ```bash
  make build
  ```

- **Install the build**: This prepares the ISO image for running.
  ```bash
  make install
  ```
- **Run the OS**: This launches the built ISO in QEMU for testing.
  ```bash
  make run
  ```
- **Generate clangd**: This generates clangd configuration for IDE support.
  ```bash
  make clangd
  ```
- **Full clean**: This removes all build artifacts and resets the build environment.
  ```bash
  make full-clean
  ```
