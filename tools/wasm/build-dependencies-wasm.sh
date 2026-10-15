#!/usr/bin/env bash
# Builds the third-party libraries PCSX2 needs for an Emscripten (wasm32) build.
# Versions and hashes follow .github/workflows/scripts/linux/build-dependencies-qt.sh.
# Everything is static and compiled with -pthread: linking a shared-memory wasm
# module fails if any object was built without atomics/bulk-memory. libpng,
# libjpeg and FreeType use setjmp, so they also need -sSUPPORT_LONGJMP=wasm to
# match the PCSX2 build (otherwise they reference the JS-based emscripten_longjmp).
# Qt, FFmpeg, libbacktrace, shaderc and Vulkan are not built; the browser build
# does not use them.
#
# Usage: source ~/emsdk/emsdk_env.sh && tools/wasm/build-dependencies-wasm.sh deps-wasm

set -e

if [ "$#" -ne 1 ]; then
	echo "Syntax: $0 <output directory>"
	exit 1
fi

if ! command -v emcmake >/dev/null; then
	echo "emcmake not found; run: source ~/emsdk/emsdk_env.sh"
	exit 1
fi

INSTALLDIR="$1"
if [ "${INSTALLDIR:0:1}" != "/" ]; then
	INSTALLDIR="$PWD/$INSTALLDIR"
fi

ZLIB=1.3.1
FREETYPE=2.13.3
LIBJPEGTURBO=3.2.0
LIBPNG=1.6.58
LIBWEBP=1.6.0
SDL=SDL3-3.4.18
LZ4=1.10.0
ZSTD=1.5.7
PLUTOVG=1.3.3
PLUTOSVG=0.0.8
RAPIDYAML=0.12.1

mkdir -p deps-build-wasm
cd deps-build-wasm

grep . > SHASUMS <<EOF
9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23  zlib-$ZLIB.tar.gz
0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289  freetype-$FREETYPE.tar.xz
6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e  libjpeg-turbo-$LIBJPEGTURBO.tar.gz
28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775  libpng-$LIBPNG.tar.xz
eee7dea22ed502868017971c86c63c4ed1e6085de0baebfdcc3d3322f00f3eb0  libpng-$LIBPNG-apng.patch.gz
e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564  libwebp-$LIBWEBP.tar.gz
9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3  $SDL.tar.gz
537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b  lz4-$LZ4.tar.gz
eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3  zstd-$ZSTD.tar.gz
2b0d17a6e016f47b86f9c00e2cb82600041b1ea1f7d2a00c2d46ae542cbfed3c  plutovg-$PLUTOVG.tar.gz
49d5cfe772d3aa10cd4879f2f6e189f5083c08e4c8ea01bf3d5b87c97dfca7d2  plutosvg-$PLUTOSVG.tar.gz
e9efcdd17f86287748793cf21d106e461fcad8d103a3e5a23632afe93828660d  rapidyaml-$RAPIDYAML-src.tgz
EOF

if ! shasum -sa 256 --check SHASUMS 2> /dev/null; then
	curl -L \
		-O "https://github.com/madler/zlib/releases/download/v$ZLIB/zlib-$ZLIB.tar.gz" \
		-O "https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE.tar.xz" \
		-O "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LIBJPEGTURBO/libjpeg-turbo-$LIBJPEGTURBO.tar.gz" \
		-O "https://downloads.sourceforge.net/project/libpng/libpng16/$LIBPNG/libpng-$LIBPNG.tar.xz" \
		-O "https://download.sourceforge.net/libpng-apng/libpng-$LIBPNG-apng.patch.gz" \
		-O "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$LIBWEBP.tar.gz" \
		-O "https://libsdl.org/release/$SDL.tar.gz" \
		-O "https://github.com/lz4/lz4/releases/download/v$LZ4/lz4-$LZ4.tar.gz" \
		-O "https://github.com/facebook/zstd/releases/download/v$ZSTD/zstd-$ZSTD.tar.gz" \
		-O "https://github.com/sammycage/plutovg/archive/v$PLUTOVG/plutovg-$PLUTOVG.tar.gz" \
		-O "https://github.com/sammycage/plutosvg/archive/v$PLUTOSVG/plutosvg-$PLUTOSVG.tar.gz" \
		-O "https://github.com/biojppm/rapidyaml/releases/download/v$RAPIDYAML/rapidyaml-$RAPIDYAML-src.tgz"
fi

shasum -a 256 --check --strict SHASUMS

