CC = cc
CFLAGS = -Wall -Wextra -Werror -std=c11 -I..
BINS = run_config

all: $(BINS)
	./run_config

run_config: test_config.c ../config_logic.c
	$(CC) $(CFLAGS) test_config.c ../config_logic.c -o run_config

clean:
	rm -f $(BINS)
