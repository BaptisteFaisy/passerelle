> **Provenance / Provenance** : copie integrale et fidele du cahier des charges source « Partage d un dossier du PC avec QNX sous QEMU (9P et virtiofs) », version 0.1 du 11 novembre 2026, recu tel quel par le coordinateur du projet Passerelle. Ce document fait foi ; sa traduction anglaise est dans [spec-en.md](spec-en.md).

# Partage d'un dossier du PC avec QNX sous QEMU (9P et virtiofs) — cahier des charges

**Nom de travail : « Passerelle ».**
Version 0.1 — 11 novembre 2026 — projet candidat au QNX Global Hackathon.
Date limite de soumission : mardi 15 décembre 2026 à 20 h, heure de Paris.

---

## 0. Lecture rapide

**Le besoin.** Aujourd'hui, pour essayer dans QNX un programme qu'on vient de compiler sur le PC, il faut le copier dans la machine virtuelle ou reconstruire l'image disque. Un dossier partagé supprime cette étape : le fichier compilé sur le PC est visible et exécutable dans QNX à l'instant.

**Ce qu'il faut savoir avant tout.** L'idée de départ, « personne n'a écrit de client 9P pour QNX », est fausse. Deux projets publics existent, et l'un est très abouti :

- `gvergine/qnx-fs-9p` (octobre 2026) : client 9P complet, pensé pour le développement quotidien, mais **limité aux invités ARM64**. Il ne pilote pas le périphérique PCI des machines x86.
- `eclipse-score/qnx_unit_tests` : client 9P pour x86_64 et ARM64, mais **réduit au nécessaire pour des tests** : pas de liens symboliques, pas de changement de droits ni de dates.

**Ce qui reste réellement à faire, et qui a de la valeur :**

1. **Un partage complet sur x86_64**, l'architecture par défaut des images QEMU de QNX Everywhere. Aucun des deux projets ne l'offre.
2. **virtiofs pour un invité QNX.** C'est le successeur de 9P, plus rapide. Je n'en ai trouvé aucune réalisation publique.
3. **Un banc de conformité et de mesure** qui compare objectivement les clients existants et le nouveau.
4. **Un comportement temps réel** : une lecture prioritaire ne doit pas attendre derrière une grosse copie de basse priorité.

**Trajectoire recommandée.** S'appuyer sur l'existant pour le 9P, et créer du neuf avec virtiofs : atteindre vite un partage 9P utilisable sur x86_64, puis consacrer l'essentiel de l'effort à virtiofs et au banc de mesure. Le détail est en section 3.

**Trois niveaux d'ambition.** **S** socle, **C** complet, **X** extrême. Chaque exigence indique aussi si un projet existant l'annonce déjà.

**Deux décisions à prendre tout de suite** (section 3) : la catégorie de soumission, car le kit de pilotes QNX qui définit la catégorie « bas niveau » ne couvre pas ce type de pilote ; et le choix entre écrire un nouveau client 9P ou contribuer à un client existant.

---

## 1. Contexte

### 1.1 Le problème, vu d'un poste de développement

La boucle de travail actuelle sous QEMU ressemble à ceci : compiler sur le PC, copier le binaire dans l'invité (par `scp`, ou en reconstruisant l'image), l'exécuter, recommencer. Plusieurs projets du hackathon automatisent cette boucle sans la supprimer : d'après leurs descriptions, ils reconstruisent l'image ou livrent les fichiers à l'invité à chaque essai.

Avec un dossier partagé, la boucle devient : compiler, exécuter. Les bibliothèques, les données d'essai et les résultats vivent dans un dossier du PC.

### 1.2 Deux techniques de partage

| | 9P | virtiofs |
|---|---|---|
| Origine | Protocole de Plan 9 ; variante 9P2000.L pour Linux | Protocole FUSE transporté par virtio |
| Côté PC | Serveur intégré à QEMU, rien à lancer en plus | Un processus à part, `virtiofsd`, et la mémoire de l'invité déclarée partagée |
| PC pris en charge | Linux, et macOS depuis QEMU 7.0. Windows non intégré à ce jour | Linux |
| Périphérique QEMU | `virtio-9p-pci` (x86) ou `virtio-9p-device` (ARM) | `vhost-user-fs-pci` |
| Client QNX existant | Deux, voir section 2 | Aucun trouvé |
| Réputation | Simple, lent sur les opérations de métadonnées | Plus rapide, standard actuel des machines virtuelles Linux |

Les deux reposent sur le même transport : des files d'attente virtio partagées entre l'invité et QEMU. Écrire ce transport une fois sert aux deux.

### 1.3 Cadre du hackathon

- **Catégories possibles** : Logiciel bas niveau, Outils de développement, ou Contribution open source & portages. Le choix n'est pas évident, voir la décision D1.
- **Critères du jury**, à poids égal : réalisation technique, produit complet, impact, innovation.
- **À fournir** : dépôt public neuf sous licence libre, instructions de construction précisant la version de QNX, la cible et la chaîne de compilation, vidéo de moins de trois minutes, le tout en anglais.
- **Bonus cumulables** de 3 500 $ : meilleur usage des technologies propres à QNX ; meilleur tutoriel.

---

## 2. État de l'art

Lecture des dépôts faite les 10 et 11 novembre 2026, d'après leur documentation. Rien n'a encore été exécuté.

