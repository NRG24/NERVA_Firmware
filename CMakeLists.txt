cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ring_ppg)

target_sources(app PRIVATE
	src/main.c
	src/maxm86161.c
	src/hr.c
	src/selftest.c
	src/ble.c
	src/imu.c
	src/wdt.c
	src/steps.c
	src/sleep.c
	src/calories.c
	src/profile.c
	src/hrv.c
	src/spo2.c
)
