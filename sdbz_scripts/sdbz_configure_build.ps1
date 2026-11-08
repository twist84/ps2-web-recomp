$ErrorActionPreference = 'Stop'

$Root = Resolve-Path -LiteralPath (Join-Path -Path $PSScriptRoot -ChildPath "..\sdbz")
$ProjectRoot = Resolve-Path -LiteralPath (Join-Path -Path $PSScriptRoot -ChildPath "..")
$BuildDir = "$Root\build-clang"
$GenDir = "$Root\generated\SuperDragonBallZ"
$Elf = "$Root\games\SuperDragonBallZ\disc\SLES_541.61"
$Entries = "$Root\games\SuperDragonBallZ\entries.txt"
$Vu1Dir = "$Root\games\SuperDragonBallZ\vu1"

$SDL2Version = "2.30.9"
$SDL2Zip = "$env:TEMP\SDL2-devel-$SDL2Version-VC.zip"
$SDL2Source = "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2Version/SDL2-devel-$SDL2Version-VC.zip"
$SDL2Deps = "$BuildDir\_deps\SDL2"

# Embedded SDL2 cmake config (not part of the official VC dev package).
$Sdl2ConfigCmake = @'
# SDL2 CMake configuration file:
cmake_minimum_required(VERSION 3.0...3.5)

include(FeatureSummary)
set_package_properties(SDL2 PROPERTIES
    URL "https://www.libsdl.org/"
    DESCRIPTION "low level access to audio, keyboard, mouse, joystick, and graphics hardware"
)

macro(set_and_check _var _file)
    set(${_var} "${_file}")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
    endif()
endmacro()

macro(check_required_components _NAME)
    foreach(comp ${${_NAME}_FIND_COMPONENTS})
        if(NOT ${_NAME}_${comp}_FOUND)
            if(${_NAME}_FIND_REQUIRED_${comp})
                set(${_NAME}_FOUND FALSE)
            endif()
        endif()
    endforeach()
endmacro()

set(SDL2_FOUND TRUE)

if(CMAKE_SIZEOF_VOID_P STREQUAL "4")
    set(_sdl_arch_subdir "x86")
elseif(CMAKE_SIZEOF_VOID_P STREQUAL "8")
    set(_sdl_arch_subdir "x64")
else()
    set(SDL2_FOUND FALSE)
    return()
endif()

set_and_check(SDL2_PREFIX       "${CMAKE_CURRENT_LIST_DIR}/..")
set_and_check(SDL2_EXEC_PREFIX  "${CMAKE_CURRENT_LIST_DIR}/..")
set_and_check(SDL2_INCLUDE_DIR  "${SDL2_PREFIX}/include")
set(SDL2_INCLUDE_DIRS           "${SDL2_INCLUDE_DIR}")
set_and_check(SDL2_BINDIR       "${SDL2_PREFIX}/lib/${_sdl_arch_subdir}")
set_and_check(SDL2_LIBDIR       "${SDL2_PREFIX}/lib/${_sdl_arch_subdir}")

set(SDL2_LIBRARIES      SDL2::SDL2main SDL2::SDL2)
set(SDL2MAIN_LIBRARY    SDL2::SDL2main)
set(SDL2TEST_LIBRARY    SDL2::SDL2test)

set(_sdl2_library     "${SDL2_LIBDIR}/SDL2.lib")
set(_sdl2_dll_library "${SDL2_BINDIR}/SDL2.dll")
if(EXISTS "${_sdl2_library}" AND EXISTS "${_sdl2_dll_library}")
    if(NOT TARGET SDL2::SDL2)
        add_library(SDL2::SDL2 SHARED IMPORTED)
        set_target_properties(SDL2::SDL2
            PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
                IMPORTED_IMPLIB "${_sdl2_library}"
                IMPORTED_LOCATION "${_sdl2_dll_library}"
                COMPATIBLE_INTERFACE_BOOL "SDL2_SHARED"
                INTERFACE_SDL2_SHARED "ON"
                COMPATIBLE_INTERFACE_STRING "SDL_VERSION"
                INTERFACE_SDL_VERSION "SDL2"
        )
    endif()
    set(SDL2_SDL2_FOUND TRUE)
else()
    set(SDL2_SDL2_FOUND FALSE)
endif()
unset(_sdl2_library)
unset(_sdl2_dll_library)

set(SDL2_SDL2-static_FOUND FALSE)