| | A — `gvergine/qnx-fs-9p` | B — `eclipse-score/qnx_unit_tests` (dossier `virtio9p`) |
|---|---|---|
| Date | Créé le 3 octobre 2026, un seul commit, aucune étoile | Créé en mars 2026, actif |
| Licence | Apache-2.0 | Apache-2.0 |
| But affiché | Développement quotidien : l'image reste minimale, programmes et bibliothèques s'exécutent depuis le partage | Partager binaires et données de test, « infrastructure de test seulement » |
| Architectures | **ARM64 uniquement.** x86_64 « non pris en charge et jamais testé » | x86_64 et ARM64 |
| Transport | virtio-mmio | PCI (découverte automatique, MSI-X avec repli) et MMIO, versions 1 et 2 |
| Opérations | Lecture, écriture, création, troncature, droits, propriétaire, dates, `fsync`, répertoires, renommage, liens symboliques et durs, `df`, exécution depuis le partage | Lecture, écriture, création, répertoires, suppression, renommage |
| Non pris en charge | Écarts listés dans son guide (non lus) | Droits, propriétaire, dates, liens symboliques, nœuds de périphérique, verrous |
| Fonctionnement | Plusieurs requêtes en vol, interruption par signal, démontage propre, reprise sur délai dépassé, cache de métadonnées facultatif | Non précisé |
| Privilèges | Sans droits root, avec trois capacités | Non précisé |
| Tests | Comparaison aux traces du serveur 9P de QEMU | Non précisé |

**Autres éléments utiles :**

- `kodebooth/qnx-virtio` (avril 2026, licence MIT) : une bibliothèque de transport virtio pour QNX 8 (cœur, files, PCI, MMIO) avec un pilote GPIO d'exemple. Pas de système de fichiers.
- **QNX Hypervisor** documente un périphérique virtio-fs, mais QNX y est l'**hôte** qui partage ses dossiers avec des invités Linux ou Android. Je n'ai trouvé aucune mention d'un pilote pour invité QNX.
- Recherches GitHub sans résultat, sur le nom et la description des dépôts : « qnx virtiofs », « qnx virtio-fs », « qnx virtfs », « qnx 9pfs », « qnx shared folder ».

**Ce que personne n'offre aujourd'hui :**

| Manque | Pourquoi il compte |
|---|---|
| Partage complet (liens, droits, exécution) sur invité x86_64 | C'est l'architecture par défaut de `mkqnximage` |
| virtiofs côté invité QNX | Technique plus rapide ; premier du genre |
| Mesures comparées et banc de conformité | Aucun chiffre public ne permet de choisir |
| Ordonnancement des requêtes selon la priorité | C'est ce qu'on attend d'un pilote sur un système temps réel |
| Mise en route en une commande sur une image `mkqnximage` | Sans elle, l'outil reste réservé aux initiés |

---

## 3. Stratégie et décisions

### 3.1 Trois façons d'aborder le projet

| Option | Contenu | Pour | Contre |
|---|---|---|---|
| 1 — Tout réécrire | Nouveau client 9P complet, puis virtiofs | Maîtrise totale, architecture cohérente | La partie 9P refait ce que A fait déjà ; deux semaines dépensées sans nouveauté |
| 2 — Contribuer | Ajouter le transport PCI et la cible x86_64 au projet A, proposer la modification à son auteur | Rapide, honnête, utile tout de suite | Dépend d'un auteur inconnu ; gloire modeste |
| 3 — virtiofs d'abord | Nouveau pilote virtiofs pour invité QNX ; les clients 9P existants servent de référence | Vraie nouveauté, mesurable | Plus risqué ; côté PC réservé à Linux |

**Recommandation : combiner 2 et 3.** Le transport PCI est nécessaire dans les deux cas. On l'écrit une fois, on s'en sert pour offrir le 9P complet sur x86_64, puis on construit virtiofs par-dessus. Le banc de conformité et de mesure s'applique à tous les clients et devient un livrable à part entière.

Si l'auteur de A ne répond pas, le socle 9P sur x86_64 est réalisé dans le nouveau dépôt, en réutilisant sa bibliothèque de protocole avec attribution (la licence Apache-2.0 le permet) ou en la réécrivant.

### 3.2 Décisions à prendre

| | Décision | Recommandation |
|---|---|---|
| D1 | **Catégorie.** Le kit de pilotes QNX (DDK), qui définit « Logiciel bas niveau », couvre le série, l'I2C, le SPI, le HID, le tactile, le réseau et le CAN. Un pilote virtio de système de fichiers n'en utilise aucune partie. | Poser la question aux organisateurs sur le Discord. Sans réponse favorable, soumettre en « Outils de développement », où le projet correspond à la définition mais où la concurrence est la plus forte. |
| D2 | **Nouveau client 9P ou contribution.** | Contribution d'abord (option 2), repli sur le nouveau dépôt. |
| D3 | **Transport : réutiliser `kodebooth/qnx-virtio` ou l'écrire.** | L'évaluer pendant la première semaine. L'écrire si la reprise coûte plus d'une journée. |
| D4 | **Cache.** Un cache accélère, mais peut masquer une modification faite sur le PC. | Aucun cache de données par défaut. La documentation de QEMU signale que les caches du client Linux ne revalident pas les changements faits côté hôte : c'est le piège à éviter pour un usage de développement. |

### 3.3 Règle de présentation

La soumission cite les deux projets existants et dit précisément ce qu'elle ajoute. Un des juges dirige l'ingénierie open source de QNX ; passer l'existant sous silence serait une faute.

---

## 4. Périmètre

### 4.1 Objectifs

- Monter un dossier du PC dans un invité QNX 8.0 sous QEMU, en lecture et en écriture, sur x86_64 et ARM64.
- Exécuter un programme et charger ses bibliothèques directement depuis le partage.
- Se comporter comme un système de fichiers POSIX, écarts documentés.
- Prouver la conformité et mesurer les performances, face aux clients existants.
- Offrir une mise en route en une commande.

