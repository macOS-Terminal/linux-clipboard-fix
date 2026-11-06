#!/usr/bin/env bash
# cbfix-bridge integration test suite
set -u
BRIDGE=${BRIDGE:-./build/cbfix-bridge}
PASS=0; FAIL=0
ok(){ printf '  \033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
no(){ printf '  \033[31mFAIL\033[0m %s\n' "$1"; printf '       expected: %s\n       actual  : %s\n' "${2:-}" "${3:-}"; FAIL=$((FAIL+1)); }
chk(){ if [ "$2" = "$3" ]; then ok "$1"; else no "$1" "$2" "$3"; fi; }

command -v wl-copy >/dev/null || { echo "wl-copy (wl-clipboard) is required"; exit 1; }
command -v javac >/dev/null || { echo "javac is required for the Java tests"; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# --- helpers -------------------------------------------------------
cat > "$TMP/X11Read.java" <<'EOF'
import java.awt.*;
import java.awt.datatransfer.*;
public class X11Read {
  public static void main(String[] a) {
    try {
      Clipboard c = Toolkit.getDefaultToolkit().getSystemClipboard();
      System.out.print(c.isDataFlavorAvailable(DataFlavor.stringFlavor)
        ? String.valueOf(c.getData(DataFlavor.stringFlavor)) : "");
    } catch (Throwable t) { System.out.print("ERR"); }
    System.exit(0);
  }
}
EOF
cat > "$TMP/X11Write.java" <<'EOF'
import java.awt.*;
import java.awt.datatransfer.*;
public class X11Write {
  public static void main(String[] a) throws Exception {
    Toolkit.getDefaultToolkit().getSystemClipboard()
      .setContents(new StringSelection(a[0]), null);
    Thread.sleep(1200); System.exit(0);
  }
}
EOF
javac -d "$TMP" "$TMP/X11Read.java" "$TMP/X11Write.java" 2>/dev/null

pkill -x cbfix-bridge 2>/dev/null; sleep 0.3
DISPLAY=${DISPLAY:-:0} setsid "$BRIDGE" > "$TMP/bridge.log" 2>&1 &
sleep 1
pgrep -x cbfix-bridge >/dev/null || { echo "bridge failed to start"; cat "$TMP/bridge.log"; exit 1; }

echo "== X11 app (Java/AWT, the HMCL toolkit) -> Wayland =="
V="java-to-system-$RANDOM"
DISPLAY=${DISPLAY:-:0} java -cp "$TMP" X11Write "$V" >/dev/null 2>&1
sleep 0.6; chk "text survives to wl-paste" "$V" "$(wl-paste --no-newline 2>&1)"

echo "== Wayland -> X11 app (Java/AWT) =="
V="system-to-java-$RANDOM"; wl-copy "$V"; sleep 0.7
chk "text survives to AWT" "$V" "$(DISPLAY=${DISPLAY:-:0} java -cp "$TMP" X11Read 2>&1)"

echo "== Unicode / CJK / emoji =="
U='中文测试 🎉 Ünïcode ✅ 日本語テスト “quotes” ≠'
wl-copy "$U"; sleep 0.7
chk "unicode round-trips" "$U" "$(DISPLAY=${DISPLAY:-:0} java -cp "$TMP" X11Read 2>&1)"

echo "== multi-line text =="
M=$'line1\nline2\ttabbed\nline3'
wl-copy "$M"; sleep 0.7
chk "newlines and tabs preserved" "$M" "$(DISPLAY=${DISPLAY:-:0} java -cp "$TMP" X11Read 2>&1)"

echo "== empty clipboard =="
printf '' | wl-copy; sleep 0.6
printf '' | DISPLAY=${DISPLAY:-:0} java -cp "$TMP" X11Write "" >/dev/null 2>&1
sleep 0.4; ok "empty payload does not crash the bridge"
pgrep -x cbfix-bridge >/dev/null && ok "bridge still alive" || no "bridge still alive" "alive" "dead"

echo "== rapid alternation (10x each way) =="
cd=0
for i in $(seq 1 10); do
  wl-copy "alt-wl-$i" >/dev/null; sleep 0.25
  grep -q "alt-wl-$i" <(DISPLAY=${DISPLAY:-:0} java -cp "$TMP" X11Read 2>&1) && cd=$((cd+1))
  DISPLAY=${DISPLAY:-:0} java -cp "$TMP" X11Write "alt-x11-$i" >/dev/null 2>&1; sleep 0.25
  grep -q "alt-x11-$i" <(wl-paste --no-newline 2>&1) && cd=$((cd+1))
done
chk "20/20 alternations correct" "20" "$cd"

echo "== no echo loop: one copy -> exactly one bridge action =="
before=$(grep -c "X11 -> Wayland" "$TMP/bridge.log" 2>/dev/null || echo 0)
DISPLAY=${DISPLAY:-:0} java -cp "$TMP" X11Write "loop-check-$RANDOM" >/dev/null 2>&1
sleep 1.2
after=$(grep -c "X11 -> Wayland" "$TMP/bridge.log" 2>/dev/null || echo 0)
chk "exactly one X11->WL push" "1" "$((after-before))"

echo "== stability: bridge still running =="
pgrep -x cbfix-bridge >/dev/null && ok "bridge alive at end" || no "bridge alive at end" "alive" "dead"

pkill -x cbfix-bridge 2>/dev/null
echo
printf 'TOTAL: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
