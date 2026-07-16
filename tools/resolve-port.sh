#!/usr/bin/env bash
# Resolve the serial port of the board to flash/monitor, so Makefile targets never hardcode
# /dev/ttyUSBn — the numbering is assigned in random plug order (see CLAUDE.md, "Identify a
# board by its ESP32 MAC").
#
# Usage: tools/resolve-port.sh [PORT]
#   - PORT given (non-empty): echo it back verbatim and succeed (caller pinned the board).
#   - exactly one /dev/ttyUSB*: echo it and succeed.
#   - zero, or more than one: print guidance to stderr and fail, forcing the caller to pass
#     PORT=/dev/ttyUSBn. On the ambiguous case we annotate each port with its ESP32 MAC (a
#     read-only eFuse read) so the right one is obvious.
set -euo pipefail

want=${1:-}
if [ -n "$want" ]; then
	printf '%s\n' "$want"
	exit 0
fi

shopt -s nullglob
ports=(/dev/ttyUSB*)

case ${#ports[@]} in
0)
	echo "resolve-port: no /dev/ttyUSB* found — plug in a board, or pass PORT=/dev/ttyUSBn." >&2
	exit 1
	;;
1)
	printf '%s\n' "${ports[0]}"
	exit 0
	;;
*)
	{
		echo "resolve-port: ${#ports[@]} boards attached — pass PORT=/dev/ttyUSBn (ttyUSB numbering is random per plug):"
		for d in "${ports[@]}"; do
			mac=""
			if command -v esptool.py >/dev/null 2>&1; then
				# Best-effort: a busy/unresponsive port must annotate as unknown, not abort (set -e).
				mac=$(esptool.py -p "$d" read_mac 2>/dev/null | awk '/^MAC:/{print $2; exit}') || true
			fi
			printf '  %s  %s\n' "$d" "${mac:-(mac unknown)}"
		done
		echo "  known: 8c:94:df:92:21:e4=CYD2.8  b0:cb:d8:03:5c:d8=CYD3.5  c0:cd:d6:cb:e7:e4=controller"
	} >&2
	exit 1
	;;
esac