### 4.2 Hors périmètre

- Les autres hyperviseurs (VMware, VirtualBox, QNX Hypervisor comme hôte).
- Le partage réseau entre machines réelles : QNX a déjà des clients NFS et CIFS.
- Les attributs étendus : QNX n'a pas d'interface pour eux.
- La projection en mémoire partagée et modifiable d'un fichier du partage.
- L'accès direct à la mémoire de l'hôte (DAX) de virtiofs.

### 4.3 Hypothèses

- QNX SDP 8.0 par QNX Everywhere, images produites par `mkqnximage`.
- PC sous Linux. Le 9P doit aussi fonctionner avec un PC sous macOS, sans que ce soit testé au niveau socle.
- QEMU 8.2 ou plus récent.

---

## 5. Conception proposée

Cette section décrit une solution. Seules les sections 6 et 7 engagent.

### 5.1 Trois couches

```
  programmes QNX :  open  read  write  stat  readdir  exec ...
        │   messages QNX, traités à la priorité de l'appelant
 ┌──────▼─────────────────────────────────────────────────────┐
 │ Couche 3 — gestionnaire de ressources « fichiers »         │
 │   chemins, descripteurs, répertoires, liens, droits        │
 ├──────────────────────────────┬─────────────────────────────┤
 │ Couche 2 — protocole 9P2000.L│ Couche 2 — protocole FUSE   │
 │                              │ (virtiofs)                  │
 ├──────────────────────────────┴─────────────────────────────┤
 │ Couche 1 — transport virtio                                │
 │   files partagées, PCI ou MMIO, interruptions              │
 └──────┬─────────────────────────────────────────────────────┘
        │
   QEMU : serveur 9P intégré      ou      virtiofsd sur le PC
        │
   dossier du PC
```

- **Couche 1** est commune aux deux protocoles, et réutilisable pour d'autres pilotes virtio.
- **Couche 2** est écrite en C portable. Elle se compile aussi sur le PC, où elle est testée et soumise à des entrées aléatoires.
- **Couche 3** est commune : les deux protocoles offrent la même interface interne (chercher, ouvrir, lire, écrire, lister, créer, renommer, supprimer).

### 5.2 Trajet d'une requête

1. Un programme appelle `read()`. La bibliothèque C envoie un message au pilote.
2. Un fil du pilote le reçoit, à la priorité de l'appelant.
3. Il fabrique la requête du protocole dans un tampon, la place dans la file virtio et prévient QEMU.
4. Il rend la main sans répondre : le client reste bloqué, le fil est libre pour une autre requête.
5. QEMU exécute l'opération sur le dossier du PC et dépose la réponse.
6. Une interruption réveille un fil dédié, qui retrouve la requête par son étiquette et répond au client.

### 5.3 Points délicats

| Sujet | Difficulté | Traitement |
|---|---|---|
| Valeurs Linux | 9P2000.L transporte les options d'ouverture et les codes d'erreur avec leurs valeurs numériques Linux, différentes de celles de QNX | Tables de traduction dans les deux sens, testées valeur par valeur |
| Liens symboliques | Sous QNX, c'est le client qui reprend la résolution du chemin : le pilote doit renvoyer une réponse de redirection, pas suivre le lien lui-même | À valider sur la documentation des messages de connexion ; essai dédié |
| Exécution depuis le partage | Le chargeur de programmes lit et projette le fichier à travers le pilote | Essai de faisabilité E3 |
| Interruptions | QNX 8.0 a supprimé `InterruptAttach()` ; il faut attacher un fil à l'interruption | `InterruptAttachThread()` ou `InterruptAttachEvent()` |
| Interruption PCI | Le projet B programme MSI-X lui-même et dépend d'un réglage du serveur PCI | Essai de faisabilité E1 |
| Taille des messages | Trop petite, les débits s'effondrent. Le client Linux a longtemps utilisé 8 Kio par défaut, puis 128 Kio | Valeur par défaut choisie par mesure, réglable |
| Identités | QEMU propose plusieurs modèles de sécurité ; le recommandé enregistre droits et propriétaires dans des attributs du fichier | Modèle recommandé par défaut, droits contrôlés dans l'invité |
| Numéros de fichier | Deux fichiers de disques différents du PC peuvent avoir le même numéro | Recommander l'option de QEMU qui les renumérote |
| Casse | Sur un PC macOS, le système de fichiers ignore souvent la casse | Documenté, non corrigé |

### 5.4 Mise en route visée

```
PC      $ qshare ~/projet              # affiche ou ajoute les options QEMU, prépare l'image
invité  # ls /host                     # le dossier du PC
PC      $ make                         # compile
invité  # /host/build/mon_programme    # s'exécute, sans copie
```

---

## 6. Exigences

Niveau : **S** socle, **C** complet, **X** extrême.
Vérification : **T** test automatique, **M** mesure, **I** inspection, **D** démonstration.
Existant : **A** annoncé par `gvergine/qnx-fs-9p` (ARM64 seulement), **B** annoncé par `eclipse-score`, **—** annoncé par aucun, **?** non précisé.

