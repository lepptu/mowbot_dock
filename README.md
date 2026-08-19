# mowbot_dock

Software for the mowbot docking station: a Raspberry Pi 3 Model B running
ROS 2 Jazzy (rmw_zenoh) that supervises the charging dock via an Arduino Nano
over USB serial.

Plans and hardware documentation: [mowbot_plans / Docking plan](https://github.com/lepptu/mowbot_plans/tree/main/Docking%20plan)

## Layout

```
mowbot_dock_builder/   arm64 cross-build environment (Docker + qemu, dev machine only)
mowbot_dock_ws/        colcon workspace
  src/mowbot_dock/     the ROS 2 package (dock_agent_node)
TODO.md                implementation plan
```

The workspace path `~/mowbot_dock/mowbot_dock_ws` must be identical on the
dev machine, inside the build container, and on the dock Pi, because the
colcon `install/` tree bakes in absolute paths.

## Building (dev machine, x86)

One-time setup: install `docker.io qemu-user-static binfmt-support`, then

```bash
docker build -t mowbot-dock-builder mowbot_dock_builder/
```

Build the workspace (cross-compiles to arm64 inside the container):

```bash
./mowbot_dock_builder/dock-build.sh                                # everything
./mowbot_dock_builder/dock-build.sh --packages-select mowbot_dock  # one package
```

## Deploying (dock Pi)

Only `mowbot_dock_ws/install/` is shipped to the Pi (rsync), to the same
path: `~/mowbot_dock/mowbot_dock_ws/install/`. Source, build artifacts and
the builder never leave the dev machine.

The dock Pi runs its own zenoh router that connects out to the robot's
router at `tcp/192.168.1.90:7447`. All ROS 2 processes on the Pi need
`RMW_IMPLEMENTATION=rmw_zenoh_cpp` (in `.bashrc` and in every systemd unit).