CMAKE_COMMON=(
	-G Ninja
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_INSTALL_PREFIX="$INSTALLDIR"
	-DCMAKE_PREFIX_PATH="$INSTALLDIR"
	-DCMAKE_FIND_ROOT_PATH="$INSTALLDIR"
	-DBUILD_SHARED_LIBS=OFF
	-DCMAKE_C_FLAGS="-pthread -sSUPPORT_LONGJMP=wasm"
	-DCMAKE_CXX_FLAGS="-pthread -sSUPPORT_LONGJMP=wasm"
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5
)

echo "Building zlib..."
rm -fr "zlib-$ZLIB"
tar xf "zlib-$ZLIB.tar.gz"
cd "zlib-$ZLIB"
emcmake cmake "${CMAKE_COMMON[@]}" -DZLIB_BUILD_EXAMPLES=OFF -B build
cmake --build build --parallel
ninja -C build install
# zlib always builds the shared target too; static linking must not pick it up.
rm -f "$INSTALLDIR"/lib/libz.so*
cd ..

echo "Building libpng..."
rm -fr "libpng-$LIBPNG"
tar xf "libpng-$LIBPNG.tar.xz"
gzip -kd -f "libpng-$LIBPNG-apng.patch.gz"
cd "libpng-$LIBPNG"
patch -p1 < "../libpng-$LIBPNG-apng.patch"
emcmake cmake "${CMAKE_COMMON[@]}" -DPNG_TESTS=OFF -DPNG_STATIC=ON -DPNG_SHARED=OFF -DPNG_TOOLS=OFF -DPNG_HARDWARE_OPTIMIZATIONS=OFF -B build
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building libjpegturbo..."
rm -fr "libjpeg-turbo-$LIBJPEGTURBO"
tar xf "libjpeg-turbo-$LIBJPEGTURBO.tar.gz"
cd "libjpeg-turbo-$LIBJPEGTURBO"
emcmake cmake "${CMAKE_COMMON[@]}" -DENABLE_STATIC=ON -DENABLE_SHARED=OFF -DWITH_SIMD=OFF -DCMAKE_INSTALL_DEFAULT_LIBDIR="lib" -B build
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building LZ4..."
rm -fr "lz4-$LZ4"
tar xf "lz4-$LZ4.tar.gz"
cd "lz4-$LZ4"
emcmake cmake "${CMAKE_COMMON[@]}" -DBUILD_STATIC_LIBS=ON -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -B build-dir build/cmake
cmake --build build-dir --parallel
ninja -C build-dir install
cd ..

echo "Building Zstandard..."
rm -fr "zstd-$ZSTD"
tar xf "zstd-$ZSTD.tar.gz"
cd "zstd-$ZSTD"
emcmake cmake "${CMAKE_COMMON[@]}" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -B build build/cmake
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building WebP..."
rm -fr "libwebp-$LIBWEBP"
tar xf "libwebp-$LIBWEBP.tar.gz"
cd "libwebp-$LIBWEBP"
emcmake cmake "${CMAKE_COMMON[@]}" -B build \
	-DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
	-DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_WEBP_JS=OFF
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building SDL..."
rm -fr "$SDL"
tar xf "$SDL.tar.gz"
cd "$SDL"
emcmake cmake "${CMAKE_COMMON[@]}" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_PTHREADS=ON -DSDL_VIDEO=OFF -DSDL_POWER=OFF -DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -B build
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building FreeType..."
rm -fr "freetype-$FREETYPE"
tar xf "freetype-$FREETYPE.tar.xz"
cd "freetype-$FREETYPE"
emcmake cmake "${CMAKE_COMMON[@]}" -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -B build
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building PlutoVG..."
rm -fr "plutovg-$PLUTOVG"
tar xf "plutovg-$PLUTOVG.tar.gz"
cd "plutovg-$PLUTOVG"
emcmake cmake "${CMAKE_COMMON[@]}" -DPLUTOVG_BUILD_EXAMPLES=OFF -B build
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building PlutoSVG..."
rm -fr "plutosvg-$PLUTOSVG"
tar xf "plutosvg-$PLUTOSVG.tar.gz"
cd "plutosvg-$PLUTOSVG"
emcmake cmake "${CMAKE_COMMON[@]}" -DPLUTOSVG_ENABLE_FREETYPE=OFF -DPLUTOSVG_BUILD_EXAMPLES=OFF -B build
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building RapidYAML..."
rm -fr "rapidyaml-$RAPIDYAML-src"
tar xf "rapidyaml-$RAPIDYAML-src.tgz"
cd "rapidyaml-$RAPIDYAML-src"
emcmake cmake "${CMAKE_COMMON[@]}" -B build
cmake --build build --parallel
ninja -C build install
cd ..

echo "Done. Dependencies installed to $INSTALLDIR"