### 6.1 Transport virtio

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| VIO-01 | Le pilote trouve le périphérique 9P sur le bus PCI (vendeur 0x1AF4, périphérique 0x1009, ou 0x1049 en mode moderne seul). | S | T | B |
| VIO-02 | Il utilise l'interface PCI moderne décrite par les capacités du périphérique. | S | T | B |
| VIO-03 | Il utilise virtio-mmio, versions 1 et 2 (machine `virt` ARM64, machine `microvm` x86). | C | T | A, B |
| VIO-04 | Il reçoit les interruptions par les interfaces de QNX 8.0, par MSI-X avec repli sur l'interruption classique. | S | T | B |
| VIO-05 | Il lit l'étiquette de partage dans la configuration du périphérique et choisit le périphérique par cette étiquette. | S | T | A |
| VIO-06 | Plusieurs partages sont montés en même temps, un par périphérique. | C | T | ? |
| VIO-07 | Les tampons de requête sont remis à QEMU sans copie intermédiaire dans le transport. | C | I | ? |
| VIO-08 | Les descripteurs indirects et la suppression des notifications inutiles sont négociés quand QEMU les propose. | C | T, M | ? |
| VIO-09 | Au démontage, le périphérique est réinitialisé et aucune interruption ne reste attachée. | S | T | A |
| VIO-10 | La couche de transport est une bibliothèque documentée, utilisable par un autre pilote virtio. | C | I, D | — |

### 6.2 Protocole 9P2000.L

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| P9-01 | Négociation de la version 9P2000.L et de la taille maximale de message, réglable. | S | T | A, B |
| P9-02 | Messages : attachement, parcours de chemin, ouverture, création, lecture, écriture, fermeture, attributs, lecture de répertoire, création de répertoire, suppression, renommage, synchronisation, statistiques. | S | T | A, B |
| P9-03 | Messages : modification d'attributs, lien symbolique, lecture de lien, lien dur. | C | T | A |
| P9-04 | Les options d'ouverture sont traduites vers leurs valeurs Linux, et les codes d'erreur Linux vers ceux de QNX. | S | T | A |
| P9-05 | Plusieurs requêtes sont en vol en même temps. | C | T | A |
| P9-06 | Un appel interrompu (signal, délai) annule sa requête auprès du serveur. | C | T | A |
| P9-07 | Tout identifiant de fichier ouvert auprès du serveur est libéré. Aucune fuite après un million d'ouvertures et fermetures. | S | T | ? |
| P9-08 | Lectures, écritures et listes de répertoire plus grandes qu'un message sont découpées et reprises. | S | T | ? |
| P9-09 | Chaque réponse est validée : taille, type, étiquette, cohérence des longueurs internes. | S | T | ? |
| P9-10 | La bibliothèque de protocole se compile et se teste sur le PC, sans QNX. | S | T | ? |

### 6.3 Comportement de système de fichiers

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| FS-01 | Ouvrir, lire, écrire, se déplacer, fermer ; fichiers de plus de 4 Gio. | S | T | A, B |
| FS-02 | `stat` renvoie taille, type, droits, propriétaire, dates à la nanoseconde, nombre de liens et un numéro de fichier stable. | S | T | A |
| FS-03 | La lecture de répertoire est cohérente : pas d'entrée perdue ni doublée, reprise à une position, type de chaque entrée renseigné. | S | T | ? |
| FS-04 | Création exclusive, troncature à l'ouverture, écriture en fin de fichier. | S | T | A, B |
| FS-05 | Créer et supprimer des répertoires, supprimer des fichiers, renommer de façon atomique, y compris d'un répertoire à un autre. | S | T | A, B |
| FS-06 | Un programme s'exécute depuis le partage et y charge ses bibliothèques partagées. | S | T, D | A |
| FS-07 | Changer droits, propriétaire, dates et taille. | C | T | A |
| FS-08 | Liens symboliques : création, lecture, résolution, détection des boucles. Liens durs. | C | T | A |
| FS-09 | Les droits sont contrôlés dans l'invité selon l'identité de l'appelant. Options de montage : lecture seule, sans exécution, sans élévation de privilèges. | C | T | A |
| FS-10 | `fsync` atteint le PC ; `df` renvoie l'espace du dossier partagé. | C | T | A |
| FS-11 | Un fichier supprimé mais encore ouvert reste lisible. | C | T | ? |
| FS-12 | Un fichier modifié sur le PC est vu modifié dans l'invité à la lecture suivante, sans remontage. | S | T | ? |
| FS-13 | Un cache d'attributs facultatif a une durée de validité bornée et réglable. | C | T | A |
| FS-14 | Les verrous de fichier fonctionnent entre processus de l'invité. Ils ne sont pas relayés au PC, et c'est documenté. | C | T | — |
| FS-15 | Noms en UTF-8 jusqu'à 255 octets, caractères spéciaux compris. | C | T | ? |
| FS-16 | Les verrous sont relayés au serveur. | X | T | — |

### 6.4 Intégration à QNX

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| QNX-01 | Le pilote est un gestionnaire de ressources qui prend en charge toute l'arborescence sous son point de montage. | S | I | A, B |
| QNX-02 | Il démarre depuis l'image de démarrage, sans autre dépendance que le noyau, la bibliothèque C et le serveur PCI. | S | T | A |
| QNX-03 | Le traitement d'une requête s'exécute à la priorité du programme appelant, ce qu'une trace noyau montre. | S | D | ? |
| QNX-04 | Quand la file est pleine, les requêtes en attente partent par ordre de priorité. | C | M | — |
| QNX-05 | Un client bloqué n'immobilise pas un fil du pilote. | C | T | ? |
| QNX-06 | Le pilote fonctionne sans droits root, avec le minimum de capacités, et une politique de sécurité QNX est fournie. | C | T | A |
| QNX-07 | Il s'utilise avec les commandes `mount` et `umount`. | C | D | B |
| QNX-08 | Il écrit dans le journal système et expose des compteurs par opération. | C | D | ? |
| QNX-09 | Il insère des événements dans la trace noyau à l'envoi et à la réception de chaque requête. | C | M | — |

