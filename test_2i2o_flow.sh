#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  ./test_2i2o_flow.sh <benchmark.v> [options]

Positional arguments:
  <benchmark.v>                                    Input benchmark file path.

Options:
  --exp-name <name>                                Optional experiment suffix.
  --map <comma-separated-list>                     Mapper gate set, e.g. ha,and,or,xor,inv.
                                                   Default: all2
  --fiction-bin <path>                             Path to fiction binary.
                                                   Default: <repo_root>/build/cli/fiction

  --gold-timeout <sec>                             Maps to gold --timeout
  --gold-num-vertex-expansions <n>                 Maps to gold --num_vertex_expansions
  --gold-effort-mode <0|1|2|3>                     Maps to gold --effort_mode
  --gold-cost-objective <0|1|2|3>                  Maps to gold --cost_objective
  --gold-return-first                              Maps to gold --return_first
  --gold-planar                                    Maps to gold --planar
  --gold-multithreading                            Maps to gold --multithreading
  --gold-verbose                                   Maps to gold --verbose
  --gold-seed <n>                                  Maps to gold --seed
  --gold-straight-inverters                        Maps to gold --straight_inverters
  --gold-tiles-to-skip-between-pis <n>             Maps to gold --tiles_to_skip_between_pis
  --gold-randomize-tiles-to-skip-between-pis       Maps to gold --randomize_tiles_to_skip_between_pis
  --gold-grid <cartesian|hex>                      Maps to gold --grid

  -h, --help                                       Show this help.
EOF
}

die() {
    echo "[e] $*" >&2
    exit 1
}

require_value() {
    local opt_name="$1"
    local opt_value="${2:-}"
    [[ -n "$opt_value" ]] || die "missing value for ${opt_name}"
}

sanitize_name() {
    local value="$1"
    value="${value// /_}"
    value="${value//[^A-Za-z0-9._-]/_}"
    echo "$value"
}

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
FICTION_BIN="${SCRIPT_DIR}/build/cli/fiction"

BENCHMARK_PATH=""
EXP_NAME=""
MAP_CSV="all2"

GOLD_TIMEOUT=""
GOLD_NUM_VERTEX_EXPANSIONS=""
GOLD_EFFORT_MODE=""
GOLD_COST_OBJECTIVE=""
GOLD_SEED=""
GOLD_TILES_TO_SKIP=""
GOLD_GRID="cartesian"
GOLD_GRID_SET=0

GOLD_RETURN_FIRST=0
GOLD_PLANAR=0
GOLD_MULTITHREADING=0
GOLD_VERBOSE=0
GOLD_STRAIGHT_INVERTERS=0
GOLD_RANDOMIZE_TILES_TO_SKIP=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --exp-name)
            require_value "$1" "${2:-}"
            EXP_NAME="$2"
            shift 2
            ;;
        --map)
            require_value "$1" "${2:-}"
            MAP_CSV="$2"
            shift 2
            ;;
        --fiction-bin)
            require_value "$1" "${2:-}"
            FICTION_BIN="$2"
            shift 2
            ;;
        --gold-timeout)
            require_value "$1" "${2:-}"
            GOLD_TIMEOUT="$2"
            shift 2
            ;;
        --gold-num-vertex-expansions)
            require_value "$1" "${2:-}"
            GOLD_NUM_VERTEX_EXPANSIONS="$2"
            shift 2
            ;;
        --gold-effort-mode)
            require_value "$1" "${2:-}"
            GOLD_EFFORT_MODE="$2"
            shift 2
            ;;
        --gold-cost-objective)
            require_value "$1" "${2:-}"
            GOLD_COST_OBJECTIVE="$2"
            shift 2
            ;;
        --gold-seed)
            require_value "$1" "${2:-}"
            GOLD_SEED="$2"
            shift 2
            ;;
        --gold-tiles-to-skip-between-pis)
            require_value "$1" "${2:-}"
            GOLD_TILES_TO_SKIP="$2"
            shift 2
            ;;
        --gold-grid)
            require_value "$1" "${2:-}"
            GOLD_GRID="$2"
            GOLD_GRID_SET=1
            shift 2
            ;;
        --gold-return-first)
            GOLD_RETURN_FIRST=1
            shift
            ;;
        --gold-planar)
            GOLD_PLANAR=1
            shift
            ;;
        --gold-multithreading)
            GOLD_MULTITHREADING=1
            shift
            ;;
        --gold-verbose)
            GOLD_VERBOSE=1
            shift
            ;;
        --gold-straight-inverters)
            GOLD_STRAIGHT_INVERTERS=1
            shift
            ;;
        --gold-randomize-tiles-to-skip-between-pis)
            GOLD_RANDOMIZE_TILES_TO_SKIP=1
            shift
            ;;
        --gold-*)
            die "unsupported gold option: $1"
            ;;
        -*)
            die "unsupported option: $1"
            ;;
        *)
            if [[ -z "$BENCHMARK_PATH" ]]; then
                BENCHMARK_PATH="$1"
                shift
            else
                die "unexpected positional argument: $1"
            fi
            ;;
    esac
