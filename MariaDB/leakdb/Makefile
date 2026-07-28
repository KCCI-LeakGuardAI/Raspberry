# =============================================================================
#  Leak Guard — 누수 로그 조회 도구
#
#   두 가지다. 목적이 다르다:
#     leak_tail    지금 쌓이는 행을 실시간으로 흘려보기 (tail -f)
#     leak_report  이미 쌓인 것을 시간대별로 집계해 표로 보기
#
#   make              둘 다 빌드
#   make db           스키마 적용 (DB/계정/테이블/뷰 생성, 최초 1회 · sudo)
#
#   make tail         실시간 로그        (make tail N=30 으로 과거 30행부터)
#   make tailev       FSM 전이(leak_event)만 실시간으로
#   make show         전체 기간 리포트   (make show DAYS=7 로 기간 제한)
#   make week         요일 x 시간대까지 같이 출력
#   make sql          mysql 콘솔 접속 (leakdb)
#   make peek         지금까지 쌓인 행 수만 확인
#
#   make clean        빌드 산출물 삭제
# =============================================================================

CC     := gcc
CFLAGS := -Wall -Wextra -O2
LDLIBS := -lmysqlclient

TARGETS := leak_report leak_tail
OBJS    := $(TARGETS:=.o)

# 기간 제한 (비어 있으면 전체 기간). 예: make show DAYS=7
DAYS ?=
DFLAG := $(if $(DAYS),-d $(DAYS))

# 실시간 로그 시작 시 먼저 뿌릴 과거 행 수. 예: make tail N=30
N ?=
NFLAG := $(if $(N),-n $(N))

DBUSER := iot
DBPASS := pwiot
DBNAME := leakdb

.PHONY: all db tail tailev show week sql peek clean help

all: $(TARGETS)

# 패턴 규칙(%: %.o)을 쓰지 않고 풀어 쓴다 — phony 타깃(tail, show ...)까지
# 암시적 규칙 탐색에 걸려 엉뚱한 .c 를 찾는 일을 아예 만들지 않기 위함이다.
leak_report: leak_report.o
	$(CC) -o $@ $< $(LDLIBS)

leak_tail: leak_tail.o
	$(CC) -o $@ $< $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

db:
	sudo mysql -u root < schema.sql
	@echo ">> leakdb 생성 완료 (leak_sample / leak_event / 뷰 3개)"

tail: leak_tail
	./leak_tail $(NFLAG)

tailev: leak_tail
	./leak_tail $(NFLAG) -e

show: leak_report
	./leak_report $(DFLAG)

week: leak_report
	./leak_report $(DFLAG) -w

sql:
	mysql -u $(DBUSER) -p$(DBPASS) $(DBNAME)

peek:
	@mysql -u $(DBUSER) -p$(DBPASS) $(DBNAME) -e \
	 "SELECT (SELECT COUNT(*) FROM leak_sample) AS samples, \
	         (SELECT COUNT(*) FROM leak_event)  AS events,  \
	         (SELECT MAX(ts)  FROM leak_sample) AS last_ts;"

clean:
	rm -f $(OBJS) $(TARGETS)

help:
	@echo "make / db / tail [N=n] / tailev / show [DAYS=n] / week [DAYS=n] / sql / peek / clean"
