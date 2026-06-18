#!/usr/bin/env bash
#
# doc-lint.sh — documentation anti-drift guard for the qbm-pgsql module.
#
# Fails on: retired tokens in docs, broken internal Markdown links, missing module
# governance files. Warns on: pages missing a "Verified-against" marker.
# Module-wide policy (versioning, support) lives in the qb framework; this guard
# checks only qbm-pgsql's own Markdown (README.md, readme/**, CHANGELOG/SECURITY/CONTRIBUTING).
#
# Usage:  ./scripts/doc-lint.sh   (from the qbm-pgsql root)
#
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}" || exit 2
fail=0; warn=0
red(){ printf '\033[31m%s\033[0m\n' "$1"; }; grn(){ printf '\033[32m%s\033[0m\n' "$1"; }; ylw(){ printf '\033[33m%s\033[0m\n' "$1"; }

doc_files(){ { echo "README.md"; echo "CHANGELOG.md"; echo "SECURITY.md"; echo "CONTRIBUTING.md"; find readme -name '*.md' 2>/dev/null; } | sort -u | while read -r f; do [ -f "$f" ] && echo "$f"; done; }

echo "== 1. Forbidden token scan =="
# Retired identifiers must not be USED in examples. A line is allowed to NAME them
# when it is removal-guidance ("retired/removed/no longer/do not use/...") — that
# steers readers away and is not a usage hazard.
FORBIDDEN='qb::Timestamp|qb::Duration|qb::TimePoint|to_timestamp\(|to_time_point\('
RMCTX='retired|removed|no longer|never|do not|don'\''t|must not|forbidden|gone|replaced|not the'
is_allowed(){ case "$1" in CHANGELOG.md|CONTRIBUTING.md) return 0;; *) return 1;; esac; }
hits=0
while read -r f; do
  is_allowed "$f" && continue
  bad=$(grep -nE "${FORBIDDEN}" "$f" 2>/dev/null | grep -ivE "${RMCTX}")
  if [ -n "$bad" ]; then
    printf '%s\n' "$bad" | while IFS= read -r l; do red "  ${f}: ${l}"; done
    hits=1
  fi
done < <(doc_files)
[ "$hits" -eq 0 ] && grn "  no forbidden tokens (usage)" || fail=1

echo "== 2. Internal link check =="
while read -r f; do
  dir="$(dirname "$f")"
  awk 'BEGIN{c=0} /^[[:space:]]*```/{c=!c; next} !c{print}' "$f" 2>/dev/null \
    | grep -oE '\]\([^) ]+\)' 2>/dev/null | sed -E 's/^\]\(//; s/\)$//' | while IFS= read -r t; do
    case "$t" in http://*|https://*|mailto:*|\#*) continue;; esac
    p="${t%%#*}"; [ -z "$p" ] && continue
    # Only validate things that look like a file link (contain a slash or a dot);
    # this skips inline C++ like operator[](size_type) and lambda params (auto ctx).
    case "$p" in */*|*.*) ;; *) continue;; esac
    case "$p" in /*) r="${ROOT}${p}";; *) r="${dir}/${p}";; esac
    [ ! -e "$r" ] && { red "  ${f} -> ${t} (missing)"; echo X >> /tmp/qbmpgsql-doclint-broken.$$; }
  done
done < <(doc_files)
broken=0; [ -f /tmp/qbmpgsql-doclint-broken.$$ ] && { broken=$(wc -l < /tmp/qbmpgsql-doclint-broken.$$); rm -f /tmp/qbmpgsql-doclint-broken.$$; }
[ "${broken:-0}" -eq 0 ] && grn "  all internal links resolve" || fail=1

echo "== 3. Module governance presence =="
missing=0
for g in README.md CHANGELOG.md SECURITY.md CONTRIBUTING.md LICENSE; do
  [ ! -f "$g" ] && { red "  missing: ${g}"; missing=1; }
done
[ "$missing" -eq 0 ] && grn "  module governance present" || fail=1

echo "== 4. Verified-against marker (warning) =="
nomarker=0
while read -r f; do
  case "$f" in CHANGELOG.md) continue;; esac
  grep -q 'Verified-against' "$f" 2>/dev/null || { ylw "  no Verified-against: ${f}"; nomarker=$((nomarker+1)); }
done < <(doc_files)
[ "$nomarker" -eq 0 ] && grn "  all pages carry a Verified-against marker" || warn=1

echo
if [ "$fail" -ne 0 ]; then red "doc-lint: FAILED"; exit 1; fi
[ "$warn" -ne 0 ] && ylw "doc-lint: passed with warnings" || grn "doc-lint: passed"
exit 0
