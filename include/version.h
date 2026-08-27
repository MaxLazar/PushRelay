// PushRelay — firmware version identity.
//
// The real version string is injected at build time by scripts/pio_version.py
// as -DFIRMWARE_VERSION="x.y.z" (x.y from version.txt, z = commits since
// version.txt last changed; CI overrides it via the PUSHRELAY_VERSION env var
// so it matches the published release tag exactly). The fallbacks below only
// apply to a build with no git history available.
#pragma once

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-dev"
#endif

#ifndef FIRMWARE_GIT_SHA
#define FIRMWARE_GIT_SHA "nogit"
#endif

#define FIRMWARE_BUILD_DATE __DATE__ " " __TIME__
