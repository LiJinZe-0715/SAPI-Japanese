// The product version, in the one place it is written.
//
// Included by version_info.inc (the VERSIONINFO block of every binary) and by tools/install.h
// (DisplayVersion in "Apps & features"). It used to be typed into both by hand, and a version
// bump that missed one of them compiled and installed without a word. Plain #defines only:
// rc.exe preprocesses this file too.

#pragma once

#define VV_VERSION_COMMA   1,0,0,0
#define VV_VERSION_STRING  "1.0"
#define VV_VERSION_DISPLAY "1.0"