set(_sdl2main_library "${SDL2_LIBDIR}/SDL2main.lib")
if(EXISTS "${_sdl2main_library}")
    if(NOT TARGET SDL2::SDL2main)
        add_library(SDL2::SDL2main STATIC IMPORTED)
        set_target_properties(SDL2::SDL2main
        PROPERTIES
            IMPORTED_LOCATION "${_sdl2main_library}"
            COMPATIBLE_INTERFACE_STRING "SDL_VERSION"
            INTERFACE_SDL_VERSION "SDL2"
        )
    endif()
    set(SDL2_SDL2main_FOUND TRUE)
else()
    set(SDL2_SDL2main_FOUND FALSE)
endif()
unset(_sdl2main_library)

set(_sdl2test_library "${SDL2_LIBDIR}/SDL2test.lib")
if(EXISTS "${_sdl2test_library}")
    if(NOT TARGET SDL2::SDL2test)
        add_library(SDL2::SDL2test STATIC IMPORTED)
        set_target_properties(SDL2::SDL2test
            PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
                IMPORTED_LOCATION "${_sdl2test_library}"
                COMPATIBLE_INTERFACE_STRING "SDL_VERSION"
                INTERFACE_SDL_VERSION "SDL2"
        )
    endif()
    set(SDL2_SDL2test_FOUND TRUE)
else()
    set(SDL2_SDL2test_FOUND FALSE)
endif()
unset(_sdl2test_library)

check_required_components(SDL2)
'@

$Sdl2ConfigVersionCmake = @'
# SDL2 CMake version configuration file:

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../include/SDL_version.h")
    message(AUTHOR_WARNING "Could not find SDL_version.h.")
    return()
endif()

file(READ "${CMAKE_CURRENT_LIST_DIR}/../include/SDL_version.h" _sdl_version_h)
string(REGEX MATCH "#define[ \t]+SDL_MAJOR_VERSION[ \t]+([0-9]+)" _sdl_major_re "${_sdl_version_h}")
set(_sdl_major "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define[ \t]+SDL_MINOR_VERSION[ \t]+([0-9]+)" _sdl_minor_re "${_sdl_version_h}")
set(_sdl_minor "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define[ \t]+SDL_PATCHLEVEL[ \t]+([0-9]+)" _sdl_patch_re "${_sdl_version_h}")
set(_sdl_patch "${CMAKE_MATCH_1}")
if(_sdl_major_re AND _sdl_minor_re AND _sdl_patch_re)
    set(PACKAGE_VERSION "${_sdl_major}.${_sdl_minor}.${_sdl_patch}")
else()
    message(AUTHOR_WARNING "Could not extract version from SDL_version.h.")
    return()
endif()

if(PACKAGE_FIND_VERSION_RANGE)
    if ((PACKAGE_FIND_VERSION_RANGE_MIN STREQUAL "INCLUDE" AND PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION_MIN)
        OR ((PACKAGE_FIND_VERSION_RANGE_MAX STREQUAL "INCLUDE" AND PACKAGE_VERSION VERSION_GREATER PACKAGE_FIND_VERSION_MAX)
        OR (PACKAGE_FIND_VERSION_RANGE_MAX STREQUAL "EXCLUDE" AND PACKAGE_VERSION VERSION_GREATER_EQUAL PACKAGE_FIND_VERSION_MAX)))
        set(PACKAGE_VERSION_COMPATIBLE FALSE)
    else()
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    endif()
else()
    if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
        set(PACKAGE_VERSION_COMPATIBLE FALSE)
    else()
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
        if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
            set(PACKAGE_VERSION_EXACT TRUE)
        endif()
    endif()
endif()

if("${CMAKE_SIZEOF_VOID_P}" STREQUAL "")
    set(PACKAGE_VERSION_UNSUITABLE TRUE)
endif()

if(NOT (CMAKE_SIZEOF_VOID_P STREQUAL "8" OR CMAKE_SIZEOF_VOID_P STREQUAL "4"))
    set(PACKAGE_VERSION "${PACKAGE_VERSION} (32+64bit)")
    set(PACKAGE_VERSION_UNSUITABLE TRUE)
endif()
'@

