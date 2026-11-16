# shellcheck shell=bash
# Runs on the macOS node, sent over ssh by scripts/regress/clean.sh with
# REGRESS_REPOS (the node checkouts) and REGRESS_TREES (the warm build trees),
# one per line, relative to HOME or absolute, in the environment. Removes what
# the regression runner left there and nothing else: run folders under
# ~/.cache/dusk-regress whose process group is gone, the regress-* branches it
# pushed that no worktree has checked out, and package run folders under
# ~/dusk-regress-pkg. The build trees stay. It never signals a process: a pid
# in a file weeks old may since have become anyone's, the maintainer's own
# Dusk Studio included, so anything still alive leaves its folder in place.
# The node's bash is 3.2, which wants a regex in a variable.
set -uo pipefail
status=0
stamp_re='^[0-9]{8}-[0-9]{6}$'
branch_re='^regress-(pkg-)?[0-9a-f]{7,40}$'

group_alive() {
    ps -ax -o pgid=,stat= | awk -v group="$1" '$1 == group && $2 !~ /^Z/ { found = 1 } END { exit !found }'
}
size_kb() {
    du -sk "$1" 2>/dev/null | awk '{ print $1 }'
}
gb() {
    awk -v k="${1:-0}" 'BEGIN { printf "%.1f GB", k / 1048576 }'
}
remove() {
    local what="$1" path="$2" kb
    kb="$(size_kb "$path")"
    if rm -rf "$path" && [[ ! -e "$path" ]]; then
        echo "removed $what $path ($(gb "$kb"))"
    else
        echo "error: could not remove $what $path"
        status=1
    fi
}

base="$HOME/.cache/dusk-regress"
for run in "$base"/regress-*; do
    [[ -d "$run" ]] || continue
    token="${run##*/}"
    pid=""
    [[ -s "$run/pid" ]] && read -r pid <"$run/pid"
    # A pid file weeks old may name a process that is someone else's by now:
    # its group keeps the folder while its leader still runs this run's own
    # command, or has gone while what it started runs on. No new process takes
    # the pid of a group that still has members.
    if [[ "$pid" =~ ^[0-9]+$ ]] && group_alive "$pid"; then
        command="$(ps -ww -o command= -p "$pid" 2>/dev/null)"
        if [[ -z "$command" ]]; then
            echo "warn: left alone, its process group $pid still runs, with its leader gone: $run"
            continue
        elif [[ "$command" == *" dusk-regress $token" ]]; then
            echo "warn: left alone, its run is still going (process group $pid): $run"
            continue
        fi
    fi
    remove "the run folder" "$run"
done

# A source run's claim on a checkout names its run folder; with the folder gone
# the claim is a leftover. The lock around claims is held for a moment only, and
# a run lets go of it with the process that took it, so a lock file no one
# holds is a leftover too; it goes while this holds it.
for claim in "$base"/source-claim-*; do
    [[ -f "$claim" ]] || continue
    holder=""
    read -r holder <"$claim" || true
    if [[ -n "$holder" && -d "$base/$holder" ]]; then
        echo "warn: left alone, the claim $claim of $holder, whose run folder is still there"
    else
        remove "the source claim" "$claim"
    fi
done
lock="$base/source-claim.lock"
if [[ -f "$lock" ]]; then
    if (exec 9>>"$lock" && perl -MFcntl=:flock -e 'open(my $fh, ">&=", 9) or exit 2; exit(flock($fh, LOCK_EX | LOCK_NB) ? 0 : 1)' \
        && rm -f "$lock"); then
        echo "removed the claim lock $lock"
    else
        echo "warn: left alone, the claim lock $lock, which a run holds"
    fi
fi

pkg="$HOME/dusk-regress-pkg"
for run in "$pkg"/*; do
    [[ -d "$run" ]] || continue
    name="${run##*/}"
    if [[ ! -f "$run/.dusk-regress-run" && ! "$name" =~ $stamp_re ]]; then
        echo "warn: left alone, not a package run folder: $run"
        continue
    fi
    live=""
    cpid=""
    [[ -f "$run/caffeinate.pid" ]] && read -r cpid <"$run/caffeinate.pid"
    if [[ "$cpid" =~ ^[0-9]+$ && "$(ps -p "$cpid" -o comm= 2>/dev/null)" == *caffeinate ]]; then
        live="its caffeinate (pid $cpid) still runs"
    fi
    if [[ -z "$live" && -f "$run/pids" ]]; then
        while read -r apid; do
            if [[ "$apid" =~ ^[0-9]+$ ]] && ps -p "$apid" >/dev/null 2>&1; then
                live="pid $apid, which it started, is alive ($(ps -p "$apid" -o comm= 2>/dev/null))"
                break
            fi
        done <"$run/pids"
    fi
    if [[ -z "$live" ]] && mount | grep -qF " on $run/mnt "; then
        live="its disk image is still mounted at $run/mnt"
    fi
    if [[ -n "$live" ]]; then
        echo "error: left alone, $live: $run"
        status=1
        continue
    fi
    remove "the package run folder" "$run"
done
[[ -d "$pkg" ]] && rmdir "$pkg" 2>/dev/null

while IFS= read -r repo; do
    [[ -n "$repo" ]] || continue
    [[ "$repo" == /* ]] || repo="$HOME/$repo"
    [[ -e "$repo/.git" ]] || continue
    worktrees="$(git -C "$repo" worktree list --porcelain | awk '
        /^worktree / { path = substr($0, 10) }
        /^branch refs\/heads\// { print substr($0, 19) "\t" path }')"
    while IFS= read -r branch; do
        [[ -n "$branch" ]] || continue
        where="$(awk -F '\t' -v b="$branch" '$1 == b { print $2 }' <<<"$worktrees")"
        if [[ ! "$branch" =~ $branch_re ]]; then
            echo "left alone, not a branch name the runner gives: $branch in $repo"
        elif [[ -n "$where" ]]; then
            echo "warn: $where is checked out on $branch, which a run never put back; check out the branch you want there, then clean again"
        elif git -C "$repo" branch -q -D "$branch"; then
            echo "removed the branch $branch from $repo"
        else
            echo "error: could not delete the branch $branch from $repo"
            status=1
        fi
    done < <(git -C "$repo" for-each-ref --format='%(refname:short)' 'refs/heads/regress-*')
done <<<"$REGRESS_REPOS"
while IFS= read -r tree; do
    [[ -n "$tree" ]] || continue
    [[ "$tree" == /* ]] || tree="$HOME/$tree"
    [[ -d "$tree" ]] && echo "regress:tree $(size_kb "$tree") $tree"
done <<<"$REGRESS_TREES"
echo "regress:free $(df -k "$HOME" | awk 'NR == 2 { print $4 }')"
exit "$status"
