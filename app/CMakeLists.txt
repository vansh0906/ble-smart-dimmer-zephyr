cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ble_smart_dimmer)

target_sources(app PRIVATE
  src/main.c
  src/ble_svc.c
  src/power_stage_sim.c
  ../lib/dimmer.c
  ../lib/meter.c)
target_include_directories(app PRIVATE ../lib src)
