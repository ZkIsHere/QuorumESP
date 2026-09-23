CC = cc
CFLAGS = -Wall -Wextra -Werror -std=c11 -I.. -I../../qdevice
BINS = run_ffsplit

all: $(BINS)
	./run_ffsplit

run_ffsplit: test_ffsplit.c ../ffsplit.c
	$(CC) $(CFLAGS) test_ffsplit.c ../ffsplit.c -o run_ffsplit

clean:
	rm -f $(BINS)