### 6.5 Performances

Sous QEMU, les durées dépendent de la machine. Les exigences sont donc exprimées en comparaison, et en nombre d'allers-retours, qui ne dépend pas de la machine.

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| PERF-01 | Le nombre d'allers-retours avec le serveur est mesuré pour chaque appel POSIX courant, à partir des traces de QEMU, et publié. | S | M | ? |
| PERF-02 | Ce nombre ne dépasse pas celui du meilleur client existant sur chaque appel. | C | M | — |
| PERF-03 | Lecture et écriture séquentielles d'un fichier de 1 Gio : au moins le débit du meilleur client existant ; au moins la moitié de celui d'un invité Linux sur la même machine. | C | M | — |
| PERF-04 | Métadonnées : créer, lire les attributs et supprimer 10 000 fichiers ; parcourir un arbre de 50 000 entrées. Mêmes seuils. | C | M | — |
| PERF-05 | Une lecture prioritaire de 4 Kio, lancée pendant une copie massive de basse priorité, ne prend pas plus du double de son temps à vide. | C | M | — |
| PERF-06 | Le montage prend moins de 50 ms. | C | M | ? |
| PERF-07 | Un rapport comparatif est publié : ce pilote, A, B, la copie par `scp`, et virtiofs au niveau extrême. | C | D | — |

### 6.6 Robustesse

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| ROB-01 | Toute erreur du serveur revient au programme comme un code d'erreur correct, jamais comme un blocage. | S | T | ? |
| ROB-02 | La mort d'un programme pendant une requête ne laisse ni fichier ouvert côté serveur ni mémoire perdue. | C | T | ? |
| ROB-03 | Le démontage sous charge se termine proprement. | C | T | A |
| ROB-04 | Un fichier supprimé ou remplacé sur le PC pendant son usage donne des erreurs propres. | C | T | ? |
| ROB-05 | Une requête sans réponse est abandonnée après un délai réglable. | C | T | A |
| ROB-06 | La bibliothèque de protocole ne plante sur aucune réponse, vérifié par des entrées aléatoires avec détection d'erreurs mémoire. | C | T | ? |
| ROB-07 | Face à un serveur volontairement fautif (réponses tronquées, étiquettes inconnues, lenteur), le pilote ne plante pas et ne corrompt rien. | X | T | — |

### 6.7 Sécurité

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| SEC-01 | Par défaut, le partage est monté sans élévation de privilèges par les fichiers. | S | T | A |
| SEC-02 | Le pilote n'accède à aucun autre périphérique que le sien. | C | I | ? |
| SEC-03 | La documentation explique les modèles de sécurité de QEMU et recommande celui qui n'exige pas de lancer QEMU en root. | S | I | ? |

### 6.8 Confort d'utilisation

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| DX-01 | Une commande sur le PC, `qshare`, donne les options QEMU nécessaires et fournit le fragment `mkqnximage` qui embarque le pilote et monte le partage au démarrage. | S | D | — |
| DX-02 | La démonstration complète se rejoue en une commande. | S | D | — |
| DX-03 | Une commande `qrun` exécute dans l'invité un programme du partage et renvoie sa sortie et son code de retour. | C | D | — |
| DX-04 | Des exemples montrent le lancement des tests d'un projet CMake et d'un projet Rust dans l'invité. | C | D | — |
| DX-05 | Guide d'utilisation, guide de conception, et tutoriel « écrire un pilote virtio pour QNX 8.0 ». | C | I | A en partie |
| DX-06 | Le système entier démarre avec sa racine sur le partage : l'image ne contient que le noyau et le pilote. | X | D | A en partie |

### 6.9 virtiofs

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| VFS-01 | Le pilote trouve le périphérique virtiofs (identifiant virtio 26) et lit son étiquette et son nombre de files de requêtes. | X | T | — |
| VFS-02 | Il utilise la file prioritaire pour les messages d'oubli et d'interruption, et les files de requêtes pour le reste. | X | T | — |
| VFS-03 | Il négocie la version du protocole FUSE et prend en charge recherche, attributs, ouverture, lecture, écriture, fermeture, répertoires, création, suppression, renommage, liens, statistiques et synchronisation. | X | T | — |
| VFS-04 | Il tient à jour les compteurs de référence des fichiers connus du serveur et les lui rend. | X | T | — |
| VFS-05 | Il réutilise la couche 3 et passe le même banc de conformité que le client 9P. | X | T | — |
| VFS-06 | `qshare` lance `virtiofsd` et configure la mémoire partagée de QEMU. | X | D | — |
| VFS-07 | Le rapport comparatif inclut virtiofs face aux clients 9P. | X | M | — |

### 6.10 Sens inverse

| ID | Exigence | Niv. | Vérif. | Existant |
|---|---|---|---|---|
| SRV-01 | Un serveur 9P tournant dans QNX exporte une arborescence de l'invité, que le PC monte avec le client 9P de Linux par le réseau. | X | D | — |
| SRV-02 | L'export peut inclure `/proc` et `/dev`, pour inspecter l'invité depuis le PC avec des outils ordinaires. | X | D | — |

---

## 7. Plan de validation

### 7.1 Cinq niveaux d'essai

