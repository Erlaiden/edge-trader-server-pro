#!/bin/bash
echo "=========================================="
echo "EDGE TRADER AI - SIGNAL SYSTEM AUDIT"
echo "=========================================="
echo ""
echo "Date: $(date)"
echo ""

# 1. Проверяем запущен ли сервер
echo "1. SERVER STATUS:"
if ps aux | grep -q "[e]dge_trader_server"; then
    echo "   ✓ Server is RUNNING"
    ps aux | grep "[e]dge_trader_server" | awk '{print "   PID:", $2, "CPU:", $3"%", "MEM:", $4"%"}'
else
    echo "   ✗ Server is NOT running"
fi
echo ""

# 2. Тестируем API
echo "2. API INFERENCE TEST:"
RESPONSE=$(curl -s "http://localhost:3000/api/infer?symbol=AIAUSDT&interval=15")
echo "   Response received: $(echo $RESPONSE | wc -c) bytes"
echo ""

# 3. Парсим ключевые поля
echo "3. SIGNAL ANALYSIS:"
echo "$RESPONSE" | python3 << 'PYTHON_EOF'
import json
import sys

try:
    data = json.load(sys.stdin)
    
    print(f"   Regime: {data.get('regime', 'N/A')}")
    print(f"   Regime Note: {data.get('regime_note', 'N/A')}")
    print(f"   Signal: {data.get('signal', 'N/A')}")
    print(f"   Confidence: {data.get('confidence', 0)}%")
    print(f"   Score15: {data.get('score15', 0)}")
    print(f"   Market Mode: {data.get('market_mode', 'N/A')}")
    print(f"   Threshold: {data.get('thr', 0)}")
    print("")
    
    print("4. HTF DATA:")
    htf = data.get('htf', {})
    for tf in ['1440', '240', '60']:
        if tf in htf:
            h = htf[tf]
            score = h.get('score')
            print(f"   {tf:>4}m: score={score}, strong={h.get('strong')}, agree={h.get('agree')}")
        else:
            print(f"   {tf:>4}m: NOT FOUND")
    print("")
    
    print("5. TP/SL LEVELS:")
    print(f"   TP: {data.get('tp', 0)*100:.2f}%")
    print(f"   SL: {data.get('sl', 0)*100:.2f}%")
    print(f"   Last Close: ${data.get('last_close', 0)}")
    print("")
    
    print("6. PROBLEM DIAGNOSIS:")
    
    # Проверка 1: Режим
    regime = data.get('regime', '')
    if regime in ['UNCERTAIN', 'MANIPULATION']:
        print(f"   ❌ PROBLEM: Regime is '{regime}' - blocks all trading")
        print(f"      → System won't generate signals in this regime")
    else:
        print(f"   ✓ Regime '{regime}' allows trading")
    
    # Проверка 2: HTF Daily
    htf_1440 = htf.get('1440', {})
    if htf_1440.get('score') is None:
        print(f"   ❌ PROBLEM: Daily (1440m) HTF score is NULL")
        print(f"      → Cannot determine market trend direction")
        print(f"      → Defaults to UNCERTAIN regime")
    else:
        print(f"   ✓ Daily HTF data exists")
    
    # Проверка 3: Confidence
    conf = data.get('confidence', 0)
    if conf == 0:
        print(f"   ❌ PROBLEM: Confidence is 0%")
        print(f"      → All signals are blocked")
    else:
        print(f"   ✓ Confidence: {conf}%")
    
    # Проверка 4: Min confidence порог
    regime_params = {
        'STRONG_UPTREND': 60.0,
        'STRONG_DOWNTREND': 60.0,
        'RANGE_BOUND': 60.0,
        'CORRECTION_UP': 65.0,
        'CORRECTION_DOWN': 65.0,
        'BREAKOUT_UP': 60.0,
        'BREAKOUT_DOWN': 60.0,
        'UNCERTAIN': 100.0,
        'MANIPULATION': 100.0
    }
    
    min_conf = regime_params.get(regime, 100.0)
    if conf < min_conf:
        print(f"   ⚠️  WARNING: Confidence {conf}% < Required {min_conf}%")
        print(f"      → Signal would be rejected by robot")
    
    print("")
    print("7. ROOT CAUSE:")
    if htf_1440.get('score') is None:
        print("   🔥 MAIN ISSUE: Daily HTF data not loading properly")
        print("      Possible causes:")
        print("      1. utils_data.cpp: load_cached_matrix() fails for 1440")
        print("      2. Insufficient daily candles (<50)")
        print("      3. Data format/parsing issue")
        print("")
        print("   SOLUTION: Fix HTF data loading for 1440 timeframe")
    elif regime in ['UNCERTAIN', 'MANIPULATION']:
        print("   🔥 MAIN ISSUE: Regime detection too strict")
        print("      System defaults to UNCERTAIN when trends unclear")
        print("")
        print("   SOLUTION: Relax regime detection logic")
    else:
        print("   ✓ No obvious issues found")

except json.JSONDecodeError as e:
    print(f"   ✗ Failed to parse JSON: {e}")
except Exception as e:
    print(f"   ✗ Error: {e}")
PYTHON_EOF

echo ""
echo "8. DATA FILES CHECK:"
echo "   Daily (1440m) data:"
if [ -f "cache/clean/AIAUSDT_1440.csv" ]; then
    LINES=$(wc -l < cache/clean/AIAUSDT_1440.csv)
    echo "   ✓ File exists: $LINES lines"
    if [ $LINES -lt 50 ]; then
        echo "   ⚠️  WARNING: Only $LINES candles (need 50+ for HTF)"
    fi
else
    echo "   ✗ File NOT found"
fi

echo "   4H (240m) data:"
if [ -f "cache/clean/AIAUSDT_240.csv" ]; then
    echo "   ✓ File exists: $(wc -l < cache/clean/AIAUSDT_240.csv) lines"
else
    echo "   ✗ File NOT found"
fi

echo "   1H (60m) data:"
if [ -f "cache/clean/AIAUSDT_60.csv" ]; then
    echo "   ✓ File exists: $(wc -l < cache/clean/AIAUSDT_60.csv) lines"
else
    echo "   ✗ File NOT found"
fi

echo ""
echo "=========================================="
echo "AUDIT COMPLETE"
echo "=========================================="
