# Projet TFTP - Conformité RFC 1350

Ce projet implémente un client et un serveur TFTP en C, conçus pour respecter les spécifications du protocole TFTP (RFC 1350).

## Tableau de Conformité RFC 1350

Le tableau ci-dessous détaille l'état de conformité du code vis-à-vis des exigences de la RFC.

| Point RFC 1350 | Description | Statut | Détails d'implémentation |
| :--- | :--- | :---: | :--- |
| **Header Validation** | Structure `Opcode + Filename + 0 + Mode + 0` | ✅ Conforme | Le serveur analyse et valide strictement la présence des délimiteurs nuls et le format du header avant tout traitement. |
| **Opcodes** | Support des 5 types de paquets (RRQ, WRQ, DATA, ACK, ERROR) | ✅ Conforme | Le serveur et le client vérifient systématiquement l'Opcode des paquets reçus. Rejet des Opcodes inconnus. |
| **Modes de Transfert** | Support de "netascii", "octet" (case-insensitive) | ⚠️ Partiel | Les modes sont parsés, validés (insensible à la casse) et acceptés. **Limitation** : La conversion de fin de ligne pour `netascii` n'est pas effectuée (transfert binaire par défaut). |
| **Gestion des Erreurs** | Envoi et réception de paquets ERROR (Opcode 5) | ✅ Conforme | Envoi de codes d'erreur standard (1: File not found, 4: Illegal operation, etc.). Le client décode et affiche les messages d'erreur textuels du serveur. |
| **TID (Transfer ID)** | Gestion des ports écoute vs ports transaction | ✅ Conforme | Le serveur vérifie l'IP et le Port de l'émetteur pour chaque paquet DATA/ACK afin de garantir l'intégrité de la session (TID). |
| **Timeouts & Retries** | Gestion des pertes de paquets | ✅ Conforme | Utilisation de `SO_RCVTIMEO` et boucles de retransmission (limite de 5 essais) pour gérer les pertes réseau côté client et serveur. |
| **Terminaison** | Fin de transfert | ✅ Conforme | Le transfert se termine correctement à la réception d'un bloc de données de taille < 512 octets. |


## Compilation et Exécution

### Compilation
```bash
gcc server.c -o server
gcc client.c -o client
```

### Lancement du Serveur
```bash
./server
```
Le serveur écoute sur le port 69 (ou port privilégié, nécessite `sudo` si < 1024, sinon modifier PORT dans le code).

### Lancement du Client
**Lecture (GET)** :
```bash
./client <ip_serveur> get <nom_fichier>
```

**Écriture (PUT)** :
```bash
./client <ip_serveur> put <nom_fichier>
```