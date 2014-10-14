#!/bin/bash
set -o pipefail
set -e
#
# Note: Jenkins is set up to restore the repositories to pristine
# state before building, so we rebuild from scratch every time.  If
# you use this script yourself, it will NOT auto-clean before building

# Set up the default path to cmake
CMAKE_DEFAULT="$(which cmake || echo /usr/local/bin/cmake)"

# Declare the set of known settings along with each one's description
#
# If you add a user-settable variable, add it to this list.
#
# a default value of "" indicates that the corresponding variable
# will remain unset unless set explicitly
KNOWN_SETTINGS=(
    # name                      default          description
    build-args                  ""               "arguments to the build tool; defaults to -j8 when CMake generator is \"Unix Makefiles\""
    build-dir                   ""               "out-of-tree build directory; default is in-tree"
    build-type                  Debug            "the CMake build variant: Debug, RelWithDebInfo, Release, etc."
    cmake                       "$CMAKE_DEFAULT" "path to the cmake binary"
    distcc                      ""               "use distcc in pump mode"
    config-args                 ""               "User-supplied arguments to cmake when used to do configuration"
    cmake-generator             "Unix Makefiles" "kind of build system to generate; see output of cmake --help for choices"
    prefix                      "/usr"           "installation prefix"
    show-sdks                   ""               "print installed Xcode and SDK versions"
    skip-ios                    ""               "set to skip everything iOS-related"
    skip-build-llvm             ""               "set to skip building LLVM/Clang"
    skip-build-swift            ""               "set to skip building Swift"
    skip-build-ios              ""               "set to skip building Swift stdlibs for iOS"
    skip-build-ios-device       ""               "set to skip building Swift stdlibs for iOS devices (i.e. build simulators only)"
    skip-build-ios-simulator    ""               "set to skip building Swift stdlibs for iOS simulators (i.e. build devices only)"
    skip-build-sourcekit        ""               "set to skip building SourceKit"
    skip-test-swift             ""               "set to skip testing Swift"
    skip-test-ios               ""               "set to skip testing Swift stdlibs for iOS"
    skip-test-ios-device        ""               "set to skip testing Swift stdlibs for iOS devices (i.e. test simulators only)"
    skip-test-ios-simulator     ""               "set to skip testing Swift stdlibs for iOS simulators (i.e. test devices only)"
    skip-test-sourcekit         ""               "set to skip testing SourceKit"
    skip-test-swift-performance ""               "set to skip testing Swift performance"
    skip-test-validation        ""               "set to skip validation test suite"
    stress-test-sourcekit       ""               "set to run the stress-SourceKit target"
    workspace                   "${HOME}/src"    "source directory containing llvm, clang, swift, and SourceKit"
    run-with-asan-compiler      ""               "the AddressSanitizer compiler to use (non-asan build if empty string is passed)"
    disable-assertions          ""               "set to disable assertions"
)

function toupper() {
    echo "$@" | tr '[:lower:]' '[:upper:]'    
}

function to_varname() {
    toupper "${1//-/_}"
}

