set -euo pipefail

SOURCE="main.cpp"
OUTPUT="luasun"
BUILD_DIR="build"
ENTRY="game/main.lua"

RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'
BLUE=$'\033[34m'; BOLD=$'\033[1m'; OFF=$'\033[0m'

info()  { printf '%s==>%s %s\n' "$BLUE$BOLD" "$OFF$BOLD" "$1$OFF"; }
ok()    { printf '%s  ok%s %s\n' "$GREEN" "$OFF" "$1"; }
warn()  { printf '%s  !!%s %s\n' "$YELLOW" "$OFF" "$1"; }
die()   { printf '%s  xx%s %s\n' "$RED" "$OFF" "$1" >&2; exit 1; }

SUDO=""

setup_sudo() {
    if [ -n "${PREFIX:-}" ] && [ -d "${PREFIX:-}/bin" ] && command -v pkg >/dev/null 2>&1; then
        SUDO=""                       # Termux
    elif [ "$(id -u)" -eq 0 ]; then
        SUDO=""
    elif command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    elif command -v doas >/dev/null 2>&1; then
        SUDO="doas"
    else
        die "preciso de root (sudo/doas) para instalar pacotes"
    fi
}

install_deps() {
    setup_sudo

    if command -v pkg >/dev/null 2>&1 && [ -n "${PREFIX:-}" ]; then
        info "Termux detectado"
        pkg install -y clang make pkg-config lua54 sdl2 sdl2-image sdl2-ttf

    elif command -v apt-get >/dev/null 2>&1; then
        info "Debian / Ubuntu / Mint detectado"
        $SUDO apt-get update
        $SUDO apt-get install -y \
            build-essential pkg-config \
            liblua5.4-dev \
            libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev \
            fonts-dejavu-core

    elif command -v pacman >/dev/null 2>&1; then
        info "Arch / Manjaro detectado"
        $SUDO pacman -S --needed --noconfirm \
            base-devel pkgconf lua sdl2 sdl2_image sdl2_ttf ttf-dejavu

    elif command -v dnf >/dev/null 2>&1; then
        info "Fedora detectado"
        $SUDO dnf install -y \
            gcc-c++ make pkgconf-pkg-config \
            lua-devel SDL2-devel SDL2_image-devel SDL2_ttf-devel \
            dejavu-sans-fonts

    elif command -v zypper >/dev/null 2>&1; then
        info "openSUSE detectado"
        $SUDO zypper install -y \
            gcc-c++ make pkg-config \
            lua54-devel libSDL2-devel libSDL2_image-devel libSDL2_ttf-devel

    elif command -v apk >/dev/null 2>&1; then
        info "Alpine detectado"
        $SUDO apk add \
            build-base pkgconf lua5.4-dev \
            sdl2-dev sdl2_image-dev sdl2_ttf-dev font-dejavu

    else
        die "gerenciador de pacotes nao reconhecido - instale a mao: SDL2, SDL2_image, SDL2_ttf, Lua 5.4 (headers) e g++/clang"
    fi

    ok "dependencias instaladas"
}

LUA_CFLAGS=""; LUA_LIBS=""
SDL_CFLAGS=""; SDL_LIBS=""

find_lua() {
    for name in lua5.4 lua-5.4 lua54 lua; do
        if pkg-config --exists "$name" 2>/dev/null; then
            LUA_CFLAGS="$(pkg-config --cflags "$name")"
            LUA_LIBS="$(pkg-config --libs "$name")"
            ok "Lua encontrado via pkg-config ($name)"
            return 0
        fi
    done

    for dir in /usr/include/lua5.4 /usr/include/lua54 /usr/include/lua \
               "${PREFIX:-/usr}/include/lua5.4" "${PREFIX:-/usr}/include"; do
        if [ -f "$dir/lua.h" ]; then
            LUA_CFLAGS="-I$dir"
            for lib in lua5.4 lua54 lua; do
                LUA_LIBS="-l$lib"
                break
            done
            warn "Lua achado em $dir (sem pkg-config)"
            return 0
        fi
    done

    die "headers do Lua 5.4 nao encontrados"
}

