-- =============================================================================
--  Leak Guard 누출 로그 스키마 (MariaDB 10.2+)
--
--  적용:  sudo mysql -u root < schema.sql
--         (계정이 이미 있으면 CREATE USER 줄에서 에러가 나므로 IF NOT EXISTS 사용)
--
--  시간대 분석이 목적이므로 ts 는 "지역시(local time)" 로 저장한다.
--  Pi 타임존이 UTC 로 되어 있으면 그래프가 9시간 밀린다:
--      sudo timedatectl set-timezone Asia/Seoul
--
--  ── 기존 DB 마이그레이션 (leak_sample.rtt_ms 제거) ──────────────────────────
--  CREATE TABLE IF NOT EXISTS 는 이미 있는 테이블을 고치지 않는다. 예전에
--  만든 DB 에는 `rtt_ms FLOAT NOT NULL` 이 그대로 남아 있고, 기본값이 없어서
--  새 server 의 INSERT(rtt_ms 를 안 넣음)가 통째로 실패한다. 한 번만 실행:
--
--      mysql -u iot -ppwiot leakdb -e "ALTER TABLE leak_sample DROP COLUMN rtt_ms"
--
--  (make db 로 이 파일을 다시 적용해도 컬럼은 안 사라진다 — 위 명령이 필요하다)
-- =============================================================================

CREATE DATABASE IF NOT EXISTS leakdb
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

CREATE USER IF NOT EXISTS 'iot'@'localhost' IDENTIFIED BY 'pwiot';
GRANT ALL PRIVILEGES ON leakdb.* TO 'iot'@'localhost';
CREATE USER IF NOT EXISTS 'iot'@'127.0.0.1' IDENTIFIED BY 'pwiot';
GRANT ALL PRIVILEGES ON leakdb.* TO 'iot'@'127.0.0.1';
FLUSH PRIVILEGES;

USE leakdb;

-- -----------------------------------------------------------------------------
-- FSM 코드 이름표
--   이 순서는 STM32 `My/ap/safety_fsm.h` 의 fsm_state_t, server.c 의 fsm_name[]
--   과 반드시 일치해야 한다. 한쪽만 바꾸면 통계 라벨이 조용히 어긋난다.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS fsm_code (
  code TINYINT     NOT NULL PRIMARY KEY,
  name VARCHAR(12) NOT NULL
) ENGINE=InnoDB;

REPLACE INTO fsm_code(code, name) VALUES
  (-1,'(시작)'), (0,'INIT'), (1,'NORMAL'), (2,'CAUTION'),
  (3,'LEAK'), (4,'DANGER'), (5,'COMM_WARN'), (6,'COMM_TRIP');

-- -----------------------------------------------------------------------------
-- Jetson 판정 단계 이름표 (leak_sample.state / leak_event 의 원인 값)
--   원본은 Jetson `leakguard/tcp_client.py` 의 LEVEL_TO_STATE 이고
--   STM32 `My/ap/packet.h` 의 PKT_STATE_* 와 같은 값이다.
--   **주의**: state 1 은 "누수 있음" 이 아니라 SMALL_LEAK 이다.
--   누수 여부를 세려면 state = 1 이 아니라 `state BETWEEN 1 AND 3` 을 써야 한다.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS leak_state_code (
  code TINYINT     NOT NULL PRIMARY KEY,
  name VARCHAR(12) NOT NULL
) ENGINE=InnoDB;

REPLACE INTO leak_state_code(code, name) VALUES
  (0,'NORMAL'), (1,'SMALL_LEAK'), (2,'WARNING'), (3,'DANGER'), (4,'SENSOR_ERR');

-- -----------------------------------------------------------------------------
-- 1) leak_sample : 1초 주기 샘플 (state 가 바뀐 프레임은 주기와 무관하게 추가)
--      area/spread 단위는 0.01% (500 = 5.00%) — Jetson 이 보내는 단위 그대로.
--      hh 는 HOUR(ts) 를 미리 계산해 인덱스를 태우기 위한 생성 컬럼.
--      (MariaDB 키워드는 PERSISTENT. MySQL 이면 STORED 로 바꿀 것)
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS leak_sample (
  id        BIGINT   NOT NULL AUTO_INCREMENT PRIMARY KEY,
  ts        DATETIME NOT NULL,
  hh        TINYINT  AS (HOUR(ts)) PERSISTENT,
  state     TINYINT  NOT NULL,          -- Jetson 판정 단계 (leak_state_code 참조)
  area      INT      NOT NULL,          -- 누출 면적 (0.01% 단위)
  spread    INT      NOT NULL,          -- 확산 확정 플래그 0/1
  zone      TINYINT  NOT NULL,          -- 충전 단자 침범 0/1
  fsm       TINYINT  NOT NULL,          -- 그 시점 STM32 FSM
  latched   TINYINT  NOT NULL,
  comm_ok   TINYINT  NOT NULL,
  KEY idx_ts (ts),
  KEY idx_hh (hh)
) ENGINE=InnoDB;

