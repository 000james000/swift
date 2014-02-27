//===--- FastEntryPoints.h - Swift Language Assembly Entry Points ABI -----===//
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
//
// Swift Language Assembly Entry Points ABI -- offsets of interest
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_FASTENTRYPOINTS_H
#define SWIFT_RUNTIME_FASTENTRYPOINTS_H

#include <TargetConditionals.h>

// Note: This file is #included in assembly files.

// XXX FIXME -- we need to clean this up when the project isn't a secret.
// Allocation cache layout.
// This uses slots in pthread direct tsd. There are 256 slots. 
// Most of the first 128 are reserved for OS use. 
// The last 128 are unused except on iOS Simulator.
// We store two caches in these unused slots.
#define ALLOC_CACHE_COUNT 64
#define ALLOC_CACHE_START 128
#define ALLOC_RAW_CACHE_START (ALLOC_CACHE_START + ALLOC_CACHE_COUNT)

#ifdef __LP64__
# define SWIFT_TSD_RAW_ALLOC_BASE (ALLOC_RAW_CACHE_START*8)
#else
# define SWIFT_TSD_RAW_ALLOC_BASE (ALLOC_RAW_CACHE_START*4)
#endif

#define RC_OFFSET 0x8
#define RC_INTERVAL 4
#define RC_MASK 0xfffffffc
#define RC_ATOMIC_BIT 0x1
#define RC_DEALLOCATING_BIT 0x2
#define WRC_OFFSET 0xc
#define WRC_INTERVAL 1
#define WRC_MASK 0xffffffff
#define SWIFT_TRYALLOC 0x0001
#define SWIFT_RAWALLOC 0x0002
#define SWIFT_TRYRAWALLOC 0x0003

#ifdef SWIFT_HAVE_FAST_ENTRY_POINTS
#error "Do not try to override SWIFT_HAVE_FAST_ENTRY_POINTS"
#endif

#if __x86_64__ && !TARGET_IPHONE_SIMULATOR
# define SWIFT_HAVE_FAST_ENTRY_POINTS 1
#endif

#endif // SWIFT_RUNTIME_FASTENTRYPOINTS_H

