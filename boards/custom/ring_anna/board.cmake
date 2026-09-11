# Raspberry Pi Debug Probe enumerates as CMSIS-DAP, so pyOCD is the default
# runner here. nRF52833 is a builtin pyOCD target -- no CMSIS pack needed.
board_runner_args(pyocd "--target=nrf52833")
board_runner_args(jlink "--device=nRF52833_xxAA" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
