CC = cc
CFLAGS = -Wall -Wextra -Werror -std=c11 -I.. -I../../common
BINS = run_tlv run_msg

all: $(BINS)
	./run_tlv
	./run_msg

run_tlv: test_tlv.c ../tlv.c
	$(CC) $(CFLAGS) test_tlv.c ../tlv.c -o run_tlv

run_msg: test_msg.c ../tlv.c ../msg.c
	$(CC) $(CFLAGS) test_msg.c ../tlv.c ../msg.c -o run_msg

clean:
	rm -f $(BINS)
