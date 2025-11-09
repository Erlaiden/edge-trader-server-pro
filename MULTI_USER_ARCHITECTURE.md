# Multi-User Architecture Design

## Objective
Transform single-user robot into scalable SaaS platform for 100K+ users

## Current Problems
❌ Single keys.json file - last user overwrites all
❌ No authentication/authorization
❌ No data isolation
❌ Not scalable

## Solution Architecture

### 1. Database Schema (PostgreSQL)
```sql
-- Users table
CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    email VARCHAR(255) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    created_at TIMESTAMP DEFAULT NOW(),
    subscription_tier VARCHAR(50) DEFAULT 'free',
    subscription_expires_at TIMESTAMP
);

-- API Keys (encrypted)
CREATE TABLE user_api_keys (
    id SERIAL PRIMARY KEY,
    user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
    api_key_encrypted TEXT NOT NULL,
    api_secret_encrypted TEXT NOT NULL,
    testnet BOOLEAN DEFAULT false,
    created_at TIMESTAMP DEFAULT NOW()
);

-- Robot configs
CREATE TABLE user_configs (
    id SERIAL PRIMARY KEY,
    user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
    symbol VARCHAR(20) NOT NULL,
    leverage INTEGER DEFAULT 10,
    balance_percent REAL DEFAULT 90.0,
    tp_percent REAL DEFAULT 2.0,
    sl_percent REAL DEFAULT 1.0,
    min_confidence REAL DEFAULT 60.0,
    auto_trade BOOLEAN DEFAULT false,
    updated_at TIMESTAMP DEFAULT NOW()
);

-- Trade journal
CREATE TABLE trades (
    id SERIAL PRIMARY KEY,
    user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
    symbol VARCHAR(20) NOT NULL,
    side VARCHAR(10) NOT NULL,
    entry_price REAL NOT NULL,
    exit_price REAL,
    pnl REAL,
    opened_at TIMESTAMP NOT NULL,
    closed_at TIMESTAMP,
    reason VARCHAR(50)
);

CREATE INDEX idx_trades_user_id ON trades(user_id);
CREATE INDEX idx_trades_opened_at ON trades(opened_at DESC);
```

### 2. Authentication Flow
```
Client → POST /api/auth/register {email, password}
Server → Create user, return JWT token

Client → POST /api/auth/login {email, password}
Server → Verify password, return JWT token

Client → All requests with Header: Authorization: Bearer <token>
Server → Verify token, extract user_id, proceed
```

### 3. JWT Token Structure
```json
{
  "user_id": 12345,
  "email": "user@example.com",
  "tier": "premium",
  "exp": 1735660800
}
```

### 4. API Changes

**Before:**
```
GET /api/robot/balance
→ Returns last user's balance
```

**After:**
```
GET /api/robot/balance
Header: Authorization: Bearer <token>
→ Extract user_id from token
→ Load user's API keys from DB
→ Call Bybit with user's keys
→ Return user's balance
```

### 5. Security

- **Encryption**: AES-256-GCM for API keys
- **Hashing**: bcrypt for passwords (cost=12)
- **HTTPS**: Mandatory in production
- **Rate limiting**: 100 req/min per user
- **SQL injection**: Prepared statements only

### 6. Scalability

- **Connection pool**: 20 DB connections
- **Redis cache**: Per-user inference results (5min TTL)
- **Load balancer**: Multiple C++ server instances
- **Monitoring**: Prometheus + Grafana

## Implementation Priority

### Phase 1: Core Auth (Week 1)
- [ ] PostgreSQL setup
- [ ] User registration/login
- [ ] JWT middleware
- [ ] Encrypt/decrypt API keys

### Phase 2: Data Isolation (Week 2)
- [ ] Migrate /robot/* endpoints
- [ ] Per-user configs
- [ ] Per-user trades journal
- [ ] Testing with 2+ users

### Phase 3: Production Ready (Week 3)
- [ ] Rate limiting
- [ ] Stress testing
- [ ] Monitoring
- [ ] Documentation

## Success Metrics

- ✅ 100K+ concurrent users
- ✅ <50ms auth overhead
- ✅ Zero data leaks between users
- ✅ 99.9% uptime
