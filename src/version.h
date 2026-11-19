// The one place the version lives: the exe's version resource, the updater and the MCP server
// read it from here. tools/package_release.py refuses to package a different version.
#pragma once
#define CR_VER_MAJOR 1
#define CR_VER_MINOR 3
#define CR_VER_PATCH 5
#define CR_VERSION_STR "1.3.5"
#define CR_VERSION_NAME "1.3.5"  // shown to people (a suffix such as " Beta" may follow the numbers)
