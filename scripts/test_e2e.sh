#!/bin/bash
# test_e2e.sh — End-to-end test harness for skey Vietnamese IME.
#
# Tests the full pipeline: key input → fcitx5 → skey → text output.
# Works by monitoring the skey debug log (/tmp/skey.log) while you type.
#
# Usage:
#   ./scripts/test_e2e.sh              # Interactive test mode
#   ./scripts/test_e2e.sh --auto       # Automated tests (uinput mode)
#   ./scripts/test_e2e.sh --mode preedit  # Test in preedit mode
#   ./scripts/test_e2e.sh --rapid      # Rapid typing stress test

set -euo pipefail

# ── Colors ─────────────────────────────────────────────────────────────────
RED='\033[31m'; GREEN='\033[32m'; YELLOW='\033[33m'
CYAN='\033[36m'; MAGENTA='\033[35m'; BOLD='\033[1m'; NC='\033[0m'

PASS="${GREEN}PASS${NC}"
FAIL="${RED}FAIL${NC}"
SKIP="${YELLOW}SKIP${NC}"

# ── Config ─────────────────────────────────────────────────────────────────
OUTPUT_MODE="${SKEY_MODE:-Uinput}"
INPUT_METHOD="${SKEY_IM:-Telex}"
DEBUG_LOG="/tmp/skey.log"
SKEY_CONF="$HOME/.config/fcitx5/conf/skey.conf"
PASSED=0
FAILED=0
SKIPPED=0

# ── Helpers ────────────────────────────────────────────────────────────────

die() { echo -e "${RED}ERROR: $*${NC}" >&2; exit 1; }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
ok() { echo -e "  ${GREEN}✓${NC} $*"; PASSED=$((PASSED+1)); }
fail() { echo -e "  ${RED}✗${NC} $*"; FAILED=$((FAILED+1)); }
skip() { echo -e "  ${YELLOW}○${NC} $* (SKIPPED)"; SKIPPED=$((SKIPPED+1)); }

check_deps() {
    for cmd in fcitx5-remote busctl; do
        command -v "$cmd" &>/dev/null || die "$cmd is required but not installed"
    done
}

# ── fcitx5 interaction ─────────────────────────────────────────────────────

fcitx5_im() {
    # Get current IM
    busctl --user get-property org.fcitx.Fcitx5 /controller \
        org.fcitx.Fcitx5.Controller1 CurrentInputMethod 2>/dev/null | grep -oP '"([^"]*)"' | tr -d '"'
}

ensure_skey() {
    local current
    current=$(fcitx5_im)
    if [[ "$current" != "skey" ]]; then
        info "Switching IM: $current → skey"
        busctl --user call org.fcitx.Fcitx5 /controller \
            org.fcitx.Fcitx5.Controller1 SetCurrentIM s "skey" 2>/dev/null || true
        sleep 0.5
        current=$(fcitx5_im)
        if [[ "$current" != "skey" ]]; then
            die "Cannot switch to skey. Current: $current"
        fi
    fi
    ok "skey is active"
}

write_skey_config() {
    local mode="${1:-Uinput}"
    local im="${2:-Telex}"
    mkdir -p "$(dirname "$SKEY_CONF")"
    cat > "$SKEY_CONF" <<EOF
[SKeyConfig]
InputMethod=${im}
OutputMode=${mode}
TonePosition=Modern
FreeMarking=True
AutoRestore=True
ShowPreedit=True
Debug=True
EOF
    info "Config: OutputMode=${mode} InputMethod=${im} Debug=True"
    # Reload fcitx5
    fcitx5-remote -r 2>/dev/null || true
    sleep 0.8
}

clear_debug_log() {
    : > "$DEBUG_LOG" 2>/dev/null || true
}

watch_log_tail() {
    # Return the last N non-empty lines from the debug log
    local n="${1:-20}"
    tail -n "$n" "$DEBUG_LOG" 2>/dev/null | grep -v '^$' || true
}

check_log_contains() {
    # Check if the debug log contains a pattern
    local pattern="$1"
    local context="${2:-50}"
    if tail -n "$context" "$DEBUG_LOG" 2>/dev/null | grep -q "$pattern"; then
        return 0
    fi
    return 1
}

# ── Test cases ─────────────────────────────────────────────────────────────

