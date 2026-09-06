# arm64 build environment for the mowbot dock software (ROS 2 Jazzy).
# Matches the dock Pi runtime: Ubuntu 24.04 arm64 + ros-jazzy + rmw-zenoh-cpp.
# nav2/ublox msgs + paho/yaml-cpp/nlohmann are build deps of the
# mowbot_mqtt_bridge submodule (second bridge instance for the dock).
# Runs on an x86 host via qemu-user-static binfmt emulation.
FROM --platform=linux/arm64 ros:jazzy-ros-base

RUN apt-get update && apt-get install -y --no-install-recommends \
        ros-jazzy-rmw-zenoh-cpp \
        ros-jazzy-nav2-msgs \
        ros-jazzy-ublox-ubx-msgs \
        libpaho-mqttpp-dev \
        libpaho-mqtt-dev \
        libyaml-cpp-dev \
        nlohmann-json3-dev \
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
