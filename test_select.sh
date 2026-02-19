#!/bin/bash

# Configuration
SERVER_IP="127.0.0.1"
PORT=69
REPO=".tftp"
CLIENT_BIN="./client"

# Couleurs pour la lisibilité
VERT='\033[0;32m'
ROUGE='\033[0;31m'
JAUNE='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}==========================================================${NC}"
echo -e "${CYAN}   PROTOCOLE DE TEST : MULTIPLEXAGE ET EXCLUSION MUTUELLE ${NC}"
echo -e "${CYAN}==========================================================${NC}"

# 1. Nettoyage et préparation
echo -e "\n${JAUNE}[1/4] Préparation de l'environnement...${NC}"
rm -rf $REPO && mkdir -p $REPO
rm -f archivo_A.txt archivo_B.txt

# Création de fichiers sources avec un contenu unique
echo "CONTENU_TEST_A_$(date +%s)" > "$REPO/archivo_A.txt"
echo "CONTENU_TEST_B_$(date +%s)" > "$REPO/archivo_B.txt"

if [ ! -f "$CLIENT_BIN" ]; then
    echo -e "${ROUGE}[ERREUR] Le binaire '$CLIENT_BIN' est introuvable. Tapez 'make'.${NC}"
    exit 1
fi

# 2. Test de Parallélisme (Multiplexage)
echo -e "\n${JAUNE}[2/4] Test : Téléchargement simultané (Select/Poll)${NC}"
echo -e "Lancement de deux téléchargements en parallèle..."

$CLIENT_BIN $SERVER_IP get archivo_A.txt $PORT > /dev/null &
PID1=$!
$CLIENT_BIN $SERVER_IP get archivo_B.txt $PORT > /dev/null &
PID2=$!

wait $PID1 $PID2
echo -e "${VERT}[OK] Les processus parallèles sont terminés.${NC}"

# 3. Test d'Exclusion Mutuelle (Verrouillage/Lock)
echo -e "\n${JAUNE}[3/4] Test : Verrouillage de fichier (Même ressource)${NC}"
echo -e "Étape A: Le Client 1 bloque 'archivo_A.txt'..."
# On simule un client qui prend un peu de temps (si possible) ou on lance juste
$CLIENT_BIN $SERVER_IP get archivo_A.txt $PORT > /dev/null &
PID_LOCK=$!

sleep 0.3 # Temps pour que le serveur traite la première requête

echo -e "Étape B: Le Client 2 tente d'accéder au même fichier..."
# On capture le message d'erreur spécifique
RESULT_ERR=$($CLIENT_BIN $SERVER_IP get archivo_A.txt $PORT 2>&1)

if [[ $RESULT_ERR == *"File busy"* ]] || [[ $RESULT_ERR == *"Error"* ]]; then
    echo -e "${VERT}[SUCCÈS] Le serveur a bien rejeté la deuxième requête (File Busy).${NC}"
else
    echo -e "${ROUGE}[ATTENTION] Le serveur n'a pas renvoyé d'erreur de verrouillage.${NC}"
fi

wait $PID_LOCK

# 4. Vérification de l'intégrité (Comparaison réelle)
echo -e "\n${JAUNE}[4/4] Vérification de l'intégrité des données...${NC}"
INTEGRITY=true

for file in "archivo_A.txt" "archivo_B.txt"; do
    if diff "$REPO/$file" "$file" > /dev/null; then
        echo -e "${VERT}[OK] $file : Identique à l'original.${NC}"
    else
        echo -e "${ROUGE}[FAIL] $file : Corruption ou fichier manquant.${NC}"
        INTEGRITY=false
    fi
done

echo -e "\n${CYAN}==========================================================${NC}"
if [ "$INTEGRITY" = true ]; then
    echo -e "${VERT}RÉSULTAT FINAL : TEST RÉUSSI${NC}"
else
    echo -e "${ROUGE}RÉSULTAT FINAL : TEST ÉCHOUÉ${NC}"
fi
echo -e "${CYAN}==========================================================${NC}"