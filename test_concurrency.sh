#!/bin/bash

# Configuration
SERVER_IP="127.0.0.1"
PORT=69
DIR=".tftp"

# Colors
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${CYAN}--- Pruebas de Concurrencia TFTP ---${NC}"

# 1. Create test files (10MB)
echo -e "${YELLOW}[INFO] Generando archivos de prueba de 20MB...${NC}"
mkdir -p $DIR
dd if=/dev/urandom of=$DIR/file_A.bin bs=1M count=20 status=none
dd if=/dev/urandom of=$DIR/file_B.bin bs=1M count=20 status=none

# 2. Test of real parallelism (Differents files)
echo -e "\n${GREEN}[TEST 1] Paralelismo Real (Archivos Diferentes)${NC}"
echo "Iniciando Cliente 1 (file_A.bin)..."
./client $SERVER_IP get file_A.bin $PORT &
PID1=$!

echo "Iniciando Cliente 2 (file_B.bin)..."
./client $SERVER_IP get file_B.bin $PORT &
PID2=$!

wait $PID1
wait $PID2
echo -e "${GREEN}[OK] Descargas paralelas terminadas.${NC}"

# 3. Prueba de Exclusión Mutua (Mismo Archivo)
# El segundo cliente debería esperar al primero (si el mutex funciona)
echo -e "\n${GREEN}[TEST 2] Exclusión Mutua (Mismo Archivo)${NC}"
echo "Iniciando Cliente 1 (file_A.bin)..."
./client $SERVER_IP get file_A.bin $PORT &
PID1=$!

# Pequeña pausa para asegurar que el 1 gane el lock
sleep 0.2 

echo "Iniciando Cliente 2 (file_A.bin) - Debería esperar..."
./client $SERVER_IP get file_A.bin $PORT &
PID2=$!

wait $PID1
wait $PID2
echo -e "${GREEN}[OK] Test de exclusión mutua terminado.${NC}"

# Limpieza
rm -f file_A.bin file_B.bin
