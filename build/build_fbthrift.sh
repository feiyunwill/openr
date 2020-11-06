#!/bin/bash

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

errorCheck() {
  return_code=$?
  if [ $return_code -ne 0 ]; then
    echo "[ERROR]: $1"
    exit $return_code
  fi
}

GETDEPS="$(dirname "$0")/fbcode_builder/getdeps.py"
INSTALL_PREFIX="/opt/facebook"
PYTHON3=$(command -v python3)

if [ "$PYTHON3" == "" ]; then
  echo "ERROR: No \`python3\` in PATH"
  exit 1
fi

# Ninja fails on 20.04
#python3 "$GETDEPS" --allow-system-packages install-system-deps --recursive fbthrift
#errorCheck "Failed to install-system-deps for fbthrift"

python3 "$GETDEPS" --allow-system-packages build --no-tests --install-prefix "$INSTALL_PREFIX" fbthrift
errorCheck "Failed to build fbthrift"

# TODO: Maybe fix src-dir to be absolute reference to dirname $0's parent
#python3 "$GETDEPS" fixup-dyn-deps --strip --src-dir=. openr _artifacts/linux  --project-install-prefix openr:"$INSTALL_PREFIX" --final-install-prefix "$INSTALL_PREFIX"
#errorCheck "Failed to fixup-dyn-deps for openr"

THRIFT_BIN_DIR="/opt/facebook/fbthrift/bin"
if [ ! -d "${THRIFT_BIN_DIR}" ]; then
  echo "No ${THRIFT_BIN_DIR} dir ... exiting ..."
  exit 2
fi
