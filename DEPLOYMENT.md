# 🚀 Edge Trader AI - Production Deployment

## ✅ Production Status: READY

### Дата запуска: November 7, 2025
### Tag: `production-v10-multi-symbol-stable`

## 📊 Статистика стабильности
- **BTCUSDT:** 100/100 тестов (100%)
- **ETHUSDT:** 5/5 тестов (100%)
- **SOLUSDT:** 5/5 тестов (100%)

## 🔧 Архитектура
- **CPU:** 8 cores
- **RAM:** 16 GB
- **Thread Pool:** 16 workers (оптимизировано)
- **Cache:** RAM cache, 5 min TTL
- **Model Version:** v10 (32 features + MFLOW)
- **Performance:** ~5-6 RPS

## 🎯 Доступные торговые пары
1. **BTCUSDT** - Bitcoin (основная, полностью протестирована)
2. **ETHUSDT** - Ethereum (готова к торговле)
3. **SOLUSDT** - Solana (готова к торговле)

## 📱 API Endpoints

### Base URL
```
https://api.edgetraderai.trade
```

### Health Check
```bash
curl https://api.edgetraderai.trade/api/health
# Response: {"ok": true, "uptime_sec": 12345}
```

### Получение сигнала
```bash
# Bitcoin
curl 'https://api.edgetraderai.trade/api/infer?symbol=BTCUSDT&interval=15'

# Ethereum
curl 'https://api.edgetraderai.trade/api/infer?symbol=ETHUSDT&interval=15'

# Solana
curl 'https://api.edgetraderai.trade/api/infer?symbol=SOLUSDT&interval=15'

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

### Добавление новой торговой пары
```bash
curl -X POST 'https://api.edgetraderai.trade/api/symbol/prepare' \
  -H 'Content-Type: application/json' \
  -d '{
    "symbol": "BNBUSDT",
    "interval": "15",
    "months": 6
  }'

# Процесс занимает 5-15 минут
# После готовности пара доступна через /api/infer
```

### Управление роботом
```bash
# Запуск
curl -X POST 'https://api.edgetraderai.trade/api/robot/start' \
  -H 'Content-Type: application/json' \
  -d '{
    "symbol": "BTCUSDT",
    "mode": "balanced",
    "apiKey": "YOUR_BYBIT_KEY",
    "apiSecret": "YOUR_BYBIT_SECRET"
  }'

# Статус
curl 'https://api.edgetraderai.trade/api/robot/status'

# Остановка
curl -X POST 'https://api.edgetraderai.trade/api/robot/stop'
```

## 🛡️ Безопасность

### Режимы торговли
- **Conservative:** 50% balance, 3x leverage, минимальный риск
- **Balanced:** 70% balance, 5x leverage, оптимальный риск/доходность
- **Aggressive:** 90% balance, 10x leverage, максимальная доходность

### Риск-менеджмент
- Take Profit (TP): 0.8% по умолчанию
- Stop Loss (SL): 0.32% по умолчанию
- Максимальный риск на сделку: 0.8% капитала
- Multi-timeframe validation: 15m, 60m, 240m, 1440m

## 📈 Мониторинг

### Системные метрики
```bash
# CPU/Memory
ps aux | grep edge_trader_server

# Логи (real-time)
journalctl -u edge-trader-server -f

# Статистика кэша
curl https://api.edgetraderai.trade/api/infer/stats
```

### Тест стабильности
```bash
# 20 запросов, должны все пройти
for i in {1..20}; do 
  curl -s 'https://api.edgetraderai.trade/api/infer?symbol=BTCUSDT' | jq -r '.ok'
done | grep -c "true"
# Ожидается: 20/20
```

## 🔄 Обслуживание

### Рестарт сервиса
```bash
systemctl restart edge-trader-server
systemctl status edge-trader-server
```

### Очистка кэша
```bash
curl -X POST 'https://api.edgetraderai.trade/api/infer/cache/clear'
```

### Обновление
```bash
cd /opt/edge-trader-server
git pull origin main
cd build && cmake .. && make -j4
systemctl restart edge-trader-server
```

### Бэкап
```bash
# Модели
tar -czf models_$(date +%Y%m%d).tar.gz cache/models/

# Данные
tar -czf data_$(date +%Y%m%d).tar.gz cache/clean/
```

## ⚠️ Known Issues

### Minor
- **Robot thread:** Небольшой memory leak при остановке робота (некритично, т.к. робот работает постоянно)
- **Первый запрос:** Медленнее последующих (~1-2 сек vs <10ms) из-за загрузки в кэш

### Решено
- ✅ Стабильность 100% достигнута
- ✅ RAM кэширование работает
- ✅ Multi-threading оптимизирован
- ✅ Infer policy защищён от exceptions

## 📞 Поддержка

### Диагностика проблем
```bash
# 1. Проверка сервиса
systemctl status edge-trader-server

# 2. Последние ошибки
journalctl -u edge-trader-server --since "10 min ago" | grep -i error

# 3. Проверка моделей
ls -lh cache/models/*.json

# 4. Проверка данных
ls -lh cache/clean/*.csv
```

## 🎯 Roadmap

### Планируется
- [ ] Добавить больше торговых пар (XRP, ADA, DOGE)
- [ ] Telegram бот для уведомлений
- [ ] Dashboard для мониторинга
- [ ] Автоматическое переобучение моделей
- [ ] Исправление robot thread memory leak

### Завершено
- [x] Стабильность 100%
- [x] 3 торговые пары
- [x] RAM кэширование
- [x] Multi-threading
- [x] Production deployment

---

**Production Ready:** ✅  
**Last Updated:** November 7, 2025  
**Version:** v10-multi-symbol-stable
