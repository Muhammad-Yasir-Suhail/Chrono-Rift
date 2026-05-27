# Chrono Rift

Chrono Rift is a multi-process, SFML-based game project built around core operating-systems concepts. The arbiter process launches the HIP and ASP processes, coordinates turn-taking through shared memory and semaphores, and drives the UI, combat flow, inventory management, deadlock handling, and end-game screens.

Docker Hub image: [yasir0566/chrono-rift](https://hub.docker.com/repository/docker/yasir0566/chrono-rift/general)

## Highlights

- Single-entry launcher: run `./arbiters` and it starts the arbiter, HIP, and ASP processes.
- Shared-memory architecture using POSIX `shm_open`, `mmap`, and `ftruncate`.
- Synchronization with POSIX semaphores and `pthread`-based worker threads.
- Signal-based control flow for pause/resume and graceful shutdown.
- Deadlock detection and recovery in the arbiter.
- Inventory system with multi-slot weapons, storage eviction, and pickup handling.
- SFML-powered UI with HUD updates, inventory rendering, and win/lose screens.
- Headless Docker support for reproducible builds and demos.

## Operating System Concepts Used

This project is designed to demonstrate how common OS concepts appear in a real program:

- **Processes**: the arbiter uses `fork()` and `execl()` to launch `hips` and `asps`.
- **Shared memory**: all game state lives in one shared segment so the three binaries can read and update the same data.
- **Semaphores**: used for startup handshakes, state protection, and turn coordination.
- **Threads**: the arbiter uses threads for rendering, signal handling, and deadlock monitoring.
- **Signals**: used to stop, resume, and manage special combat states.
- **Deadlock handling**: the arbiter detects lock cycles and releases locks to recover progress.

## Project Structure

```text
arbiter/    Arbiter process, shared-state logic, renderer, and coordination code
hip/        HIP process implementation
asp/        ASP process implementation
assets/     Fonts, sprites, backgrounds, UI assets, and artifacts
Dockerfile  Container build for headless execution
Makefile    Native build rules for all binaries
requirements.txt  System package list used by the Docker build
```

## Build Requirements

### Native build

- `g++`
- `make`
- SFML development libraries
- POSIX threading and realtime support

On Ubuntu/Debian, the project needs packages similar to the ones listed in `requirements.txt` and the Dockerfile.

### Docker build

- Docker Engine installed and running
- A logged-in Docker Hub account if you want to push the image

## Build

### Local build

```bash
make
```

This produces:

- `arbiters`
- `hips`
- `asps`

### Docker build

```bash
docker build -t yasir0566/chrono-rift:latest .
```

## Run

### Native run

```bash
./arbiters
```

### Docker run

```bash
docker run --rm -it yasir0566/chrono-rift:latest
```

The container uses `xvfb-run` so the SFML UI can run headlessly.

## Push to Docker Hub

If you want to publish the image to Docker Hub, make sure Docker Engine is installed and running, then use:

```bash
docker login
docker build -t yasir0566/chrono-rift:latest .
docker push yasir0566/chrono-rift:latest
```

## Notes

- The repository’s `.dockerignore` excludes local binaries, object files, logs, and editor metadata.
- If Docker reports `unix:///var/run/docker.sock`, the daemon is not installed or not running on your machine.
- The Docker image is intended for demo and testing environments where a graphical display is not available.