# Each test case: "name" "keys" "expected_output" "mode_to_use"
# mode_to_use: uinput, surrounding, preedit, any

declare -A TEST_NAMES TEST_KEYS TEST_EXPECTED TEST_MODES

register_test() {
    local idx="$1"; shift
    TEST_NAMES[$idx]="$1"
    TEST_KEYS[$idx]="$2"
    TEST_EXPECTED[$idx]="$3"
    TEST_MODES[$idx]="${4:-any}"
}

# Telex — high frequency words
register_test 1  "xin chào (Telex)"       "xin"        "xin"     "any"
register_test 2  "chào (Telex)"           "chafo"      "chào"    "any"
register_test 3  "tiếng (Telex)"          "tieengs"    "tiếng"   "any"
register_test 4  "việt (Telex)"           "vieetj"     "việt"    "any"
register_test 5  "được (Telex)"           "dduwowcj"   "được"    "any"
register_test 6  "không (Telex)"          "khoong"     "không"   "any"
register_test 7  "người (Telex)"          "nguwowif"   "người"   "any"
register_test 8  "những (Telex)"          "nhuwngx"    "những"   "any"
register_test 9  "trường (Telex)"         "truwowngf"  "trường"  "any"
register_test 10 "đường (Telex)"          "dduwowngf"  "đường"   "any"
register_test 11 "các (Telex)"            "cacs"       "các"     "any"

# Telex — edge cases
register_test 12 "vãi (undo test)"        "vaix"       "vãi"     "any"
register_test 13 "undo ô (ooo)"           "ooo"        "oo"      "any"
register_test 14 "undo đ (ddd)"           "ddd"        "dd"      "any"
register_test 15 "đc abbrev"              "ddc"        "đc"      "any"
register_test 16 "English hello"          "hello"      "hello"   "any"

# Telex — tones
register_test 20 "á (a+s)"                "as"         "á"       "any"
register_test 21 "à (a+f)"                "af"         "à"       "any"
register_test 22 "ả (a+r)"                "ar"         "ả"       "any"
register_test 23 "ã (a+x)"                "ax"         "ã"       "any"
register_test 24 "ạ (a+j)"                "aj"         "ạ"       "any"

# Telex — compound vowels
register_test 30 "â (aa)"                 "aa"         "â"       "any"
register_test 31 "ê (ee)"                 "ee"         "ê"       "any"
register_test 32 "ô (oo)"                 "oo"         "ô"       "any"
register_test 33 "ă (aw)"                 "aw"         "ă"       "any"
register_test 34 "ơ (ow)"                 "ow"         "ơ"       "any"
register_test 35 "ư (uw)"                 "uw"         "ư"       "any"
register_test 36 "đ (dd)"                 "dd"         "đ"       "any"

# VNI — basic
register_test 40 "á (VNI a1)"             "a1"         "á"       "any"
register_test 41 "à (VNI a2)"             "a2"         "à"       "any"
register_test 42 "â (VNI a6)"             "a6"         "â"       "any"
register_test 43 "ă (VNI a8)"             "a8"         "ă"       "any"
register_test 44 "đ (VNI d9)"             "d9"         "đ"       "any"
register_test 45 "tiếng (VNI)"            "tie6ng1"    "tiếng"   "any"

# Edge cases — auto-restore
register_test 50 "wood→wood (restore)"    "wood"       "wood"    "any"
register_test 51 "wtf→wtf (restore)"      "wtf"        "wtf"     "any"

# ── Test execution ─────────────────────────────────────────────────────────

run_manual_test() {
    # Single interactive test — shows what to type and verifies result
    local name="$1"
    local keys="$2"
    local expected="$3"

    echo ""
    echo -e "${BOLD}── Test: ${name} ──${NC}"
    echo -e "  Keys to type: ${YELLOW}${keys}${NC} (then press Space)"
    echo -e "  Expected output: ${GREEN}${expected}${NC}"
    echo ""
    read -r -p "  Press Enter when ready (q to quit)... " response
    if [[ "$response" == "q" ]]; then
        return 1
    fi

    clear_debug_log
    echo "  [NOW TYPE: $keys followed by a SPACE]"

    # Wait for commit to appear in log
    local waited=0
    while [[ $waited -lt 8 ]]; do
        if check_log_contains "Commit:" 5; then
            local commit
            commit=$(grep "Commit:" "$DEBUG_LOG" 2>/dev/null | tail -1 | grep -oP "'([^']*)'" | tr -d "'")
            if [[ "$commit" == "$expected" ]]; then
                ok "$name — got '$commit'"
            elif [[ -n "$commit" ]]; then
                fail "$name — expected '$expected', got '$commit'"
            fi
            return 0
        fi
        sleep 0.5
        ((waited++))
    done
    fail "$name — no commit detected in 4s"
    return 0
}