| Niveau | Où | Contenu |
|---|---|---|
| 0 — unitaire | PC | Codage des messages, tables de traduction, gestion des identifiants, logique de chemins |
| 1 — entrées aléatoires | PC | Analyse des réponses soumise à des données aléatoires, avec détection d'erreurs mémoire |
| 2 — conformité | QEMU | Test différentiel, vérification de contenu, cas dirigés |
| 3 — performance | QEMU | Allers-retours par appel, débits, métadonnées, priorité |
| 4 — robustesse | QEMU | Arrêts forcés, démontage sous charge, modifications côté PC, serveur fautif |

### 7.2 Le test différentiel

Le dossier du PC est la vérité. Le banc exécute dans l'invité des suites aléatoires d'opérations sur le partage, puis compare l'arborescence vue par l'invité à celle du PC : noms, types, tailles, droits, dates, cibles des liens, empreinte du contenu.

- Chaque suite est rejouable à partir de sa graine.
- Toute divergence est réduite automatiquement à la plus courte suite qui la reproduit, puis conservée comme test de non-régression.
- Le même banc s'applique tel quel aux clients A et B, et plus tard à virtiofs. C'est ce qui rend la comparaison objective.

### 7.3 Vérification de contenu

Un exerciseur écrit, lit, tronque et rallonge un fichier au hasard en gardant en mémoire le contenu attendu, et vérifie chaque lecture. Il tourne seul, puis à seize processus en parallèle sur des fichiers distincts et sur un fichier commun.

### 7.4 Cas dirigés

Une table de cas couvre ce que le hasard atteint mal : renommage sur un fichier existant, suppression d'un fichier ouvert, création exclusive concurrente, matrice de droits, boucle de liens symboliques, nom de 255 octets, fichier de 5 Gio, répertoire de 100 000 entrées, chemin de longueur maximale.

### 7.5 Mesures

| Charge | Ce qu'elle mesure |
|---|---|
| Lecture puis écriture d'un fichier de 1 Gio | Débit, selon la taille de message |
| 10 000 petits fichiers créés, lus, supprimés | Coût des métadonnées |
| Parcours d'un arbre de 50 000 entrées | Lecture de répertoires |
| Décompression d'une archive de sources puis recherche de texte | Charge mixte proche d'un vrai usage |
| Lancement de 1 000 programmes depuis le partage | Chemin d'exécution |
| Lecture prioritaire pendant une copie de basse priorité | Comportement temps réel |

Chaque charge est exécutée sur ce pilote, sur A (ARM64), sur B, sur un invité Linux de référence, et avec la méthode de copie classique.

### 7.6 Seuils de réussite

| Mesure | Socle | Complet | Extrême |
|---|---|---|---|
| Opérations du test différentiel sans divergence | 100 000 | 1 000 000 sur x86_64 et ARM64 | Idem sur virtiofs |
| Opérations de l'exerciseur de contenu | 1 000 000 | 10 000 000, à seize processus | Idem sur virtiofs |
| Cas dirigés réussis | Ceux du niveau socle | Tous | Tous sur virtiofs |
| Entrées aléatoires sur l'analyse des réponses | — | 1 heure sans défaut | 24 heures |
| Fuites après un million d'ouvertures | 0 | 0 | 0 |
| Rapport comparatif | Allers-retours par appel | Complet face à A et B | Avec virtiofs |

---

## 8. Démonstration (vidéo de moins de trois minutes)

| Temps | À l'écran | Message |
|---|---|---|
| 0:00 – 0:20 | La boucle actuelle : compiler, copier ou reconstruire l'image, exécuter | Chaque essai coûte une copie |
| 0:20 – 0:55 | `qshare ~/projet`, puis `make` sur le PC et exécution immédiate dans QNX ; un fichier modifié sur le PC change sous les yeux dans l'invité | Plus de copie |
| 0:55 – 1:25 | Le test différentiel défile, compteur à un million, zéro divergence | Ce n'est pas un bricolage |
| 1:25 – 2:00 | Graphique : clients 9P existants, ce pilote, virtiofs | Le chiffre qui justifie le projet |
| 2:00 – 2:30 | Une lecture prioritaire pendant une grosse copie ; la trace noyau montre la priorité héritée | Un pilote pensé pour un système temps réel |
| 2:30 – 2:45 | Le pilote tourne sans droits root | Moindre privilège |
| 2:45 – 3:00 | Dépôt, tutoriel, mention des projets existants et de ce qui est ajouté | Honnêteté et réutilisation |

---

## 9. Livrables et dépôt

```
passerelle/
├── README.md            anglais : ce que c'est, ce qui existait, ce qui est ajouté, démo en une commande
├── LICENSE              Apache-2.0
├── NOTICE               attribution des projets réutilisés
├── virtio/              transport : cœur, files, PCI, MMIO, transport simulé pour les tests
├── proto9p/             protocole 9P2000.L, portable
├── protofuse/           protocole FUSE, portable (niveau extrême)
├── rmfs/                couche gestionnaire de ressources commune
├── drivers/             les deux pilotes
├── host/
│   ├── qshare/          mise en route côté PC
│   ├── fsproof/         test différentiel, exerciseur, cas dirigés
│   └── bench/           mesures et rapport
├── image/               fragments mkqnximage, politique de sécurité
├── upstream/            modifications proposées aux projets existants
├── docs/                guides, résultats, ce cahier des charges
└── codelab/             tutoriel
```

- **Langages** : C11 pour les pilotes et les bibliothèques ; Python 3 pour les outils sur PC.
- **Preuves d'usage de QNX** à pointer dans la description : gestionnaire de ressources, héritage de priorité par message, fils d'interruption de QNX 8.0, capacités et politique de sécurité, serveur PCI, trace noyau.
- **Tutoriel** candidat au bonus : « Écrire un pilote virtio pour QNX 8.0, de la découverte PCI au premier octet lu ».

