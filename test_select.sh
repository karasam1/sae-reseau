#!/bin/bash

# Configuration
SERVER_IP="127.0.0.1"
PORT=69
REPO=".tftp"
CLIENT_BIN="./client"

# Couleurs pour une meilleure lisibilité
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}=== DÉMARRAGE DU PROTOCOLE DE TEST POUR SERVER_SELECT ===${NC}"

# 1. Nettoyage et préparation
echo -e "${YELLOW}[1/4] Préparer l'environnement...${NC}"
rm -rf $REPO
mkdir -p $REPO
rm -f fichier_A.txt fichier_B.txt

# Créez des fichiers de test dans le dépôt du serveur
echo "Voici le fichier A - Test de Select" > "$REPO/fichier_A.txt"
echo "Voici le fichier B - Test de multiplexage" > "$REPO/fichier_B.txt"

# 2. Verifier si le client exists
if [ ! -f "$CLIENT_BIN" ]; then
    echo -e "${RED}[ERROR] Le fichier exécutable est introuvable. '$CLIENT_BIN'."
    exit 1
fi

# 3. Test de parallelisme (lectures simultanées)
echo -e "${YELLOW}[2/4] Test: Téléchargement simultané (parallélisme)${NC}"
echo "Telechargement simultanée des fichiers fichier_A.txt et fichier_B.txt ..."

$CLIENT_BIN $SERVER_IP get fichier_A.txt $PORT > /dev/null &
PID1=$!
$CLIENT_BIN $SERVER_IP get fichier_B.txt $PORT > /dev/null &
PID2=$!

wait $PID1 $PID2
echo -e "${GREEN}[OK] Les deux telechargement demandes.${NC}"

# 4. Test d'exclusion mutuelle (La logique de fichier de verrouillage)
echo -e "${YELLOW}[3/4] Test: Verrouillage du fichier (Même fichier)${NC}"
echo "Lancement du Client 1 pur le fichier_A.txt..."
$CLIENT_BIN $SERVER_IP get fichier_A.txt $PORT > /dev/null &
PID_LOCK=$!

sleep 0.2 # Brève pause pour permettre au serveur d'enregistrer le verrou

echo "Lancement du Client 2 pour le même fichier (Devrait être rejeté)..."
# Capturew la sortie pour voir l'erreur << Fichier occupé >>
$CLIENT_BIN $SERVER_IP get fichier_A.txt $PORT

wait $PID_LOCK
echo -e "${GREEN}[OK] Test d'exclusion mutuelle terminé.${NC}"

# 5. Verification de fichiers
echo -e "${YELLOW}[4/4] Vérification de l'integrité...${NC}"
if [ -f "fichier_A.txt" ] && [ -f "fichier_B.txt" ]; then
    echo -e "${GREEN}[SUCCES] Les fichiers ont été reçus correctement.${NC}"
else
    echo -e "${RED}[ECHEC] Certains fichiers n'ont pas été téléchargés.${NC}"
fi