run_log_test() {
    # Test by analyzing the debug log
    local name="$1"
    local keys="$2"
    local expected="$3"

    # For log-based tests, we check if the engine transforms correctly
    # by looking for Key-related entries
    local last_key_entry
    last_key_entry=$(grep "Key '" "$DEBUG_LOG" 2>/dev/null | tail -5 || true)

    if [[ -z "$last_key_entry" ]]; then
        skip "$name — no key events in log"
        return
    fi

    # Check if any 'new=' entry matches expected
    if grep -q "new='${expected}'" "$DEBUG_LOG" 2>/dev/null; then
        ok "$name — composed '$expected' detected in log"
    else
        fail "$name — '$expected' not found in log"
    fi
}

# ── Automated test mode ────────────────────────────────────────────────────

run_auto_tests() {
    echo ""
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Automated Log-Based Tests${NC}"
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "  ${YELLOW}⚠${NC}  Automated mode analyzes existing debug log entries."
    echo "  Results depend on recent typing activity."
    echo ""

    for idx in $(printf '%s\n' "${!TEST_NAMES[@]}" | sort -n); do
        local name="${TEST_NAMES[$idx]}"
        local keys="${TEST_KEYS[$idx]}"
        local expected="${TEST_EXPECTED[$idx]}"
        run_log_test "$name" "$keys" "$expected"
    done
}

# ── Interactive mode ───────────────────────────────────────────────────────

run_interactive_tests() {
    echo ""
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Interactive Test Mode${NC}"
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "  For each test, type the shown keys followed by ${BOLD}SPACE${NC}."
    echo -e "  The script will verify the committed text from the debug log."
    echo ""

    for idx in $(printf '%s\n' "${!TEST_NAMES[@]}" | sort -n); do
        local name="${TEST_NAMES[$idx]}"
        local keys="${TEST_KEYS[$idx]}"
        local expected="${TEST_EXPECTED[$idx]}"
        run_manual_test "$name" "$keys" "$expected" || break
    done
}

# ── Mode cycling tests ─────────────────────────────────────────────────────

test_all_modes() {
    echo ""
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Multi-Mode Test${NC}"
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"

    local test_keys="tieengs"  # Should produce "tiếng" in any mode
    local modes=("Uinput" "SurroundingText" "Preedit")

    for mode in "${modes[@]}"; do
        echo ""
        echo -e "${BOLD}── Mode: ${mode} ──${NC}"
        write_skey_config "$mode" "Telex"
        ensure_skey

        clear_debug_log
        echo -e "  ${YELLOW}Type: tieengs then SPACE${NC}"
        read -r -p "  Press Enter when done... "

        if check_log_contains "tiếng" 20; then
            ok "Mode $mode: 'tiếng' produced correctly"
        else
            local last_commit
            last_commit=$(grep "Commit:" "$DEBUG_LOG" 2>/dev/null | tail -1 || echo "NONE")
            fail "Mode $mode: expected 'tiếng', last commit: $last_commit"
        fi
    done
}

# ── Rapid typing stress test ────────────────────────────────────────────────

test_rapid() {
    echo ""
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Rapid Typing Stress Test${NC}"
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "  ${YELLOW}Type the following phrases as FAST as you can:${NC}"
    echo ""

    local phrases=(
        "tieengs Vieetj|tiếng Việt"
        "dduwowcj khoong|được không"
        "xin chafo|xin chào"
        "cacs banj|cc ban"  # Edge: rapid typing may cause issues
        "vaix|vãi"
        "nhuwngx gif|những gì"
    )

    for phrase_spec in "${phrases[@]}"; do
        local expected="${phrase_spec#*|}"
        local keys="${phrase_spec%|*}"
        echo -e "  ${BOLD}Type:${NC} ${YELLOW}${keys}${NC}"
        echo -e "  ${BOLD}Expect:${NC} ${GREEN}${expected}${NC}"

        clear_debug_log
        read -r -p "  Press Enter after typing... "

        if check_log_contains "${expected}" 20; then
            ok "'${expected}' — correct"
        else
            fail "'${expected}' — check output"
        fi
        echo ""
    done
}

