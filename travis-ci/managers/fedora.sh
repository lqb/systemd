#!/bin/bash

# Run this script from the root of the systemd's git repository
# or set REPO_ROOT to a correct path.
#
# Example execution on Fedora:
# dnf config-manager --add-repo https://download.docker.com/linux/fedora/docker-ce.repo
# dnf install -y docker-ce
# systemctl start docker
# export CONT_NAME="my-fancy-container"
# travis-ci/managers/fedora.sh SETUP RUN CLEANUP

PHASES=(${@:-SETUP RUN CLEANUP})
FEDORA_RELEASE="${FEDORA_RELEASE:-rawhide}"
CONT_NAME="${CONT_NAME:-fedora-$FEDORA_RELEASE-$RANDOM}"
DOCKER_EXEC="${DOCKER_EXEC:-docker exec -it $CONT_NAME}"
DOCKER_RUN="${DOCKER_RUN:-docker run}"
REPO_ROOT="${REPO_ROOT:-$PWD}"
ADDITIONAL_DEPS=(dnf-plugins-core python2 iputils hostname libasan)

function info() {
    echo -e "\033[33;1m$1\033[0m"
}

set -e

for phase in "${PHASES[@]}"; do
    case $phase in
        SETUP)
            info "Setup phase"
            info "Using Fedora $FEDORA_RELEASE"
            MACHINE_ID="/etc/machine-id"
            if [ ! -f $MACHINE_ID ]; then
                MACHINE_ID="/var/lib/dbus/machine-id"
            fi
            # Pull a Docker image and start a new container
            docker pull fedora:$FEDORA_RELEASE
            info "Starting container $CONT_NAME"
            $DOCKER_RUN -v $REPO_ROOT:/build:rw \
                        -v $MACHINE_ID:/etc/machine-id:ro \
                        -w /build --privileged=true --name $CONT_NAME \
                        -dit --net=host fedora:$FEDORA_RELEASE /sbin/init
            $DOCKER_EXEC dnf makecache
            # Install necessary build/test requirements
            $DOCKER_EXEC dnf -y install "${ADDITIONAL_DEPS[@]}"
            $DOCKER_EXEC dnf -y builddep systemd
            ;;
        RUN)
            info "Run phase"
            # Build systemd
            $DOCKER_EXEC meson build
            $DOCKER_EXEC ninja -C build
            # Run 'make check'
            $DOCKER_EXEC ninja -C build test
            ;;
        CLEANUP)
            info "Cleanup phase"
            docker stop $CONT_NAME
            docker rm -f $CONT_NAME
            ;;
        *)
            echo >&2 "Unknown phase '$phase'"
            exit 1
    esac
done
