CC = cc
CFLAGS = -Wall -Wextra -Werror -std=c11 -I.. -I../../common
BINS = run_ffsplit run_lms

all: $(BINS)
	./run_ffsplit
	./run_lms

run_ffsplit: test_ffsplit.c ../ffsplit.c
	$(CC) $(CFLAGS) test_ffsplit.c ../ffsplit.c -o run_ffsplit

run_lms: test_lms.c ../lms.c ../ffsplit.c
	$(CC) $(CFLAGS) test_lms.c ../lms.c ../ffsplit.c -o run_lms

clean:
	rm -f $(BINS)