-- -----------------------------------------------------------------------------
-- 2) leak_event : FSM/latch 가 바뀐 순간만
--      prev_dur_s = 직전 상태에 머문 시간(초). "LEAK 로 얼마나 오래 있었나" 를
--      샘플 개수로 세지 않고 바로 합산할 수 있게 하기 위함.
--      prev_fsm = -1 은 서버 기동 후 첫 텔레메트리(직전 상태 없음).
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS leak_event (
  id         BIGINT   NOT NULL AUTO_INCREMENT PRIMARY KEY,
  ts         DATETIME NOT NULL,
  hh         TINYINT  AS (HOUR(ts)) PERSISTENT,
  prev_fsm   TINYINT  NOT NULL,
  fsm        TINYINT  NOT NULL,
  prev_dur_s INT      NOT NULL,
  latched    TINYINT  NOT NULL,
  comm_ok    TINYINT  NOT NULL,
  area       INT      NOT NULL,
  spread     INT      NOT NULL,
  zone       TINYINT  NOT NULL,
  KEY idx_ts  (ts),
  KEY idx_hh  (hh),
  KEY idx_fsm (fsm)
) ENGINE=InnoDB;

-- -----------------------------------------------------------------------------
-- 3) 시간대별 통계 뷰 (샘플 기반)
--      leak_time_pct  : 그 시간대 샘플 중 누수 비율 -> "누수 시간 비중"
--      avg_area_pct   : 전체 평균(정상 구간 0 포함)
--      avg_leak_area  : 누수 중일 때만의 평균 -> "터지면 얼마나 크게 터지나"
--      days           : 관측된 날짜 수 (표본이 하루뿐이면 평균을 믿지 말 것)
--
--   "누수" 의 정의는 `state BETWEEN 1 AND 3` (SMALL_LEAK/WARNING/DANGER) 이다.
--   state = 1 하나만 세면 WARNING 과 DANGER 가 통계에서 빠진다 — 즉 심한 누수일수록
--   집계에서 사라지는 정반대의 결과가 나온다.
-- -----------------------------------------------------------------------------
CREATE OR REPLACE VIEW v_leak_by_hour AS
SELECT
  hh                                                                    AS hh,
  COUNT(*)                                                              AS samples,
  COUNT(DISTINCT DATE(ts))                                              AS days,
  SUM(state BETWEEN 1 AND 3)                                            AS leak_samples,
  ROUND(100 * SUM(state BETWEEN 1 AND 3) / COUNT(*), 2)                 AS leak_time_pct,
  SUM(state BETWEEN 2 AND 3)                                            AS warn_samples,
  SUM(state = 3)                                                        AS danger_samples,
  ROUND(AVG(area) / 100.0, 3)                                           AS avg_area_pct,
  ROUND(AVG(CASE WHEN state BETWEEN 1 AND 3 THEN area END) / 100.0, 3)  AS avg_leak_area_pct,
  ROUND(MAX(area) / 100.0, 3)                                           AS max_area_pct,
  SUM(zone = 1)                                                         AS zone_samples,
  SUM(spread = 1)                                                       AS spreading_samples,
  SUM(state = 4)                                                        AS sensor_err_samples
FROM leak_sample
GROUP BY hh
ORDER BY hh;

-- -----------------------------------------------------------------------------
-- 4) 시간대별 사건 뷰 (전이 기반)
--      leak_enter   : LEAK/DANGER 로 진입한 횟수
--      leak_dur_s   : LEAK/DANGER 상태에서 빠져나오며 확정된 체류 시간 합
--                     (진행 중인 상태는 아직 안 잡힌다 — 전이 시점에 기록되므로)
-- -----------------------------------------------------------------------------
CREATE OR REPLACE VIEW v_event_by_hour AS
SELECT
  hh                                                              AS hh,
  COUNT(*)                                                        AS transitions,
  SUM(fsm IN (3,4))                                               AS leak_enter,
  SUM(fsm = 4)                                                    AS danger_enter,
  SUM(fsm IN (5,6))                                               AS comm_fault,
  SUM(latched = 1 AND fsm = 4)                                    AS latch_events,
  SUM(CASE WHEN prev_fsm IN (3,4) THEN prev_dur_s ELSE 0 END)     AS leak_dur_s
FROM leak_event
GROUP BY hh
ORDER BY hh;

-- -----------------------------------------------------------------------------
-- 5) 요일 x 시간대 (근무 패턴이 보이는지 확인용)
-- -----------------------------------------------------------------------------
CREATE OR REPLACE VIEW v_leak_by_dow_hour AS
SELECT
  DAYOFWEEK(ts)                                          AS dow,   -- 1=일 ... 7=토
  hh                                                     AS hh,
  COUNT(*)                                               AS samples,
  ROUND(100 * SUM(state BETWEEN 1 AND 3) / COUNT(*), 2)  AS leak_time_pct,
  ROUND(AVG(area) / 100.0, 3)                            AS avg_area_pct
FROM leak_sample
GROUP BY dow, hh
ORDER BY dow, hh;
