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

THRIFT_BIN_DIR="/opt/facebook/fbthrift/bin"
if [ ! -d "${THRIFT_BIN_DIR}" ]; then
  echo "No ${THRIFT_BIN_DIR} dir ... exiting ..."
  exit 1
fi

export PATH="${PATH}:${THRIFT_BIN_DIR}"
pip3 --no-cache-dir install --upgrade pip setuptools wheel
python3 setup.py install
