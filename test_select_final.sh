#!/bin/bash

# Configuración - AJUSTA EL PUERTO SI LO CAMBIASTE EN EL .C
SERVER_IP="127.0.0.1"
PORT=69
REPO=".tftp"
CLIENT_BIN="./client"

# Colores para legibilidad
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}=== INICIANDO PROTOCOLO DE PRUEBA PARA SERVER_SELECT ===${NC}"

# 1. Limpieza y preparación
echo -e "${YELLOW}[1/4] Preparando entorno...${NC}"
rm -rf $REPO
mkdir -p $REPO
rm -f descarga_A.txt descarga_B.txt

# Crear archivos de prueba dentro del repositorio del servidor
echo "Este es el archivo A - Prueba de Select" > "$REPO/archivo_A.txt"
echo "Este es el archivo B - Prueba de Multiplexacion" > "$REPO/archivo_B.txt"

# 2. Verificar si el cliente existe
if [ ! -f "$CLIENT_BIN" ]; then
    echo -e "${RED}[ERROR] No se encuentra el ejecutable '$CLIENT_BIN'. Corre 'make' primero.${NC}"
    exit 1
fi

# 3. Test de Paralelismo (Archivos distintos)
echo -e "${YELLOW}[2/4] Test: Descarga simultánea (Paralelismo)${NC}"
echo "Descargando archivo_A.txt y archivo_B.txt al mismo tiempo..."

$CLIENT_BIN $SERVER_IP get archivo_A.txt $PORT > /dev/null &
PID1=$!
$CLIENT_BIN $SERVER_IP get archivo_B.txt $PORT > /dev/null &
PID2=$!

wait $PID1 $PID2
echo -e "${GREEN}[OK] Ambas descargas solicitadas.${NC}"

# 4. Test de Exclusión Mutua (Tu lógica de lock_file)
echo -e "${YELLOW}[3/4] Test: Bloqueo de archivo (Mismo archivo)${NC}"
echo "Lanzando Cliente 1 para archivo_A.txt..."
$CLIENT_BIN $SERVER_IP get archivo_A.txt $PORT > /dev/null &
PID_LOCK=$!

sleep 0.2 # Pausa breve para que el servidor registre el lock

echo "Lanzando Cliente 2 para el MISMO archivo (Debería ser rechazado)..."
# Capturamos la salida para ver el error "File busy"
$CLIENT_BIN $SERVER_IP get archivo_A.txt $PORT

wait $PID_LOCK
echo -e "${GREEN}[OK] Prueba de exclusión completada.${NC}"

# 5. Verificación de archivos
echo -e "${YELLOW}[4/4] Verificando integridad...${NC}"
if [ -f "archivo_A.txt" ] && [ -f "archivo_B.txt" ]; then
    echo -e "${GREEN}[EXITO] Los archivos fueron recibidos correctamente.${NC}"
else
    echo -e "${RED}[FALLO] Algunos archivos no se descargaron.${NC}"
fi

# Limpieza final (opcional)
# rm archivo_A.txt archivo_B.txt