---

## 10. Planning

Il reste cinq semaines moins un jour.

| Semaine | Dates | Contenu | Jalon |
|---|---|---|---|
| 1 | 11 – 17 nov. | Essais de faisabilité E1 à E5. Exécution des clients A et B, premières mesures. Transport PCI : la négociation de version 9P aboutit sur x86_64. Questions posées sur le Discord et à l'auteur de A. | Décisions D1 à D3 prises |
| 2 | 18 – 24 nov. | Partage 9P en lecture, écriture et exécution sur x86_64, par contribution à A ou dans le nouveau dépôt. `qshare`. Test différentiel, première version. | **Socle atteint** : compiler sur le PC, exécuter dans QNX |
| 3 | 25 nov. – 1er déc. | Protocole FUSE portable et pilote virtiofs en lecture. Exerciseur de contenu, cas dirigés. | virtiofs lit un fichier |
| 4 | 2 – 8 déc. | virtiofs en écriture. Ordonnancement par priorité. Mesures comparatives. ARM64. Moindre privilège. | Rapport comparatif complet |
| 5 | 9 – 14 déc. | README, tutoriel, vidéo. Soumission le 14 au soir. | Soumis, un jour de marge |

**Essais de faisabilité (quatre premiers jours).**

| Essai | Question | Si la réponse est non |
|---|---|---|
| E1 | Sous QEMU x86_64, un pilote QNX obtient-il l'interruption du périphérique virtio et échange-t-il un premier message 9P ? | Reprendre la méthode du projet B pour MSI-X ; à défaut, scrutation périodique le temps d'avancer |
| E2 | Les clients A et B fonctionnent-ils comme annoncé ? Que mesurent-ils ? | Les écarts trouvés deviennent des arguments du projet |
| E3 | QNX exécute-t-il un programme servi par un gestionnaire de ressources tiers, bibliothèques partagées comprises ? | Étudier comment A y parvient ; à défaut, limiter l'objectif aux données |
| E4 | `mkqnximage` accepte-t-il des options QEMU supplémentaires ? | `qshare` lance QEMU lui-même sur l'image produite |
| E5 | Sur ce PC, `virtiofsd` et la mémoire partagée fonctionnent-ils avec un invité Linux ? | virtiofs sort du projet ; l'effort passe sur le 9P complet et les mesures |

**Ce qui saute en premier en cas de retard** : le sens inverse, la racine sur le partage, le serveur fautif, ARM64 pour virtiofs. Le socle et le banc de conformité ne sautent pas.

---

## 11. Risques

| Risque | Gravité | Parade |
|---|---|---|
| Le projet est perçu comme une redite des clients existants | Forte | Positionnement de la section 3 ; virtiofs et les mesures comme apport central ; existant cité |
| La catégorie « bas niveau » est refusée faute d'usage du kit de pilotes | Forte | Question posée dès la première semaine ; repli sur « Outils de développement » |
| Un pilote virtiofs pour invité QNX existe déjà hors de GitHub | Moyenne | Question posée sur le Discord en semaine 1 |
| L'interruption PCI est difficile à obtenir sous QEMU | Moyenne | Essai E1 ; méthode du projet B documentée |
| L'exécution depuis le partage ne fonctionne pas sur x86_64 | Moyenne | Essai E3 ; A le fait sur ARM64 |
| virtiofs dépasse le temps disponible | Forte | Jalon intermédiaire « lit un fichier » en fin de semaine 3 ; le socle 9P est déjà livrable |
| L'auteur de A ne répond pas | Faible | Repli prévu en 3.1 |
| Les durées mesurées sous QEMU sont contestées | Moyenne | Allers-retours par appel, indépendants de la machine ; comparaisons relatives sur la même machine |
| Un cache masque une modification faite sur le PC | Moyenne | Pas de cache de données par défaut ; exigence FS-12 testée |

---

## 12. Pousser l'idée au bout

Au-delà du niveau complet, par ordre d'intérêt :

1. **virtiofs** (6.9) : la nouveauté principale.
2. **Racine sur le partage** : l'image ne contient plus que le noyau et le pilote ; tout le système vit dans un dossier du PC. Modifier un service système ne demande plus de reconstruire l'image.
3. **Sens inverse** (6.10) : parcourir `/proc` et `/dev` de l'invité depuis le PC.
4. **Serveur fautif** : un serveur 9P scripté, joint par le réseau, qui répond mal exprès.
5. **Transport réseau** pour le client 9P, utile sur une vraie carte comme le Raspberry Pi 5.
6. **Notification de changement** : prévenir un programme de l'invité quand un fichier du partage change sur le PC.
7. **Portage d'une suite de conformité POSIX reconnue**, en complément du banc maison.
8. **Modèle du protocole** : les états d'un identifiant de fichier décrits formellement et vérifiés par exploration.
9. **Intégration à l'extension VS Code de QNX** : lancer et déboguer un programme du partage en un clic.
10. **Paquet pour l'écosystème QNX** : recette de construction proposée au dépôt des portages.

---

## 13. Critères d'acceptation

**Socle — soumission possible.**

- [ ] Sur une image `mkqnximage` x86_64, une commande monte un dossier du PC.
- [ ] Un programme compilé sur le PC s'exécute dans QNX sans copie.
- [ ] Un fichier modifié sur le PC est vu modifié dans l'invité.
- [ ] 100 000 opérations de test différentiel sans divergence.
- [ ] Nombre d'allers-retours par appel publié.
- [ ] Dépôt public, licence, README en anglais citant l'existant, vidéo.

