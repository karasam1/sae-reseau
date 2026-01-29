# Projet TFTP (Client & Serveur) - RFC 1350

Ce projet implémente un client et un serveur TFTP (Trivial File Transfer Protocol) en langage C, conformes à la norme **RFC 1350**. Il permet le transfert de fichiers (lecture et écriture) via le protocole UDP de manière fiable grâce à un mécanisme de retransmission et de gestion des timeouts.

## 🚀 Fonctionnalités

*   **Conformité RFC 1350** : Respect du format des paquets (RRQ, WRQ, DATA, ACK, ERROR).
*   **Mode Client & Serveur** :
    *   **Client** : Permet de télécharger (`get`) ou téléverser (`put`) des fichiers vers un serveur TFTP.
    *   **Serveur** : Serveur itératif capable de gérer, stocker et servir des fichiers depuis un répertoire dédié (`.tftp/`).
*   **Fiabilité** :
    *   Gestion des **timeouts** (socket options + boucles de rejeu).
    *   **Retransmission** automatique des paquets perdus (Max 5 tentatives).
    *   Gestion du **TID (Transfer Identifier)** pour assurer l'unicité et la sécurité des sessions de transfert.
    *   Anti-doublons (lutte contre le *Sorcerer's Apprentice Syndrome*).
*   **Sécurité** :
    *   Protection contre le **Directory Traversal** (rejet des chemins contenant `..`).
    *   Vérification stricte de l'origine des paquets (IP et Port).

## 🛠️ Compilation

Le projet ne nécessite aucune dépendance externe hors de la bibliothèque standard C et des headers POSIX/BSD (`sys/socket.h`, `arpa/inet.h`, etc.).

Pour compiler le client et le serveur :

```bash
# Compiler le serveur
gcc server.c -o server

# Compiler le client
gcc client.c -o client
```

## 📖 Utilisation

### 1. Démarrer le Serveur

Le serveur écoute par défaut sur le port **69** (UDP). Notez que l'utilisation du port 69 nécessite généralement des privilèges administrateur (root).

```bash
sudo ./server
```
*Le serveur stockera les fichiers reçus et cherchera les fichiers demandés dans le dossier caché `.tftp/` (créé automatiquement).*

### 2. Utiliser le Client

La syntaxe d'utilisation du client est la suivante :

```bash
./client <ip_serveur> <commande> <fichier> [port]
```

*   **ip_serveur** : L'adresse IP du serveur TFTP (ex: `127.0.0.1`).
*   **commande** :
    *   `get` : Télécharger un fichier du serveur.
    *   `put` : Envoyer un fichier vers le serveur.
*   **fichier** : Nom du fichier à transférer.
*   **port** *(optionnel)* : Port du serveur (défaut : 69).

#### Exemples

**Téléchargement (GET) :**
Récupérer `image.png` depuis le serveur local :
```bash
./client 127.0.0.1 get image.png
```

**Envoi (PUT) :**
Envoyer `document.txt` vers le serveur :
```bash
./client 127.0.0.1 put document.txt
```

**Sur un port spécifique (ex: 8080) :**
```bash
./client 192.168.1.50 get data.bin 8080
```

## 📂 Structure du Projet

*   **`client.c`** : Code source du client. Gère l'analyse des arguments, l'initialisation socket, et les boucles de transfert (machines à états implicites).
*   **`server.c`** : Code source du serveur. Écoute sur le port principal, puis délègue le traitement RRQ/WRQ à des sockets éphémères dédiées.
*   **`.tftp/`** : Répertoire de stockage par défaut du serveur (créé à l'exécution).

## ⚠️ Notes Techniques

*   **Taille de bloc** : Fixée à 512 octets (standard RFC 1350).
*   **Timeout** : Configuré à 5 secondes avec 5 essais maximum avant abandon.
*   **Mode** : Le transfert se fait implicitement en mode "octet" (binaire), adapté à tous types de fichiers.

## Auteurs

Projet réalisé dans le cadre de la SAE Réseaux.
