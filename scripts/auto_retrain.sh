#!/bin/bash

DB_NAME="edge_trader_prod"
DB_USER="edge_user"
DB_PASS="edge_secure_2025"

LOG_FILE="/opt/edge-trader-server/cache/logs/auto_retrain.log"
mkdir -p /opt/edge-trader-server/cache/logs

echo "[$(date)] === AUTO RETRAIN START ===" >> "$LOG_FILE"

# АВТОМАТИЧЕСКИ регистрируем активные монеты (из сделок за последние 24ч)
PGPASSWORD="$DB_PASS" psql -h localhost -U "$DB_USER" -d "$DB_NAME" -c \
  "SELECT auto_register_active_models();" >> "$LOG_FILE" 2>&1

# Находим модели для переобучения
MODELS=$(PGPASSWORD="$DB_PASS" psql -h localhost -U "$DB_USER" -d "$DB_NAME" -t -c \
  "SELECT symbol || ',' || interval || ',' || episodes || ',' || retrain_interval_hours 
   FROM active_models 
   WHERE is_active = true AND next_retrain_at <= NOW() 
   LIMIT 5")

if [ -z "$MODELS" ]; then
    echo "[$(date)] No models to retrain" >> "$LOG_FILE"
    exit 0
fi

echo "$MODELS" | while IFS=',' read -r symbol interval episodes retrain_hours; do
    symbol=$(echo "$symbol" | xargs)
    interval=$(echo "$interval" | xargs)
    episodes=$(echo "$episodes" | xargs)
    retrain_hours=$(echo "$retrain_hours" | xargs)
    
    echo "[$(date)] Training $symbol $interval (episodes=$episodes)" >> "$LOG_FILE"
    
    RESULT=$(curl -s --max-time 600 "http://localhost:3000/api/train?symbol=${symbol}&interval=${interval}&episodes=${episodes}")
    
    if echo "$RESULT" | grep -q '"val_accuracy"'; then
        ACCURACY=$(echo "$RESULT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('val_accuracy', 0))" 2>/dev/null || echo "0")
        WINRATE=$(echo "$RESULT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('val_winrate', 0))" 2>/dev/null || echo "0")
        
        PGPASSWORD="$DB_PASS" psql -h localhost -U "$DB_USER" -d "$DB_NAME" -c \
          "UPDATE active_models SET 
           trained_at = NOW(), 
           next_retrain_at = NOW() + INTERVAL '${retrain_hours} hours',
           last_accuracy = ${ACCURACY},
           last_winrate = ${WINRATE},
           updated_at = NOW()
           WHERE symbol = '${symbol}' AND interval = '${interval}'" >> "$LOG_FILE" 2>&1
        
        echo "[$(date)] ✅ $symbol trained (acc=$ACCURACY wr=$WINRATE)" >> "$LOG_FILE"
    else
        echo "[$(date)] ❌ $symbol failed" >> "$LOG_FILE"
    fi
    
    sleep 5
done

echo "[$(date)] === AUTO RETRAIN END ===" >> "$LOG_FILE"
