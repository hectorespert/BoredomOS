# BoredomOS

[![BoredomOS CI](https://github.com/hectorespert/BoredomOS/actions/workflows/main.yml/badge.svg)](https://github.com/hectorespert/BoredomOS/actions/workflows/main.yml)

Software and documentation for a Cubesat based on [https://www.thingiverse.com/thing:4096437](https://www.thingiverse.com/thing:4096437)


Pending features and changes are tracked in [TODO.md](TODO.md).

## MAVProxy

```bash
mavproxy.py --master=/dev/ttyACM0,115200 --load-module system_time
```