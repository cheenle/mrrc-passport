cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# M1a hardware probe: ILI9341 panel + FT6336G touch on the ES3C28P board.
# Throw-away project — nothing here ships.
project(mrrc_probe)
