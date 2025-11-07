# 🚀 Edge Trader AI - Production Server

## ✅ Статус: PRODUCTION READY
- **Стабильность:** 100% (100/100 тестов)
- **Версия модели:** v10 (32 features with MFLOW)
- **Thread pool:** 16 workers (оптимизировано под 8 CPU)
- **RAM cache:** Enabled (5 min TTL)
- **Tag:** `production-v10-stable-100pct`

## 🔧 Технические характеристики
- **CPU:** 8 cores
- **RAM:** 16 GB
- **Storage:** 320 GB
- **RPS:** ~5-6 requests/second
- **Latency:** ~50-200ms (первый запрос), <10ms (кэш)

## 📱 API для мобильного приложения

### Base URL
```
https://api.edgetraderai.trade
```

### 1. Подготовка нового символа
```bash
POST /api/symbol/prepare
Content-Type: application/json

{
  "symbol": "ETHUSDT",
  "interval": "15",
  "months": 6
}

# Response:
{
  "ok": true,
  "ready": true,
  "symbol": "ETHUSDT",
  "steps": ["backfill_15", "backfill_60", "backfill_240", "backfill_1440", "training", "loading_model"]
}
```

### 2. Обучение модели
```bash
GET /api/train?symbol=ETHUSDT&interval=15&episodes=10000&tp=0.008&sl=0.004&ma=12&fetch=1&months=6

# Response:
{
  "ok": true,
  "version": 10,
  "feat_dim": 32,
  "tp": 0.008,
  "sl": 0.004,
  "best_thr": 0.38,
  "metrics": {
    "val_accuracy": 0.85,
    "M_labeled": 685,
    "val_sharpe": 1.25
  }
}
```

### 3. Получение торговых сигналов
```bash
GET /api/infer?symbol=BTCUSDT&interval=15

# Response:
{
  "ok": true,
  "signal": "SHORT",
  "confidence": 100.0,
  "score15": -0.95,
  "market_mode": "trendDown",
  "version": 10,
  "feat_dim_used": 32,
  "tp": 0.008,
  "sl": 0.0032,
  "last_close": 103744.1,
  "tp_price_short": 102914.15,
  "sl_price_short": 104076.08,
  "htf": {
    "60": {"agree": true, "score": -0.99, "strong": true},
    "240": {"agree": true, "score": -0.99, "strong": true},
    "1440": {"agree": true, "score": -0.60, "strong": true}
  },
  "from_cache": true
}
```

### 4. Управление роботом
```bash
# Старт торговли
POST /api/robot/start
Content-Type: application/json

{
  "symbol": "BTCUSDT",
  "interval": "15",
  "mode": "balanced",
  "apiKey": "your_bybit_key",
  "apiSecret": "your_bybit_secret"
}

# Стоп торговли
POST /api/robot/stop

# Статус
GET /api/robot/status
```

## 🔄 Управление сервером

### Рестарт
```bash
systemctl restart edge-trader-server
```

### Логи
```bash
# Реальное время
journalctl -u edge-trader-server -f

# Последние 100 строк
journalctl -u edge-trader-server -n 100

# За последний час
journalctl -u edge-trader-server --since "1 hour ago"
```

### Очистка кэша
```bash
curl -X POST https://api.edgetraderai.trade/api/infer/cache/clear
```

### Проверка здоровья
```bash
curl https://api.edgetraderai.trade/api/health
```

## 📊 Мониторинг

### Метрики системы
```bash
# CPU/RAM
top -b -n 1 | grep edge_trader

# Статистика кэша
curl https://api.edgetraderai.trade/api/infer/stats
```

### Тест стабильности
```bash
cd /opt/edge-trader-server
for i in {1..20}; do 
  curl -s 'http://localhost:3000/api/infer?symbol=BTCUSDT' | jq -r '.ok'
done | grep -c "true"
# Должно вернуть 20/20
```

## 🛡️ Безопасность

### Режимы торговли
- **Conservative:** 50% balance, 3x leverage
- **Balanced:** 70% balance, 5x leverage  
- **Aggressive:** 90% balance, 10x leverage
- **Custom:** Пользовательские настройки

### Защита средств
- TP/SL обязательны для каждой сделки
- Максимальный риск на сделку: 0.8%
- Anti-manipulation detection: включен
- Multi-timeframe validation: 60m, 240m, 1440m

## 🔧 Обслуживание

### Бэкап
```bash
# Модели
tar -czf models_backup_$(date +%Y%m%d).tar.gz cache/models/

# Конфигурация
cp /etc/systemd/system/edge-trader-server.service ~/backup/
```

### Обновление
```bash
cd /opt/edge-trader-server
git pull origin main
cd build && cmake .. && make -j4
systemctl restart edge-trader-server
```

## ⚠️ Важные замечания

1. **Первый запрос медленный** (~1-2 сек) - загрузка в RAM кэш
2. **Последующие быстрые** (<10ms) - из кэша
3. **Кэш живёт 5 минут** - автоматическое обновление
4. **16 потоков** - оптимально для 8 CPU
5. **Не перегружать** - максимум 10 RPS рекомендуется

## 📞 Поддержка

При проблемах проверить:
```bash
# 1. Сервис работает?
systemctl status edge-trader-server

# 2. Порт открыт?
netstat -tlnp | grep 3000

# 3. Модель загружена?
ls -lh cache/models/BTCUSDT_15_ppo_pro.json

# 4. Последние ошибки
journalctl -u edge-trader-server --since "10 min ago" | grep -i error
```

---

**Production Tag:** `production-v10-stable-100pct`  
**Дата запуска:** November 7, 2025  
**Статус:** ✅ READY FOR PRODUCTION