# ── Mode-specific behavior tests ────────────────────────────────────────────

test_app_types() {
    echo ""
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  App Type Test${NC}"
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "  Open these apps and type 'tieengs' in each:"
    echo ""
    echo -e "  1. ${BOLD}Terminal${NC} (tabby/konsole/gnome-terminal):"
    echo "     Expected mode: Uinput"
    echo ""
    echo -e "  2. ${BOLD}GTK Editor${NC} (gedit):"
    echo "     Expected mode: SurroundingText"
    echo ""
    echo -e "  3. ${BOLD}Electron App${NC} (VS Code/discord/slack):"
    echo "     Expected mode: Uinput (no SurroundingText)"
    echo ""
    echo -e "  4. ${BOLD}Browser${NC} (Firefox/Chrome):"
    echo "     Expected mode: SurroundingText"
    echo ""

    # Analyze which apps and modes are active
    if [[ -f "$DEBUG_LOG" ]]; then
        echo -e "  ${BOLD}Recent app activations from log:${NC}"
        grep "app=" "$DEBUG_LOG" 2>/dev/null | tail -10 | while read -r line; do
            local app mode
            app=$(echo "$line" | grep -oP 'app=\S+' | cut -d= -f2)
            mode=$(echo "$line" | grep -oP 'mode=\S+' | cut -d= -f2)
            echo -e "    app=${GREEN}${app}${NC} mode=${YELLOW}${mode}${NC}"
        done || echo "    (no recent activity)"
    fi
}

# ── Report ──────────────────────────────────────────────────────────────────

report() {
    local total=$((PASSED + FAILED + SKIPPED))
    echo ""
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    if [[ $FAILED -eq 0 && $SKIPPED -eq 0 ]]; then
        echo -e "  ${GREEN}ALL ${total} TESTS PASSED ✓${NC}"
    elif [[ $FAILED -eq 0 ]]; then
        echo -e "  ${GREEN}${PASSED} passed${NC}, ${YELLOW}${SKIPPED} skipped${NC}"
    else
        echo -e "  ${GREEN}${PASSED} passed${NC}, ${RED}${FAILED} FAILED${NC}, ${YELLOW}${SKIPPED} skipped${NC}"
    fi
    echo -e "${CYAN}═════════════════════════════════════════════════${NC}"
    return $FAILED
}

# ── Main ────────────────────────────────────────────────────────────────────

main() {
    local mode="${1:-interactive}"

    echo -e "${MAGENTA}"
    echo "╔══════════════════════════════════════════════════════╗"
    echo "║   skey E2E Test Harness — Real-World IME Testing     ║"
    echo "╚══════════════════════════════════════════════════════╝"
    echo -e "${NC}"

    check_deps

    # Parse mode from args
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --auto)    mode="auto"; shift ;;
            --interactive) mode="interactive"; shift ;;
            --rapid)   mode="rapid"; shift ;;
            --app-types) mode="app-types"; shift ;;
            --all-modes) mode="all-modes"; shift ;;
            --mode)
                OUTPUT_MODE="$2"
                write_skey_config "$OUTPUT_MODE" "$INPUT_METHOD"
                shift 2 ;;
            --im)
                INPUT_METHOD="$2"
                write_skey_config "$OUTPUT_MODE" "$INPUT_METHOD"
                shift 2 ;;
            *) shift ;;
        esac
    done

    ensure_skey

    # Check debug log
    if [[ ! -f "$DEBUG_LOG" ]]; then
        echo -e "  ${YELLOW}[WARN]${NC} No debug log at $DEBUG_LOG"
        echo "  Enable Debug=True in ~/.config/fcitx5/conf/skey.conf"
    else
        ok "Debug log found: $DEBUG_LOG"
    fi

    case "$mode" in
        auto)         run_auto_tests ;;
        interactive)  run_interactive_tests ;;
        rapid)        test_rapid ;;
        app-types)    test_app_types ;;
        all-modes)    test_all_modes ;;
        *)            run_interactive_tests ;;
    esac

    report
}

main "$@"
