#!/usr/bin/env bash
#
# @@@ START COPYRIGHT @@@
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements. See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership. The ASF licenses this file
# to you under the Apache License, Version 2.0.
#
# @@@ END COPYRIGHT @@@
#

set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/install-local-lite-deps.sh [options]

Install native OS packages needed by the Trafodion local-lite build.
This script deliberately does not install Java, Maven, Hadoop, HBase, or Hive.

Options:
  -y, --yes          Run package manager without an interactive prompt.
  -n, --dry-run      Print commands and checks without changing the system.
      --no-sudo      Do not use sudo; require the current user to have access.
      --prefix DIR   MPICH compatibility directory to create.
                     Default: core/sqf/opt/local-lite-mpich
  -h, --help         Show this help.
USAGE
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
assume_yes=0
dry_run=0
use_sudo=1
mpich_prefix="$repo_root/core/sqf/opt/local-lite-mpich"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -y|--yes)
      assume_yes=1
      shift
      ;;
    -n|--dry-run)
      dry_run=1
      shift
      ;;
    --no-sudo)
      use_sudo=0
      shift
      ;;
    --prefix)
      [[ $# -ge 2 ]] || { echo "ERROR: --prefix requires a directory" >&2; exit 2; }
      mpich_prefix="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

run() {
  if [[ "$dry_run" -eq 1 ]]; then
    printf '+'
    printf ' %q' "$@"
    printf '\n'
  else
    "$@"
  fi
}

sudo_prefix=()
if [[ "$use_sudo" -eq 1 && "${EUID:-$(id -u)}" -ne 0 ]]; then
  if command -v sudo >/dev/null 2>&1; then
    sudo_prefix=(sudo)
  elif [[ "$dry_run" -eq 0 ]]; then
    echo "ERROR: sudo is required for package installation; rerun as root or pass --no-sudo." >&2
    exit 1
  fi
fi

require_confirmation() {
  [[ "$assume_yes" -eq 1 || "$dry_run" -eq 1 ]] && return 0

  printf 'Install local-lite native dependencies now? [y/N] '
  read -r reply
  case "$reply" in
    y|Y|yes|YES) ;;
    *) echo "Aborted."; exit 1 ;;
  esac
}

detect_os() {
  if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    printf '%s\n' "${ID:-unknown}:${ID_LIKE:-}"
  else
    printf 'unknown:\n'
  fi
}

package_manager() {
  local os_id_like="$1"

  if command -v apt-get >/dev/null 2>&1 && [[ "$os_id_like" == *debian* || "$os_id_like" == ubuntu:* ]]; then
    printf 'apt\n'
  elif command -v dnf >/dev/null 2>&1; then
    printf 'dnf\n'
  elif command -v yum >/dev/null 2>&1; then
    printf 'yum\n'
  else
    printf 'unsupported\n'
  fi
}

install_packages() {
  local manager="$1"
  local packages=()

  case "$manager" in
    apt)
      packages=(
        build-essential gcc g++ make cmake ninja-build
        autoconf automake libtool pkg-config
        bison flex gawk byacc ksh perl python3 python3-dev
        libreadline-dev libncurses-dev libcurl4-openssl-dev
        libssl-dev zlib1g-dev libevent-dev libboost-dev libsqlite3-dev sqlite3
        libprotobuf-dev protobuf-compiler
        liblog4cxx-dev libthrift-dev thrift-compiler
        mpich libmpich-dev
      )
      echo "Package manager: apt-get"
      echo "Packages: ${packages[*]}"
      require_confirmation
      run "${sudo_prefix[@]}" apt-get update
      run "${sudo_prefix[@]}" apt-get install -y "${packages[@]}"
      ;;
    dnf|yum)
      packages=(
        gcc gcc-c++ make cmake ninja-build
        autoconf automake libtool pkgconf-pkg-config
        bison flex gawk byacc ksh perl python3 python3-devel
        readline-devel ncurses-devel libcurl-devel openssl-devel
        zlib-devel libevent-devel boost-devel sqlite-devel sqlite
        protobuf-devel protobuf-compiler
        log4cxx-devel thrift-devel thrift
        mpich mpich-devel
      )
      echo "Package manager: $manager"
      echo "Packages: ${packages[*]}"
      require_confirmation
      run "${sudo_prefix[@]}" "$manager" install -y "${packages[@]}"
      ;;
    *)
      echo "ERROR: unsupported package manager. Install MPICH, Thrift, log4cxx, protobuf, SQLite, curl, OpenSSL, readline, ncurses, bison, flex, and C/C++ build tools manually." >&2
      exit 1
      ;;
  esac
}

first_existing() {
  local path
  for path in "$@"; do
    if [[ -e "$path" ]]; then
      printf '%s\n' "$path"
      return 0
    fi
  done
  return 1
}

