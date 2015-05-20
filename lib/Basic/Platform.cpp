//===-- Platform.cpp - Implement platform-related helpers -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Platform.h"
#include "llvm/ADT/Triple.h"

using namespace swift;

bool swift::tripleIsiOSSimulator(const llvm::Triple &triple) {
  llvm::Triple::ArchType arch = triple.getArch();
  return (triple.isiOS() &&
          (arch == llvm::Triple::x86 || arch == llvm::Triple::x86_64));
}

#if defined(SWIFT_ENABLE_TARGET_TVOS)
bool swift::tripleIsAppleTVSimulator(const llvm::Triple &triple) {
  llvm::Triple::ArchType arch = triple.getArch();
  return (triple.isTvOS() &&
         (arch == llvm::Triple::x86 || arch == llvm::Triple::x86_64));
}
#endif // SWIFT_ENABLE_TARGET_TVOS

bool swift::tripleIsWatchSimulator(const llvm::Triple &triple) {
  llvm::Triple::ArchType arch = triple.getArch();
  return (triple.isWatchOS() &&
         (arch == llvm::Triple::x86 || arch == llvm::Triple::x86_64));
}

bool swift::tripleIsAnySimulator(const llvm::Triple &triple) {
#if defined(SWIFT_ENABLE_TARGET_TVOS)
  if (tripleIsAppleTVSimulator(triple))
    return true;
#endif // SWIFT_ENABLE_TARGET_TVOS

  return tripleIsiOSSimulator(triple) || tripleIsWatchSimulator(triple);
}

StringRef swift::getPlatformNameForTriple(const llvm::Triple &triple) {
  if (triple.isiOS()) {
#if defined(SWIFT_ENABLE_TARGET_TVOS)
    if (triple.isTvOS()) {
      if (tripleIsAppleTVSimulator(triple))
        return "appletvsimulator";
      return "appletvos";
    }
#endif // SWIFT_ENABLE_TARGET_TVOS

    if (tripleIsiOSSimulator(triple))
      return "iphonesimulator";
    return "iphoneos";
  }

  if (triple.isWatchOS()) {
    if (tripleIsWatchSimulator(triple))
        return "watchsimulator";
    return "watchos";
  }

  if (triple.isMacOSX())
    return "macosx";

  if (triple.isOSLinux())
    return "linux";

  return "";
}
