# rudp-bench

Cross-library benchmark harness for reliable UDP / RUDP / QUIC implementations.
See `docs/superpowers/specs/2026-04-28-rudp-bench-design.md`.

## Build

cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
