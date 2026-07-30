# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/santosh/esp/esp-idf/components/bootloader/subproject"
  "/home/santosh/Videos/archive/esp/forest_acoustic_classifier/build/bootloader"
  "/home/santosh/Videos/archive/esp/forest_acoustic_classifier/build/bootloader-prefix"
  "/home/santosh/Videos/archive/esp/forest_acoustic_classifier/build/bootloader-prefix/tmp"
  "/home/santosh/Videos/archive/esp/forest_acoustic_classifier/build/bootloader-prefix/src/bootloader-stamp"
  "/home/santosh/Videos/archive/esp/forest_acoustic_classifier/build/bootloader-prefix/src"
  "/home/santosh/Videos/archive/esp/forest_acoustic_classifier/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/santosh/Videos/archive/esp/forest_acoustic_classifier/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/santosh/Videos/archive/esp/forest_acoustic_classifier/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