function Ensure-Sdl2Deps {
    $marker = "$SDL2Deps\.sdl2-ready"
    if (Test-Path -LiteralPath $marker) { return }

    if (!(Test-Path -LiteralPath $SDL2Zip)) {
        Write-Host "Downloading SDL2 $SDL2Version from GitHub..."
        Invoke-WebRequest -Uri $SDL2Source -OutFile $SDL2Zip -TimeoutSec 300
    }

    Write-Host "Extracting SDL2 to $SDL2Deps..."
    New-Item -ItemType Directory -Force -Path "$SDL2Deps\lib\x64" | Out-Null
    New-Item -ItemType Directory -Force -Path "$SDL2Deps\include" | Out-Null
    New-Item -ItemType Directory -Force -Path "$SDL2Deps\cmake" | Out-Null

    Expand-Archive -Path $SDL2Zip -DestinationPath "$env:TEMP\sdl2-extract" -Force

    $extracted = Get-ChildItem -LiteralPath "$env:TEMP\sdl2-extract" -Directory | Select-Object -First 1
    $srcRoot = $extracted.FullName

    Copy-Item -Path "$srcRoot\lib\x64\SDL2.dll" -Destination "$SDL2Deps\lib\x64\" -Force
    Copy-Item -Path "$srcRoot\lib\x64\SDL2.lib" -Destination "$SDL2Deps\lib\x64\" -Force
    Copy-Item -Path "$srcRoot\lib\x64\SDL2main.lib" -Destination "$SDL2Deps\lib\x64\" -Force
    Copy-Item -Path "$srcRoot\include\*" -Destination "$SDL2Deps\include\" -Force -Recurse

    $Sdl2ConfigCmake | Set-Content -Path "$SDL2Deps\cmake\sdl2-config.cmake" -Encoding UTF8
    $Sdl2ConfigVersionCmake | Set-Content -Path "$SDL2Deps\cmake\sdl2-config-version.cmake" -Encoding UTF8

    Remove-Item -LiteralPath "$env:TEMP\sdl2-extract" -Recurse -Force -ErrorAction SilentlyContinue

    "SDL2 $SDL2Version ready" | Set-Content -Path $marker -Encoding UTF8
    Write-Host "SDL2 $SDL2Version ready in $SDL2Deps"
}

if (!(Test-Path -LiteralPath $Elf)) { throw "ELF not found: $Elf" }

Set-Location -LiteralPath $ProjectRoot

$AlreadyConfigured = $false
if (Test-Path -LiteralPath $BuildDir) {
    $cache = Join-Path -Path $BuildDir -ChildPath "CMakeCache.txt"
    if (Test-Path -LiteralPath $cache) {
        $existing = Get-Content -LiteralPath $cache | Select-String "PS2_GENERATED:"
        if ($existing) {
            $AlreadyConfigured = $true
            Write-Host "build-clang already configured with PS2_GENERATED."
        }
    }
}

if (-not $AlreadyConfigured) {
    Write-Host "Phase 1: configuring build-clang for tools..."
    if (Test-Path -LiteralPath $BuildDir) {
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }
    if (!(Test-Path -LiteralPath $BuildDir)) {
        New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    }
    Ensure-Sdl2Deps

    cmake -S $ProjectRoot -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ "-DSDL2_DIR=$SDL2Deps\cmake"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure (phase 1) failed" }
}

Write-Host "Building eerecomp and vurecomp..."
Set-Location -LiteralPath $BuildDir
ninja eerecomp vurecomp
if ($LASTEXITCODE -ne 0) { throw "ninja build of eerecomp/vurecomp failed" }

$Eerecomp = "$BuildDir\eerecomp.exe"
$Vurecomp = "$BuildDir\vurecomp.exe"
if (!(Test-Path -LiteralPath $Eerecomp)) { throw "eerecomp.exe missing: $Eerecomp" }
if (!(Test-Path -LiteralPath $Vurecomp)) { throw "vurecomp.exe missing: $Vurecomp" }

Set-Location -LiteralPath $ProjectRoot

Write-Host "Phase 2: generating units..."
New-Item -ItemType Directory -Force -Path $GenDir | Out-Null
& $Eerecomp $Elf --entries $Entries --out $GenDir
if ($LASTEXITCODE -ne 0) { throw "eerecomp failed" }

if (Test-Path -LiteralPath $Vu1Dir) {
    $bins = Get-ChildItem -LiteralPath $Vu1Dir -Filter *.bin -File
    if ($bins) {
        & $Vurecomp $Vu1Dir $GenDir
        if ($LASTEXITCODE -ne 0) { throw "vurecomp failed" }
    } else {
        Write-Host "No VU .bin programs recorded, skipping vurecomp."
    }
} else {
    Write-Host "No VU directory, skipping vurecomp."
}

if (-not $AlreadyConfigured) {
    Write-Host "Phase 3: reconfiguring build-clang with PS2_GENERATED..."
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
    if (!(Test-Path -LiteralPath $BuildDir)) {
        New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    }
    Ensure-Sdl2Deps

    cmake -S $ProjectRoot -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ "-DSDL2_DIR=$SDL2Deps\cmake" "-DPS2_GENERATED=$GenDir"

    if ($LASTEXITCODE -ne 0) { throw "cmake configure (phase 3) failed" }
}

Set-Location -LiteralPath $ProjectRoot
Write-Host "Done. Build with:  .\sdbz_build.cmd"