**Complet — version visée.**

- [ ] Toutes les exigences de niveau C vérifiées sur x86_64 et ARM64.
- [ ] Un million d'opérations sans divergence ; exerciseur à seize processus.
- [ ] Rapport comparatif face à A et B.
- [ ] Lecture prioritaire mesurée pendant une copie de basse priorité.
- [ ] Pilote sans droits root ; tutoriel publié.

**Extrême.**

- [ ] virtiofs passe le même banc de conformité et figure dans le rapport comparatif.
- [ ] Au moins un autre élément de la section 12.

---

## 14. Ce qui est vérifié et ce qui reste à valider

| Affirmation | Statut |
|---|---|
| Existence et caractéristiques annoncées des clients A et B | Lu dans leur documentation. **Non exécutés.** |
| Contenu de `kodebooth/qnx-virtio` | Liste des fichiers lue ; code non examiné |
| Le kit de pilotes QNX ne couvre ni virtio ni les systèmes de fichiers | Vérifié sur la page du kit |
| Options de QEMU pour le 9P, modèles de sécurité, remarques sur la taille de message et les caches | Vérifié sur la documentation de QEMU |
| macOS pris en charge comme hôte depuis QEMU 7.0 ; Windows non intégré | Vérifié sur la documentation de QEMU |
| Messages de 9P2000.L ; valeurs Linux des options et des erreurs | Vérifié sur la description du protocole du projet diod |
| Configuration du périphérique 9P (étiquette précédée de sa longueur) | Vérifié sur l'en-tête Linux cité par la recherche |
| Identifiants PCI 0x1049 et 0x105A | Déduits de la règle de la spécification virtio, non relus |
| virtiofs : identifiant 26, étiquette de 36 octets, file prioritaire et files de requêtes | D'après des sources secondaires ; la section de la spécification officielle reste à relire |
| Mise en route de virtiofs côté PC | Vérifié sur le guide du projet virtiofs |
| `InterruptAttach()` supprimée dans QNX 8.0 | Vérifié dans le guide de migration |
| Aucun pilote virtiofs public pour invité QNX | Recherches sans résultat. **Absence non prouvée** : à demander sur le Discord |
| Résolution des liens symboliques par redirection ; exécution depuis un gestionnaire de ressources | **Hypothèses**, essais dédiés |
| Options QEMU supplémentaires par `mkqnximage` | Non trouvé dans la documentation, essai E4 |
| Limite de taille de message imposée par la file virtio de QEMU | De mémoire, à mesurer |

---

## 15. Glossaire

| Terme | Sens |
|---|---|
| 9P, 9P2000.L | Protocole d'accès à des fichiers distants issu de Plan 9 ; variante adaptée à Linux |
| virtiofs | Partage de fichiers pour machines virtuelles, qui transporte le protocole FUSE par virtio |
| FUSE | Protocole Linux entre le noyau et un système de fichiers réalisé par un programme ordinaire |
| virtio | Norme de périphériques virtuels ; l'invité et l'hôte échangent par des files en mémoire partagée |
| PCI, MMIO | Deux façons de présenter un périphérique virtio à l'invité : sur le bus PCI (x86) ou à une adresse mémoire fixe (ARM) |
| MSI-X | Mécanisme d'interruption des périphériques PCI |
| Étiquette de partage | Nom donné au partage sur la ligne de commande de QEMU, que l'invité utilise pour le monter |
| Taille de message | Taille maximale d'un échange 9P ; elle borne ce qu'une lecture ramène en un aller-retour |
| Aller-retour | Une requête envoyée au serveur et sa réponse |
| Gestionnaire de ressources | Processus QNX qui prend en charge une partie de l'arborescence des fichiers |
| Capacités | Droits fins qu'un processus QNX peut garder après avoir renoncé aux droits root |
| Test différentiel | Test qui compare le résultat obtenu à celui d'une référence, ici le dossier du PC |
| `mkqnximage` | Outil QNX qui fabrique et lance une image de machine virtuelle |

---

## 16. Sources

**Projets existants**

- https://github.com/gvergine/qnx-fs-9p
- https://github.com/eclipse-score/qnx_unit_tests (dossier `src/common/virtio9p`)
- https://github.com/kodebooth/qnx-virtio
- QNX Hypervisor, périphérique virtio-fs côté hôte : https://qnx.com/developers/docs/7.1/com.qnx.doc.qavf.overview/topic/virtfs_arch.html

**9P et QEMU**

- Mise en place du 9P sous QEMU : https://wiki.qemu.org/Documentation/9psetup
- Documentation développeur du 9P de QEMU : https://wiki.qemu.org/Documentation/9p
- Description de 9P2000.L (projet diod) : https://github.com/chaos/diod/blob/master/protocol.md

**virtiofs**

- Guide QEMU du projet virtiofs : https://virtio-fs.gitlab.io/howto-qemu.html

**QNX**

- Kit de pilotes (DDK) : https://www.qnx.com/developers/docs/qnxeverywhere/com.qnx.doc.ddk/topic/about.html
- Gestionnaires de ressources de type système de fichiers : https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/fsystems.html
- Lecture de répertoire par un gestionnaire de ressources : https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/fsystems_ReturnEntries.html
- Guide de migration, noyau (interruptions) : https://qnx.com/developers/docs/8.0/com.qnx.doc.qnxsdp.migration/topic/kernel.html
- `InterruptAttachThread()` : https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/interruptattachthread.html
- `mkqnximage` : https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnximage.html

**Hackathon**

- Page et règlement : https://qnx.devpost.com/ et https://qnx.devpost.com/rules