done

[[ -n "$BENCHMARK_PATH" ]] || {
    usage
    die "missing benchmark file path"
}

[[ -x "$FICTION_BIN" ]] || die "fiction binary is not executable: ${FICTION_BIN}"
[[ "$GOLD_GRID" == "cartesian" || "$GOLD_GRID" == "hex" ]] || die "--gold-grid must be 'cartesian' or 'hex'"

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
EXP_NAME_CLEAN="$(sanitize_name "$EXP_NAME")"
if [[ -n "$EXP_NAME_CLEAN" ]]; then
    EXP_NAME_FULL="${TIMESTAMP}-${EXP_NAME_CLEAN}"
else
    EXP_NAME_FULL="${TIMESTAMP}"
fi

OUT_DIR="${PWD}/2i2o-exp-out/${EXP_NAME_FULL}"
mkdir -p "$OUT_DIR"

GOLD_BASENAME="${EXP_NAME_FULL}_gold_${GOLD_GRID}"
GOLD_FGL="${OUT_DIR}/${GOLD_BASENAME}.fgl"
GOLD_DOT="${OUT_DIR}/${GOLD_BASENAME}.dot"

HEX_BASENAME="${EXP_NAME_FULL}_gold_${GOLD_GRID}_hex"
HEX_FGL="${OUT_DIR}/${HEX_BASENAME}.fgl"
HEX_DOT="${OUT_DIR}/${HEX_BASENAME}.dot"

FICTION_SCRIPT="${OUT_DIR}/${EXP_NAME_FULL}.fs"
RUN_LOG="${OUT_DIR}/${EXP_NAME_FULL}.log"

MAP_ARGS=()
IFS=',' read -r -a MAP_TOKENS <<< "$MAP_CSV"
for raw_token in "${MAP_TOKENS[@]}"; do
    token="${raw_token//[[:space:]]/}"
    token="${token#--}"
    [[ -z "$token" ]] && continue
    MAP_ARGS+=("--${token}")
