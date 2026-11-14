#!/bin/bash
# dsynco: cross-build for Linux/AMD64 | Windows/AMD64

OS=${OS:-linux}
CPU=${CPU:-amd64}
ARGS=${@@Q}

IMAGE_NAME=dsynco-debtx-builder
CONTAINER_NAME=dsynco_debtx_build
if [[ "$OS" == "windows" ]]; then
  IMAGE_NAME=dsynco-win64-builder
  CONTAINER_NAME=dsynco_win64_build
fi

set -xe

DS_DIR="$(dirname "$0")"

if ! podman container exists $CONTAINER_NAME ; then
  if ! podman image exists $IMAGE_NAME ; then
    # Create builder image
    podman build -t $IMAGE_NAME -f "$DS_DIR/builder/Dockerfile.$OS-$CPU" .
  fi

  # Create builder container
  podman create --attach --tty \
    -v "$(pwd)":/build:Z \
    -v "$DS_DIR/..":/src:Z \
    --workdir /build \
    --name $CONTAINER_NAME \
    $IMAGE_NAME \
    sleep 3600
fi

if ! podman container top $CONTAINER_NAME ; then
  # Start container in background
  podman start --attach $CONTAINER_NAME &
  # Wait until the container is ready
  while ! podman container top $CONTAINER_NAME ; do
    sleep .5
  done
fi

ODIR=_$OS-$CPU
ARGS_OS=
TESTS_RUN=
if [[ "$OS" == "windows" ]]; then
  ARGS_OS="OS=windows"
fi
JOBS=$(nproc)

# Prepare build script
cat >build.sh <<EOF
set -xe

export PATH=\$PATH:/usr/lib/llvm-19/bin
mkdir -p $ODIR
make -j$JOBS \
 -C $ODIR \
 -f /src/dsynco/Makefile \
 ROOT=/src \
 COMPILER=clang \
 $ARGS_OS \
 $ARGS

if test "$TESTS_RUN" != "" ; then
  cd $ODIR/dsynco-0
  bash /src/dsynco/test.sh $TESTS_RUN
fi
EOF

# Build inside the container
podman exec $CONTAINER_NAME \
  bash build.sh