# Set up an "associative array" of settings for error checking, and set
# (or unset) each corresponding variable to its default value
# If the Mac's bash were not stuck in the past, we could "declare -A" an
# associative array, but instead we have to hack it by defining variables
# declare -A IS_KNOWN_SETTING
for ((i = 0; i < ${#KNOWN_SETTINGS[@]}; i += 3)); do
    setting="${KNOWN_SETTINGS[i]}"
    
    default_value="${KNOWN_SETTINGS[$((i+1))]}"
    
    varname="$(to_varname "${setting}")"    # upcase the setting name to get the variable
    eval "${varname}_IS_KNOWN_SETTING=1"

    if [[ "${default_value}" ]] ; then
        # For an explanation of the backslash see http://stackoverflow.com/a/9715377
        eval ${varname}=$\default_value
    else
        unset ${varname}
    fi
done

COMMAND_NAME="$(basename "$0")"

# Print instructions for using this script to stdout
usage() {
    echo "Usage: ${COMMAND_NAME} [--help|-h] [ --SETTING=VALUE | --SETTING VALUE | --SETTING | --release ] *"
    echo
    echo "  Available settings. Each setting corresponds to a variable,"
    echo "  obtained by upcasing its name, in this script.  A variable"
    echo "  with no default listed here will be unset in the script if"
    echo "  not explicitly specified.  A setting passed in the 3rd form"
    echo "  will set its corresponding variable to \"1\"."
    echo

    setting_list="
 | |Setting| Default|Description
 | |-------| -------|-----------
"

    for ((i = 0; i < ${#KNOWN_SETTINGS[@]}; i += 3)); do
        setting_list+="\
 | |--${KNOWN_SETTINGS[i]}| ${KNOWN_SETTINGS[$((i+1))]}|${KNOWN_SETTINGS[$((i+2))]}
"
    done
    echo "${setting_list}" | column -x -s'|' -t
    echo
    echo "Note: when using the form --SETTING VALUE, VALUE must not begin "
    echo "      with a hyphen."
    echo "Note: the \"--release\" option creates a pre-packaged combination"
    echo "      of settings used by the buildbot."
}

# Scan all command-line arguments
while [[ "$1" ]] ; do
    case "$1" in
        -h | --help )
            usage
            exit
            ;;

        --release)
            BUILD_TYPE=RelWithDebInfo
            # Include a custom name to avoid picking up stale module files.
            CUSTOM_VERSION_NAME="release $(date -j '+%Y-%m-%d %H-%M-%S')"
            ;;
        
        --* )
            dashless="${1:2}"
            
            # drop suffix beginning with the first "="
            setting="${dashless%%=*}" 

            # compute the variable to set
            varname="$(to_varname "${setting}")"

            # check to see if this is a known option
            known_var_name="${varname}_IS_KNOWN_SETTING"
            if [[ ! "${!known_var_name}" ]] ; then
                echo "Error: Unknown setting: ${setting}" 1>&2
                usage 1>&2
                exit 1
            fi

            # find the intended value
            if [[ "${dashless}" == *=* ]] ; then              # if there's an '=', the value
                value="${dashless#*=}"                        #   is everything after the first '='
            elif [[ "$2" ]] && [[ "${2:0:1}" != "-" ]] ; then # else if the next parameter exists
                value="$2"                                    #    but isn't  an option, use that
                shift
            else                                             # otherwise, the value is 1
                value=1                                  
            fi
            
            # For explanation of backslash see http://stackoverflow.com/a/9715377
            eval ${varname}=$\value
            ;;

        *)
            usage
            exit 1
    esac
    shift
done

# Set this to the install prefix for release builds.
INSTALL_PREFIX="${PREFIX}"

# Set these to the paths of the OS X SDK and toolchain.
if [[ "$(uname -s)" == "Darwin" ]] ; then
  SYSROOT="$(xcrun --show-sdk-path --sdk macosx10.10internal)"
  TOOLCHAIN="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain"
else
  SYSROOT="/"
  TOOLCHAIN="/"
fi

# WORKSPACE and BUILD_DIR must be absolute paths
case "${WORKSPACE}" in
  /*) ;;
   *) echo "workspace must be an absolute path (was '${WORKSPACE}')" ; exit 1 ;;
esac
case "${BUILD_DIR}" in
  /*) ;;
  "") ;;
   *) echo "build-dir must be an absolute path (was '${BUILD_DIR}')" ; exit 1 ;;
esac

# WORKSPACE must exist
if [ ! -e "$WORKSPACE" ]; then
    echo "Workspace does not exist (tried $WORKSPACE)"
    exit 1
fi

# Swift-project products, in the order they must be built
SWIFT_BUILD_PRODUCTS=(swift)
if [[ ! "$SKIP_BUILD_SOURCEKIT" ]]; then
    SWIFT_BUILD_PRODUCTS=("${SWIFT_BUILD_PRODUCTS[@]}" SourceKit)
fi

SWIFT_TEST_PRODUCTS=("${SWIFT_BUILD_PRODUCTS[@]}")

LLVM_TARGETS_TO_BUILD="X86;ARM;AArch64"

# Swift stdlib build products
# macosx-x86_64 stdlib is part of the swift product itself
if [[ ! "$SKIP_IOS" ]]; then
    IOS_SIMULATOR_PRODUCTS=(swift_stdlib_ios_simulator_x86_64 swift_stdlib_ios_simulator_i386)
    IOS_DEVICE_PRODUCTS=(swift_stdlib_ios_arm64 swift_stdlib_ios_armv7)
    if [[ ! "$SKIP_BUILD_IOS" ]]; then
        if [[ ! "$SKIP_BUILD_IOS_SIMULATOR" ]]; then
            IOS_BUILD_PRODUCTS=("${IOS_BUILD_PRODUCTS[@]}" "${IOS_SIMULATOR_PRODUCTS[@]}")
        fi
        if [[ ! "$SKIP_BUILD_IOS_DEVICE" ]]; then
            IOS_BUILD_PRODUCTS=("${IOS_BUILD_PRODUCTS[@]}" "${IOS_DEVICE_PRODUCTS[@]}")
        fi
        SWIFT_BUILD_PRODUCTS=("${SWIFT_BUILD_PRODUCTS[@]}" "${IOS_BUILD_PRODUCTS[@]}")
    fi
    if [[ ! "$SKIP_TEST_IOS" ]]; then
        if [[ ! "$SKIP_TEST_IOS_SIMULATOR" ]]; then
            IOS_TEST_PRODUCTS=("${IOS_TEST_PRODUCTS[@]}" "${IOS_SIMULATOR_PRODUCTS[@]}")
        fi
        if [[ ! "$SKIP_TEST_IOS_DEVICE" ]]; then
            IOS_TEST_PRODUCTS=("${IOS_TEST_PRODUCTS[@]}" "${IOS_DEVICE_PRODUCTS[@]}")
        fi
        SWIFT_TEST_PRODUCTS=("${SWIFT_TEST_PRODUCTS[@]}" "${IOS_TEST_PRODUCTS[@]}")
    fi
fi

# All build products, in the order they must be built
ALL_BUILD_PRODUCTS=(llvm "${SWIFT_BUILD_PRODUCTS[@]}")

#
# Calculate source directories for each product.
#

# iOS build products use the same source directory as swift itself.
# Default to $WORKSPACE/swift if SWIFT_SOURCE_DIR if not set above.
for product in "${IOS_BUILD_PRODUCTS[@]}" "${IOS_TEST_PRODUCTS[@]}" ; do
    varname="$(toupper "${product}")"_SOURCE_DIR
    eval $varname=${SWIFT_SOURCE_DIR:-$WORKSPACE/swift}
done

# Default source directory is $WORKSPACE/$product if not set above.
for product in "${ALL_BUILD_PRODUCTS[@]}" ; do
    _PRODUCT_SOURCE_DIR="$(toupper "${product}")"_SOURCE_DIR
    eval dir=\${$_PRODUCT_SOURCE_DIR:=$WORKSPACE/$product}
    if [ ! -e "${dir}" ] ; then
        echo "Can't find source directory for $product (tried $dir)"
        exit 1
    fi
done

#
# Calculate build directories for each product
#

# Build directories are $BUILD_DIR/$product or $PRODUCT_SOURCE_DIR/build
for product in "${ALL_BUILD_PRODUCTS[@]}" ; do
    PRODUCT=$(toupper "${product}")
    eval source_dir=\${${PRODUCT}_SOURCE_DIR}
    if [[ "${BUILD_DIR}" ]] ; then
        product_build_dir="${BUILD_DIR}/${product}"
    else
        product_build_dir="${source_dir}/build"
    fi
    eval "${PRODUCT}_BUILD_DIR=\$product_build_dir"
done

# iOS build products use their own build directories, which are
# subdirectories of swift's build directory if BUILD_DIR is not set.
for product in "${IOS_BUILD_PRODUCTS[@]}" "${IOS_TEST_PRODUCTS[@]}" ; do
    _PRODUCT_BUILD_DIR="$(toupper "${product}")"_BUILD_DIR
    if [[ "${BUILD_DIR}" ]] ; then
        eval ${_PRODUCT_BUILD_DIR}="${BUILD_DIR}/${product}"
    else
        eval ${_PRODUCT_BUILD_DIR}="${SWIFT_BUILD_DIR}/${product}"
    fi
done


# Symlink clang into the llvm tree.
CLANG_SOURCE_DIR="${LLVM_SOURCE_DIR}/tools/clang"
CLANG_BUILD_DIR="${LLVM_BUILD_DIR}"
if [ ! -e "${WORKSPACE}/clang" ]; then
    # If llvm/tools/clang is already a directory, use that and skip the symlink.
    if [ ! -d "${CLANG_SOURCE_DIR}" ]; then
        echo "Can't find source directory for clang (tried ${WORKSPACE}/clang and ${CLANG_SOURCE_DIR})"
        exit 1
    fi
fi
if [ ! -d "${CLANG_SOURCE_DIR}" ]; then
    ln -sf  "${WORKSPACE}/clang" "${CLANG_SOURCE_DIR}"
fi

if [[ "$(uname -s)" == "Darwin" ]] ; then
  HOST_CC="$TOOLCHAIN/usr/bin/clang"
  HOST_CXX="$TOOLCHAIN/usr/bin/clang++"
else
  HOST_CC="clang"
  HOST_CXX="clang++"
fi

if [[ "$DISTCC" ]] ; then
    DISTCC_PUMP="$(which pump)"
    CMAKE_COMPILER_OPTIONS=(
        -DSWIFT_DISTCC="$(which distcc)"
        -DCMAKE_C_COMPILER="$(which distcc)"
        -DCMAKE_C_COMPILER_ARG1="${HOST_CC}"
        -DCMAKE_CXX_COMPILER="$(which distcc)"
        -DCMAKE_CXX_COMPILER_ARG1="${HOST_CXX}"
    )
else
    CMAKE_COMPILER_OPTIONS=(
        -DCMAKE_C_COMPILER="${HOST_CC}"
        -DCMAKE_CXX_COMPILER="${HOST_CXX}"
    )
fi

CMAKE_C_FLAGS=""
CMAKE_CXX_FLAGS=""
CMAKE_EXE_LINKER_FLAGS=""
CMAKE_SHARED_LINKER_FLAGS=""

if [[ "${RUN_WITH_ASAN_COMPILER}" ]]; then
    CMAKE_COMPILER_OPTIONS=(
        -DCMAKE_C_COMPILER="${RUN_WITH_ASAN_COMPILER}"
        -DCMAKE_CXX_COMPILER="${RUN_WITH_ASAN_COMPILER}++"
    )
    CMAKE_C_FLAGS="-fsanitize=address -O1 -g -fno-omit-frame-pointer -mllvm -asan-globals=0"
    CMAKE_CXX_FLAGS="-fsanitize=address -O1 -g -fno-omit-frame-pointer -mllvm -asan-globals=0"
    CMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
    CMAKE_SHARED_LINKER_FLAGS="-fsanitize=address"
fi

ENABLE_ASSERTIONS="ON"
if [[ "$DISABLE_ASSERTIONS" ]]; then
    ENABLE_ASSERTIONS="OFF"
fi

# CMake options used for all targets, including LLVM/Clang
COMMON_CMAKE_OPTIONS=(
    "${CMAKE_COMPILER_OPTIONS[@]}"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DLLVM_ENABLE_ASSERTIONS="${ENABLE_ASSERTIONS}"
)

case "${CMAKE_GENERATOR}" in
    'Unix Makefiles')
        BUILD_ARGS="${BUILD_ARGS:--j8}"
        ;;
    Xcode)
        BUILD_TARGET_FLAG=-target
        COMMON_CMAKE_OPTIONS=(
            "${COMMON_CMAKE_OPTIONS[@]}"
            -DCMAKE_XCODE_ATTRIBUTE_GCC_VERSION:STRING=com.apple.compilers.llvm.clang.1_0
            -DCMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY:STRING=libc++
            -DCMAKE_OSX_DEPLOYMENT_TARGET=10.8
            -DCMAKE_CONFIGURATION_TYPES="Debug;Release"
        )
        ;;
esac

# Record SDK and tools versions for posterity
if [[ "$SHOW_SDKS" ]]; then
    echo "--- SDK versions ---"
    xcodebuild -version || :
    echo
    xcodebuild -version -sdk macosx.internal || :
    if [[ ! "$SKIP_IOS" ]]; then
        xcodebuild -version -sdk iphoneos.internal || :
        xcodebuild -version -sdk iphonesimulator || :
    fi
fi

# Build LLVM and Clang (x86 target only).
if [ \! "$SKIP_BUILD_LLVM" ]; then
  echo "--- Building LLVM and Clang ---"
  if [[ ! -f  "${LLVM_BUILD_DIR}/CMakeCache.txt" ]] ; then
      mkdir -p "${LLVM_BUILD_DIR}"
      # Note: we set LLVM_IMPLICIT_PROJECT_IGNORE below because this
      # script builds swift separately, and people often have reasons
      # to symlink the swift directory into llvm/tools, e.g. to build
      # LLDB
      (cd "${LLVM_BUILD_DIR}" &&
          "$CMAKE" -G "${CMAKE_GENERATOR}" "${COMMON_CMAKE_OPTIONS[@]}" \
              -DCMAKE_C_FLAGS="${CMAKE_C_FLAGS}" \
              -DLLVM_IMPLICIT_PROJECT_IGNORE="${LLVM_SOURCE_DIR}/tools/swift" \
              -DLLVM_TARGETS_TO_BUILD="${LLVM_TARGETS_TO_BUILD}" \
              -DCLANG_REPOSITORY_STRING="$CUSTOM_VERSION_NAME" \
              ${CONFIG_ARGS} \
              "${LLVM_SOURCE_DIR}" || exit 1)
  fi
  $DISTCC_PUMP "$CMAKE" --build "${LLVM_BUILD_DIR}" -- ${BUILD_ARGS}
fi

#
# Now build all the Swift products
#

if [[ "$(uname -s)" == "Darwin" ]] ; then
  SWIFT_CMAKE_OPTIONS=(
      -DCMAKE_OSX_SYSROOT="${SYSROOT}"
      -DMODULES_SDK="${SYSROOT}"
      -DCMAKE_C_FLAGS="-isysroot${SYSROOT} ${CMAKE_C_FLAGS}"
      -DCMAKE_CXX_FLAGS="-isysroot${SYSROOT} ${CMAKE_CXX_FLAGS}"
      -DSWIFT_RUN_LONG_TESTS="ON"
      -DLLVM_CONFIG="${LLVM_BUILD_DIR}/bin/llvm-config"
  )
else
  SWIFT_CMAKE_OPTIONS=(
      -DCMAKE_C_FLAGS="${CMAKE_C_FLAGS}"
      -DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS}"
      -DSWIFT_RUN_LONG_TESTS="ON"
      -DLLVM_CONFIG="${LLVM_BUILD_DIR}/bin/llvm-config"
  )
fi

# set_ios_options options_var platform deployment_target internal_suffix arch
function set_ios_options {
    local platform=$2
    local deployment_target=$3
    local internal_suffix=$4
    local arch=$5

    local sdkroot=$(xcrun -sdk ${platform}${internal_suffix} -show-sdk-path)

    local opts=(
        -DCMAKE_TOOLCHAIN_FILE="${SWIFT_SOURCE_DIR}/cmake/${platform}.cmake"
        -DCMAKE_SYSTEM_PROCESSOR=${arch}
        -DCMAKE_OSX_ARCHITECTURES=${arch}
        -DCMAKE_OSX_SYSROOT="${sdkroot}"
        -DMODULES_SDK="${sdkroot}"
        -DCMAKE_C_FLAGS="-isysroot${sdkroot}"
        -DCMAKE_CXX_FLAGS="-isysroot${sdkroot}"
        -DSWIFT_DEPLOYMENT_OS=${platform}
        -DSWIFT_DEPLOYMENT_TARGET=${deployment_target}
        -DSWIFT_BUILD_TOOLS=OFF
        -DSWIFT_RUN_LONG_TESTS="ON"
        -DPATH_TO_SWIFT_BUILD="${SWIFT_BUILD_DIR}"
        -DSWIFT_INCLUDE_DOCS=OFF
        -DLLVM_CONFIG="${LLVM_BUILD_DIR}/bin/llvm-config"
    )

    eval $1=\(\${opts[@]}\)
}


if [[ ! "$SKIP_BUILD_IOS" ]]; then
    set_ios_options SWIFT_STDLIB_IOS_ARM64_CMAKE_OPTIONS iphoneos 7.0 .internal arm64
    set_ios_options SWIFT_STDLIB_IOS_SIMULATOR_X86_64_CMAKE_OPTIONS iphonesimulator 7.0 "" x86_64
    set_ios_options SWIFT_STDLIB_IOS_ARMV7_CMAKE_OPTIONS iphoneos 7.0 .internal armv7
    set_ios_options SWIFT_STDLIB_IOS_SIMULATOR_I386_CMAKE_OPTIONS iphonesimulator 7.0 "" i386
fi

SOURCEKIT_CMAKE_OPTIONS=(
    -DSOURCEKIT_PATH_TO_SWIFT_SOURCE="${SWIFT_SOURCE_DIR}"
    -DSOURCEKIT_PATH_TO_SWIFT_BUILD="${SWIFT_BUILD_DIR}"
    -DLLVM_CONFIG="${LLVM_BUILD_DIR}/bin/llvm-config"
    -DCMAKE_C_FLAGS="-isysroot${SYSROOT} ${CMAKE_C_FLAGS}"
    -DCMAKE_CXX_FLAGS="-isysroot${SYSROOT} ${CMAKE_CXX_FLAGS}"
)

ASAN_CMAKE_OPTIONS=(
    -DSOURCEKIT_USE_INPROC_LIBRARY="ON"
    -DSWIFT_ASAN_BUILD="ON"
)

# Build each one. Note: in this block, names beginning with underscore
# are used indirectly to access product-specific variables.
for product in "${SWIFT_BUILD_PRODUCTS[@]}" ; do
    PRODUCT=$(toupper "${product}")
    
    _SKIP_BUILD_PRODUCT=SKIP_BUILD_${PRODUCT}
    if [[ ! "${!_SKIP_BUILD_PRODUCT}" ]]; then
        
        echo "--- Building ${product} ---"
        _PRODUCT_SOURCE_DIR=${PRODUCT}_SOURCE_DIR
        _PRODUCT_BUILD_DIR=${PRODUCT}_BUILD_DIR

        # Clean product-local module cache.
        swift_module_cache="${!_PRODUCT_BUILD_DIR}/swift-module-cache"
        rm -rf "${swift_module_cache}"
        mkdir -p "${swift_module_cache}"

        # Configure if necessary.
        if [[ ! -f  "${!_PRODUCT_BUILD_DIR}/CMakeCache.txt" ]] ; then
            mkdir -p "${!_PRODUCT_BUILD_DIR}"

            _PRODUCT_CMAKE_OPTIONS=${PRODUCT}_CMAKE_OPTIONS[@]
            if [[ "${RUN_WITH_ASAN_COMPILER}" ]]; then
              _ASAN_OPTIONS=ASAN_CMAKE_OPTIONS[@]
            fi

            case "${PRODUCT}" in
                SWIFT_STDLIB*) var_prefix="SWIFT" ;;
                *) var_prefix=${PRODUCT} ;;
            esac

            (cd "${!_PRODUCT_BUILD_DIR}" &&
                "$CMAKE" -G "${CMAKE_GENERATOR}" "${COMMON_CMAKE_OPTIONS[@]}" \
                    "${!_PRODUCT_CMAKE_OPTIONS}" \
                    "${!_ASAN_OPTIONS}" \
                    -DSWIFT_MODULE_CACHE_PATH="${swift_module_cache}" \
                    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
                    -D${var_prefix}_PATH_TO_CLANG_SOURCE="${CLANG_SOURCE_DIR}" \
                    -D${var_prefix}_PATH_TO_CLANG_BUILD="${CLANG_BUILD_DIR}" \
                    -D${var_prefix}_PATH_TO_LLVM_SOURCE="${LLVM_SOURCE_DIR}" \
                    -D${var_prefix}_PATH_TO_LLVM_BUILD="${LLVM_BUILD_DIR}" \
                    ${CONFIG_ARGS} \
                    "${!_PRODUCT_SOURCE_DIR}" || exit 1)
        fi

        # Build.
        $DISTCC_PUMP "$CMAKE" --build "${!_PRODUCT_BUILD_DIR}" -- ${BUILD_ARGS}
    fi
done

# Trap function to print the current test configuration when tests fail.
# This is a function so the text is not unnecessarily displayed when running -x.
tests_busted ()
{
    echo "*** Failed while running tests for $1"
}

# Run the tests for each product
for product in "${SWIFT_TEST_PRODUCTS[@]}" ; do
    PRODUCT=$(toupper "${product}")
    
    _SKIP_TEST_PRODUCT=SKIP_TEST_${PRODUCT}
    if [[ ! "${!_SKIP_TEST_PRODUCT}" ]]; then
        
        echo "--- Running tests for ${product} ---"
        trap "tests_busted ${product}" ERR
        _PRODUCT_SOURCE_DIR=${PRODUCT}_SOURCE_DIR
        _PRODUCT_BUILD_DIR=${PRODUCT}_BUILD_DIR
    
        build_cmd=("$CMAKE" --build "${!_PRODUCT_BUILD_DIR}" -- ${BUILD_ARGS})

        if [[ "${product}" == SourceKit ]] ; then
            "${build_cmd[@]}" ${BUILD_TARGET_FLAG} SourceKitUnitTests
        else
            "${build_cmd[@]}" ${BUILD_TARGET_FLAG} SwiftUnitTests
        fi

        if [[ "${product}" == SourceKit ]] ; then
            test_target=check-${product}
            if [[ "$STRESS_TEST_SOURCEKIT" ]]; then
              test_target="${test_target} stress-SourceKit"
            fi
        else
            test_target=check-${product}-all
            if [[ "$SKIP_TEST_VALIDATION" ]]; then
                test_target=check-${product}
            fi
        fi

        if [[ "${CMAKE_GENERATOR}" == Ninja ]] ; then
            # Ninja buffers command output to avoid scrambling the output
            # of parallel jobs, which is awesome... except that it
            # interferes with the progress meter when testing.  Instead of
            # executing ninja directly, have it dump the commands it would
            # run, strip Ninja's progress prefix with sed, and tell the
            # shell to execute that.
            sh -c "$("${build_cmd[@]}" -n -v ${test_target} | sed -e 's/[^]]*] //')"
        else
            "${build_cmd[@]}" ${BUILD_TARGET_FLAG} ${test_target}
        fi

        trap - ERR
        echo "--- Finished tests for ${product} ---"
    fi
done

# Run the Swift performance tests.
if [ \! "$SKIP_TEST_SWIFT_PERFORMANCE" ]; then
  # Currently we use the toolchain-specific Clang as our CC under test, because
  # our locally built one might not end up invoking an LD that supports
  # autolinking on older machines. We can reconsider this when it becomes useful
  # to have the C/C++ tests be running using the same LLVM basis as the Swift we
  # are testing.
  echo "--- Running Swift Performance Tests ---"
  export CLANG="$TOOLCHAIN/usr/bin/clang"
  export SWIFT="${SWIFT_BUILD_DIR}/bin/swift"
  if (cd "${SWIFT_BUILD_DIR}" &&
          "${LLVM_BUILD_DIR}/bin/llvm-lit" -v benchmark \
              -j1 --output benchmark/results.json); then
      PERFORMANCE_TESTS_PASSED=1
  else
      PERFORMANCE_TESTS_PASSED=0
  fi
  echo "--- Submitting Swift Performance Tests ---"
  swift_source_revision="$("${LLVM_SOURCE_DIR}/utils/GetSourceVersion" "${SWIFT_SOURCE_DIR}")"
  (cd "${SWIFT_BUILD_DIR}" &&
    "${SWIFT_SOURCE_DIR}/utils/submit-benchmark-results" benchmark/results.json \
        --output benchmark/lnt_results.json \
        --machine-name "matte.apple.com--${BUILD_TYPE}--x86_64--O3" \
        --run-order "$swift_source_revision" \
        --submit http://localhost:32169/submitRun) \
  || echo "*** Swift performance test results not submitted."

  # If the performance tests failed, fail the build.
  if [ "$PERFORMANCE_TESTS_PASSED" -ne 1 ]; then
      echo "*** ERROR: Swift Performance Tests failed ***"
      exit 1
  fi
fi