done
[[ ${#MAP_ARGS[@]} -gt 0 ]] || die "--map did not produce any valid mapping flags"

MAP_CMD="map"
for arg in "${MAP_ARGS[@]}"; do
    MAP_CMD+=" ${arg}"
done
MAP_CMD+=" -v"

GOLD_ARGS=()
[[ -n "$GOLD_TIMEOUT" ]] && GOLD_ARGS+=("--timeout ${GOLD_TIMEOUT}")
[[ -n "$GOLD_NUM_VERTEX_EXPANSIONS" ]] && GOLD_ARGS+=("--num_vertex_expansions ${GOLD_NUM_VERTEX_EXPANSIONS}")
[[ -n "$GOLD_EFFORT_MODE" ]] && GOLD_ARGS+=("--effort_mode ${GOLD_EFFORT_MODE}")
[[ -n "$GOLD_COST_OBJECTIVE" ]] && GOLD_ARGS+=("--cost_objective ${GOLD_COST_OBJECTIVE}")
[[ $GOLD_RETURN_FIRST -eq 1 ]] && GOLD_ARGS+=("--return_first")
[[ $GOLD_PLANAR -eq 1 ]] && GOLD_ARGS+=("--planar")
[[ $GOLD_MULTITHREADING -eq 1 ]] && GOLD_ARGS+=("--multithreading")
[[ $GOLD_VERBOSE -eq 1 ]] && GOLD_ARGS+=("--verbose")
[[ -n "$GOLD_SEED" ]] && GOLD_ARGS+=("--seed ${GOLD_SEED}")
[[ $GOLD_STRAIGHT_INVERTERS -eq 1 ]] && GOLD_ARGS+=("--straight_inverters")
[[ -n "$GOLD_TILES_TO_SKIP" ]] && GOLD_ARGS+=("--tiles_to_skip_between_pis ${GOLD_TILES_TO_SKIP}")
[[ $GOLD_RANDOMIZE_TILES_TO_SKIP -eq 1 ]] && GOLD_ARGS+=("--randomize_tiles_to_skip_between_pis")
[[ $GOLD_GRID_SET -eq 1 ]] && GOLD_ARGS+=("--grid ${GOLD_GRID}")

GOLD_CMD="gold"
if [[ ${#GOLD_ARGS[@]} -gt 0 ]]; then
    GOLD_CMD+=" ${GOLD_ARGS[*]}"
fi

{
    echo "read -a ${BENCHMARK_PATH}"
    echo "ps -n"
    echo "${MAP_CMD}"
    echo "ps -n"
    echo "equiv -n"
    echo "${GOLD_CMD}"
    echo "ps -g"
    echo "check"
    echo "equiv -n -g"
    echo "fgl ${GOLD_FGL}"
    echo "show -g --silent --filename ${GOLD_DOT}"

    if [[ "$GOLD_GRID" == "cartesian" ]]; then
        echo "hex -io"
        echo "ps -g"
        echo "check"
        echo "equiv -n -g"
        echo "equiv -g"
        echo "fgl ${HEX_FGL}"
        echo "show -g --silent --filename ${HEX_DOT}"
    fi

    echo "quit"
} > "$FICTION_SCRIPT"

echo "[i] Experiment name : ${EXP_NAME_FULL}"
echo "[i] Output directory: ${OUT_DIR}"
echo "[i] Fiction script  : ${FICTION_SCRIPT}"
echo "[i] Running fiction..."

"$FICTION_BIN" -f "$FICTION_SCRIPT" | tee "$RUN_LOG"

if grep -q "^\[e\]" "$RUN_LOG"; then
    die "fiction reported one or more errors; see ${RUN_LOG}"
fi

EXPECTED_FILES=("$GOLD_FGL" "$GOLD_DOT")
if [[ "$GOLD_GRID" == "cartesian" ]]; then
    EXPECTED_FILES+=("$HEX_FGL" "$HEX_DOT")
fi

for file in "${EXPECTED_FILES[@]}"; do
    [[ -s "$file" ]] || die "missing expected output file: ${file}"
done

if command -v dot >/dev/null 2>&1; then
    DOT_FILES=("$GOLD_DOT")
    if [[ "$GOLD_GRID" == "cartesian" ]]; then
        DOT_FILES+=("$HEX_DOT")
    fi

    for dot_file in "${DOT_FILES[@]}"; do
        dot -Tsvg "$dot_file" -o "${dot_file%.dot}.svg"
        dot -Tpng "$dot_file" -o "${dot_file%.dot}.png"
    done
else
    echo "[w] graphviz 'dot' not found; skipping SVG/PNG generation"
fi

echo "[i] Completed successfully."
echo "[i] Log: ${RUN_LOG}"
