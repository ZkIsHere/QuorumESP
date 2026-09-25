CC = cc
CFLAGS = -Wall -Wextra -Werror -std=c11 -I..
BINS = run_ota

all: $(BINS)
	./run_ota

run_ota: test_ota.c ../ota_logic.c
	$(CC) $(CFLAGS) test_ota.c ../ota_logic.c -o run_ota

clean:
	rm -f $(BINS)
