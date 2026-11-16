// The one place the version lives: the exe's version resource, the updater and the MCP server
// read it from here. tools/package_release.py refuses to package a different version.
#pragma once
#define CR_VER_MAJOR 1
#define CR_VER_MINOR 3
#define CR_VER_PATCH 2
#define CR_VERSION_STR "1.3.2-beta"  // compareVersions reads 1.3.2 and stops at the suffix
#define CR_VERSION_NAME "1.3.2 Beta"  // shown to people
