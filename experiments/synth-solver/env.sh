# Source to use the local CIRCT build (tools, Python bindings).
CIRCT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export LD_LIBRARY_PATH=/nix/store/41ym1jm1b7j3rhglk82gwg9jml26z1km-gcc-14.3.0-lib/lib:/nix/store/4kj51k0kzrbwx5m4h3fpjkkl5wc2gdbd-alsa-lib-1.2.16.1/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export PATH=$CIRCT_ROOT/build/bin:$PATH
export PYTHONPATH=$CIRCT_ROOT/build/tools/circt/python_packages/circt_core${PYTHONPATH:+:$PYTHONPATH}
export CIRCT_PY=$CIRCT_ROOT/.venv/bin/python