find_sdl() {
    if pkg-config --exists sdl2 2>/dev/null; then
        SDL_CFLAGS="$(pkg-config --cflags sdl2)"
        SDL_LIBS="$(pkg-config --libs sdl2) -lSDL2_image -lSDL2_ttf"
    elif command -v sdl2-config >/dev/null 2>&1; then
        SDL_CFLAGS="$(sdl2-config --cflags)"
        SDL_LIBS="$(sdl2-config --libs) -lSDL2_image -lSDL2_ttf"
    else
        die "SDL2 nao encontrado"
    fi

    ok "SDL2 encontrado"
}

make_lua_shim() {
    local header_dir=""

    for flag in $LUA_CFLAGS; do
        case "$flag" in
            -I*) [ -f "${flag#-I}/lua.hpp" ] && header_dir="${flag#-I}" ;;
        esac
    done

    [ -z "$header_dir" ] && [ -f /usr/include/lua5.4/lua.hpp ] && header_dir=/usr/include/lua5.4

    if [ -z "$header_dir" ]; then
        # lua.hpp nao existe (pacote so tem lua.h) - geramos o wrapper
        for flag in $LUA_CFLAGS; do
            case "$flag" in
                -I*) [ -f "${flag#-I}/lua.h" ] && header_dir="${flag#-I}" ;;
            esac
        done

        [ -z "$header_dir" ] && die "nao achei lua.h para montar o shim"

        mkdir -p "$BUILD_DIR/compat/lua5.4"

        cat > "$BUILD_DIR/compat/lua5.4/lua.hpp" <<EOF
// gerado pelo install.sh
extern "C" {
#include "$header_dir/lua.h"
#include "$header_dir/lualib.h"
#include "$header_dir/lauxlib.h"
}
EOF
        LUA_CFLAGS="$LUA_CFLAGS -I$BUILD_DIR/compat"
        warn "lua.hpp gerado em $BUILD_DIR/compat"
        return
    fi

    case "$header_dir" in
        */lua5.4)
            LUA_CFLAGS="$LUA_CFLAGS -I$(dirname "$header_dir")"
            ;;
        *)
            mkdir -p "$BUILD_DIR/compat/lua5.4"
            ln -sf "$header_dir"/*.h  "$BUILD_DIR/compat/lua5.4/" 2>/dev/null || true
            ln -sf "$header_dir/lua.hpp" "$BUILD_DIR/compat/lua5.4/lua.hpp"
            LUA_CFLAGS="$LUA_CFLAGS -I$BUILD_DIR/compat"
            warn "pasta de compatibilidade criada em $BUILD_DIR/compat"
            ;;
    esac
}

build() {
    [ -f "$SOURCE" ] || die "$SOURCE nao encontrado nesta pasta"

    if command -v g++ >/dev/null 2>&1; then
        CXX_BIN="g++"
    elif command -v clang++ >/dev/null 2>&1; then
        CXX_BIN="clang++"
    else
        die "nenhum compilador C++ encontrado"
    fi

    mkdir -p "$BUILD_DIR"

    find_lua
    find_sdl
    make_lua_shim

    info "compilando com $CXX_BIN"

    $CXX_BIN -std=c++17 -O2 -Wall -Wextra \
        $SDL_CFLAGS $LUA_CFLAGS \
        "$SOURCE" -o "$OUTPUT" \
        $SDL_LIBS $LUA_LIBS -lm

    ok "binario gerado: ./$OUTPUT"

    if [ ! -f "$ENTRY" ]; then
        warn "$ENTRY nao existe - crie seu jogo ou rode: ./$OUTPUT exemplo.lua"
    fi
}

clean() {
    rm -rf "$BUILD_DIR" "$OUTPUT"
    ok "limpo"
}

case "${1:-all}" in
    --deps)  install_deps ;;
    --build) build ;;
    --clean) clean ;;
    --run)   build; info "rodando $ENTRY"; ./"$OUTPUT" "$ENTRY" ;;
    all|"")  install_deps; build ;;
    -h|--help)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *) die "opcao desconhecida: $1" ;;
esac
