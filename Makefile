# =============================================================================
#  Leak Guard — Raspberry Pi 중계 서버
#
#   make            server 빌드 (server.c + leak_db.c)
#   make run        빌드 후 실행         (make run DEV=/dev/ttyUSB0 처럼 덮어쓰기 가능)
#   make dblog      --dblog 로 실행      (DB 에 남기는 행을 화면에도 출력)
#   make dump       --dump 로 실행       (원시 수신 바이트 진단)
#   make nodb       --nodb 로 실행       (DB 로깅 없이 중계만)
#   make avg        --avg=N 로 실행      (N초마다 DB 집계 한 줄, 기본 10초)
#
#   make deps       필요한 패키지 설치   (최초 1회)
#   make db         DB/테이블/뷰 생성    (최초 1회, sudo 필요)
#   make report     조회 도구(leak_tail · leak_report) 빌드
#   make tail       쌓이는 행을 다른 터미널에서 실시간으로 보기
#   make web        leakGraph.php 를 /var/www/html/ 로 복사
#
#   옵션은 DBLOG=1 처럼 붙일 수도 있다: make run DBLOG=1 DEV=/dev/ttyUSB0
#   주기는 초 단위로 덮어쓴다: make avg AVG=30
#
#   make clean      이 폴더 빌드 산출물 삭제
#   make distclean  하위 폴더(MariaDB/leakdb) 산출물까지 삭제
# =============================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -pthread
LDLIBS  := -lmysqlclient -pthread

TARGET  := server
SRCS    := server.c leak_db.c
OBJS    := $(SRCS:.c=.o)
HDRS    := leak_db.h

DBDIR   := MariaDB/leakdb
WEBSRC  := MariaDB/html/leakGraph.php
WEBDIR  := /var/www/html

# 실행 인자 (명령줄에서 덮어쓰기: make run PORT=5001 DEV=/dev/ttyUSB0)
PORT ?= 5000
DEV  ?= /dev/rfcomm0
BAUD ?= 115200

# DBLOG=1 이면 어떤 실행 타깃에든 --dblog 가 붙는다 (make dump DBLOG=1 도 가능)
DBLOG ?=
DBFLAG := $(if $(DBLOG),--dblog)

# 집계 주기 (초). 아래 avg 타깃이 쓴다.
AVG  ?= 10

ARGS := $(PORT) $(DEV) $(BAUD) $(DBFLAG)

.PHONY: all run dblog dump nodb avg deps db report tail web clean distclean help

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDLIBS)

# 헤더가 바뀌어도 다시 컴파일되도록 $(HDRS) 를 의존성에 넣는다
%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET) $(ARGS)

dblog: $(TARGET)
	./$(TARGET) $(PORT) $(DEV) $(BAUD) --dblog

dump: $(TARGET)
	./$(TARGET) $(ARGS) --dump

nodb: $(TARGET)
	./$(TARGET) $(ARGS) --nodb

# --avg 는 DB 를 읽어 집계하므로 --nodb 와 같이 쓰면 아무것도 안 나온다.
avg: $(TARGET)
	./$(TARGET) $(ARGS) --avg=$(AVG)

# --------------------------------- 최초 1회 ---------------------------------
deps:
	sudo apt-get install -y build-essential mariadb-server libmariadb-dev-compat
	@echo ">> 시간대 분석이 목적이므로 타임존도 맞춰야 합니다:"
	@echo "   sudo timedatectl set-timezone Asia/Seoul"

db:
	sudo mysql -u root < $(DBDIR)/schema.sql
	@echo ">> leakdb 생성 완료"

web:
	sudo cp $(WEBSRC) $(WEBDIR)/
	@echo ">> http://<Pi주소>/leakGraph.php 로 접속하세요"

# ------------------------------ 조회 도구 -----------------------------------
report:
	$(MAKE) -C $(DBDIR)

# server 를 띄운 터미널은 그대로 두고, 다른 터미널에서 이걸 돌린다.
# 읽기 전용이라 중계에 영향을 주지 않는다.
tail:
	$(MAKE) -C $(DBDIR) tail

# --------------------------------- 정리 -------------------------------------
clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	$(MAKE) -C $(DBDIR) clean

help:
	@echo "make / run [DBLOG=1] / dblog / dump / nodb / avg [AVG=N]"
	@echo "deps / db / report / tail / web / clean / distclean"