find_mpi_header() {
  first_existing \
    /usr/include/*/mpich/mpi.h \
    /usr/include/mpich/mpi.h \
    /usr/lib/*/openmpi/include/mpi.h \
    /usr/include/mpi.h
}

find_binary() {
  local name
  for name in "$@"; do
    if command -v "$name" >/dev/null 2>&1; then
      command -v "$name"
      return 0
    fi
  done
  return 1
}

find_hydra_pmi_proxy() {
  find_binary hydra_pmi_proxy hydra_pmi_proxy.mpich || \
    first_existing \
      /usr/lib/*/mpich/bin/hydra_pmi_proxy \
      /usr/lib64/mpich/bin/hydra_pmi_proxy \
      /usr/lib/mpich/bin/hydra_pmi_proxy
}

link_mpich_compat_tree() {
  local mpi_h mpi_include_dir mpicxx mpirun hydra header

  if [[ "$dry_run" -eq 1 ]]; then
    echo "Would create MPICH compatibility tree at: $mpich_prefix"
    return 0
  fi

  mpi_h="$(find_mpi_header || true)"
  mpicxx="$(find_binary mpicxx.mpich mpicxx || true)"
  mpirun="$(find_binary mpirun.mpich mpirun || true)"
  hydra="$(find_hydra_pmi_proxy || true)"

  [[ -n "$mpi_h" ]] || { echo "ERROR: could not find mpi.h after installing MPICH." >&2; exit 1; }
  [[ -n "$mpicxx" ]] || { echo "ERROR: could not find mpicxx after installing MPICH." >&2; exit 1; }
  [[ -n "$mpirun" ]] || { echo "ERROR: could not find mpirun after installing MPICH." >&2; exit 1; }
  [[ -n "$hydra" ]] || { echo "ERROR: could not find hydra_pmi_proxy after installing MPICH." >&2; exit 1; }

  mkdir -p "$mpich_prefix/include" "$mpich_prefix/bin"
  mpi_include_dir="$(dirname "$mpi_h")"
  for header in "$mpi_include_dir"/*; do
    [[ -f "$header" ]] || continue
    ln -sfn "$header" "$mpich_prefix/include/$(basename "$header")"
  done
  ln -sfn "$mpicxx" "$mpich_prefix/bin/mpicxx"
  ln -sfn "$mpirun" "$mpich_prefix/bin/mpirun"
  ln -sfn "$hydra" "$mpich_prefix/bin/hydra_pmi_proxy"

  echo "MPICH compatibility tree: $mpich_prefix"
}

check_glob() {
  local label="$1"
  local pattern="$2"
  if compgen -G "$pattern" >/dev/null; then
    echo "OK: $label"
  else
    echo "MISSING: $label ($pattern)"
    return 1
  fi
}

check_any_glob() {
  local label="$1"
  shift

  local pattern
  for pattern in "$@"; do
    if compgen -G "$pattern" >/dev/null; then
      echo "OK: $label"
      return 0
    fi
  done

  echo "MISSING: $label ($*)"
  return 1
}

verify_deps() {
  if [[ "$dry_run" -eq 1 ]]; then
    echo "Dry-run: skipped dependency verification."
    return 0
  fi

  local failures=0
  check_any_glob "libthrift shared library" \
    "/usr/lib/*/libthrift*.so*" \
    "/usr/lib64/libthrift*.so*" \
    "/usr/lib/libthrift*.so*" || failures=$((failures + 1))
  check_any_glob "liblog4cxx shared library" \
    "/usr/lib/*/liblog4cxx*.so*" \
    "/usr/lib64/liblog4cxx*.so*" \
    "/usr/lib/liblog4cxx*.so*" || failures=$((failures + 1))
  [[ -e "$mpich_prefix/include/mpi.h" ]] && echo "OK: MPICH compatibility mpi.h" || { echo "MISSING: $mpich_prefix/include/mpi.h"; failures=$((failures + 1)); }
  [[ -e "$mpich_prefix/bin/mpirun" ]] && echo "OK: MPICH compatibility mpirun" || { echo "MISSING: $mpich_prefix/bin/mpirun"; failures=$((failures + 1)); }
  [[ -e "$mpich_prefix/bin/hydra_pmi_proxy" ]] && echo "OK: MPICH compatibility hydra_pmi_proxy" || { echo "MISSING: $mpich_prefix/bin/hydra_pmi_proxy"; failures=$((failures + 1)); }

  if [[ "$failures" -ne 0 ]]; then
    echo "ERROR: $failures dependency check(s) failed." >&2
    exit 1
  fi
}

main() {
  local os manager
  os="$(detect_os)"
  manager="$(package_manager "$os")"

  echo "Repository: $repo_root"
  echo "Detected OS: $os"
  echo "Java/Hadoop packages: intentionally skipped"

  install_packages "$manager"
  link_mpich_compat_tree
  verify_deps

  echo
  echo "Next step:"
  echo "  make local-lite"
}

main "$@"
