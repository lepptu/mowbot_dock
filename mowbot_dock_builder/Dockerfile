# arm64 build environment for the mowbot dock software (ROS 2 Jazzy).
# Matches the dock Pi runtime: Ubuntu 24.04 arm64 + ros-jazzy + rmw-zenoh-cpp.
# Runs on an x86 host via qemu-user-static binfmt emulation.
FROM --platform=linux/arm64 ros:jazzy-ros-base

RUN apt-get update && apt-get install -y --no-install-recommends \
        ros-jazzy-rmw-zenoh-cpp \
        build-essential \
        cmake \
        git \
        python3-colcon-common-extensions \
        ccache \
    && rm -rf /var/lib/apt/lists/*

# The ubuntu user (UID/GID 1000) ships with the base image and matches the
# host user, so files written into the bind mount stay owned by the host user.
USER ubuntu
WORKDIR /home/ubuntu/mowbot_dock/mowbot_dock_ws
