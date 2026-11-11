# Test d'amorçage QEMU avec un invité Linux (E5, et le versant hôte de DX-01)

Ce banc répond à l'essai de faisabilité **E5** du cahier des charges : sur ce PC,
`virtiofsd` et la mémoire partagée fonctionnent-ils avec un invité ? Il vérifie
au passage le versant 9P des mêmes options.

**Ce qu'il prouve** : les options QEMU que `qshare --emit` affiche sont celles
que QEMU accepte vraiment, `virtiofsd` démarre et sert le dossier, un noyau
invité monte le partage avec l'étiquette annoncée, lit un fichier écrit par le
PC avant le démarrage, et en écrit un que le PC retrouve ensuite.

**Ce qu'il ne prouve pas** : quoi que ce soit sur le pilote QNX. L'invité est
Linux, donc le client est celui de Linux ; seul le **côté PC** de Passerelle est
exercé ici. E1, E3 et E4 demandent toujours le SDP QNX.

## Lancer

```bash
python3 tests/qemu/run_share_check.py                     # virtiofs puis 9p
python3 tests/qemu/run_share_check.py --protos 9p         # un seul protocole
python3 tests/qemu/run_share_check.py --workdir /tmp/e5   # garder les artefacts
```

Codes de retour : `0` tous les marqueurs sont PASS, `1` un contrôle a échoué,
`2` un prérequis manque (saut explicite, jamais un feu vert silencieux).

Prérequis : `qemu-system-x86_64`, `virtiofsd` (pour virtiofs), `curl`, `cpio`,
`gzip`, `unzstd`, un noyau hôte **lisible** dans `/boot` avec ses modules dans
`/lib/modules`. Sur Ubuntu, le noyau courant est en 0600 : le banc choisit tout
seul le noyau le plus récent qui soit lisible.

## Comment ça marche

1. `mkinitramfs.sh` fabrique un initramfs minimal : minirootfs Alpine (3,4 Mio,
   téléchargé une fois puis mis en cache dans `~/.cache/passerelle`) + les
   modules du système de fichiers du noyau hôte, renommés avec un préfixe
   numérique car **l'ordre de chargement compte** (`netfs` avant `9pnet` avant
   `9p`) + `guest-init.sh` comme `/init`.
2. `run_share_check.py` demande à `qshare` (par son API Python, `build_qemu_cmd`
   et `share_backend`) la ligne de commande et le démarrage de `virtiofsd` : ce
   sont donc bien les options produites par l'outil qui sont testées.
3. QEMU démarre en boot direct de noyau (`-kernel`/`-initrd`/`-append`), sans
   disque ; l'invité passe par `pgw.share` et `pgw.tag` sur la ligne de commande
   du noyau.
4. L'invité affiche des marqueurs `PGW-E5 PASS|FAIL|INFO` sur la console série,
   que l'hôte relit dans le journal de console, puis il s'éteint.
5. L'hôte vérifie que le fichier écrit par l'invité est bien arrivé dans le
   dossier partagé.

Un disque factice (`unused-disk.img`) est passé à `build_qemu_cmd` parce que
celui-ci choisit `-kernel` ou `-drive` d'après le nom de l'image et résout les
liens symboliques : un noyau hôte ne peut donc pas être présenté comme l'image.
Avec `-kernel` présent, QEMU ignore ce disque.

## Résultat mesuré

Voir `docs/verification-status.md` §3 (E5) pour le dernier résultat daté, avec
la durée et les marqueurs obtenus.
