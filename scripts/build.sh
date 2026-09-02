#!/usr/bin/env bash
# memflux : build + install
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

if [[ "${1:-}" == "--install" ]]; then
  sudo install -Dm755 build/memfluxd  /usr/local/bin/memfluxd
  sudo install -Dm755 build/memfluxctl /usr/local/bin/memfluxctl
  sudo install -Dm755 build/libmemflux-preload.so /usr/local/lib/libmemflux-preload.so
  sudo ldconfig
  sudo install -Dm644 config/memflux.conf /etc/memflux.conf
  sudo install -Dm644 config/memfluxd.service /etc/systemd/system/memfluxd.service
  sudo systemctl daemon-reload
  echo "=== installé. Démarrage : sudo systemctl start memfluxd"
  echo "=== ou manuel :      sudo memfluxd -c /etc/memflux.conf"
fi
echo "=== build OK"