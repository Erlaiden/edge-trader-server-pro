# PPO-PRO+ Environment v1 — RFC

## Цель
Дать сверхточность через on-policy обучение на симулированной среде:
- сделки, комиссия, TP/SL, баланс, эквити;
- episode rollout -> advantage (GAE) -> policy update;
- валидация по out-of-time.

## Контракты

### 1) State (D=32)
- Вход: тот же vector<double> фич (feat_ver=10, feat_dim=32).
- Расширения (опционально): pos_flag, pnl_norm, drawdown_norm.

### 2) Action
- {-1, 0, +1} = {SHORT, FLAT, LONG}.
- В будущем: дискретный TP/SL сет.

### 3) Reward (live)
- R_t = w1*profit_factor_t - w2*drawdown_t + w3*winrate_smooth_t
- Параметры: fee, slippage, risk_lambda.

### 4) Episode
- Стартовый капитал: 1.0.
- Шаг: 1 бар.
- Termination: конец выборки или max_drawdown breach.

### 5) Параметры
- fee_per_trade=0.0005
- risk_lambda=1.0
- max_dd=0.25
- gamma=0.99, gae_lambda=0.95

### 6) Метрики
- edge_reward_live, edge_winrate_live, edge_max_dd, edge_profit_factor
- отчёт: equity curve, PF, Sharpe, DD.

## API (план)
- C++: EnvTrading, RewardLive, EpisodeRunner.
- Роут: /api/train_env?symbol=...&interval=15&episodes=50&fee=...  (отключено до интеграции)
- Логика обновления policy через ppo_pro_plus_update(...).

## Безопасность внедрения
- Этап A: файлы-заглушки (без сборки).
- Этап B: интеграция в CMake в отдельной ветке + unit-тесты.
- Этап C: скрытый роут behind feature-flag.
