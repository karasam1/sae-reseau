CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -std=c99
INCLUDES = -Imulti

SRCDIR  = multi
TESTDIR = $(SRCDIR)/test

all: server client test_multithreading

# ---- Binaires principaux ----

server: $(SRCDIR)/server.c $(SRCDIR)/tftp_protocole.h $(SRCDIR)/tftp_errors.h
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCDIR)/server.c -o server

client: $(SRCDIR)/client.c $(SRCDIR)/tftp_protocole.h $(SRCDIR)/tftp_errors.h
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCDIR)/client.c -o client

# ---- Programme de test (dans multi/test/) ----

test_multithreading: $(TESTDIR)/test_multithreading.c
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(TESTDIR)/test_multithreading.c -o test_multithreading

# ---- Cibles utilitaires ----

.PHONY: clean run-tests

# Lance le test : démarre le serveur en arrière-plan, attend 1s, lance le simulateur
run-tests: all
	@echo "==> Création du répertoire .tftp pour le serveur"
	mkdir -p .tftp
	@echo "==> Copie d'un fichier source pour les tests GET"
	cp $(SRCDIR)/client.c .tftp/
	@echo "==> Démarrage du serveur en arrière-plan"
	./server &
	sleep 1
	@echo "==> Lancement du simulateur multithread"
	./test_multithreading
	@echo "==> Arrêt du serveur"
	kill %1 2>/dev/null || true

# Version avec le script shell
run-shell-test: all
	@echo "==> Démarrage du serveur"
	./server &
	sleep 1
	@echo "==> Lancement du script de test"
	cd $(TESTDIR) && ./test.sh
	@echo "==> Arrêt du serveur"
	kill %1 2>/dev/null || true

clean:
	rm -f server client test_multithreading *.o
	rm -rf .tftp
