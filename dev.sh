#!/bin/bash
open -a XQuartz
sleep 2
/opt/X11/bin/xhost +localhost
docker run -it --rm --init --name tdev \
  -e DISPLAY=host.docker.internal:0 \
  --cap-add=SYS_PTRACE --security-opt seccomp=unconfined \
  -v "$PWD":/work terminator-dev
